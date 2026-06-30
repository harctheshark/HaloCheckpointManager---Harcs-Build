#include "pch.h"
#include "UncapVisibilityLimits.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include <Windows.h>
#include <vector>
#include <cstring>

// ============================================================================
//  Halo 2 visibility index-collection caps (MCC halo2.dll, build 1.3528).
//  Visibility init sub_180709F90 builds two collections (camera unk_1815FE020, shadow unk_181600B30) from
//  3 packed-dword maximums each: {proj+clusters, [5486]+[5487], [5488]+[5489]}. The camera collection's
//  caps decode to clusters=128, [5486]=64, index=256, [5488]=384. We raise [5486]/[5487]/[5488] to 4096
//  for both collections (clusters left at 128 - it's a separate CHAR_MAX/region-struct limit). Each maximum
//  is a `mov [rsp+disp], imm32` in sub_180709F90; we rewrite the imm32. See sapien.visidx.exe + memory
//  visibility-projection-cap for the full RE.
// ============================================================================

namespace
{
	struct Patch { uint32_t rva; uint8_t orig[4]; uint8_t patched[4]; };

	// imm32 of each `mov [rsp+disp], imm32` (instruction + 4). 0x10001000 = {low16=4096, high16=4096}.
	constexpr Patch kPatches[] = {
		// camera A: [5486]=64 / [5487]=256  (0x01000040 -> 0x10001000)
		{ 0x709FA5, { 0x40,0x00,0x00,0x01 }, { 0x00,0x10,0x00,0x10 } },
		// camera A: [5488]=384 / [5489]=0   (0x00000180 -> 0x00001000)
		{ 0x709FB2, { 0x80,0x01,0x00,0x00 }, { 0x00,0x10,0x00,0x00 } },
		// shadow B: [5486]=1  / [5487]=128  (0x00800001 -> 0x10001000)
		{ 0x709FC2, { 0x01,0x00,0x80,0x00 }, { 0x00,0x10,0x00,0x10 } },
		// shadow B: [5488]=96 / [5489]=256  (0x01000060 -> 0x01001000, keep [5489]=256)
		{ 0x709FCA, { 0x60,0x00,0x00,0x01 }, { 0x00,0x10,0x00,0x01 } },
	};

	class Patcher
	{
		uintptr_t base = 0;
		bool applied = false;

		static void write4(uintptr_t addr, const uint8_t* data)
		{
			DWORD o; VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &o);
			memcpy((void*)addr, data, 4);
			VirtualProtect((void*)addr, 4, o, &o);
			FlushInstructionCache(GetCurrentProcess(), (void*)addr, 4);
		}

	public:
		bool isApplied() const { return applied; }

		// returns false (no throw) if halo2.dll isn't loaded yet; throws on a real mismatch
		bool apply()
		{
			if (applied) return true;
			base = (uintptr_t)GetModuleHandleA("halo2.dll");
			if (!base) return false;

			// verify every site matches stock (build gate) - if already patched, treat as applied
			for (const Patch& p : kPatches)
			{
				if (memcmp((void*)(base + p.rva), p.orig, 4) == 0) continue;
				if (memcmp((void*)(base + p.rva), p.patched, 4) == 0) continue;
				throw HCMRuntimeException(std::format("UncapVisibilityLimits: bytes @rva 0x{:X} don't match build 1.3528; refusing to patch.", p.rva));
			}
			for (const Patch& p : kPatches)
				write4(base + p.rva, p.patched);
			applied = true;
			return true;
		}

		void revert()
		{
			if (!applied) return;
			if (base && GetModuleHandleA("halo2.dll"))
				for (const Patch& p : kPatches)
					write4(base + p.rva, p.orig);
			applied = false;
		}

		~Patcher() { revert(); }
	};
} // namespace


template <GameState::Value gameT>
class UncapVisibilityLimitsImpl : public IUncapVisibilityLimitsImpl
{
private:
	GameState mGame;
	Patcher mPatcher;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "UncapVisibilityLimits onToggle, newValue: " << newValue;
		try
		{
			if (newValue) mPatcher.apply();
			else mPatcher.revert();
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << ex.what();
			try { lockOrThrow(messagesGUIWeak, m); m->addMessage(ex.what()); } catch (...) {}
			return;
		}
		try
		{
			lockOrThrow(messagesGUIWeak, m);
			m->addMessage(newValue ? "Visibility limits raised - reload the level for it to take effect" : "Visibility limits restored");
		}
		catch (HCMRuntimeException&) {}
	}

	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return;
		try
		{
			lockOrThrow(settingsWeak, settings);
			if (settings->uncapVisibilityLimitsToggle->GetValue() && !mPatcher.isApplied())
				mPatcher.apply();
		}
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "UncapVisibilityLimits: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	UncapVisibilityLimitsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->uncapVisibilityLimitsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
	}

	~UncapVisibilityLimitsImpl()
	{
		try { mPatcher.revert(); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


UncapVisibilityLimits::UncapVisibilityLimits(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<UncapVisibilityLimitsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("UncapVisibilityLimits not impl for this game");
	}
}

UncapVisibilityLimits::~UncapVisibilityLimits()
{
	PLOG_DEBUG << "~" << getName();
}
