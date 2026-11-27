#include "pch.h"
#include "NoInvisibleCollision.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "IMCCStateHook.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"

// ================================================================================================================================
// HOW (halo2.dll 1.3528; read 2026-11-15).
//
// The user's modded Cartographer xlive (Halo 2 Vista) has "Ghost faces (non-solid)": it patches slot 0 of the world collision
// filter's shape-collection-filter vtable (xlive sub_1008CD90 / sub_1008CD30) and answers "no collision" for the keys on its
// list. Havok asks that question when a body's bounding box first reaches a child shape of a shape collection (the level):
// a "no" makes the child an hkNullAgent, so the body never collides with that polygon.
//
// MCC has the same filter. Halo's filter object (built by sub_180703FF0) has its shape-collection filter at vtable
// halo2+0xB18E88, slot 0 = sub_180706490:
//     hkBool* isCollisionEnabled(this, hkBool* out, const hkCollisionInput& input, const hkCdBody& a, const hkCdBody& b,
//                                const hkShapeContainer& container, hkShapeKey key)
// Called from the BV-tree agent's child reconcile (sub_1809A9AC0) for every key NEW to a body's list. HCM hooks its entry;
// a key whose collision surface carries flag 2 ("invisible", the "*" surfaces) is refused, everything else goes to the game.
//
// Which surface a key is (notes: H2 Ghosting Research\notes\W2-tim-key-mapping.md):
//   container vtable halo2+0xB3C618 = the LEVEL collection (getChildShape sub_180792E90):
//     key >> 29 == 1: structure surface (key & 0xFFFF) of collision_bsp element 0 (*(halo2+0xE6F780))
//     key >> 29 == 5: instanced geometry: instance key & 0xFFFF (sbsp +0x140 / +0x144, stride 0x58, definition i16 +0x34),
//                     surface (key >> 16) & 0x1FFF of that definition (sbsp +0x138 / +0x13C, stride 200; surfaces count
//                     +0x98, offset +0x9C)
//     other categories (scenery 3 / 4, whole giant instances 2): left alone
//   container vtable halo2+0xB3C538 = a giant instance's own collection (getChildShape sub_180792E30): collision_bsp at
//     container +24, key = surface index
// collision_bsp: surfaces {count +0x28, offset +0x2C}, 8-byte records, flags byte +4 (2 = invisible).
//
// What it cannot do: a polygon a body ALREADY has an agent for keeps it until the polygon leaves the body's (slightly grown)
// bounding box, and a refused one stays refused just as long - Havok only asks about new keys. So switching it while
// pressed against an invisible wall takes effect once you step away from it.
// ================================================================================================================================
namespace
{
	constexpr uintptr_t kFilterRva = 0x706490;            // sub_180706490
	constexpr uintptr_t kLevelCollectionVt = 0xB3C618;    // slot 13 = sub_180792E90
	constexpr uintptr_t kInstanceCollectionVt = 0xB3C538; // slot 13 = sub_180792E30
	constexpr uintptr_t kSbsp = 0xE6F770, kWorldCollisionBsp = 0xE6F780, kPoolA = 0xE80AB0, kPoolB = 0xE80AC0;
	constexpr uint8_t kInvisibleFlag = 0x02;

	// mov [rsp+10h],rbp; push rsi; push rdi; push r14; sub rsp,120h
	const uint8_t kProbe[] = { 0x48, 0x89, 0x6C, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };

