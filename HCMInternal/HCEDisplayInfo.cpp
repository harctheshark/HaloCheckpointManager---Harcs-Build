#include "pch.h"
#include "HCEDisplayInfo.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCEGetCameraData.h"   // getElectionDiagnostics, for the Linux-visible camera readout
#include <fstream>              // Z:\proc\self\limits, for the Wine handle readout
#include <sstream>
#include <psapi.h>              // GetProcessHandleCount
#include "RenderTextHelper.h"
#include "GlobalKill.h"

// ================================================================================================================
// Halo Campaign Evolved 2D info overlay. Rows come from HCM_Evolved information.py, and ONLY what that file can
// actually provide - no health, shields, tag name or custom-object tracking on HCE yet. (A view angle IS
// available now, via HCEGetPlayerState::getPlayerViewAngle - it just has no row here.)
//
// Three of the seven rows are asymmetric in a way that has already cost time once (see HCEStateHook.cpp):
//     current_level  simBase + 0xCA2F00   a NUL-terminated ASCII STRING (<=40 bytes), not an index
//     current_bsp    simBase + 0x9A14E0   an int32 read DIRECTLY, no dereference
//     tick_counter   simBase + 0x12944C8  a POINTER to the int32, one dereference
//
// Update cadence is the render event throttled to ~33 ms, matching main.pyw's _refresh_information_loop. There is
// no GameTickEventHook on HCE, and since render and update run on the same (present) thread there is no need for
// DisplayPlayerInfo's A/B double-buffered string either.
// ================================================================================================================

class HCEDisplayInfo::HCEDisplayInfoImpl
{
private:
	GameState mGame;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<HCEGetPlayerState> playerStateWeak;

	std::atomic<bool> mReady{ false };
	bool mIsActive = false;
	std::string mDataString;
	std::chrono::steady_clock::time_point mLastUpdate{};

	// ================================================================================================
	// WINE / PROTON HANDLE PRESSURE READOUT.
	//
	// ⚠ THIS IS AN INSTRUMENT, NOT A FEATURE, and it exists because of one specific open bug: the game
	// crashes on Linux with a null dereference inside UE's own pak reader
	// (FPakFile::GetSharedReader returns the result of CreatePakReader WITHOUT null-checking it, and the
	// check() in operator-> is compiled out in Shipping). The null comes from IPlatformFile::OpenRead
	// FAILING on an already-mounted .pak - which essentially never happens on Windows, where handles are
	// cheap, but is entirely possible under Wine, where EVERY Win32 handle costs a real Linux file
	// descriptor and the process is bound by RLIMIT_NOFILE.
	//
	// The leading hypothesis is therefore descriptor exhaustion, and this is how to prove or kill it
	// WITHOUT A LOG FILE: watch the number on screen while toggling the overlay that triggers the crash.
	// Climbing toward the ceiling proves it. Flat at a few hundred against a ceiling in the hundreds of
	// thousands kills it, and the answer is elsewhere.
	//
	// Proton maps Z: to the Linux root, so /proc/self is reachable as Z:\proc\self from inside the
	// process. On real Windows that path does not exist, every probe fails, and the row says so.
	// ⚠ Sampled once a SECOND, never per frame: counting a directory is a syscall per entry.
	static std::string wineHandleDiagnostics()
	{
		static const bool underWine =
			GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;

		static std::string cached;
		static ULONGLONG lastSampleTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (!cached.empty() && (now - lastSampleTick) < 1000) return cached;
		lastSampleTick = now;

		DWORD handleCount = 0;
		const bool haveHandles = GetProcessHandleCount(GetCurrentProcess(), &handleCount) != 0;

		if (!underWine)
		{
			cached = haveHandles
				? std::format("win32 handles {} (not running under Wine)", handleCount)
				: "not running under Wine";
			return cached;
		}

		// Count entries in Z:\proc\self\fd. Each is one open Linux descriptor.
		int fdCount = -1;
		{
			WIN32_FIND_DATAA fd{};
			HANDLE h = FindFirstFileA("Z:\\proc\\self\\fd\\*", &fd);
			if (h != INVALID_HANDLE_VALUE)
			{
				fdCount = 0;
				do
				{
					if (fd.cFileName[0] == '.') continue;   // . and ..
					++fdCount;
				} while (FindNextFileA(h, &fd));
				FindClose(h);
			}
		}

		// "Max open files" out of Z:\proc\self\limits - the ceiling the count is racing.
		long long fdCeiling = -1;
		{
			std::ifstream limits("Z:\\proc\\self\\limits");
			std::string line;
			while (std::getline(limits, line))
			{
				if (line.rfind("Max open files", 0) != 0) continue;
				// "Max open files   <soft>  <hard>  files"
				std::istringstream parse(line.substr(std::string("Max open files").size()));
				std::string soft;
				if (parse >> soft) { try { fdCeiling = std::stoll(soft); } catch (...) {} }
				break;
			}
		}

		cached = std::format("WINE  fds {}{}  win32 handles {}",
			fdCount < 0 ? std::string("?") : std::to_string(fdCount),
			fdCeiling < 0 ? std::string("") : std::format(" / {}", fdCeiling),
			haveHandles ? std::to_string(handleCount) : std::string("?"));
		return cached;
	}

