#include "pch.h"
#include "SphereSpecularForce.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "ScopedThreadSuspender.h"
#include <array>

// The patched site is the guard at the top of render_light (sub_1807E9E50) that decides whether to zero a
// spherical light's specular colour:
//
//     call cinematic_in_progress          ; sub_1806F4A20
//     test al, al
//     jnz  keep                           ; <-- PATCH SITE (halo2.dll+0x7E9F08): if cinematic, keep specular
//     cmp  word [rbx+4], 0                 ; light_type == 0  (spherical; is_spherical inlined here)
//     jnz  keep                           ; if not spherical, keep specular
//     xorps xmm0/1/2 ; movss cs:specular_colour, 0   ; else ZERO the specular colour (qword_181997420 / dword_181997428)
//   keep:
//
// i.e. stock zeroes specular for `!cinematic && spherical`. We rewrite the first `jnz keep` (0x75) to an
// unconditional `jmp keep` (0xEB) - the rel8 operand (0x2A) is unchanged and still targets the keep branch -
// so the zero-out is never reached and sphere lights keep their specular regardless of cinematic state.
template <GameState::Value gameT>
class SphereSpecularForceImpl : public ISphereSpecularForceImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	std::shared_ptr<MultilevelPointer> sphereSpecularForcePatch;

	static constexpr std::array<uint8_t, 1> kOnBytes{ 0xEB };  // jmp keep  (always skip the specular zero-out)
	static constexpr std::array<uint8_t, 1> kOffBytes{ 0x75 }; // jnz keep  (stock: only skip when cinematic)

	// writes the single opcode byte; throws if the site isn't resolvable yet (e.g. halo2.dll not loaded at the
	// MCC menu). protectedMemory = true: target is executable .text, so this goes through VirtualProtect.
	void writePatch(const std::array<uint8_t, 1>& bytes)
	{
		if (!sphereSpecularForcePatch->writeArrayData(const_cast<uint8_t*>(bytes.data()), bytes.size(), true))
			throw HCMRuntimeException(std::format("Failed to write SphereSpecularForce patch: {}", MultilevelPointer::GetLastError()));
	}

	void applyCurrentState()
	{
		lockOrThrow(settingsWeak, settings);
		writePatch(settings->sphereSpecularForceToggle->GetValue() ? kOnBytes : kOffBytes);
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "SphereSpecularForce onToggle, newValue: " << newValue;

		// the patch is read per light-render frame, so it takes effect live. if halo2.dll isn't loaded yet
		// (e.g. toggled at the MCC menu) it'll be applied on the next Halo 2 load via onMCCStateChanged.
		try { writePatch(newValue ? kOnBytes : kOffBytes); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "SphereSpecularForce: patch deferred (" << ex.what() << ")"; }

		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(newValue ? "Sphere specular forced on" : "Sphere specular restored to stock");
		}
		catch (HCMRuntimeException&) {}
	}

	// Lifecycle: our patch is live ONLY while fully in a game. Outside that (save-and-quit, loading screens) we
	// force the site back to stock, and re-enable it when we're back in-game on the new map (if the toggle is on).
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return; // not Halo 2 (dll may be unloaded) - nothing to touch
		if (newState.currentPlayState != PlayState::Ingame)
		{
			// force stock during teardown/menu. Single-byte opcode -> atomic, no thread-suspend needed.
			try { writePatch(kOffBytes); }
			catch (HCMRuntimeException&) {} // halo2.dll gone / not resolvable -> nothing to revert
			return;
		}
		try { applyCurrentState(); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "SphereSpecularForce: load-time apply skipped (" << ex.what() << ")"; }
	}

public:
	SphereSpecularForceImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->sphereSpecularForceToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); }),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		sphereSpecularForcePatch = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(sphereSpecularForcePatch), mGame);
	}

	// restore stock behaviour so we never leave the game patched after HCM unloads
	~SphereSpecularForceImpl()
	{
		// Suspend other threads while restoring this .text patch: the render thread runs this
		// instruction every light-render frame, so reverting it underneath the running thread can
		// crash the game. Only the memory write happens inside the suspend window. (See ScopedThreadSuspender.)
		try { ScopedThreadSuspender suspend; writePatch(kOffBytes); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


SphereSpecularForce::SphereSpecularForce(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<SphereSpecularForceImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("SphereSpecularForce not impl for this game");
	}
}

SphereSpecularForce::~SphereSpecularForce()
{
	PLOG_DEBUG << "~" << getName();
}