	bool bytesAt(uintptr_t a, const uint8_t* expect, size_t n)
	{
		__try { return memcmp((const void*)a, expect, n) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template <class T> bool rd(uintptr_t a, T& out)
	{
		if (a < 0x10000) return false;
		__try { out = *(const T*)a; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// a tag block {u32 count @blk, i32 offset @blk+4} -> first element (dual tag pools); false when unreadable / empty
	bool block(uintptr_t base, uintptr_t blk, uintptr_t& first, uint32_t& count)
	{
		int32_t off = -1;
		if (!rd(blk, count) || !rd(blk + 4, off) || count == 0 || off == -1) return false;
		uintptr_t pool = 0;
		if (!rd(base + (off >= 0 ? kPoolA : kPoolB), pool) || !pool) return false;
		first = pool + (uint32_t)(off >= 0 ? off : (off & 0x7FFFFFFF));
		return true;
	}

	// flags byte of surface `s` of a surfaces block {count, offset}; -1 = unreadable
	int surfaceFlags(uintptr_t base, uintptr_t surfacesBlock, uint32_t s)
	{
		uintptr_t first = 0; uint32_t count = 0;
		if (!block(base, surfacesBlock, first, count) || s >= count || count > 1000000) return -1;
		uint8_t f = 0;
		return rd(first + 8 * (uintptr_t)s + 4, f) ? f : -1;
	}

	// Is (container, key) an invisible collision surface? Anything not understood = no.
	bool invisibleSurface(uintptr_t base, uintptr_t container, uint32_t key)
	{
		uintptr_t vt = 0;
		if (!rd(container, vt)) return false;
		int flags = -1;
		if (vt == base + kLevelCollectionVt)
		{
			const uint32_t cat = key >> 29;
			if (cat == 1)
			{
				uintptr_t cb = 0;
				if (!rd(base + kWorldCollisionBsp, cb) || !cb) return false;
				flags = surfaceFlags(base, cb + 0x28, key & 0xFFFF);
			}
			else if (cat == 5)
			{
				uintptr_t sbsp = 0, iF = 0, dF = 0; uint32_t iN = 0, dN = 0;
				if (!rd(base + kSbsp, sbsp) || !sbsp) return false;
				if (!block(base, sbsp + 0x140, iF, iN) || !block(base, sbsp + 0x138, dF, dN)) return false;
				const uint32_t inst = key & 0xFFFF;
				if (inst >= iN) return false;
				int16_t def = -1;
				if (!rd(iF + 0x58 * (uintptr_t)inst + 0x34, def) || def < 0 || (uint32_t)def >= dN) return false;
				flags = surfaceFlags(base, dF + 200 * (uintptr_t)def + 0x98, (key >> 16) & 0x1FFF);
			}
			else return false;
		}
		else if (vt == base + kInstanceCollectionVt)
		{
			uintptr_t cb = 0;
			if (!rd(container + 24, cb) || !cb) return false;
			flags = surfaceFlags(base, cb + 0x28, key);
		}
		else return false;
		return flags >= 0 && (flags & kInvisibleFlag) != 0;
	}
}

class NoInvisibleCollision::NoInvisibleCollisionImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	std::mutex mMutex;
	std::unique_ptr<ModuleInlineHook> mHook;
	bool mVerified = false, mVerifyFailed = false;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	static inline std::atomic<bool> sOn{ false };
	static inline std::atomic<uintptr_t> sBase{ 0 };
	static inline std::atomic<ModuleInlineHook*> sHook{ nullptr };
	static inline std::atomic<int> sInFlight{ 0 };
	static inline std::atomic<uint32_t> sRefused{ 0 };

public:
	static bool wallsOffInGame() { return sOn.load() && sBase.load() != 0 && sHook.load() != nullptr; }
private:

	// the game's physics, at the entry of Halo's shape-collection filter
	static uint8_t* __fastcall detour(uintptr_t self, uint8_t* out, uintptr_t input, uintptr_t a, uintptr_t b, uintptr_t container, uint32_t key)
	{
		++sInFlight;
		uint8_t* r = out;
		const uintptr_t base = sBase.load();
		if (sOn.load() && base && out && invisibleSurface(base, container, key))
		{
			*out = 0;   // no collision: Havok makes this child an hkNullAgent
			++sRefused;
		}
		else if (ModuleInlineHook* const hook = sHook.load())
			r = hook->getInlineHook().call<uint8_t*, uintptr_t, uint8_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t>(self, out, input, a, b, container, key);
		--sInFlight;
		return r;
	}

	void say(const std::string& text)
	{
		try { lockOrThrow(messagesGUIWeak, m); m->addMessage(text); }
		catch (HCMRuntimeException&) {}
	}

	// 0 the game is not loaded (applies when it is), 1 applied, -1 not this build
	int apply(bool on)
	{
		std::scoped_lock lk(mMutex);
		sOn = on;
		const HMODULE mod = GetModuleHandleW(L"halo2.dll");
		if (!mod) return 0;
		const uintptr_t base = (uintptr_t)mod;
		if (!mVerified)
		{
			if (mVerifyFailed) return -1;
			uintptr_t s1 = 0, s2 = 0;
			const bool ok = bytesAt(base + kFilterRva, kProbe, sizeof(kProbe))
				&& rd(base + kLevelCollectionVt + 13 * 8, s1) && s1 == base + 0x792E90
				&& rd(base + kInstanceCollectionVt + 13 * 8, s2) && s2 == base + 0x792E30;
			if (!ok)
			{
				mVerifyFailed = true;
				PLOG_ERROR << "No invisible collision: halo2.dll is not the build this was made for (1.3528) - not applied";
				return -1;
			}
			mVerified = true;
		}
		sBase = base;
		mHook->setWantsToBeAttached(on);
		PLOG_INFO << "No invisible collision (Halo 2): " << (on ? "on" : "off") << ", " << sRefused.load() << " surfaces refused so far";
		return 1;
	}

	void onToggle(bool& on)
	{
		const int r = apply(on);
		if (r < 0) say("Invisible walls: this Halo 2 build is not supported (made for MCC 1.3528)");
		else if (r == 0 && on) say("Invisible walls off - applies when Halo 2 loads");
		else say(on ? "Invisible walls (*) off - real walls stay solid. A wall you are touching now goes once you step away from it"
			: "Invisible walls (*) back on - one you are inside now stays passable until you step out of it");
	}

	void onMCCStateChanged(const MCCState& s)
	{
		if (s.currentGameState != mGame) return;
		auto settings = settingsWeak.lock();
		if (!settings || !settings->noInvisibleCollisionToggle->GetValue()) return;
		apply(true);
	}

public:
	NoInvisibleCollisionImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->noInvisibleCollisionToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		if (game.operator GameState::Value() != GameState::Value::Halo2) throw HCMInitException("No invisible collision is Halo 2 only");
		if (sHook.load()) throw HCMInitException("No invisible collision already exists");
		auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)kFilterRva });
		mHook = ModuleInlineHook::make(L"halo2.dll", ptr, (void*)&detour, false);   // attached by apply(), after the build check
		sHook = mHook.get();
		if (dicon.Resolve<SettingsStateAndEvents>().lock()->noInvisibleCollisionToggle->GetValue()) apply(true);
	}

	~NoInvisibleCollisionImpl()
	{
		sOn = false;
		if (mHook) mHook->setWantsToBeAttached(false);
		for (int i = 0; i < 500 && sInFlight.load() > 0; ++i) Sleep(1);
		sHook = nullptr;
	}
};

NoInvisibleCollision::NoInvisibleCollision(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<NoInvisibleCollisionImpl>(gameImpl, dicon))
{
}

NoInvisibleCollision::~NoInvisibleCollision() = default;

bool NoInvisibleCollision::wallsOffInGame() { return NoInvisibleCollisionImpl::wallsOffInGame(); }
