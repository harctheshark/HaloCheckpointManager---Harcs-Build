#include "pch.h"
#include "DropShadowsOnObjects.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include <array>

// The patched site sets the 4th argument (a4 = object-receive shadow_mode bitmask) of the drop-shadow
// receiver-filter `sub_1809EEDD0`. The lightmap drop-shadow caster pass calls it with a4 = 0 via
// `xor r9d, r9d`; the filter then strips bit 0x1000 (the "draw this object as a receiver" flag) from every
// non-caster object whose shadow_mode bit isn't set in a4. With a4 = 0, no object ever receives. We rewrite
// the 3-byte `xor r9d, r9d` to `mov r9b, 0xFF`, setting a4's low byte (covers shadow_modes 0-7 = all real
// object types) so objects keep their receive bit. a3 (caster self-receive) is left at 0 by the following
// `xor r8d, r8d`, so this only adds object->object receiving, not caster self-shadowing.
template <GameState::Value gameT>
class DropShadowsOnObjectsImpl : public IDropShadowsOnObjectsImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	std::shared_ptr<MultilevelPointer> dropShadowsOnObjectsPatch;

	static constexpr std::array<uint8_t, 3> kOnBytes{ 0x41, 0xB1, 0xFF };  // mov r9b, 0xFF   (a4 = all shadow modes)
	static constexpr std::array<uint8_t, 3> kOffBytes{ 0x45, 0x33, 0xC9 }; // xor r9d, r9d    (a4 = 0, stock)

	// writes the instruction bytes; throws if the site isn't resolvable yet (e.g. halo2.dll not loaded at the
	// MCC menu). protectedMemory = true: target is executable .text, so this goes through VirtualProtect.
	void writePatch(const std::array<uint8_t, 3>& bytes)
	{
		if (!dropShadowsOnObjectsPatch->writeArrayData(const_cast<uint8_t*>(bytes.data()), bytes.size(), true))
			throw HCMRuntimeException(std::format("Failed to write DropShadowsOnObjects patch: {}", MultilevelPointer::GetLastError()));
	}

	void applyCurrentState()
	{
		lockOrThrow(settingsWeak, settings);
		writePatch(settings->dropShadowsOnObjectsToggle->GetValue() ? kOnBytes : kOffBytes);
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "DropShadowsOnObjects onToggle, newValue: " << newValue;

		// the patch is read per shadow-render frame, so it takes effect live. if halo2.dll isn't loaded yet
		// (e.g. toggled at the MCC menu) it'll be applied on the next Halo 2 load via onMCCStateChanged.
		try { writePatch(newValue ? kOnBytes : kOffBytes); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "DropShadowsOnObjects: patch deferred (" << ex.what() << ")"; }

		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(newValue ? "Object drop shadows on" : "Object drop shadows off");
		}
		catch (HCMRuntimeException&) {}
	}

	// re-apply on each Halo 2 load in case the toggle was flipped while halo2.dll wasn't loaded
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return;
		if (newState.currentPlayState == PlayState::MainMenu) return;
		try { applyCurrentState(); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "DropShadowsOnObjects: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	DropShadowsOnObjectsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->dropShadowsOnObjectsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		dropShadowsOnObjectsPatch = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(dropShadowsOnObjectsPatch), mGame);
	}

	// restore stock behaviour so we never leave the game patched after HCM unloads
	~DropShadowsOnObjectsImpl()
	{
		try { writePatch(kOffBytes); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


DropShadowsOnObjects::DropShadowsOnObjects(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<DropShadowsOnObjectsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("DropShadowsOnObjects not impl for this game");
	}
}

DropShadowsOnObjects::~DropShadowsOnObjects()
{
	PLOG_DEBUG << "~" << getName();
}
