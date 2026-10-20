#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>
#include "H2CV_Collision.h"
#include "H2CV_Renderer.h"
#include "H2CV_Engine.h"
#include "H2CV_Halo3.h"

namespace h2cv
{
	// InFrame: drawn inside the game's frame at the first-person pass, against the game's real world depth (default).
	// Own: drawn at Present against the viewer's own collision depth (automatic fallback when InFrame can't run,
	//      e.g. remastered graphics). XRay: no occlusion.
	enum class Occlusion : int { InFrame = 0, PostBloom, Own, XRay, Count };
	const char* occlusionName(Occlusion o);

	struct Settings
	{
		std::atomic_bool show[kCatCount] = { true, false, true, true, false, false, false, false, true };
		std::atomic_bool fills{ true };            // translucent invisible-barrier and kill-trigger faces
		std::atomic<float> barrierOpacity{ 0.03f };
		std::atomic<float> killOpacity{ 0.04f };
		std::atomic_int occlusion{ (int)Occlusion::InFrame };
		std::atomic_bool gameZReversed{ true };   // F12 diagnostic: game depth stored as 1 - z/w (W1: reversed-Z on by default)
		std::atomic<float> lineWidth{ 2.0f };     // px
		std::atomic<float> pull{ 0.0015f };       // relative depth pull (0.15% of distance)
		std::atomic<float> hiddenAlpha{ 0.0f };   // 0 = hidden lines invisible
		std::atomic<float> nearRadius{ 60.0f };   // wu around the camera
		std::atomic_bool tim{ true };             // TIM overlay (player's level-pair children)
		std::atomic_bool timLabels{ true };
		std::atomic_bool pills{ true };           // Havok capsules of bipeds
		std::atomic_bool bipedOccluders{ true };  // capsules of OTHER bipeds/vehicles occlude lines
		std::atomic_bool hud{ true };
		std::atomic_bool creases{ false };             // static acute-crease PREDICTION (CollisionWorld::Crease) - disproved as a rule (90 deg corners ghost too); kept as a visual aid
		std::atomic_bool creaseFaces{ true };          // also outline both faces of each crease
		// ghost desync LOGGER: while Ghost debug runs, writes backstep / rewind / over-credit episodes (with the polygon,
		// direction, velocities and the surfaces being touched) to GhostDesyncLog_<date>.jsonl next to the module
		std::atomic_bool ghostLog{ false };
		// ---- ghosting desync debug view (Home / End, see dllmain.cpp) ----
		std::atomic_bool ghost{ false };                // master toggle [Home]
		std::atomic_bool ghostPrev{ true };             // (b) Havok step-start pose (motion PREV)
		std::atomic_bool ghostHalo{ true };             // (c) Halo's own object position
		std::atomic_bool ghostTrail{ true };            // (d) last N ticks of Havok poses            [End]
		std::atomic_bool ghostRewind{ true };           // (e/g) backstep rewind target + pre-rewind pose
		std::atomic_bool ghostJump{ true };             // Halo 3 / ODST: last jump (com0 != previous com1) markers
		std::atomic_bool ghostBelieved{ true };         // (f) poses the over-credited TIMs believe     [Shift+Home]
		// (f2) the same for OBJECTS: baked scenery children of the level welder (key categories 3 / 4) and the subject's pairs
		// against other rigid bodies (crates, machines, vehicles, dynamic scenery, other bipeds; biped subject only).
		// Also feeds the lime state, the desync edges and the logger while on.
		std::atomic_bool ghostBelievedObjects{ true };
		std::atomic_bool ghostLinkLabels{ false };       // label each believed-pose link line with its length (= the over-credit, wu)
		std::atomic_bool ghostPass{ true };              // flash (green) welded polygons / object pieces the current offset passes through
		// weld zones (Halo 2, biped): for each polygon / object, the box the player's welder query-box CENTRE has to enter
		// for that polygon to get a welder child (= its bounds grown by the half-extents of the welder's stored query box).
		// Near = every level polygon / rigid body whose zone is within weldZoneRange of that centre; Welded = only the
		// ones that currently have a child (polygons, baked scenery) or a pair (rigid bodies).
		// ghost travel region: for each over-credited level polygon, where the body can go without ending the over-credit
		std::atomic_bool ghostTravel{ false };          // level polygons
		std::atomic_bool ghostTravelObjects{ false };   // objects: rigid-body pairs and baked scenery
		std::atomic_bool weldZonesNear{ false };
		std::atomic_bool weldZonesWelded{ false };
		std::atomic<float> weldZoneRange{ 1.0f };
		std::atomic_bool ghostEdges{ true };            // lime/red edges of skipped / over-credited polygons [Shift+End]
		std::atomic_bool ghostEdgesSuspectOnly{ false };// false = every skipped child (TIM>0), true = only stale / over-credited ones
		std::atomic_bool ghostStrictSkip{ false };      // true = ANY skipped child turns the capsule lime (very common on floors)
		std::atomic_int ghostTrailLen{ 8 };             // ticks
		std::atomic_int ghostHold{ 8 };                 // ticks a backstep keeps the capsule lime / the rewind target visible
		std::atomic_int ghostBelievedMax{ 6 };          // max believed-pose capsules per player
		std::atomic<float> ghostPrevMinDelta{ 0.001f }; // wu: draw (b) only when it differs from the current pose by more than this
		std::atomic<float> ghostHaloMinDelta{ 0.0005f };// wu: draw (c) only when Halo's position differs from Havok's by more than this
		std::atomic<float> ghostOverMin{ 0.005f };      // wu of TIM over-credit that counts as "suspect"
		std::atomic_bool ghostShowReal{ true };         // draw (a), the capsule where Havok really is (off = only the desync poses)
		std::atomic_bool ghostVehicles{ true };         // seated in a vehicle (any seat): show the vehicle's Havok body instead of the biped's
		// capsule colours, packed like rgba() (r | g<<8 | b<<16 | a<<24)
		std::atomic<uint32_t> ghostColNormal{ 0xFFB469FFu };   // (a) no backstep / nothing suspect  (pink 255,105,180)
		std::atomic<uint32_t> ghostColDesync{ 0xFF00FFAAu };   // (a) backstep / suspect child          (lime 170,255,0)
		std::atomic<uint32_t> ghostColBelieved{ 0xFF00FFAAu }; // (f) believed poses                   (lime)
	};

