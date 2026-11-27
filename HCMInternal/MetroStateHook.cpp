#include "pch.h"
#include "MetroStateHook.h"
#include <thread>

namespace
{
	// metro.exe RVAs - the same in Metro 2033 Redux and Last Light Redux (one engine build). BINARY_SURVEY_20261124.md.
	constexpr uintptr_t kLevelRVA = 0xFBD820;     // level object*
	constexpr uintptr_t kLevelPlayerOff = 56;     // player entity* (the controlled entity)
	constexpr uintptr_t kLevelNameHolderOff = 304; // *(level + 304) + 20 = the level's name (C string)
	constexpr uintptr_t kLevelNameOff = 20;

	// SEH only - no C++ objects with destructors in here
	bool sehRead(void* dest, const void* src, size_t size)
	{
		__try { memcpy(dest, src, size); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool readPtr(uintptr_t at, uintptr_t& out) { return at > 0x10000 && sehRead(&out, (const void*)at, sizeof(out)) && out > 0x10000 && out < 0x7FFFFFFFFFFFull; }
}

class MetroStateHook::Impl
{
public:
	MCCState mState{ GameState::Value::NoGame, PlayState::MainMenu, (LevelID)0 };
	std::mutex mStateMutex;
	bool mCursorShowing = true;
	std::string mLevelName;
	std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> mStateChangedEvent = std::make_shared<eventpp::CallbackList<void(const MCCState&)>>();
	std::atomic<bool> mRun{ true };
	std::thread mThread;

	Impl() { mThread = std::thread([this]() { pollLoop(); }); }
	~Impl() { mRun = false; if (mThread.joinable()) mThread.join(); }

	std::string readLevelName(uintptr_t level)
	{
		uintptr_t holder = 0;
		if (!readPtr(level + kLevelNameHolderOff, holder)) return {};
		char buf[96]{};
		if (!sehRead(buf, (const void*)(holder + kLevelNameOff), sizeof(buf) - 1)) return {};
		for (char& c : buf) if (c && (c < 0x20 || c > 0x7E)) return {};   // not text: the guess was wrong, report nothing
		return std::string(buf);
	}

	void pollLoop()
	{
		const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
		while (mRun.load(std::memory_order_acquire))
		{
			uintptr_t level = 0, player = 0;
			const bool live = readPtr(base + kLevelRVA, level) && readPtr(level + kLevelPlayerOff, player);
			const std::string name = live ? readLevelName(level) : std::string();
			MCCState next = live
				? MCCState{ GameState::Value::MetroRedux, PlayState::Ingame, (LevelID)0 }
				: MCCState{ GameState::Value::NoGame, PlayState::MainMenu, (LevelID)0 };
			bool changed = false;
			{
				std::scoped_lock lk(mStateMutex);
				const bool levelChanged = live && name != mLevelName;
				if (next.currentGameState != mState.currentGameState || next.currentPlayState != mState.currentPlayState || levelChanged)
				{
					mState = next;
					changed = true;
				}
				if (live) mLevelName = name;
			}
			if (changed)
			{
				PLOG_INFO << "MetroStateHook: state -> " << (live ? "MetroRedux/Ingame, level '" + name + "'" : std::string("NoGame/MainMenu"));
				(*mStateChangedEvent)(mState);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}
};

MetroStateHook::MetroStateHook() : pimpl(std::make_unique<Impl>()) {}
MetroStateHook::~MetroStateHook() { PLOG_VERBOSE << "~MetroStateHook"; }

const MCCState& MetroStateHook::getCurrentMCCState() { return pimpl->mState; }

bool MetroStateHook::isGameCurrentlyPlaying(GameState gameToCheck)
{
	std::scoped_lock lk(pimpl->mStateMutex);
	return pimpl->mState.currentGameState == gameToCheck && pimpl->mState.currentPlayState == PlayState::Ingame;
}

std::shared_ptr<eventpp::CallbackList<void(const MCCState&)>> MetroStateHook::getMCCStateChangedEvent() { return pimpl->mStateChangedEvent; }

bool* MetroStateHook::getCursorShowingFlag() { return &pimpl->mCursorShowing; }

std::string MetroStateHook::getLevelName()
{
	std::scoped_lock lk(pimpl->mStateMutex);
	return pimpl->mLevelName;
}
