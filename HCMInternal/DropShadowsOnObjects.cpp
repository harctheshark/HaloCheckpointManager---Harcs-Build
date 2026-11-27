#include "pch.h"
#include "DropShadowsOnObjects.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "ScopedThreadSuspender.h"
#include "H2SafeWrite.h"
#include <array>

// TWO patches in halo2.dll (1.3528), both on while the toggle is on:
//
// 1. dropShadowsOnObjectsFilterJump (0x9EEF1B, one byte). The drop-shadow caster pass calls the receiver-filter
//    `sub_1809EEDD0` once per caster. For every other object the filter does `bt a4, type` (a4 = an
//    object-receive mask, type = the object's type byte at +0xAA) and a `jb`: taken, the object keeps bit
//    0x1000 ("draw this object as a receiver"); not taken, the bit is stripped. Stock passes a4 = 0, so no
//    object ever receives. We rewrite that `jb` (72) to `jmp` (EB): every object type keeps its receive
//    bit. The filter's only caller is that pass (0x7EAC37). The caster's own entry takes another branch.
//
// 2. dropShadowsOnObjectsSphereSelect (0x709951, 62 bytes rewritten in place, same length), in `sub_1807098C0`,
//    which adds an object to a visible set. For the shadow passes (set mode != 0) stock tests the object's
//    SECOND sphere - centre +0x50, radius +0x5C, the tag's dynamic light / shadow sphere - instead of its
//    bounding sphere (+0x30 / +0x3C). Six tags set that sphere to almost nothing (under a tenth of the
//    bounding radius): Ivory Tower's hanging plant pot (crate hang_planter: 0.15 against 3.0, up at its
//    hanging point), the scarab, high_banner, door_plug, library_blocker, regret_holo. The shadow passes then
//    never collect them, so nothing can cast a drop shadow onto them. The rewrite keeps stock behaviour
//    EXCEPT for such an object: when the shadow radius is under about a tenth of the bounding radius (their
//    float bit patterns differ by >= 0x01A00000, i.e. 9.1x .. 10x) the bounding sphere is used, centre and
//    radius. The 53 tags with a moderately smaller shadow sphere (27% .. 86%: trees, bushes, stalagmites - a
//    deliberate sphere on part of the object) and every object whose spheres are equal are untouched.
//    Tag survey: H2EKMine\HCM Collision Viewer Notes\object-sphere-audit\. The new bytes were executed in a
//    test wrapper against the stock bytes before going in (H2 Live Probe\dso_rule_test.py: 84 cases).
//
//      stock                                        new
//      test  ecx, ecx                               mov   r14d, 30h
//      jnz   +7                                     test  ecx, ecx
//      movss xmm6, [rdi+3Ch]                        jz    use
//      jmp   +5                                     mov   eax, [rdi+3Ch]
//      movss xmm6, [rdi+5Ch]                        sub   eax, [rdi+5Ch]
//      movzx r12d, byte ptr [rsp+0C0h]              cmp   eax, 1A00000h
//      lea   rax, [rsp+50h]                         jge   use
//      mov   [rsp+48h], rax                         mov   r14b, 50h
//      test  ecx, ecx                          use: movss xmm6, [rdi+r14+0Ch]
//      lea   rax, [rsp+51h]                         movzx r12d, byte ptr [rsp+0C0h]
//      mov   edx, 50h                               lea   rax, [rsp+50h]
//      mov   [rsp+40h], rax                         mov   [rsp+48h], rax
//      mov   r14d, 30h                              lea   rax, [rsp+51h]
//      cmovnz r14d, edx                             mov   [rsp+40h], rax
//    After the span the game overwrites rax, rdx, rcx and the flags before reading them, and nothing jumps
//    into the span from outside it (checked in IDA).
//
// History: until 2026-11-17 patch 1 was the CALLER's `xor r9d, r9d` -> `mov r9b, 0xFF` (pointer entry
// dropShadowsOnObjectsPatch, 0x7EAC2B, still in the pointer data for older builds). That only sets a4's low
// byte = object types 0-7; crates (11) and creatures (12) passed only because r9's upper bits held
// leftovers from the call before. The sphere finding was confirmed on the running game (live pokes of a
// blanket "always the bounding sphere" variant; the user saw the shadow appear on the pot).
template <GameState::Value gameT>
class DropShadowsOnObjectsImpl : public IDropShadowsOnObjectsImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;

	std::shared_ptr<MultilevelPointer> dropShadowsOnObjectsFilterJump;
	std::shared_ptr<MultilevelPointer> dropShadowsOnObjectsSphereSelect;

	static constexpr size_t kSpan = 62;
	static constexpr std::array<uint8_t, kSpan> kSphereStock{
		0x85, 0xC9,                                            // test  ecx, ecx
		0x75, 0x07,                                            // jnz   +7
		0xF3, 0x0F, 0x10, 0x77, 0x3C,                          // movss xmm6, [rdi+3Ch]
		0xEB, 0x05,                                            // jmp   +5
		0xF3, 0x0F, 0x10, 0x77, 0x5C,                          // movss xmm6, [rdi+5Ch]
		0x44, 0x0F, 0xB6, 0xA4, 0x24, 0xC0, 0x00, 0x00, 0x00,  // movzx r12d, byte ptr [rsp+0C0h]
		0x48, 0x8D, 0x44, 0x24, 0x50,                          // lea   rax, [rsp+50h]
		0x48, 0x89, 0x44, 0x24, 0x48,                          // mov   [rsp+48h], rax
		0x85, 0xC9,                                            // test  ecx, ecx
		0x48, 0x8D, 0x44, 0x24, 0x51,                          // lea   rax, [rsp+51h]
		0xBA, 0x50, 0x00, 0x00, 0x00,                          // mov   edx, 50h
		0x48, 0x89, 0x44, 0x24, 0x40,                          // mov   [rsp+40h], rax
		0x41, 0xBE, 0x30, 0x00, 0x00, 0x00,                    // mov   r14d, 30h
		0x44, 0x0F, 0x45, 0xF2 };                              // cmovnz r14d, edx
	static constexpr std::array<uint8_t, kSpan> kSpherePatched{
		0x41, 0xBE, 0x30, 0x00, 0x00, 0x00,                    // mov   r14d, 30h
		0x85, 0xC9,                                            // test  ecx, ecx
		0x74, 0x10,                                            // jz    use           (mode 0: bounding sphere, as stock)
		0x8B, 0x47, 0x3C,                                      // mov   eax, [rdi+3Ch]
		0x2B, 0x47, 0x5C,                                      // sub   eax, [rdi+5Ch]
		0x3D, 0x00, 0x00, 0xA0, 0x01,                          // cmp   eax, 1A00000h
		0x7D, 0x03,                                            // jge   use           (shadow sphere under ~a tenth)
		0x41, 0xB6, 0x50,                                      // mov   r14b, 50h     (else the shadow sphere, as stock)
		0xF3, 0x42, 0x0F, 0x10, 0x74, 0x37, 0x0C,              // use: movss xmm6, [rdi+r14+0Ch]
		0x44, 0x0F, 0xB6, 0xA4, 0x24, 0xC0, 0x00, 0x00, 0x00,  // movzx r12d, byte ptr [rsp+0C0h]
		0x48, 0x8D, 0x44, 0x24, 0x50,                          // lea   rax, [rsp+50h]
		0x48, 0x89, 0x44, 0x24, 0x48,                          // mov   [rsp+48h], rax
		0x48, 0x8D, 0x44, 0x24, 0x51,                          // lea   rax, [rsp+51h]
		0x48, 0x89, 0x44, 0x24, 0x40 };                        // mov   [rsp+40h], rax
	static constexpr uint8_t kJumpStock = 0x72, kJumpPatched = 0xEB;

	// INSIDE a suspension window: no heap, no locks, no logging - bare system calls and a copy only
	static bool rawWrite(uintptr_t at, const uint8_t* data, size_t n)
	{
		return h2safe::patch(at, data, n);   // guarded: halo2.dll can be unloaded under us (2026-11-18)
	}
	// INSIDE a suspension window: is any stopped thread executing inside [lo, hi)? "Cannot tell" counts as yes.
	static bool threadInside(const ScopedThreadSuspender& s, uintptr_t lo, uintptr_t hi)
	{
		if (!s.snapshotOk() || s.missedCount() != 0) return true;
		const HANDLE* hs = s.suspendedHandles(); const size_t n = s.suspendedCount();
		for (size_t i = 0; i < n; ++i)
		{
			CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
			if (!GetThreadContext(hs[i], &ctx)) return true;
			if (ctx.Rip >= lo && ctx.Rip < hi) return true;
		}
		return false;
	}

	// Brings both sites to `on`. Throws if a site isn't resolvable yet (e.g. halo2.dll not loaded at the MCC
	// menu), holds bytes that are neither stock nor ours (then NOTHING is written: not the code we expect), or
	// no safe moment came up. The sites are resolved and compared FIRST, unsuspended, and exceptions are built
	// outside the suspension window: those paths allocate, lock and log, and a frozen thread holding one of
	// those locks would deadlock us (see ScopedThreadSuspender).
	// The 62-byte span is straight-line code with no calls in it, so the only hazard is a thread stopped WITH
	// ITS INSTRUCTION POINTER INSIDE the span (it would resume in the middle of different instructions). Each
	// window is therefore accepted only when no stopped thread is there; otherwise it is released and another
	// moment tried. A frame that runs some calls stock and some patched is harmless: each call stands alone.
	void writePatch(bool on)
	{
		uintptr_t jumpAt = 0, sphereAt = 0;
		if (!dropShadowsOnObjectsFilterJump->resolve(&jumpAt) || !dropShadowsOnObjectsSphereSelect->resolve(&sphereAt))
			throw HCMRuntimeException(std::format("DropShadowsOnObjects: patch site not resolvable: {}", MultilevelPointer::GetLastError()));
		uint8_t jumpNow = 0;
		uint8_t sphereNow[kSpan] = {};
		if (!h2safe::copy(&jumpNow, (const void*)jumpAt, 1) || !h2safe::copy(sphereNow, (const void*)sphereAt, kSpan))
			throw HCMRuntimeException("DropShadowsOnObjects: patch site not readable");

		const bool sphereIsStock = memcmp(sphereNow, kSphereStock.data(), kSpan) == 0;
		const bool sphereIsPatched = memcmp(sphereNow, kSpherePatched.data(), kSpan) == 0;
		if ((jumpNow != kJumpStock && jumpNow != kJumpPatched) || (!sphereIsStock && !sphereIsPatched))
			throw HCMRuntimeException("DropShadowsOnObjects: the code at a patch site is not what this build expects; nothing written");

		const uint8_t jumpWant = on ? kJumpPatched : kJumpStock;
		const uint8_t* sphereWant = on ? kSpherePatched.data() : kSphereStock.data();
		const bool sphereOk = on ? sphereIsPatched : sphereIsStock;
		if (jumpNow == jumpWant && sphereOk) return;

		bool wrote = false, failed = false;
		for (int attempt = 0; attempt < 400 && !wrote && !failed; ++attempt)
		{
			{
				ScopedThreadSuspender suspend;
				if (!threadInside(suspend, sphereAt, sphereAt + kSpan))
				{
					// the sites must still hold what was read above (the dll can be unloaded, or reloaded, in between)
					if (!h2safe::equals(jumpAt, &jumpNow, 1) || !h2safe::equals(sphereAt, sphereNow, kSpan)) failed = true;
					else if (!sphereOk && !rawWrite(sphereAt, sphereWant, kSpan)) failed = true;
					else if (jumpNow != jumpWant && !rawWrite(jumpAt, &jumpWant, 1)) failed = true;
					else wrote = true;
				}
			}   // everything resumes here
			if (!wrote && !failed) Sleep(1);
		}
		if (failed)
			throw HCMRuntimeException("DropShadowsOnObjects: the code changed, went away or could not be made writable");
		if (!wrote)
			throw HCMRuntimeException("Drop Shadows On Objects: could not find a safe moment to patch. Nothing was changed - toggle it off and on again.");
	}

	void applyCurrentState()
	{
		lockOrThrow(settingsWeak, settings);
		writePatch(settings->dropShadowsOnObjectsToggle->GetValue());
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "DropShadowsOnObjects onToggle, newValue: " << newValue;

		// the patched code runs per shadow-render frame, so it takes effect live. if halo2.dll isn't loaded yet
		// (e.g. toggled at the MCC menu) it'll be applied on the next Halo 2 load via onMCCStateChanged.
		try { writePatch(newValue); }
		catch (HCMRuntimeException& ex) { PLOG_DEBUG << "DropShadowsOnObjects: patch deferred (" << ex.what() << ")"; }

		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(newValue ? "Object drop shadows on" : "Object drop shadows off");
		}
		catch (HCMRuntimeException&) {}
	}

	// Lifecycle: our patch is live ONLY while fully in a game. Outside that (save-and-quit, loading screens) we
	// force the sites back to stock so nothing of ours is live during teardown, and re-enable them when we're back
	// in-game on the new map (if the toggle is on).
	void onMCCStateChanged(const MCCState& newState)
	{
		if (newState.currentGameState != mGame) return; // not Halo 2 (dll may be unloaded) - nothing to touch
		if (newState.currentPlayState != PlayState::Ingame)
		{
			try { writePatch(false); }
			catch (HCMRuntimeException&) {} // halo2.dll gone / not resolvable -> nothing to revert
			return;
		}
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
		// two NEW pointer names (not the old dropShadowsOnObjectsPatch): with pointer data that predates them
		// this throws and the toggle is unavailable, instead of these bytes being written at the old 3-byte site
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		dropShadowsOnObjectsFilterJump = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(dropShadowsOnObjectsFilterJump), mGame);
		dropShadowsOnObjectsSphereSelect = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(dropShadowsOnObjectsSphereSelect), mGame);
	}

	// restore stock behaviour so we never leave the game patched after HCM unloads
	~DropShadowsOnObjectsImpl()
	{
		try { writePatch(false); }
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