	// Appends "label: value\n". Every row degrades on its own - a failure gives the placeholder rather than
	// killing the whole overlay, exactly like information.py's per-provider error handling.
	static void appendRow(std::string& out, std::string_view label, std::string_view value)
	{
		out.append(label);
		out.append(": ");
		out.append(value);
		out.push_back('\n');
	}

	// The velocity rows, from one already-fetched vector. Shared by the coords+velocity and velocity-only paths so
	// the two cannot format differently.
	//
	// Magnitudes match MCC's "As XY magnitude" / "As XYZ magnitude" exactly, computed from the same vector the
	// component rows print - so a reader can always verify the total against the parts on screen.
	static void appendVelocityRows(std::string& out, const SimpleMath::Vector3& v, int precision,
		bool wantComponents, bool wantXY, bool wantXYZ)
	{
		if (wantComponents)
		{
			appendRow(out, "X Velocity", std::format("{:.{}f}", v.x, precision));
			appendRow(out, "Y Velocity", std::format("{:.{}f}", v.y, precision));
			appendRow(out, "Z Velocity", std::format("{:.{}f}", v.z, precision));
		}
		if (wantXY)
			appendRow(out, "Absolute XY Velocity", std::format("{:.{}f}", std::sqrt(v.x * v.x + v.y * v.y), precision));
		if (wantXYZ)
			appendRow(out, "Absolute XYZ Velocity", std::format("{:.{}f}", std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), precision));
	}

	void updateData()
	{
		lockOrThrow(settingsWeak, settings);
		auto playerState = playerStateWeak.lock();
		if (!playerState) return;

		const int precision = settings->display2DInfoFloatPrecision->GetValue();

		std::string out;
		out.reserve(256);

		const bool wantCoords = settings->hceDisplayInfoShowCoordinates->GetValue();
		const bool wantVelComponents = settings->hceDisplayInfoShowVelocity->GetValue();
		const bool wantVelXY = settings->hceDisplayInfoShowVelocityXY->GetValue();
		const bool wantVelXYZ = settings->hceDisplayInfoShowVelocityXYZ->GetValue();

		// The magnitudes are derived from the same vector as the component rows, so ANY of the three toggles means
		// we need the velocity fetch. Gating this on the components toggle alone would leave the magnitude rows
		// blank whenever the user wanted only a speed readout.
		const bool wantVelocity = wantVelComponents || wantVelXY || wantVelXYZ;

		// Position and velocity live in the SAME physics entry, at the end of a long TLS -> object -> physics
		// chain. Fetching them separately walked that entire chain twice per refresh, which is why these two rows
		// (and only these two) hurt the framerate. When both are wanted, resolve the chain once.
		if (wantCoords && wantVelocity)
		{
			try
			{
				SimpleMath::Vector3 p, v;
				playerState->getPlayerPositionAndVelocity(p, v);
				appendRow(out, "x", std::format("{:.{}f}", p.x, precision));
				appendRow(out, "y", std::format("{:.{}f}", p.y, precision));
				appendRow(out, "z", std::format("{:.{}f}", p.z, precision));
				appendVelocityRows(out, v, precision, wantVelComponents, wantVelXY, wantVelXYZ);
			}
			catch (HCMRuntimeException) { appendRow(out, "Coordinates", "Waiting for game"); }
		}
		else if (wantCoords)
		{
			try
			{
				auto p = playerState->getPlayerPosition();
				appendRow(out, "x", std::format("{:.{}f}", p.x, precision));
				appendRow(out, "y", std::format("{:.{}f}", p.y, precision));
				appendRow(out, "z", std::format("{:.{}f}", p.z, precision));
			}
			catch (HCMRuntimeException) { appendRow(out, "Coordinates", "Waiting for game"); }
		}
		else if (wantVelocity)
		{
			try
			{
				auto v = playerState->getPlayerVelocity();
				appendVelocityRows(out, v, precision, wantVelComponents, wantVelXY, wantVelXYZ);
			}
			catch (HCMRuntimeException) { appendRow(out, "Velocity", "Waiting for game"); }
		}

		if (settings->hceDisplayInfoShowLevel->GetValue())
		{
			try { appendRow(out, "Current Level", playerState->getCurrentLevelName()); }
			catch (HCMRuntimeException) { appendRow(out, "Current Level", "None"); }
		}

		if (settings->hceDisplayInfoShowZoneSet->GetValue())
		{
			try
			{
				std::string name = playerState->getCurrentZoneSetName();
				// The index global is written BEFORE the BSPs finish loading (it is the 4th instruction of the
				// commit), so it means "switching to", not "finished". Say which.
				if (!playerState->isCurrentZoneSetFullyLoaded()) name += " (loading)";
				appendRow(out, "Current Zone Set", name);
			}
			catch (HCMRuntimeException) { appendRow(out, "Current Zone Set", "None"); }
		}

		// ⚠ DIAGNOSTIC, off by default. The only window onto the camera election on Linux/Proton, where HCM
		// writes no log file at all - see HCEGetCameraData::getElectionDiagnostics for how to read it.
		if (settings->hceDisplayInfoShowCameraDiag->GetValue())
		{
			try { appendRow(out, "Camera", HCEGetCameraData::getElectionDiagnostics()); }
			catch (...) { appendRow(out, "Camera", "unavailable"); }
			try { appendRow(out, "Process", wineHandleDiagnostics()); }
			catch (...) { appendRow(out, "Process", "unavailable"); }
		}

		// ⚠ The row below is misnamed at the source: this global is the current ZONE SET index, not a BSP
		// index. The name is kept for compatibility; the row above shows the same value as a readable name.
		if (settings->hceDisplayInfoShowBSP->GetValue())
		{
			try { appendRow(out, "Current BSP", std::to_string(playerState->getCurrentBSP())); }
			catch (HCMRuntimeException) { appendRow(out, "Current BSP", "Waiting for game"); }
		}

		if (settings->hceDisplayInfoShowTick->GetValue())
		{
			try { appendRow(out, "Tick Counter", std::to_string(playerState->getTickCounter())); }
			catch (HCMRuntimeException) { appendRow(out, "Tick Counter", "Waiting for game"); }
		}

		if (settings->hceDisplayInfoShowPlayerDatum->GetValue())
		{
			try { appendRow(out, "Player Datum", std::format("0x{:08X}", playerState->getPlayerDatum())); }
			catch (HCMRuntimeException) { appendRow(out, "Player Datum", "(dead)"); }
		}

		if (settings->hceDisplayInfoShowTEB->GetValue())
		{
			// Diagnostic row: if this shows an address, the game-thread walk and therefore every TLS-reached
			// HCE feature is alive. If it says "Waiting for game", nothing else HCE-side can work either.
			try { appendRow(out, "TEB Base", std::format("0x{:016X}", playerState->getGameThreadTeb())); }
			catch (HCMRuntimeException) { appendRow(out, "TEB Base", "Waiting for game"); }
		}

		mDataString = std::move(out);
	}

	void onRenderEvent(SimpleMath::Vector2 screenSize)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		if (!mIsActive) return;
		if (GlobalKill::isKillSet()) return;

		try
		{
			lockOrThrow(settingsWeak, settings);

			// ~33 ms refresh, matching the reference tool. Rendering still happens every frame, so the text
			// never flickers and the last good values stay up while the chain is briefly down.
			const auto now = std::chrono::steady_clock::now();
			if (mLastUpdate.time_since_epoch().count() == 0 || (now - mLastUpdate) >= std::chrono::milliseconds(33))
			{
				mLastUpdate = now;
				updateData();
			}

			if (mDataString.empty()) return;

			const SimpleMath::Vector2& cornerOffset = settings->display2DInfoScreenOffset->GetValue();
			SimpleMath::Vector2 position;
			switch (settings->display2DInfoAnchorCorner->GetValue())
			{
			case SettingsEnums::ScreenAnchorEnum::TopLeft:
				position = cornerOffset;
				break;
			case SettingsEnums::ScreenAnchorEnum::TopRight:
				position = { screenSize.x - cornerOffset.x, cornerOffset.y };
				break;
			case SettingsEnums::ScreenAnchorEnum::BottomRight:
				position = { screenSize.x - cornerOffset.x, screenSize.y - cornerOffset.y };
				break;
			case SettingsEnums::ScreenAnchorEnum::BottomLeft:
				position = { cornerOffset.x, screenSize.y - cornerOffset.y };
				break;
			default:
				position = cornerOffset;
				break;
			}

			const auto fontColour = ImGui::ColorConvertFloat4ToU32(settings->display2DInfoFontColour->GetValue());
			const float fontSize = settings->display2DInfoFontSize->GetValue();

			// RenderTextHelper is pure ImGui, so it works unchanged on the D3D12 backend.
			if (settings->display2DInfoOutline->GetValue())
				RenderTextHelper::drawOutlinedText(mDataString, position, fontColour, fontSize);
			else
				RenderTextHelper::drawText(mDataString, position, fontColour, fontSize);
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onToggleEvent(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame))
			{
				mIsActive = false;
				return;
			}

			messagesGUI->addMessage(newValue ? "Enabling Player Info overlay." : "Disabling Player Info overlay");
			mIsActive = newValue;
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	void onGameStateChanged(const MCCState& newState)
	{
		if (!mReady.load(std::memory_order_acquire)) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			mIsActive = settings->display2DInfoToggle->GetValue()
				&& newState.currentGameState == mGame
				&& newState.currentPlayState == PlayState::Ingame;
		}
		catch (HCMRuntimeException ex)
		{
			mIsActive = false;
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST, see HCECheckpointDetours.cpp.
	ScopedCallback<RenderEvent> mRenderEventCallback;
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

public:
	HCEDisplayInfoImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		playerStateWeak(resolveDependentCheat(HCEGetPlayerState)),
		mRenderEventCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRenderEvent(ss); }),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->display2DInfoToggle->valueChangedEvent, [this](bool& n) { onToggleEvent(n); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onGameStateChanged(s); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEDisplayInfo only supports Halo Campaign Evolved");

		mReady.store(true, std::memory_order_release);
	}

	~HCEDisplayInfoImpl()
	{
		mReady.store(false, std::memory_order_release);
		mIsActive = false;
		mRenderEventCallback.removeCallback();
		mToggleCallback.removeCallback();
		mGameStateChangedCallback.removeCallback();
	}
};


HCEDisplayInfo::HCEDisplayInfo(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEDisplayInfoImpl>(game, dicon))
{
}

HCEDisplayInfo::~HCEDisplayInfo()
{
	PLOG_VERBOSE << "~" << getName();
}
