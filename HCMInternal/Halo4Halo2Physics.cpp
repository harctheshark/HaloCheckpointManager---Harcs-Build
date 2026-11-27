#include "pch.h"
#include "Halo4Halo2Physics.h"
#include "SettingsStateAndEvents.h"
#include "IMessagesGUI.h"
#include "DirPathContainer.h"
#include "RuntimeExceptionHandler.h"
#include <filesystem>

namespace
{
	constexpr const wchar_t* kDllName = L"H4Halo2Physics.dll";

	// the mod's exports (see Halo4Halo2Physics.h)
	using EnableFn = int (*)(int);
	using StateFn = int (*)();

	// calls fn with the dll held, so it cannot go away under the call; -1 when it is not loaded / has no such export
	template <typename F>
	int withDll(const char* exportName, F call)
	{
		HMODULE pinned = nullptr;
		if (!GetModuleHandleExW(0, kDllName, &pinned) || !pinned) return -1;
		int r = -1;
		if (auto fn = GetProcAddress(pinned, exportName)) r = call(fn);
		FreeLibrary(pinned);
		return r;
	}
}

class Halo4Halo2Physics::Impl
{
	std::weak_ptr<SettingsStateAndEvents> mSettingsWeak;
	std::weak_ptr<IMessagesGUI> mMessagesWeak;
	std::shared_ptr<RuntimeExceptionHandler> mRuntimeExceptions;
	std::wstring mDir;   // <HCM folder>\h4h2p\ (with the trailing backslash)
	ScopedCallback<ToggleEvent> mToggleCallback;

	static void say(const std::weak_ptr<IMessagesGUI>& messages, const std::string& line)
	{
		if (line.empty()) return;
		if (auto m = messages.lock()) m->addMessage(line);
		PLOG_INFO << line;
	}
	void say(const std::string& line) { say(mMessagesWeak, line); }

	void onToggle(bool on)
	{
		try
		{
			const bool loaded = GetModuleHandleW(kDllName) != nullptr;
			if (!on)
			{
				if (loaded) withDll("h4p_enable", [](FARPROC f) { return reinterpret_cast<EnableFn>(f)(0); });
				say("Halo 2 physics off: Halo 4's own physics again");
				return;
			}
			if (!loaded)
			{
				const std::wstring dll = mDir + kDllName;
				if (!std::filesystem::exists(dll))
					throw HCMRuntimeException(std::format("Halo 2 physics (Halo 4): {} is missing. Your antivirus may have removed it (a false alarm): restore it from its quarantine, or unzip HCM again.",
						std::filesystem::path(dll).string()));
				if (!LoadLibraryW(dll.c_str()))
					throw HCMRuntimeException(std::format("Halo 2 physics (Halo 4): could not load {} (error {})", std::filesystem::path(dll).string(), GetLastError()));
			}
			withDll("h4p_enable", [](FARPROC f) { return reinterpret_cast<EnableFn>(f)(1); });
			// the dll hooks from its own thread a moment after loading; say whether it went live
			// (the thread holds only the weak message log, never this)
			std::thread([messages = mMessagesWeak]() {
				for (int i = 0; i < 30; ++i)
				{
					const int state = withDll("h4p_state", [](FARPROC f) { return reinterpret_cast<StateFn>(f)(); });
					if (state >= 1)
					{
						say(messages, "Halo 2 physics on (Halo 4)");
						return;
					}
					Sleep(100);
				}
				say(messages, "Halo 2 physics (Halo 4): the mod did not start. See h4h2p\\H4Halo2Physics.log in the HCM folder (a different MCC build?)");
				}).detach();
		}
		catch (HCMRuntimeException ex)
		{
			mRuntimeExceptions->handleMessage(ex);
		}
	}

public:
	Impl(GameState, IDIContainer& dicon)
		: mSettingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mMessagesWeak(dicon.Resolve<IMessagesGUI>()),
		mRuntimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mDir((std::filesystem::path(dicon.Resolve<DirPathContainer>().lock()->dirPath) / L"h4h2p").wstring() + L"\\"),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h4pPhysicsToggle->valueChangedEvent, [this](bool& on) { onToggle(on); })
	{
	}

	~Impl()
	{
		// HCM is closing: back to Halo 4's own physics (the dll stays, doing nothing)
		if (GetModuleHandleW(kDllName)) withDll("h4p_enable", [](FARPROC f) { return reinterpret_cast<EnableFn>(f)(0); });
	}
};

Halo4Halo2Physics::Halo4Halo2Physics(GameState gameImpl, IDIContainer& dicon)
{
	if (static_cast<GameState::Value>(gameImpl) != GameState::Value::Halo4) throw HCMInitException("Halo4Halo2Physics: Halo 4 only");
	pimpl = std::make_unique<Impl>(gameImpl, dicon);
}

Halo4Halo2Physics::~Halo4Halo2Physics() { PLOG_VERBOSE << "~" << getName(); }