	class Viewer
	{
	public:
		Settings settings;

		// ---- ghost desync LOGGER (render thread only; JSON lines next to the module, see logEvent) ----
		struct LogKey { uint64_t k; bool operator==(const LogKey& o) const { return k == o.k; } };
		struct LogState { uint32_t startTick = 0, lastFrame = 0; float peakOver = 0.f; std::string startRec; };
		std::vector<std::pair<uint64_t, LogState>> mLogActive;   // over-credit episodes in progress
		FILE* mLogFile = nullptr;
		std::string mLogPath;
		uint32_t mLogFrame = 0, mLogLines = 0;
		bool logEnsureOpen();
		void logClose();
		void logEvent(const std::string& json);

		// called from HCM's ForegroundDirectXRenderEvent: inside Present, ImGui frame open, HCM's UI not drawn yet
		void onRender(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h);
		void onFirstPersonPass(); // engine hook: entry of sub_1807E0C60 (render thread)
		// Halo 3 / ODST: engine hook at the entry of the first-person pass (view = rcx, opaque = dl == 0; render thread) and the
		// per-tick callback from the simulation thread (HCM's game tick event) that samples the player's Havok state
		void onFirstPersonPass3(uintptr_t view, bool opaquePass);
		void onSimTick(uint32_t tick);
		void onResolveDone();     // engine hook: exit of sub_180951EC0 (bloom + composite done, HUD not yet)
		void restoreBloom();      // put the game's bloom gate back (unload / mode change)
		void beforeResize();
		void shutdown(); // render thread must be stopped first

		// "game depth" input set by the engine hooks (see EngineDepth)
		void setGameDepth(const GameDepthInput& g) { mGame = g; }

	private:
		bool initDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx);
		void drawCollision(const h2::Vec3& camPos, bool depthTest);
		void drawTim();
		void drawPills();
		void drawGhost();
		void drawWeldZones();
		void drawHud(const h2::Camera& cam);

