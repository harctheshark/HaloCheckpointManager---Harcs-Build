#include "pch.h"
#include "HavokBroadphaseBypass.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ModuleCache.h"
#include "ScopedThreadSuspender.h"
#include <mutex>

// See HavokBroadphaseBypass.h for what each engine does and why the patch shape differs per game.

namespace
{
	enum class PatchKind
	{
		NopCall,   // site holds `E8 rel32` (call object_delete): write ONE 5-byte NOP `0F 1F 44 00 00` - a single
		           // instruction, so no suspended thread can ever sit inside it when the call is written back
		FlipJnz,   // site holds `75 rel8` (jnz past the delete onto the exempt path): write 0xEB (jmp, same rel8)
	};

	struct SiteSpec
	{
		const char* pattern;     // hex bytes, "??" = wildcard; must match EXACTLY ONCE in the module image
		size_t      patchOffset; // site = match + patchOffset
		PatchKind   kind;
	};

	// Per game. Every pattern was verified to match exactly once (and on every other build available on disk).
	// ⚠ The opcode byte AT patchOffset is wildcarded on purpose: the pattern must still match when the site already
	// holds OUR bytes (a patch left behind by an earlier HCM session), so it can be adopted and restored. Whether the
	// site is stock or ours is decided by resolveLocked's byte check, never by the pattern.
	// RE + verification reports: Documents\Halo Mod And Tools\Havok Broadphase Bypass\<game>\.
	std::optional<SiteSpec> siteFor(GameState game)
	{
		switch (game)
		{
		case GameState::Value::Halo2:      // jne -> jmp onto the engine's exempt epilogue (the delete is a TAIL JUMP: never NOP it)
			return SiteSpec{ "8B 4B 08 81 4B 04 00 10 00 00 E8 ?? ?? ?? ?? 84 C0 ?? 11 8B 4B 08 48 8B 5C 24 20 48 83 C4 28 E9 ?? ?? ?? ?? 48 8B 5C 24 20 48 83 C4 28 C3", 17, PatchKind::FlipJnz };
		case GameState::Value::Halo3:
			return SiteSpec{ "80 64 11 58 F7 44 8B 4F 08 41 8B C9 E8 ?? ?? ?? ?? 84 C0 75 ?? 41 8B C9 ?? ?? ?? ?? ?? EB ??", 24, PatchKind::NopCall };
		case GameState::Value::Halo3ODST:
			return SiteSpec{ "80 64 11 58 F7 44 8B 47 08 41 8B C8 E8 ?? ?? ?? ?? 84 C0 75 ?? 41 8B C8 ?? ?? ?? ?? ?? EB ??", 24, PatchKind::NopCall };
		case GameState::Value::HaloReach:
			return SiteSpec{ "80 64 CA 48 FD 44 8B 43 1C 41 8B C8 E8 ?? ?? ?? ?? 84 C0 75 ?? 41 8B C8 ?? ?? ?? ?? ?? EB ??", 24, PatchKind::NopCall };
		case GameState::Value::Halo4:      // jnz -> jmp onto the engine's own exempt (no-delete) path
			return SiteSpec{ "80 64 ?? 48 FD 8B ?? 1C E8 ?? ?? ?? ?? 84 C0 ?? ?? 8B ?? 1C E8 ?? ?? ?? ?? B3 01", 15, PatchKind::FlipJnz };
		case GameState::Value::Halo2MP:
			return SiteSpec{ "80 64 CA 48 FD 8B 4F 1C E8 ?? ?? ?? ?? 84 C0 75 ?? 8B 4F 1C ?? ?? ?? ?? ?? B3 01", 20, PatchKind::NopCall };
		case GameState::Value::HaloCER:
			return SiteSpec{ "24 FD 41 88 44 D1 48 B8 60 00 00 00 48 8B 04 30 80 78 11 04 75 11 41 8B 4E 1C E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 41 8B 4E 1C ?? ?? ?? ?? ?? 49 8D 5E 18 EB ??", 43, PatchKind::NopCall };
		default:
			return std::nullopt;
		}
	}

