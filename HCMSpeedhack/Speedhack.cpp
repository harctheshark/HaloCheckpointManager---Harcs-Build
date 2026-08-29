#include "pch.h"
#include "Speedhack.h"
#include "safetyhook.hpp" // hooking
#include <atomic>         // the timeHacker seqlock below

// this implementation of a speedhack is adapted from https://github.com/Letomaniy/Speed-Hack
// it hooks QueryPerformanceCounter, GetTickCount, GetTickCount64, and TimeGetTime,
// and basically uses a timeHacker class to keep track of a seperate flow of "fake time" to return to anyone asking the hooks



// ================================================================================================================
// ⚠ THE THREE FIELDS BELOW ARE READ BY EVERY THREAD IN THE PROCESS AND WRITTEN BY ONE. THAT IS A DATA RACE, AND
// IT IS NOT BENIGN.
//
// getCurrentTime() runs inside the QueryPerformanceCounter / GetTickCount hooks, so it is called from the game's
// render thread, its audio thread, and on this title NVIDIA's frame-generation pacer - well over a hundred times
// a second, concurrently. setSpeed() updates offset, last-update and speed as three separate stores. A reader
// that lands between them mixes a NEW offset with an OLD last-update (or vice versa), and since the result is
// `speed * (now - lastUpdate) + offset`, a mismatched pair does not produce a slightly wrong time - it produces
// a timestamp that can jump BACKWARDS BY THE WHOLE SESSION. Monotonic-clock consumers respond to that by
// dividing by a negative delta, stalling, or asserting.
//
// The obvious fix - the std::scoped_lock that is commented out at all four call sites in this file - is the
// wrong one: it puts a contended mutex in the hot path of the process's clock, taken by every thread on every
// query. So this is a SEQLOCK instead. Readers take no lock at all; they read a version counter either side of
// the payload and retry if it moved. Writers are rare (a toggle) and cheap.
//
// Same construction as HCEGetCameraData's PovSnapshot in HCMInternal, for the same reason.
// ================================================================================================================
template<class DataType>
class timeHacker {
	DataType time_offset;
	DataType time_last_update;

	double speed_;

	// Even = stable, odd = a write is in progress.
	std::atomic<uint32_t> sequence{ 0 };

public:


	timeHacker(DataType currentRealTime, double initialSpeed) {
		time_offset = currentRealTime;
		time_last_update = currentRealTime;

		speed_ = initialSpeed;
	}

	// Re-seed to a fresh clock reading, discarding any accumulated offset.
	//
	// This exists because the seqlock's std::atomic member deletes copy-assignment, and construction used to
	// re-initialise these objects by assigning a whole new timeHacker over the top. That was only safe because
	// it happens before the hooks are installed; doing it through the seqlock means it stays safe even if it
	// is ever called while readers are live.
	void reset(DataType currentRealTime, double initialSpeed) {
		sequence.fetch_add(1, std::memory_order_acq_rel);
		std::atomic_thread_fence(std::memory_order_release);

		time_offset = currentRealTime;
		time_last_update = currentRealTime;
		speed_ = initialSpeed;

		std::atomic_thread_fence(std::memory_order_release);
		sequence.fetch_add(1, std::memory_order_acq_rel);
	}

	void setSpeed(DataType currentRealTime, double speed) {
		// Computed BEFORE the write window opens, so the seqlock covers only the three stores.
		const DataType newOffset = getCurrentTime(currentRealTime);

		sequence.fetch_add(1, std::memory_order_acq_rel);            // -> odd: readers back off
		std::atomic_thread_fence(std::memory_order_release);

		time_offset = newOffset;
		time_last_update = currentRealTime;
		speed_ = speed;

		std::atomic_thread_fence(std::memory_order_release);
		sequence.fetch_add(1, std::memory_order_acq_rel);            // -> even: published
	}