		// ---- Halo 3 / ODST (Viewer3.cpp) ----
		void drawTim3();
		void drawPills3();
		void drawGhost3();
		void drawHud3();
		void bipedOccluders3(std::vector<Tri>& caps, const h2::Vec3& camPos);
		h2::Camera camera3();
		void pumpGhost3();                     // move new per-tick samples (sim thread queue, else a live sample) into the analysis state
		struct Frame3 { bool have = false; h2::GameFrame f; D3D11_VIEWPORT vp{}; uint32_t presentIdx = 0; uint32_t user = 0; };
		Frame3 mF3;
		bool mLogged3 = false; uint32_t mBad3 = 0; std::string mNote3;
		std::mutex mSimMu;
		std::deque<h3::Ghost3> mSimQ;          // per-tick samples of user 0 (sim thread pushes, render thread consumes)
		std::atomic<uint64_t> mSimLastMs{ 0 };
		std::atomic_bool mWantSim3{ false };
		struct Sample3 { uint32_t tick = 0; h2::Vec3 com0{}, com1{}; h2::Xf pose; };
		struct GhostPlayer3
		{
			uintptr_t body = 0; uint32_t lastTick = 0; bool have = false;
			h2::Vec3 lastCom1{}, lastHalo{}; bool lastHaloOk = false;
			std::deque<Sample3> trail;
			bool haveJump = false; uint32_t jumpTick = 0; h2::Vec3 jumpFrom{}, jumpTo{}; float jumpDist = 0; bool jumpHalo = false; uint32_t jumps = 0;
			bool havePair = false; bool pairDumped = false;
			h3::Ghost3 latest; bool haveLatest = false;
		};
		GhostPlayer3 mGP3;
		struct Row3 { uint32_t key; int cat; int bsp; uint32_t inst, surf; float w, trueDist, over; int age; bool hasTim, skipped, touching, stale; };
		struct GhostInfo3
		{
			bool valid = false; std::string note, detail;
			int pairs = 0, kids = 0, tims = 0, skipped = 0, touchSkipped = 0, overcredited = 0, believed = 0;
			float maxOver = 0, tol = 0, dHalo = 0, stepMove = 0; bool lime = false, red = false;
			bool layoutOk = false; int bsp = -1; uint32_t sims = 0;
		};
		GhostInfo3 mGI3;
		std::vector<Row3> mRows3;
		// per-child analysis of the latest sample (Viewer3.cpp analyse3): polygon, true gap, over-credit / stale flags
		struct An3
		{
			const h3::Pair3* pair = nullptr; const h3::Tim3* k = nullptr;
			int cat = -1; uint32_t inst = 0, surf = 0; int bsp = -1;
			bool havePoly = false; std::vector<h2::Vec3> poly;
			float trueDist = 1e9f, over = 0; bool validTime = false, skipped = false, touching = false, stale = false, overcredit = false; int age = 0;
		};
		std::vector<An3> mAn3;
		uint32_t mPumped3 = 0;
		void analyse3();

		ID3D11Device* mDev = nullptr;
		ID3D11DeviceContext* mCtx = nullptr;
		ID3D11RenderTargetView* mRtv = nullptr;
		uint32_t mW = 0, mH = 0;
		bool mImgui = false;
		Renderer mR;
		CollisionWorld mWorld;
		GameDepthInput mGame;
		std::vector<LineVertex> mScratch;
		uint64_t mLastTime = 0;