	// "8B ?? 1C" -> bytes + mask (false = wildcard)
	void parsePattern(const char* text, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask)
	{
		std::istringstream in(text);
		std::string tok;
		while (in >> tok)
		{
			if (tok == "??") { bytes.push_back(0); mask.push_back(0); }
			else { bytes.push_back((uint8_t)std::stoul(tok, nullptr, 16)); mask.push_back(1); }
		}
	}

	// SEH-only helpers: no C++ objects with destructors in here.
	uintptr_t sehScanUnique(uintptr_t base, size_t size, const uint8_t* bytes, const uint8_t* mask, size_t len, int* matchCount)
	{
		*matchCount = 0;
		__try
		{
			const uint8_t* p = (const uint8_t*)base;
			uintptr_t found = 0;
			for (size_t i = 0; i + len <= size; ++i)
			{
				size_t j = 0;
				for (; j < len; ++j)
					if (mask[j] && p[i + j] != bytes[j]) break;
				if (j != len) continue;
				++*matchCount;
				if (!found) found = base + i;
			}
			return *matchCount == 1 ? found : 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { *matchCount = -1; return 0; }
	}

	bool sehCopy(void* dest, const void* src, size_t n)
	{
		__try { memcpy(dest, src, n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

class HavokBroadphaseBypass::Impl
{
private:
	GameState mGame;
	SiteSpec mSpec;
	std::vector<uint8_t> mPatBytes, mPatMask;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::mutex mMutex;
	uintptr_t mModuleBase = 0;      // the module instance mSite belongs to (MCC unloads/reloads game dlls)
	uintptr_t mSite = 0;
	bool      mScanFailed = false;  // for THIS module instance - don't rescan a 40 MB image every state change
	bool      mApplied = false;
	bool      mHaveOriginal = false;   // mOriginal holds bytes WE read from the stock site (this module instance)
	bool      mShuttingDown = false;   // set by ~Impl under mMutex: no callback may (re)apply after it
	uint8_t   mOriginal[5]{};
	uint8_t   mPatched[5]{};

	size_t patchLength() const { return mSpec.kind == PatchKind::NopCall ? 5 : 1; }

	std::optional<uintptr_t> currentModuleBase() const
	{
		auto h = ModuleCache::getModuleHandle(mGame.toModuleName());
		if (!h.has_value() || !h.value()) return std::nullopt;
		return (uintptr_t)h.value();
	}

	// Caller holds mMutex. Finds and checks the site for the CURRENT module instance. Throws with a reason.
	void resolveLocked(uintptr_t base)
	{
		if (base != mModuleBase)
		{
			// A fresh module instance: whatever we patched before went away with the old mapping.
			mModuleBase = base; mSite = 0; mScanFailed = false; mApplied = false; mHaveOriginal = false;
		}
		if (mSite || mScanFailed) { if (!mSite) throw HCMRuntimeException("the patch site was not found in this build"); return; }

		MODULEINFO mi{};
		if (!GetModuleInformation(GetCurrentProcess(), (HMODULE)base, &mi, sizeof(mi)))
			throw HCMRuntimeException("could not query the game module's size");

		int matches = 0;
		const uintptr_t match = sehScanUnique(base, mi.SizeOfImage, mPatBytes.data(), mPatMask.data(), mPatBytes.size(), &matches);
		if (!match)
		{
			mScanFailed = true;
			throw HCMRuntimeException(matches == 0
				? "the broadphase-delete code was not found (the game may have updated) - nothing was patched"
				: std::format("the broadphase-delete signature matched {} times - refusing to guess, nothing was patched", matches));
		}

		const uintptr_t site = match + mSpec.patchOffset;
		uint8_t first = 0;
		if (!sehCopy(&first, (const void*)site, 1))
		{
			mScanFailed = true;
			throw HCMRuntimeException("could not read the broadphase-delete site");
		}
		// The site may legitimately already hold OUR bytes (HCM re-attaching to a module it patched last session).
		const bool looksStock = mSpec.kind == PatchKind::NopCall ? first == 0xE8 : first == 0x75;
		const bool looksOurs = mSpec.kind == PatchKind::NopCall ? first == 0x0F : first == 0xEB;
		if (!looksStock && !looksOurs)
		{
			mScanFailed = true;
			throw HCMRuntimeException(std::format("unexpected byte 0x{:02X} at the broadphase-delete site - refusing to patch", first));
		}
		mSite = site;
		PLOG_INFO << "HavokBroadphaseBypass(" << mGame.toString() << "): site at module+0x" << std::hex << (site - base);
	}

	// Caller holds mMutex. Writes `bytes` (patchLength) over the site. Multi-byte writes happen with every other thread
	// suspended so no thread can execute a half-written call; VirtualProtect is done OUTSIDE the suspended scope.
	void writeSiteLocked(const uint8_t* bytes)
	{
		const size_t len = patchLength();
		DWORD oldProtect = 0;
		if (!VirtualProtect((void*)mSite, len, PAGE_EXECUTE_READWRITE, &oldProtect))
			throw HCMRuntimeException("could not unprotect the broadphase-delete site");
		bool ok;
		if (len == 1)
		{
			ok = sehCopy((void*)mSite, bytes, 1);   // a single byte is atomic
		}
		else
		{
			ScopedThreadSuspender suspend;          // no allocation and no VM calls inside this scope
			ok = sehCopy((void*)mSite, bytes, len);
		}
		DWORD ignored = 0;
		VirtualProtect((void*)mSite, len, oldProtect, &ignored);
		FlushInstructionCache(GetCurrentProcess(), (void*)mSite, len);
		if (!ok) throw HCMRuntimeException("could not write the broadphase-delete site");
	}

	bool siteEquals(const uint8_t* bytes)
	{
		uint8_t now[5]{};
		return sehCopy(now, (const void*)mSite, patchLength()) && memcmp(now, bytes, patchLength()) == 0;
	}

	// Caller holds mMutex. Returns true if it changed something.
	bool applyLocked()
	{
		if (mShuttingDown) return false;
		auto base = currentModuleBase();
		if (!base) return false;                                 // module not loaded (other game / menu)
		resolveLocked(base.value());

		const size_t len = patchLength();
		if (mSpec.kind == PatchKind::NopCall) { static constexpr uint8_t kNop5[5] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 }; memcpy(mPatched, kNop5, 5); }
		else mPatched[0] = 0xEB;

		if (siteEquals(mPatched)) { mApplied = true; return false; }   // already patched (e.g. survived a re-attach)

		uint8_t cur[5]{};
		if (!sehCopy(cur, (const void*)mSite, len)) throw HCMRuntimeException("could not read the broadphase-delete site");
		const bool stock = mSpec.kind == PatchKind::NopCall ? cur[0] == 0xE8 : cur[0] == 0x75;
		if (!stock) throw HCMRuntimeException("the broadphase-delete site changed since it was found - refusing to patch");

		memcpy(mOriginal, cur, len);
		mHaveOriginal = true;
		writeSiteLocked(mPatched);
		mApplied = true;
		return true;
	}

	// Caller holds mMutex. Puts back exactly what we read, and only if the site still holds exactly what we wrote.
	bool restoreLocked()
	{
		if (!mApplied || !mSite) return false;
		auto base = currentModuleBase();
		if (!base || base.value() != mModuleBase) { mApplied = false; return false; }   // our mapping is gone
		if (!siteEquals(mPatched)) { mApplied = false; return false; }                // someone else owns it now
		if (!mHaveOriginal)
		{
			// We ADOPTED a site that was already patched (e.g. a previous HCM session died without restoring), so we
			// never saw the stock bytes. A flipped jnz has exactly one possible original (0x75); a NOP'd call's rel32 is
			// unknowable here - writing anything would corrupt the call, so leave the bypass in place and say so.
			if (mSpec.kind == PatchKind::FlipJnz) { mOriginal[0] = 0x75; mHaveOriginal = true; }
			else
			{
				PLOG_WARNING << "HavokBroadphaseBypass(" << mGame.toString() << "): cannot restore - the site was already "
					"patched when HCM attached and its original call bytes are unknown. It stays on until the game restarts.";
				mApplied = false;
				return false;
			}
		}
		writeSiteLocked(mOriginal);
		mApplied = false;
		return true;
	}

	bool wanted()
	{
		auto settings = settingsWeak.lock();
		return settings && settings->havokBroadphaseBypassToggle->GetValue();
	}

	bool playing()
	{
		auto hook = mccStateHookWeak.lock();
		return hook && hook->isGameCurrentlyPlaying(mGame);
	}

	void onToggle(bool& newValue)
	{
		try
		{
			std::string msg;
			{
				std::scoped_lock lock(mMutex);
				if (mShuttingDown) return;
				if (newValue)
				{
					if (!playing()) return;          // applied when this game is next in-game
					applyLocked();
					msg = "Havok broadphase bypass on: objects leaving the physics world are no longer deleted";
				}
				else
				{
					restoreLocked();
					if (!playing()) return;
					msg = "Havok broadphase bypass off";
				}
			}
			lockOrThrow(messagesGUIWeak, messagesGUI);
			messagesGUI->addMessage(msg);
		}
		catch (HCMRuntimeException& ex)
		{
			if (playing()) { HCMRuntimeException wrapped(std::format("Havok Broadphase Bypass: {}", ex.what())); runtimeExceptions->handleMessage(wrapped); }
			else PLOG_DEBUG << "HavokBroadphaseBypass(" << mGame.toString() << "): " << ex.what();
		}
	}

	void onMCCStateChanged(const MCCState& s)
	{
		if (s.currentGameState != mGame || s.currentPlayState != PlayState::Ingame) return;
		try
		{
			std::scoped_lock lock(mMutex);
			// Read UNDER mMutex: BinarySetting stores the new value before onToggle takes this lock, so a toggle-off
			// can no longer slip in between the check and the apply.
			if (mShuttingDown || !wanted()) return;
			applyLocked();   // a reloaded module is detected by its base and patched afresh
		}
		catch (HCMRuntimeException& ex)
		{
			HCMRuntimeException wrapped(std::format("Havok Broadphase Bypass: {}", ex.what()));
			runtimeExceptions->handleMessage(wrapped);
		}
	}

	// Declared LAST so they are destroyed FIRST: no event can reach a half-destroyed Impl.
	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mStateCallback;

public:
	Impl(GameState game, IDIContainer& dicon, SiteSpec spec)
		: mGame(game), mSpec(spec),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->havokBroadphaseBypassToggle->valueChangedEvent, [this](bool& v) { onToggle(v); }),
		mStateCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		parsePattern(mSpec.pattern, mPatBytes, mPatMask);
	}

	~Impl()
	{
		// ⚠ HCM stays resident and re-runs sessions: never leave a patch behind when this cheat goes away.
		// UNSUBSCRIBE FIRST: the destructor body runs before any member is destroyed, and MCC's load-end hook / the
		// HCE poll thread outlive this cheat, so without this a late "Ingame" event could re-patch after the restore.
		// A callback already inside the lock either finishes first (and its patch is restored below) or sees
		// mShuttingDown and does nothing.
		mStateCallback.removeCallback();
		mToggleCallback.removeCallback();
		try { std::scoped_lock lock(mMutex); mShuttingDown = true; restoreLocked(); }
		catch (...) {}
	}
};


HavokBroadphaseBypass::HavokBroadphaseBypass(GameState game, IDIContainer& dicon)
{
	auto spec = siteFor(game);
	if (!spec) throw HCMInitException(std::format("Havok Broadphase Bypass is not available for {}", game.toString()));
	pimpl = std::make_unique<Impl>(game, dicon, spec.value());
}

HavokBroadphaseBypass::~HavokBroadphaseBypass() { PLOG_VERBOSE << "~" << getName(); }