	DataType getCurrentTime(DataType currentRealTime) {
		DataType offset{};
		DataType lastUpdate{};
		double speed = 1.0;

		// Bounded, NOT an unbounded spin. If a writer were descheduled mid-update, spinning here would hang
		// whichever thread asked for the time - including the render thread. A handful of retries is far more
		// than a three-store window ever needs, and taking a torn read on the last attempt is survivable in a
		// way that a stalled clock is not.
		for (int attempt = 0; attempt < 8; ++attempt)
		{
			const uint32_t before = sequence.load(std::memory_order_acquire);
			if (before & 1u) continue;                               // mid-write, try again

			offset = time_offset;
			lastUpdate = time_last_update;
			speed = speed_;

			std::atomic_thread_fence(std::memory_order_acquire);
			if (sequence.load(std::memory_order_relaxed) == before) break;   // consistent snapshot
		}

		const DataType difference = currentRealTime - lastUpdate;
		return (DataType)(speed * difference) + offset;
	}


};



class SpeedhackImpl : public SpeedhackImplBase
{
private:

	static inline std::mutex hookMutex{};



	static inline safetyhook::InlineHook queryPerformanceCounterHook{};
	static inline safetyhook::InlineHook getTickCountHook{};
	static inline safetyhook::InlineHook getTickCount64Hook{};
	static inline safetyhook::InlineHook timeGetTimeHook{};

	static inline timeHacker<DWORD>     speedHack{GetTickCount(), 1.0};
	static inline timeHacker<ULONGLONG> speedHackULL{GetTickCount64(), 1.0};
	static inline timeHacker<LONGLONG>  speedHackLL{0, 1.0};


	static BOOL __stdcall queryPerformanceCounterHookFunction(LARGE_INTEGER* lpPerformanceCount)
	{

		//std::scoped_lock<std::mutex> lock(hookMutex);

		LARGE_INTEGER performanceCounter;
		BOOL result = queryPerformanceCounterHook.stdcall<BOOL, _LARGE_INTEGER*>(&performanceCounter);
		lpPerformanceCount->QuadPart = speedHackLL.getCurrentTime(performanceCounter.QuadPart);
		return result;
	}

	static DWORD __stdcall getTickCountHookFunction()
	{

		//std::scoped_lock<std::mutex> lock(hookMutex);
		return speedHack.getCurrentTime(getTickCountHook.stdcall<DWORD>());
	}

	static ULONGLONG __stdcall getTickCount64HookFunction()
	{

		//std::scoped_lock<std::mutex> lock(hookMutex);
		return speedHackULL.getCurrentTime(getTickCount64Hook.stdcall<ULONGLONG>());
	}

	// ================================================================================================
	// ⚠⚠ THE HOOKS ARE INSTALLED LAZILY, ON FIRST USE - AND ARE NEVER REMOVED AGAIN.
	//
	// WHY LAZY. These four functions are the cheapest calls in Windows. GetTickCount is
	// `mov ecx,7FFE0320h / mov rcx,[rcx] / mov eax,[7FFE0004h]` and GetTickCount64 is a single read of
	// KUSER_SHARED_DATA - no syscall at all - and QueryPerformanceCounter is barely more. Hooking them
	// replaces a handful of nanoseconds with a trampoline, a call through to the original, a seqlock read
	// with two fences and a double multiply, FOR EVERY CALLER IN THE PROCESS: the game, the graphics
	// driver, the frame-generation pacer, the Steam overlay, everything.
	//
	// MEASURED on Halo Campaign Evolved (UE5), which hits these clocks far harder than the titles this
	// code was written for: the game sat at 15.6 cores with 73% of that in KERNEL time and 633,000
	// context switches per second, while HCM's own threads used 0.01 cores. Installing these
	// unconditionally in the constructor meant every user paid that tax whether or not they had ever
	// touched the speedhack - which is almost all of them.
	//
	// WHY NEVER REMOVED. Uninstalling is NOT the mirror image of installing. The timeHackers carry an
	// accumulated offset, so once time has been scaled the hooked clock is ahead of the real one;
	// dropping the hook makes the process clock jump BACKWARDS by that offset, which is exactly the
	// failure the seqlock comment at the top of this file describes. So the trade is: pay nothing until
	// the speedhack is actually used, then pay for the rest of the session.
	// ================================================================================================
	static inline std::atomic<bool> hooksInstalled{ false };

