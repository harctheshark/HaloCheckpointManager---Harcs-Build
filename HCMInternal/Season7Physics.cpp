#include "pch.h"
#include "Season7Physics.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include <array>

// The patched site is a `mulss xmm6, dword ptr [rip+disp32]` that multiplies a per-node collision radius
// by a scalar constant. Only the rip displacement (byte index 4) differs between the two constants; we
// rewrite the whole 8-byte instruction. ON = Season 7 constant (60.0), OFF = the game's default (30.0).
//
// IMPORTANT (see Season7Physics_BuildTimeCache_HANDOFF): the scalar is baked into collision shapes when
// they are CONSTRUCTED at level load, not read per tick. So the patch must be in place BEFORE the level
// builds its shapes. We therefore (re)apply the patch on every Halo 2 load (MCC state change) according to
// the toggle, and treat the toggle as "armed" - flipping it mid-level only affects shapes built afterwards,
// so the user is told to reload the level for full effect.
template <GameState::Value gameT>
class Season7PhysicsImpl : public ISeason7PhysicsImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	std::shared_ptr<MultilevelPointer> season7PhysicsPatch;

	static constexpr std::array<uint8_t, 8> kSeason7Bytes{ 0xF3, 0x0F, 0x59, 0x35, 0xCA, 0x4E, 0x52, 0x00 };
	static constexpr std::array<uint8_t, 8> kDefaultBytes{ 0xF3, 0x0F, 0x59, 0x35, 0x9E, 0x4E, 0x52, 0x00 };

	// writes the instruction bytes; throws if the site isn't resolvable yet (e.g. halo2.dll not loaded at the
	// MCC menu). protectedMemory = true: target is executable .text, so this goes through VirtualProtect.
	void writePatch(const std::array<uint8_t, 8>& bytes)
	{
		if (!season7PhysicsPatch->writeArrayData(const_cast<uint8_t*>(bytes.data()), bytes.size(), true))
			throw HCMRuntimeException(std::format("Failed to write Season7Physics patch: {}", MultilevelPointer::GetLastError()));
	}

	// apply whatever the toggle currently says (used on load so shapes build with the right scalar)
	void applyCurrentState()
	{
		lockOrThrow(settingsWeak, settings);
		writePatch(settings->season7PhysicsToggle->GetValue() ? kSeason7Bytes : kDefaultBytes);
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "Season7Physics onToggle, newValue: " << newValue;

		// apply immediately if the module is loaded; if not (e.g. at the MCC menu) it'll apply on level load.
		bool appliedNow = false;
		try { writePatch(newValue ? kSeason7Bytes : kDefaultBytes); appliedNow = true; }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "Season7Physics: patch deferred to level load (" << ex.what() << ")"; }

		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			if (!newValue)
				messagesGUI->addMessage("Season 7 Physics off - reload the level to fully revert collision.");
			else if (appliedNow)
				messagesGUI->addMessage("Season 7 Physics armed - reload the level for full effect.");
			else
				messagesGUI->addMessage("Season 7 Physics armed (applies on next level load).");
		}
		catch (HCMRuntimeException&) {}
	}

	// (re)apply on each Halo 2 load so the scalar is correct before collision shapes are constructed
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return; // nothing loaded to patch/build
		try { applyCurrentState(); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "Season7Physics: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	Season7PhysicsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->season7PhysicsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		season7PhysicsPatch = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(season7PhysicsPatch), mGame);
	}

	// restore the default scalar so we never leave the game patched after HCM unloads
	~Season7PhysicsImpl()
	{
		try { writePatch(kDefaultBytes); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


Season7Physics::Season7Physics(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<Season7PhysicsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("Season7Physics not impl for this game");
	}
}

Season7Physics::~Season7Physics()
{
	PLOG_DEBUG << "~" << getName();
}
