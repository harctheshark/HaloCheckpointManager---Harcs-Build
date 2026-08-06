#pragma once

// Kills dllmain RealMain() thread
class GlobalKill
{
private:
	// ⚠ ATOMIC, NOT bool. This flag is written by the heartbeat thread and by the WndProc, and READ by the
	// MainThread teardown loop, the D3D12 present detour and every cheat's render callback. Every
	// "am I shutting down?" early-out in the codebase depends on seeing this promptly and on the writes that
	// preceded it being visible - which a plain bool does not guarantee. Benign on x86-64 in practice, but
	// the entire shutdown ordering rests on an edge that formally did not exist.
	std::atomic<bool> mKillFlag{ false };

	// singleton
	static GlobalKill& get() {
		static GlobalKill instance;
		return instance;
	}

	GlobalKill() = default;
	~GlobalKill() = default;
public:

	static void killMe()
	{
		// ⚠ SET THE FLAG FIRST, THEN LOG. The stacktrace below was MEASURED AT 403 ms in a real shutdown
		// (19:21:16.595 detection -> 19:21:16.998 flag set). boost::stacktrace uses dbghelp, which is
		// process-global and single-threaded, and the game's own crash handler uses dbghelp too - so this can
		// contend with, or run during, the game tearing itself down. killMe() is also called from the WndProc
		// on WM_DESTROY, i.e. potentially while the window is already being destroyed. Four tenths of a second
		// of every detour NOT knowing it should stand down is the opposite of what this function is for.
		GlobalKill::get().mKillFlag.store(true, std::memory_order_release);

		// For logging purposes, we want to know what called killMe.
		std::stringstream buffer;
		buffer << boost::stacktrace::stacktrace();
		PLOG_INFO << "GlobalKill::killMe (aka program shutdown) called, stacktrace: " << std::endl << buffer.str();
	}
	static bool isKillSet()
	{
		return GlobalKill::get().mKillFlag.load(std::memory_order_acquire);
	}

	// Only called in dllmain init
	static void reviveMe()
	{
		PLOG_DEBUG << "GlobalKill::reviveMe";
		GlobalKill::get().mKillFlag.store(false, std::memory_order_release);
	}

	static inline HMODULE HCMInternalModuleHandle = NULL;
};