	// The real clock, whichever side of installation we are on. Once hooked, the trampoline IS the
	// original; before that, the export itself is untouched.
	static DWORD     realGetTickCount()   { return hooksInstalled.load(std::memory_order_acquire) ? getTickCountHook.stdcall<DWORD>()       : GetTickCount(); }
	static ULONGLONG realGetTickCount64() { return hooksInstalled.load(std::memory_order_acquire) ? getTickCount64Hook.stdcall<ULONGLONG>() : GetTickCount64(); }
	static LONGLONG  realQpc()
	{
		LARGE_INTEGER c{};
		if (hooksInstalled.load(std::memory_order_acquire))
			queryPerformanceCounterHook.stdcall<BOOL, _LARGE_INTEGER*>(&c);
		else
			QueryPerformanceCounter(&c);
		return c.QuadPart;
	}

	static void ensureHooksInstalled()
	{
		if (hooksInstalled.load(std::memory_order_acquire)) return;

		std::scoped_lock<std::mutex> lock(hookMutex);
		if (hooksInstalled.load(std::memory_order_relaxed)) return;   // lost the race; someone else did it

		// Re-seed from the REAL clock immediately before going live, so the first hooked reading
		// continues from now rather than from whenever this object was constructed.
		speedHackLL.reset(realQpc(), 1.0);
		speedHack.reset(realGetTickCount(), 1.0);
		speedHackULL.reset(realGetTickCount64(), 1.0);

		queryPerformanceCounterHook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Kernel32.dll"), "QueryPerformanceCounter"), queryPerformanceCounterHookFunction);
		getTickCountHook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Kernel32.dll"), "GetTickCount"), getTickCountHookFunction);
		getTickCount64Hook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Kernel32.dll"), "GetTickCount64"), getTickCount64HookFunction);
		timeGetTimeHook = safetyhook::create_inline(GetProcAddress(GetModuleHandleA("Winmm.dll"), "timeGetTime"), getTickCountHookFunction);

		hooksInstalled.store(true, std::memory_order_release);
	}

	static void setAllTimeHackerSpeeds(double speedInput) {
		//std::scoped_lock<std::mutex> lock(hookMutex);
		speedHack.setSpeed(realGetTickCount(), speedInput);

		speedHackULL.setSpeed(realGetTickCount64(), speedInput);

		speedHackLL.setSpeed(realQpc(), speedInput);
	}

public:


	SpeedhackImpl()
	{
		// Deliberately does NOT hook. Nothing is installed until setSpeed asks for a speed other than
		// 1.0 - see ensureHooksInstalled. Constructing this object must cost the process nothing.
		LARGE_INTEGER performanceCounter;
		QueryPerformanceCounter((_LARGE_INTEGER*)&performanceCounter);
		speedHackLL.reset(performanceCounter.QuadPart, 1.0);
	}

	~SpeedhackImpl() = default;

	virtual void setSpeed(double in) override
	{
		// 1.0 while nothing is installed is the default state of the process - there is nothing to do,
		// and doing nothing is the entire point of the lazy install. Once installed we always follow
		// through, because the timeHackers still own an offset that has to keep advancing.
		if (in == 1.0 && !hooksInstalled.load(std::memory_order_acquire))
			return;

		ensureHooksInstalled();
		setAllTimeHackerSpeeds(in);
	}
};

Speedhack::Speedhack() : impl(std::make_unique< SpeedhackImpl>()) 
{
	instance = this;
}

Speedhack::~Speedhack() = default;