		// ghost debug state, one slot per local user 0..3
		struct GhostSample { uint32_t tick = 0; h2::Vec3 cur{}, prev{}; int32_t S = 0; h2::Xf pose; };
		struct GhostPlayer
		{
			uintptr_t obj = 0; uint32_t lastTick = 0; bool have = false;
			std::deque<GhostSample> trail;
			uint32_t lastBackstepTick = 0; bool everBackstep = false; int32_t prevS = 0;
			bool haveRewind = false; uint32_t rewindTick = 0; h2::Vec3 rewindFrom{}, rewindTo{}; float rewindDist = 0, tStar = 0;
		};
		struct GhostInfo
		{
			bool valid = false; int user = 0; std::string note;
			std::string subject = "biped", detail; bool vehicle = false; int nPrims = 0, nWelders = 0;
			bool pairFound = false, welder = false; int32_t S = 0;
			int nChildren = 0, nSkipped = 0, nSuspect = 0; float maxOver = 0;
			float dOrigin = 0, dCom = 0, comOff = 0; bool haloRefCom = false; float prevDelta = 0;
			float rewindDist = 0, tStar = 0; int rewindAge = -1;
			bool lime = false, hold = false; int nBelieved = 0;
			std::vector<std::string> travel;   // ghost travel region rows for the info panel (level polygons)
			std::vector<std::string> travelObj; // ... objects
			std::vector<std::string> objPairRows, machineRows;   // object pair / nearby machine diagnostics
			float tolerance = 0;
			// objects (ghostBelievedObjects)
			bool objScanned = false;
			int nObjPairs = 0, nObjBaked = 0, nObjBakedFail = 0, nObjLeaves = 0, nObjTims = 0, nObjSkipped = 0, nObjSuspect = 0, nObjUnsupported = 0, nObjBelieved = 0;
			float maxObjOver = 0;
			std::string objNote;
			// pass-through prediction (ghostPass)
			int nPassLevel = 0, nPassObj = 0; float passBest = 0;
			std::vector<std::string> passRows;
		};
		GhostPlayer mGP[4];
		GhostInfo mGI[4];
		// pass-through prediction: the side each TIM was first seen from (kept while it lives, so the normals do not flip
		// while the player passes through); key = (user << 40) | (object << 39) | key/id, value = (side, last frame seen)
		std::vector<std::pair<uint64_t, std::pair<int, uint32_t>>> mPassSide;
		int passSide(uint64_t key, bool frontNow)
		{
			for (auto& e : mPassSide) if (e.first == key) { e.second.second = mLogFrame; return e.second.first; }
			mPassSide.push_back({ key, { frontNow ? 1 : -1, mLogFrame } });
			return frontNow ? 1 : -1;
		}
		// parsed collision_bsps reached through object welders (Believed poses - objects), by collision_bsp address;
		// dropped whenever the structure bsp changes (tag data moves on a map / bsp load)
		std::unordered_map<uintptr_t, CollBsp> mObjBsp;
		uintptr_t mObjBspSbsp = 0;
		// weld zones: AABB of every level collision polygon (world + instanced), keyed like the welder's child keys,
		// built once per structure bsp load
		struct WeldPoly { h2::Vec3 mn, mx; uint32_t key; };
		std::vector<WeldPoly> mWeldPolys;
		uintptr_t mWeldPolysSbsp = 0;
		struct WeldStats
		{
			bool valid = false; int user = -1;
			int welded = 0, weldedObjects = 0, nearPolys = 0, nearObjects = 0, drawn = 0; bool capped = false;
			h2::Vec3 boxSize{}; std::string note;
		};
		WeldStats mWeld;
		// per local user: your broadphase margin around the capsule, measured only while you stand still (moving stretches
		// the broadphase box by your motion) and kept for the rest of the bsp; 0.5 x tolerance until the first sample
		float mBpMargin[4][3] = {};
		bool mBpMarginOk[4] = {};
		uintptr_t mBpMarginSbsp = 0;
		h2::Vec3 bpRestMargin(int user, uintptr_t ent, const h2::Vec3& capMn, const h2::Vec3& capMx);

		// HUD stats
		struct TimRow { uint32_t key; int cat; uint32_t inst, surf; float tim; int manifold; uint8_t kind; float trueDist; };
		std::vector<TimRow> mTimRows;
		int32_t mPairS = 0; bool mPairFound = false; bool mPairWelder = false; uint32_t mTick = 0;
		std::string mStatus;
		// in-frame bookkeeping
		uint32_t mPresentCount = 0, mInFrameCount = 0, mLastInFramePresent = 0;
		bool mInFrameThisFrame = false;
		std::string mInFrameNote;
		float mLabelScaleX = 1.f, mLabelScaleY = 1.f;
		// post-bloom: per-view snapshot taken at the first-person pass, consumed after the resolve
		bool mPbPending = false; h2::GameFrame mPbFrame{}; D3D11_VIEWPORT mPbVp{};
		// bloom gate dword_180E19438 (single reader in sub_180951EC0; 0 = bloom on)
		bool mBloomTouched = false; uint32_t mBloomOrig = 0;
		bool drawViewInto(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, const h2::GameFrame& f, const D3D11_VIEWPORT& vp);
		void setBloomSuppressed(bool off);
	};
}
