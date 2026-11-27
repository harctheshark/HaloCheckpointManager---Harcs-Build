#include "pch.h"
#include "MetroFirstPersonBody.h"
#include "MetroEngine.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "GlobalKill.h"

// First Person Body (Metro Redux). Build metro_body_107 (2026-11-25), every piece proven live with memory pokes first:
//  - DRAW: 0x261530(entity) sets entity+0x10C bit 1 ("drawn in the main view") = !IsFirstPerson (vtbl +0xF70). Hooked: after it,
//    the PLAYER only gets bit 1 (build 104/105 patched the test for every entity, which also drew the holstered gear).
//  - HIDE: bones are drawn with skinning = model x inverse bind; the inverse bind is the 3x4 at bone def +0x40 (defs stride 160,
//    own +0x30, parent +0x32). The skeleton instance M reads its defs through its OWN pointer M+0x60 (a copy of skel+0x2A0,
//    shared by every NPC with the same rig). The player's M+0x60 is pointed at a private copy with the inverse bind zeroed
//    under the hidden root -> the game's own skinning collapses those bones every frame (no timing gap, NPCs untouched).
//  - GEAR: the inventory (3 guns, charger, knife, gas mask + filter on the hips) is NOT on the body mesh or its locators: they
//    are the player's attached child entities without an attach bone (+288 == 0xFFFF, owner *(+280) - 160 == player), and the
//    player's draw submit 0x6D7870 submits them through 0x6D73B0(child, ...) after itself. In normal first person the player is
//    not submitted, so neither are they; hooked: 0x6D73B0 returns 0 for those children (proven: player child count 0 for 10 s).
//    (0x7CB9F0 locator compose and 0x7CC120 bone compose never run for the player - it has an animator at M+0xA0.)
//  - OFFSET: the skinning (+48 = model x inverse bind) is written by 0xCB200 / 0xCAC70(M, float blend in xmm1) via M vtbl
//    +0x58. Hooked: for the player, the "offset back" is added to every bone's model translation (+0) just for that call and
//    taken off again, so the skinning comes out moved with no gap.
namespace
{
	struct Site { uintptr_t rva; uint8_t prologue[15]; const char* name; };
	constexpr Site kVisFn = { 0x261530, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF9 }, "main-view flag update" };
	constexpr Site kSubmitFn = { 0x6D73B0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 }, "child draw submit" };
	constexpr Site kSkinFn = { 0xCB200, { 0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x41, 0x54, 0x41, 0x55 }, "skinning" };
	constexpr Site kSkinBlendFn = { 0xCAC70, { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81 }, "blended skinning" };

	using vis_t = uintptr_t(__fastcall*)(uintptr_t entity, uintptr_t a2);
	using submit_t = uintptr_t(__fastcall*)(uintptr_t child, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7);
	using skin_t = uintptr_t(__fastcall*)(uintptr_t M, float blend);

	constexpr uintptr_t kEntityFlagsOff = 0x10C;
	constexpr uint32_t kFlagMainView = 0x2, kFlagForceHidden = 0x200;
	constexpr uintptr_t kPlayerVisualOff = 256;    // model/visual V; skeleton instance M = V->vtbl[+0x18](V)
	constexpr uintptr_t kPlayerBoneHeadOff = 0x610, kPlayerBoneHandLOff = 0x61A, kPlayerBoneHandROff = 0x61C;   // u16 bone ids
	constexpr uintptr_t kPlayerLookOff = 0x1244, kPlayerTransformOff = 192;
	constexpr uintptr_t kMModelDefOff = 0x40, kModelSkelOff = 0x18, kMBonesOff = 0x50, kMDefsOff = 0x60, kMBoneCountOff = 0x89;
	constexpr uintptr_t kSkelBoneDefsOff = 0x2A0, kSkelBoneCountOff = 0x2AA, kSkelLocatorCountOff = 0x2D2;
	constexpr uintptr_t kBoneDefStride = 160, kBoneDefOwnOff = 0x30, kBoneDefParentOff = 0x32, kBoneDefInvBindOff = 0x40;
	constexpr int kMaxBones = 256;

	constexpr uintptr_t kEntityAttachBoneOff = 288, kEntityAttachOwnerOff = 280, kEntityLinkOff = 160;
	constexpr uintptr_t kBoneStride = 144;

	std::atomic<vis_t> gOrigVis{ nullptr };
	std::atomic<submit_t> gOrigSubmit{ nullptr };
	std::atomic<skin_t> gOrigSkin{ nullptr }, gOrigSkinBlend{ nullptr };
	std::atomic<bool> gOn{ false }, gHideGear{ true };
	std::atomic<uintptr_t> gPlayerM{ 0 };
	std::atomic<float> gShiftX{ 0.f }, gShiftZ{ 0.f };   // model-space metres added to every bone for the skinning
	std::atomic<int> gVisForced{ 0 }, gGearSkipped{ 0 }, gSkinShifted{ 0 };

	bool sehCallGetM(uintptr_t fn, uintptr_t V, uintptr_t* out)
	{
		__try { *out = ((uintptr_t(__fastcall*)(uintptr_t))fn)(V); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	uintptr_t __fastcall visDetour(uintptr_t entity, uintptr_t a2)
	{
		const uintptr_t r = gOrigVis.load(std::memory_order_acquire)(entity, a2);
		if (gOn.load(std::memory_order_relaxed) && entity && entity == metro::player())
		{
			uint32_t* flags = (uint32_t*)(entity + kEntityFlagsOff);
			if ((*flags & (kFlagMainView | kFlagForceHidden)) == 0) { *flags |= kFlagMainView; gVisForced.fetch_add(1, std::memory_order_relaxed); }
		}
		return r;
	}

	// the player's own child entities without an attach bone = the carried inventory (drawn only because the player is)
	uintptr_t __fastcall submitDetour(uintptr_t child, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
	{
		if (gOn.load(std::memory_order_relaxed) && gHideGear.load(std::memory_order_relaxed) && child
			&& *(const uint16_t*)(child + kEntityAttachBoneOff) == 0xFFFF)
		{
			const uintptr_t link = *(const uintptr_t*)(child + kEntityAttachOwnerOff);
			const uintptr_t ownerLink = link ? *(const uintptr_t*)link : 0;
			const uintptr_t pl = metro::player();
			if (ownerLink && pl && ownerLink - kEntityLinkOff == pl)
			{
				gGearSkipped.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}
		}
		return gOrigSubmit.load(std::memory_order_acquire)(child, a2, a3, a4, a5, a6, a7);
	}

	// add the offset to the skinning the game just wrote (+48 translation), only for the bones it wrote this call: those with
	// their bit in M+0x10 | M+0x20 (the writer's own test). The model matrices (+0) are never touched - build 107 moved them
	// for the call and back, which raced the physics / hit reactions writing them ("legs detach when attacked").
	void shiftSkinning(uintptr_t M, float dx, float dz)
	{
		const uintptr_t bones = *(const uintptr_t*)(M + kMBonesOff);
		const int n = *(const uint8_t*)(M + kMBoneCountOff);
		if (!bones) return;
		const uint32_t* vis = (const uint32_t*)(M + 0x10);
		const uint32_t* upd = (const uint32_t*)(M + 0x20);
		for (int b = 0; b < n; ++b)
		{
			if (((vis[b >> 5] | upd[b >> 5]) & (1u << (b & 31))) == 0) continue;
			float* s = (float*)(bones + kBoneStride * b + 48);
			s[3] += dx; s[11] += dz;
		}
	}
	template <int Which>
	uintptr_t __fastcall skinDetour(uintptr_t M, float blend)
	{
		const skin_t orig = (Which == 0 ? gOrigSkin : gOrigSkinBlend).load(std::memory_order_acquire);
		const uintptr_t r = orig(M, blend);
		const float dx = gShiftX.load(std::memory_order_relaxed), dz = gShiftZ.load(std::memory_order_relaxed);
		if (gOn.load(std::memory_order_relaxed) && M && M == gPlayerM.load(std::memory_order_relaxed) && (dx != 0.f || dz != 0.f))
		{
			shiftSkinning(M, dx, dz);
			gSkinShifted.fetch_add(1, std::memory_order_relaxed);
		}
		return r;
	}
}

class MetroFirstPersonBody::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::atomic<bool> mReady{ false };
	bool mBuildOk = false;
	bool mHooked = false;
	std::mutex mMutex;

	// the private bone defs given to the player's skeleton instance
	// no shadow from the third-person body (user 2026-11-25: its shadow showed the cut-off body with stretched edges). The
	// game's own switch (script action "actions/engine/shadowcast" -> entity vtbl +0xCF8 = 0x75AFC0 for the player): bit 0x20
	// of the word at entity+48 = "casts no shadow", set under the spin lock at entity+88. Proven live: shadow gone, body drawn.
	uintptr_t mShadowPlayer = 0;   // the player we turned the shadow off for
	bool mShadowWasOff = false;    // it already had no shadow (then we leave it that way)
	uintptr_t mDefsM = 0;          // the M whose +0x60 we pointed at our copy
	uintptr_t mDefsShared = 0;     // what it pointed at before (the rig's shared defs)
	uintptr_t mDefsCopy = 0;
	bool mDefsUpper = true;
	bool mStatsLogged = false;
	std::chrono::steady_clock::time_point mOnSince{};

	void say(const std::string& s) { if (auto m = messagesWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	bool ensureHooks()
	{
		if (mHooked) return true;
		static auto* parked = new std::vector<safetyhook::InlineHook>();   // never freed (HCM stays resident)
		auto hv = safetyhook::InlineHook::create((void*)(metro::base() + kVisFn.rva), (void*)&visDetour, safetyhook::InlineHook::StartDisabled);
		auto hs = safetyhook::InlineHook::create((void*)(metro::base() + kSubmitFn.rva), (void*)&submitDetour, safetyhook::InlineHook::StartDisabled);
		auto hk = safetyhook::InlineHook::create((void*)(metro::base() + kSkinFn.rva), (void*)&skinDetour<0>, safetyhook::InlineHook::StartDisabled);
		auto hb = safetyhook::InlineHook::create((void*)(metro::base() + kSkinBlendFn.rva), (void*)&skinDetour<1>, safetyhook::InlineHook::StartDisabled);
		if (!hv || !hs || !hk || !hb) { PLOG_ERROR << "First Person Body: could not create the hooks"; return false; }
		gOrigVis.store(hv->original<vis_t>(), std::memory_order_release);
		gOrigSubmit.store(hs->original<submit_t>(), std::memory_order_release);
		gOrigSkin.store(hk->original<skin_t>(), std::memory_order_release);
		gOrigSkinBlend.store(hb->original<skin_t>(), std::memory_order_release);
		if (!hv->enable() || !hs->enable() || !hk->enable() || !hb->enable()) { PLOG_ERROR << "First Person Body: could not enable the hooks"; return false; }
		parked->push_back(std::move(*hv)); parked->push_back(std::move(*hs)); parked->push_back(std::move(*hk)); parked->push_back(std::move(*hb));
		mHooked = true;
		return true;
	}

	// give the player's skeleton instance back the rig's shared defs (if it still points at ours)
	static constexpr uintptr_t kEntityRenderFlagsOff = 48, kEntityRenderLockOff = 88;
	static constexpr uint16_t kRenderNoShadow = 0x20;
	static bool sehSetCastShadow(uintptr_t entity, bool cast)
	{
		__try
		{
			volatile LONG* lock = (volatile LONG*)(entity + kEntityRenderLockOff);
			for (int spins = 0; InterlockedCompareExchange(lock, -1, 0) != 0; ++spins) { if (spins > 1000000) return false; YieldProcessor(); }
			volatile uint16_t* w = (volatile uint16_t*)(entity + kEntityRenderFlagsOff);
			*w = cast ? (uint16_t)(*w & ~kRenderNoShadow) : (uint16_t)(*w | kRenderNoShadow);
			InterlockedExchange(lock, 0);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	// keep the current player's shadow off; called every frame while on (the game may turn it back on, e.g. a new level)
	void keepShadowOff(uintptr_t pl)
	{
		uint16_t w = 0;
		if (!pl || !metro::read(pl + kEntityRenderFlagsOff, w)) return;
		if (pl != mShadowPlayer)
		{
			mShadowPlayer = pl; mShadowWasOff = (w & kRenderNoShadow) != 0;
			PLOG_INFO << std::format("First Person Body: turning off the shadow of the player's body (render flags {:04x})", w);
		}
		if (!(w & kRenderNoShadow)) sehSetCastShadow(pl, false);
	}
	void restoreShadow()
	{
		if (mShadowPlayer && !mShadowWasOff && mShadowPlayer == metro::player()) sehSetCastShadow(mShadowPlayer, true);
		mShadowPlayer = 0; mShadowWasOff = false;
	}

	void restoreDefs()
	{
		if (mDefsM && mDefsCopy && metro::readPtr(mDefsM + kMDefsOff) == mDefsCopy)
			metro::write(mDefsM + kMDefsOff, mDefsShared);
		mDefsM = 0; mDefsCopy = 0; mDefsShared = 0;
	}

	void apply(bool on)
	{
		std::scoped_lock lk(mMutex);
		if (on)
		{
			if (!mBuildOk) { say("First Person Body: this metro.exe build does not match the one HCM was made for - not changed"); return; }
			if (!ensureHooks()) { say("First Person Body: could not hook the game's code"); return; }
			gVisForced = 0; gGearSkipped = 0; gSkinShifted = 0; mStatsLogged = false;
			mOnSince = std::chrono::steady_clock::now();
			gOn = true;
			say("First Person Body on");
		}
		else
		{
			gOn = false;
			gPlayerM = 0;
			restoreDefs();
			restoreShadow();
			say("First Person Body off");
		}
	}

	// which bones to hide: the head subtree, or (upper) the subtree from 2 bones above the head/hands' common ancestor
	static uint16_t pickRoot(const std::vector<uint16_t>& parentOf, int count, uint16_t head, uint16_t hl, uint16_t hr, bool upper)
	{
		auto chain = [&](uint16_t b) { std::vector<uint16_t> c; for (int i = 0; i < 128 && b < count; ++i) { c.push_back(b); b = parentOf[b]; } return c; };
		auto subtree = [&](uint16_t r) { int n = 0; for (int b = 0; b < count; ++b) { const auto c = chain((uint16_t)b); if (std::find(c.begin(), c.end(), r) != c.end()) ++n; } return n; };
		if (!upper || hl >= count || hr >= count) return head;
		const auto ch = chain(head), cl = chain(hl), cr = chain(hr);
		uint16_t root = head;
		for (uint16_t b : ch)
			if (std::find(cl.begin(), cl.end(), b) != cl.end() && std::find(cr.begin(), cr.end(), b) != cr.end()) { root = b; break; }
		// the common ancestor is the upper chest; the chest below it goes too (user: "cull the body/chest") - climb up to 2,
		// never to the skeleton root or its child (pelvis) and never so far that the legs would go (subtree within count - 10)
		for (int i = 0; i < 2; ++i)
		{
			const uint16_t p = parentOf[root];
			if (p >= count || parentOf[p] >= count || parentOf[parentOf[p]] >= count || subtree(p) > count - 10) break;
			root = p;
		}
		return root;
	}

	void refresh(bool upper, float backWu)
	{
		const uintptr_t pl = metro::player();
		const uintptr_t V = pl ? metro::readPtr(pl + kPlayerVisualOff) : 0;
		const uintptr_t vt = V ? metro::readPtr(V) : 0;
		const uintptr_t getM = vt ? metro::readPtr(vt + 0x18) : 0;
		uintptr_t M = 0;
		if (!getM || !sehCallGetM(getM, V, &M) || !metro::okPtr(M)) { gPlayerM = 0; return; }

		// the offset: straight back in the body's model space. The body faces its own +Z (it turns with the player).
		// 2026-11-25 (user: after a fall "my body offsets in front of me, and i have to look up to fix it"; offsetwatch.py:
		// the applied offset flipped +0.533 / -0.533 m): build 105-109 took the direction from player+0x1244, which is the
		// camera's UP vector, not the look - its forward tilt changes sign with the pitch (landing dip / looking up).
		gShiftX = 0.f;
		gShiftZ = -backWu * metro::kMetresPerHaloUnit;

		const uintptr_t mdef = metro::readPtr(M + kMModelDefOff);
		const uintptr_t skel = mdef ? metro::readPtr(mdef + kModelSkelOff) : 0;
		const uintptr_t shared = skel ? metro::readPtr(skel + kSkelBoneDefsOff) : 0;
		uint8_t count = 0, countM = 0; uint16_t nLoc = 0;
		if (!shared || !metro::read(skel + kSkelBoneCountOff, count) || !metro::read(M + kMBoneCountOff, countM) || count == 0 || count != countM
			|| !metro::read(skel + kSkelLocatorCountOff, nLoc))
		{ gPlayerM = 0; return; }

		std::scoped_lock lk(mMutex);
		if (!gOn) return;
		if (M != mDefsM || upper != mDefsUpper)
		{
			restoreDefs();
			uint16_t head = 0xFFFF, hl = 0xFFFF, hr = 0xFFFF;
			metro::read(pl + kPlayerBoneHeadOff, head); metro::read(pl + kPlayerBoneHandLOff, hl); metro::read(pl + kPlayerBoneHandROff, hr);
			if (head >= count) { PLOG_ERROR << "First Person Body: the player's head bone is unknown"; gPlayerM = 0; mDefsM = M; mDefsUpper = upper; return; }
			std::vector<uint8_t> defs(size_t(count) * kBoneDefStride);
			if (!metro::detail::sehRead(defs.data(), (const void*)shared, defs.size())) { gPlayerM = 0; return; }
			std::vector<uint16_t> parentOf(kMaxBones, 0xFFFF);
			int badOwn = 0;
			for (int k = 0; k < count; ++k)
			{
				const uint16_t own = *(const uint16_t*)&defs[k * kBoneDefStride + kBoneDefOwnOff];
				const uint16_t par = *(const uint16_t*)&defs[k * kBoneDefStride + kBoneDefParentOff];
				if (own != k) ++badOwn;
				parentOf[k] = par < count ? par : 0xFFFF;
			}
			const uint16_t root = pickRoot(parentOf, count, head, hl, hr, upper);
			int hidden = 0;
			for (int b = 0; b < count; ++b)
			{
				uint16_t x = (uint16_t)b;
				for (int i = 0; i < 128 && x < count; ++i)
				{
					if (x == root) { memset(&defs[b * kBoneDefStride + kBoneDefInvBindOff], 0, 48); ++hidden; break; }
					x = parentOf[x];
				}
			}
			// the copy lives forever (a few KB per skeleton instance): the game may still be reading it on another thread
			static auto* keep = new std::vector<std::unique_ptr<uint8_t[]>>();
			auto copy = std::make_unique<uint8_t[]>(defs.size() + 64);
			uint8_t* aligned = (uint8_t*)(((uintptr_t)copy.get() + 15) & ~uintptr_t(15));
			memcpy(aligned, defs.data(), defs.size());
			keep->push_back(std::move(copy));
			const uintptr_t prev = metro::readPtr(M + kMDefsOff);
			if (prev != shared) PLOG_WARNING << std::format("First Person Body: the player's skeleton pointed at {:x}, not the rig's defs {:x}", prev, shared);
			mDefsM = M; mDefsShared = shared; mDefsCopy = (uintptr_t)aligned; mDefsUpper = upper;
			metro::write(M + kMDefsOff, mDefsCopy);
			PLOG_INFO << std::format("First Person Body: skeleton {:x}: {} bones, {} locators{}, head {}, hands {} / {}, hiding from bone {} = {} bones ({})",
				M, count, nLoc, badOwn ? std::format(" ({} defs out of order!)", badOwn) : "", head, hl, hr, root, hidden, upper ? "upper body" : "head only");
		}
		else if (mDefsCopy && metro::readPtr(M + kMDefsOff) != mDefsCopy)
		{
			PLOG_INFO << "First Person Body: the game reset the player's skeleton defs - pointing it at the private copy again";
			metro::write(M + kMDefsOff, mDefsCopy);
		}
		gPlayerM = M;
		keepShadowOff(pl);

		if (!mStatsLogged && std::chrono::steady_clock::now() - mOnSince > std::chrono::seconds(3))
		{
			mStatsLogged = true;
			PLOG_INFO << std::format("First Person Body: in 3 s - drawn in the main view {} times, {} carried items not drawn, skinning moved {} times",
				gVisForced.load(), gGearSkipped.load(), gSkinShifted.load());
		}
	}

	void onRender(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire) || GlobalKill::isKillSet() || !gOn.load()) return;
		auto s = settingsWeak.lock();
		auto m = mccWeak.lock();
		if (!s || !m) return;
		if (!m->isGameCurrentlyPlaying(mGame)) { gPlayerM = 0; std::scoped_lock lk(mMutex); mDefsM = 0; mDefsCopy = 0; mShadowPlayer = 0; return; }   // level gone: don't write to it
		gHideGear = s->metroFirstPersonBodyHideGear->GetValue();
		refresh(!s->metroFirstPersonBodyShowArms->GetValue(), s->metroFirstPersonBodyBack->GetValue());
	}

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<RenderEvent> mRenderCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->metroFirstPersonBodyToggle->valueChangedEvent, [this](bool& on) { apply(on); }),
		mRenderCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRender(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroFirstPersonBody is Metro Redux only");
		mBuildOk = true;
		for (const Site* s : { &kVisFn, &kSubmitFn, &kSkinFn, &kSkinBlendFn })
		{
			uint8_t pro[sizeof(s->prologue)]{};
			if (!metro::detail::sehRead(pro, (const void*)(metro::base() + s->rva), sizeof(pro)) || memcmp(pro, s->prologue, sizeof(pro)) != 0)
			{
				PLOG_ERROR << "MetroFirstPersonBody: the game's code does not match at the " << s->name;
				mBuildOk = false;
			}
		}
		if (auto s = settingsWeak.lock(); s && s->metroFirstPersonBodyToggle->GetValue()) apply(true);
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderCallback.removeCallback();
		apply(false);   // the hooks stay installed (parked) but pass everything through
	}
};

MetroFirstPersonBody::MetroFirstPersonBody(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroFirstPersonBody::~MetroFirstPersonBody() { PLOG_VERBOSE << "~" << getName(); }
