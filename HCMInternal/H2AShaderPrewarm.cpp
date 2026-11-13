#include "pch.h"
#include "H2AShaderPrewarm.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "DirPathContainer.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <format>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstring>

// ============================================================================
//  Halo 2 Anniversary-graphics shader pre-warm (MCC halo2.dll, build 1.3528).
//  The RE behind every offset below is in
//  Documents\Halo Mod And Tools\H2A Stutter Research\ (out_variant_api.txt, out_create.txt, out_purge*.txt).
//
//  Shader manager  = *(halo2 + 0x1E8D510)
//    +0      CRITICAL_SECTION (the "manager lock"), +40 = its lazily-set "initialised" byte
//    +104    40 x { prebuilt cache*, local cache*, 16 unused bytes }   (indexed by shader type)
//    +1368   manager frame number (bumped by sub_180115AF0)
//    +1496   compile context (only read on the compile-from-source path)
//    +1704   compiler object* (vtbl+8 compile from HLSL, vtbl+0x10 create the D3D11 shader, vtbl+0x18 release)
//    +411336 variant table:
//              4096 slots x 56 bytes, slot i at table + 56*i:
//                +8 key (32 bytes, the define mask), +40 shader object*, +48 last-used manager frame,
//                +52 local-cache index, +56 shader type (40 = unused slot), +57 stage (0 VS,1 PS,2 GS,3 CS,4 DS,5 HS)
//              +229384 number of live slots (then the sorted index), +237580 number of free slots
//              +0xAC024 the lock every variant creation is serialised by
//  sub_180118650(table, type, stage, key*, frame, localCache)                     -> slot index or -1 (takes no lock;
//                                                                                    the game calls it under the manager lock)
//  sub_180118C40(table, slot, compiler, prebuilt, localCache, frame, compileCtx)  -> 1 when the slot has a shader
//  sub_180115AF0()  once per rendered frame: evicts least-recently-used variants while fewer than 1024 slots are
//                   free, then frame number += 1. THIS is what we hook.
// ============================================================================

namespace
{
	constexpr uint32_t kFrameEndRva = 0x115AF0;
	constexpr uint32_t kFindSlotRva = 0x118650;
	constexpr uint32_t kMakeVariantRva = 0x118C40;
	constexpr uint32_t kModeTickRva = 0x511E0;      // sub_1800511E0: the game's own graphics-mode handler (called by sub_180068780)
	constexpr uint32_t kShaderMgrRva = 0x1E8D510;
	constexpr uint32_t kTypeTableRva = 0x1A24E70;    // 40 shader-type descriptors (pointers); the key masks live in them
	constexpr uint32_t kSaberSceneRva = 0x1A24FE0;   // Anniversary renderer scene object; +564 = its frame counter
	constexpr uint32_t kModeCurrentRva = 0xE21280;   // graphics mode in effect: 0 = Classic
	constexpr uint32_t kDeviceHolderRva = 0x1E8D4E8; // +29968 = ID3D11Device*

	constexpr size_t kSaberFrame = 564;
	constexpr size_t kDeviceOffset = 29968;
	constexpr size_t kMgrLockInit = 40;
	constexpr size_t kMgrCaches = 104;
	constexpr size_t kMgrCompileCtx = 1496;
	constexpr size_t kMgrCompiler = 1704;
	constexpr size_t kMgrTable = 411336;
	constexpr size_t kSlotSize = 56;
	constexpr size_t kSlotKey = 8, kSlotObject = 40, kSlotLastUsed = 48, kSlotType = 56, kSlotStage = 57;
	constexpr int    kSlotCount = 4096;
	constexpr size_t kTblLive = 229384;
	constexpr size_t kTblFree = 237580;
	constexpr int    kTypeCount = 40;
	constexpr int    kStageCount = 6;

	// The game evicts while fewer than 1024 slots are free. Stop pre-warming well above that so play still has room
	// to add variants without eviction churn.
	constexpr int kMinFreeSlots = 1400;
	// A scene whose frame counter is below this is still on (or just leaving) its loading screen: do the whole list
	// in one go there. Later than that (toggle switched on mid-level, or the list was thrown away by a stay in
	// Classic graphics) it is spread out instead.
	constexpr int kBlockingFrames = 600;
	constexpr double kBlockingBudgetMs = 20000.0; // hard stop for the one-go pass; the rest continues spread out
	constexpr double kSlicedBudgetMs = 2.0;       // per frame; one variant can overrun it (they cost 1-36 ms each)
	constexpr double kClassicBudgetMs = 3.0;      // per frame while Classic graphics is showing (nobody is looking at this renderer)
	constexpr ULONGLONG kFrameHookAliveMs = 500;  // the renderer's frame hook fired this recently = it is running
	constexpr ULONGLONG kClassicCheckMs = 500;    // how often the Classic path looks at the table
	constexpr ULONGLONG kClassicRetryMs = 5000;   // and how long it waits after a pass before starting another
	constexpr int kScanEveryFrames = 30;
	constexpr ULONGLONG kSaveEveryMs = 15000;
	constexpr uint32_t kForgetAfterSessions = 8;  // a variant no session of the level has drawn for this long is dropped

	const uint8_t kFrameEndBytes[] = { 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48 };
	const uint8_t kModeTickBytes[] = { 0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x30 };
	const uint8_t kFindSlotBytes[] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
	const uint8_t kMakeVariantBytes[] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C };

	using FindSlotFn = int64_t(__fastcall*)(uint8_t* table, uint8_t type, uint8_t stage, const void* key, int frame, void* localCache);
	using MakeVariantFn = char(__fastcall*)(uint8_t* table, int16_t slot, void* compiler, void* prebuilt, void* localCache, int frame, void* compileCtx);

	bool entryBytesMatch(uintptr_t at, const uint8_t* expect, size_t n)
	{
		uint8_t got[32] = {};
		if (n > sizeof(got)) return false;
		__try { memcpy(got, (const void*)at, n); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		return memcmp(got, expect, n) == 0;
	}

	// PE TimeDateStamp + SizeOfImage: the recorded keys are only meaningful for the build they were recorded on.
	bool imageIdentity(uintptr_t base, uint32_t& timestamp, uint32_t& size)
	{
		__try
		{
			const auto dos = (const IMAGE_DOS_HEADER*)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
			const auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
			timestamp = nt->FileHeader.TimeDateStamp;
			size = nt->OptionalHeader.SizeOfImage;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	double qpcMs()
	{
		static const double msPerTick = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / (double)f.QuadPart; }();
		LARGE_INTEGER c; QueryPerformanceCounter(&c);
		return (double)c.QuadPart * msPerTick;
	}

#pragma pack(push, 1)
	struct VariantKey
	{
		uint8_t type;
		uint8_t stage;
		uint8_t key[32];
		bool operator==(const VariantKey& o) const { return memcmp(this, &o, sizeof(VariantKey)) == 0; }
	};
	struct FileHeader
	{
		char magic[8];           // "H2ASWRM1"
		uint32_t imageTimestamp; // halo2.dll the keys were recorded on
		uint32_t imageSize;
		uint32_t sessions;       // how many sessions of this level have recorded so far
		uint32_t count;
	};
	struct FileEntry
	{
		VariantKey key;
		uint16_t seen;           // sessions this variant was drawn in (saturating)
		uint32_t lastSession;    // the latest of them
	};
#pragma pack(pop)
	static_assert(sizeof(VariantKey) == 34);
	static_assert(sizeof(FileEntry) == 40);
	const char kMagic[8] = { 'H', '2', 'A', 'S', 'W', 'R', 'M', '1' };

	struct VariantKeyHash
	{
		size_t operator()(const VariantKey& k) const noexcept
		{
			uint64_t h = 1469598103934665603ull; // FNV-1a
			const auto p = (const uint8_t*)&k;
			for (size_t i = 0; i < sizeof(VariantKey); ++i) { h ^= p[i]; h *= 1099511628211ull; }
			return (size_t)h;
		}
	};
	struct VariantInfo
	{
		uint16_t seen = 0;
		uint32_t lastSession = 0;
		bool thisSession = false;
	};
}

class H2AShaderPrewarm::Impl
{
private:
	GameState mGame;
	std::filesystem::path mDir;

	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;

	std::unique_ptr<ModuleInlineHook> mHook;
	std::unique_ptr<ModuleInlineHook> mModeHook;                 // Classic-time hook (see modeTickDetour)
	std::atomic<bool> mIngame{ false };                          // MCC says the level has finished loading
	std::atomic<ULONGLONG> mLastFrameHookTick{ 0 };              // GetTickCount64 of the last frameEndDetour call
	std::atomic<bool> mEnabled{ false };
	std::atomic<bool> mArmed{ false };
	bool mArmRefused = false;      // entry bytes differ: do not retry (and do not repeat the message)
	std::atomic<int> mLevel{ -1 }; // the level MCC is running (LevelID as a number), -1 = not in a Halo 2 level

	static inline std::atomic<Impl*> sSelf{ nullptr };
	static inline std::atomic<int> sInFlight{ 0 };

	// ---- everything below is only touched with mStateMutex held (the frame hook, toggle-off and the destructor) ----
	std::mutex mStateMutex;
	uint8_t* mLastSaber = nullptr;
	int mLastFrame = -1;
	int mLastLive = 0;
	int mSetLevel = -2;        // the level mSet belongs to
	bool mRecordHold = false;  // the level number changed but the old scene is still up: its variants are not the new level's
	uint32_t mTick = 0;
	ULONGLONG mNextClassicCheck = 0;
	ULONGLONG mLastClassicPass = 0;
	bool mLoggedFrameHookInClassic = false, mLoggedModeTickInClassic = false;

	std::unordered_map<VariantKey, VariantInfo, VariantKeyHash> mSet;
	uint32_t mSessions = 0;
	bool mSessionCounted = false;
	bool mDirty = false;
	ULONGLONG mLastSaveTick = 0;
	uint32_t mImageTimestamp = 0, mImageSize = 0;

	std::vector<VariantKey> mQueue;
	size_t mQueuePos = 0;
	bool mBlocking = false;
	struct Stats { int created = 0, already = 0, failed = 0, skipped = 0; double ms = 0, slowestMs = 0; bool outOfRoom = false; bool wasBlocking = false; bool classic = false; } mStats;

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;

	void say(const std::string& text)
	{
		PLOG_INFO << "[H2AShaderPrewarm] " << text;
		if (auto m = messagesGUIWeak.lock()) m->addMessage(text);
	}

	// ---------------------------------------------------------------- hook ----
	static void __fastcall frameEndDetour()
	{
		Impl* self = sSelf.load(std::memory_order_acquire);
		if (!self) return; // the destructor detaches the hook before it clears sSelf; guard only
		sInFlight.fetch_add(1, std::memory_order_acq_rel);
		self->mHook->getInlineHook().call<void>(); // the game's own eviction + frame bump first
		if (self->mEnabled.load(std::memory_order_relaxed)) self->onFrame();
		sInFlight.fetch_sub(1, std::memory_order_acq_rel);
	}

	// The game's graphics-mode handler: called by its per-frame update whichever graphics are showing. It is only a
	// place to stand while Classic is showing and the Anniversary renderer's own frame hook is silent, so that a
	// level STARTED in Classic can still have its shaders made ahead of the first switch to Anniversary - that first
	// switch is otherwise where every visible shader gets created at once (the long freeze).
	static void* __fastcall modeTickDetour()
	{
		Impl* self = sSelf.load(std::memory_order_acquire);
		if (!self) return nullptr; // guard only, as above
		sInFlight.fetch_add(1, std::memory_order_acq_rel);
		void* const result = self->mModeHook->getInlineHook().call<void*>();
		if (self->mEnabled.load(std::memory_order_relaxed)) self->onModeTick();
		sInFlight.fetch_sub(1, std::memory_order_acq_rel);
		return result;
	}

	// Arm only once halo2.dll is present and the three functions start with the bytes the RE was done on.
	bool tryArm()
	{
		if (mArmed.load()) return true;
		if (mArmRefused) return false;
		const uintptr_t base = (uintptr_t)GetModuleHandleW(L"halo2.dll");
		if (!base) return false; // not loaded yet: retried on the next MCC state change
		const bool ok = entryBytesMatch(base + kFrameEndRva, kFrameEndBytes, sizeof(kFrameEndBytes))
			&& entryBytesMatch(base + kFindSlotRva, kFindSlotBytes, sizeof(kFindSlotBytes))
			&& entryBytesMatch(base + kMakeVariantRva, kMakeVariantBytes, sizeof(kMakeVariantBytes));
		if (!ok)
		{
			mArmRefused = true;
			say("H2A Shader Pre-warm: this halo2.dll is not the 1.3528 build it was made for (or another tool hooked the same function) - not active");
			return false;
		}
		// Attached once and left attached: detaching an inline hook while the render thread is inside it races.
		// With the toggle off the detour only calls the original.
		mHook->setWantsToBeAttached(true);
		if (!mHook->isHookInstalled())
		{
			PLOG_ERROR << "[H2AShaderPrewarm] hook did not install";
			return false;
		}
		// The Classic-time hook is optional: without it only levels started in Classic lose out.
		if (entryBytesMatch(base + kModeTickRva, kModeTickBytes, sizeof(kModeTickBytes)))
		{
			mModeHook->setWantsToBeAttached(true);
			PLOG_INFO << "[H2AShaderPrewarm] mode hook " << (mModeHook->isHookInstalled() ? "armed" : "did not install");
		}
		else
			PLOG_ERROR << "[H2AShaderPrewarm] mode handler entry bytes differ - levels started in Classic will not be pre-warmed";
		mArmed = true;
		PLOG_INFO << "[H2AShaderPrewarm] frame hook armed";
		return true;
	}

	// ---------------------------------------------------------------- files ----
	std::filesystem::path fileFor(int level) const
	{
		return mDir / std::format("level_{:03}.bin", level);
	}

	// mStateMutex held
	void loadSet(int level)
	{
		mSet.clear();
		mSessions = 0;
		mSessionCounted = false;
		mDirty = false;
		std::error_code ec;
		const auto path = fileFor(level);
		if (!std::filesystem::exists(path, ec)) return;
		std::ifstream f(path, std::ios::binary);
		FileHeader h{};
		if (!f.read((char*)&h, sizeof(h)) || memcmp(h.magic, kMagic, sizeof(kMagic)) != 0 || h.count > 200000) return;
		if (h.imageTimestamp != mImageTimestamp || h.imageSize != mImageSize)
		{
			PLOG_INFO << "[H2AShaderPrewarm] " << path.filename().string() << " was recorded on another halo2.dll build - starting it again";
			return;
		}
		std::vector<FileEntry> entries(h.count);
		if (h.count && !f.read((char*)entries.data(), (std::streamsize)(sizeof(FileEntry) * h.count))) return;
		mSessions = h.sessions;
		mSet.reserve(h.count * 2);
		for (const auto& e : entries)
		{
			if (e.key.type >= kTypeCount || e.key.stage >= kStageCount) continue;
			mSet[e.key] = VariantInfo{ e.seen, e.lastSession, false };
		}
	}

	// mStateMutex held
	void saveSet()
	{
		if (!mDirty || mSetLevel < 0) return;
		mDirty = false;
		mLastSaveTick = GetTickCount64();
		std::vector<FileEntry> entries;
		entries.reserve(mSet.size());
		for (const auto& [k, v] : mSet)
		{
			if (mSessions - v.lastSession > kForgetAfterSessions) continue;
			entries.push_back(FileEntry{ k, v.seen, v.lastSession });
		}
		FileHeader h{};
		memcpy(h.magic, kMagic, sizeof(kMagic));
		h.imageTimestamp = mImageTimestamp;
		h.imageSize = mImageSize;
		h.sessions = mSessions;
		h.count = (uint32_t)entries.size();

		std::error_code ec;
		std::filesystem::create_directories(mDir, ec);
		const auto path = fileFor(mSetLevel);
		auto tmp = path; tmp += L".tmp";
		{
			std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
			if (!f) { PLOG_ERROR << "[H2AShaderPrewarm] cannot write " << tmp.string(); return; }
			f.write((const char*)&h, sizeof(h));
			if (!entries.empty()) f.write((const char*)entries.data(), (std::streamsize)(sizeof(FileEntry) * entries.size()));
			if (!f) { PLOG_ERROR << "[H2AShaderPrewarm] write failed " << tmp.string(); return; }
		}
		if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
			PLOG_ERROR << "[H2AShaderPrewarm] could not replace " << path.string() << " (error " << GetLastError() << ")";
	}

	// ---------------------------------------------------------------- per frame (render thread) ----
	// mStateMutex held. Queue every recorded variant of the level, most recently / most often drawn first.
	void queueSet(bool blocking)
	{
		mQueue.clear();
		mQueuePos = 0;
		mStats = Stats{};
		mBlocking = blocking;
		mStats.wasBlocking = blocking;
		if (mSet.empty()) return;
		std::vector<std::pair<VariantKey, VariantInfo>> order(mSet.begin(), mSet.end());
		std::sort(order.begin(), order.end(), [](const auto& a, const auto& b)
			{
				if (a.second.lastSession != b.second.lastSession) return a.second.lastSession > b.second.lastSession;
				return a.second.seen > b.second.seen;
			});
		mQueue.reserve(order.size());
		for (const auto& o : order) mQueue.push_back(o.first);
	}

	// mStateMutex held
	void runQueue(uintptr_t base, uint8_t* mgr, double budgetMs)
	{
		uint8_t* const table = mgr + kMgrTable;
		void* const compiler = *(void**)(mgr + kMgrCompiler);
		uint8_t* const holder = *(uint8_t**)(base + kDeviceHolderRva);
		if (!compiler || !holder || !*(void**)(holder + kDeviceOffset)) return; // renderer not up yet: try next frame
		const auto findSlot = (FindSlotFn)(base + kFindSlotRva);
		const auto makeVariant = (MakeVariantFn)(base + kMakeVariantRva);
		const auto lock = (CRITICAL_SECTION*)mgr;

		const double t0 = qpcMs();
		while (mQueuePos < mQueue.size())
		{
			if (*(int*)(table + kTblFree) <= kMinFreeSlots)
			{
				mStats.outOfRoom = true;
				mStats.skipped += (int)(mQueue.size() - mQueuePos);
				mQueuePos = mQueue.size();
				break;
			}
			const VariantKey& k = mQueue[mQueuePos++];
			if (k.type >= kTypeCount || k.stage >= kStageCount) { ++mStats.skipped; continue; }
			void* const descriptor = *(void**)(base + kTypeTableRva + 8ull * k.type);
			void* const prebuilt = *(void**)(mgr + kMgrCaches + 32ull * k.type);
			void* const local = *(void**)(mgr + kMgrCaches + 8 + 32ull * k.type);
			// the game's two cache lookups dereference both cache objects without a check, and "find" reads the
			// key mask out of the type descriptor: a type that lacks any of them is not one this session can make
			if (!descriptor || !prebuilt || !local) { ++mStats.skipped; continue; }

			alignas(16) uint8_t key[32];
			memcpy(key, k.key, sizeof(key));
			const double a = qpcMs();
			int outcome = 0; // 0 failed, 1 was already there, 2 created
			// The manager lock is held across BOTH calls: the game's purge and eviction take it first, so neither
			// can free the slot between "find" and "make". Frame number 0 = "never drawn": it lets the recorder tell
			// a variant the level really used from one only we made, and makes ours the first the game evicts.
			EnterCriticalSection(lock);
			const int16_t slot = (int16_t)findSlot(table, k.type, k.stage, key, 0, local);
			if (slot >= 0 && slot < kSlotCount)
			{
				const bool had = *(void**)(table + kSlotSize * slot + kSlotObject) != nullptr;
				const bool ok = makeVariant(table, slot, compiler, prebuilt, local, 0, mgr + kMgrCompileCtx) != 0;
				outcome = !ok ? 0 : (had ? 1 : 2);
			}
			LeaveCriticalSection(lock);
			const double now = qpcMs();
			if (outcome == 2) { ++mStats.created; if (now - a > mStats.slowestMs) mStats.slowestMs = now - a; }
			else if (outcome == 1) ++mStats.already;
			else ++mStats.failed;
			if (now - t0 >= budgetMs) { mStats.ms += now - t0; if (mBlocking) mBlocking = false; return; }
		}
		mStats.ms += qpcMs() - t0;

		// finished
		const Stats s = mStats;
		mQueue.clear();
		mQueuePos = 0;
		mBlocking = false;
		if (s.created + s.failed > 0 || s.outOfRoom)
		{
			std::string text = std::format("H2A Shader Pre-warm: {} shaders ready in {:.1f} s{}", s.created, s.ms / 1000.0, s.classic ? " (in the background, while in Classic)" : s.wasBlocking ? "" : " (spread over frames)");
			if (s.failed) text += std::format(", {} failed", s.failed);
			if (s.outOfRoom) text += std::format(", {} left out (table nearly full)", s.skipped);
			say(text);
			PLOG_INFO << "[H2AShaderPrewarm] already loaded " << s.already << ", slowest " << s.slowestMs << " ms";
		}
	}

	// mStateMutex held. Record every variant the level has actually drawn (last-used frame != 0).
	void scan(uint8_t* mgr)
	{
		uint8_t* const table = mgr + kMgrTable;
		const auto lock = (CRITICAL_SECTION*)mgr;
		VariantKey found[256];
		int i = 0;
		while (i < kSlotCount)
		{
			int n = 0;
			// keys are written under the manager lock, so hold it while reading them (in short runs)
			EnterCriticalSection(lock);
			for (; i < kSlotCount && n < 256; ++i)
			{
				const uint8_t* slot = table + kSlotSize * i;
				if (!*(void* const*)(slot + kSlotObject) || !*(const uint32_t*)(slot + kSlotLastUsed)) continue;
				const uint8_t type = slot[kSlotType], stage = slot[kSlotStage];
				if (type >= kTypeCount || stage >= kStageCount) continue;
				found[n].type = type;
				found[n].stage = stage;
				memcpy(found[n].key, slot + kSlotKey, 32);
				++n;
			}
			LeaveCriticalSection(lock);
			for (int j = 0; j < n; ++j)
			{
				VariantInfo& v = mSet[found[j]];
				if (v.thisSession) continue;
				if (!mSessionCounted) { mSessionCounted = true; ++mSessions; }
				v.thisSession = true;
				v.lastSession = mSessions;
				if (v.seen < 0xFFFF) ++v.seen;
				mDirty = true;
			}
		}
	}

	// mStateMutex held. Switch mSet to this level's list (saving the previous level's first).
	void ensureSet(uintptr_t base, int level)
	{
		if (level == mSetLevel) return;
		saveSet();
		if (!mImageTimestamp) imageIdentity(base, mImageTimestamp, mImageSize);
		mSetLevel = level;
		loadSet(level);
	}

	// mStateMutex held. Classic graphics is showing. If the level is in play and far fewer variants exist than its
	// list holds (the level was started in Classic, so the Anniversary renderer has drawn nothing yet), make them a
	// few milliseconds per frame. Nothing is recorded here: nothing is being drawn by this renderer.
	void onClassic(uintptr_t base, uint8_t* mgr)
	{
		if (!mIngame.load(std::memory_order_relaxed)) return; // still loading: the table may yet be emptied
		const int level = mLevel.load(std::memory_order_relaxed);
		if (level < 0 || !*(uint8_t**)(base + kSaberSceneRva)) return;
		const ULONGLONG now = GetTickCount64();
		if (mQueue.empty())
		{
			if (now < mNextClassicCheck) return;
			mNextClassicCheck = now + kClassicCheckMs;
			ensureSet(base, level);
			const int live = *(int*)(mgr + kMgrTable + kTblLive);
			if (mSet.empty() || live * 2 >= (int)mSet.size() || now - mLastClassicPass < kClassicRetryMs) return;
			queueSet(false);
			mStats.classic = true;
			mLastClassicPass = now;
			PLOG_INFO << "[H2AShaderPrewarm] level " << level << " in Classic: " << live << " variants exist, " << mSet.size() << " recorded - making them in the background";
		}
		runQueue(base, mgr, kClassicBudgetMs);
		if (mQueue.empty()) mLastClassicPass = GetTickCount64();
	}

	// From the mode-handler hook. Does nothing unless Classic is showing AND the renderer's frame hook is silent
	// (if that hook does fire in Classic, it does the same work from there).
	void onModeTick() noexcept
	{
		const ULONGLONG now = GetTickCount64();
		if (now - mLastFrameHookTick.load(std::memory_order_relaxed) < kFrameHookAliveMs) return;
		const uintptr_t base = (uintptr_t)GetModuleHandleW(L"halo2.dll");
		if (!base || *(int*)(base + kModeCurrentRva) != 0) return;
		std::unique_lock lk(mStateMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		try
		{
			uint8_t* const mgr = *(uint8_t**)(base + kShaderMgrRva);
			if (!mgr || !mgr[kMgrLockInit]) return;
			if (!mLoggedModeTickInClassic)
			{
				mLoggedModeTickInClassic = true;
				PLOG_INFO << "[H2AShaderPrewarm] Classic is showing and the renderer's frame hook is silent - the mode hook stands in (thread " << GetCurrentThreadId() << ")";
			}
			onClassic(base, mgr);
		}
		catch (...)
		{
			mEnabled = false;
			PLOG_ERROR << "[H2AShaderPrewarm] unexpected exception in the mode hook - switched off for this session";
		}
	}

	void onFrame() noexcept
	{
		mLastFrameHookTick.store(GetTickCount64(), std::memory_order_relaxed);
		std::unique_lock lk(mStateMutex, std::try_to_lock);
		if (!lk.owns_lock()) return;
		try
		{
			const uintptr_t base = (uintptr_t)GetModuleHandleW(L"halo2.dll");
			if (!base) return;
			uint8_t* const mgr = *(uint8_t**)(base + kShaderMgrRva);
			if (!mgr || !mgr[kMgrLockInit]) return;
			if (*(int*)(base + kModeCurrentRva) == 0)
			{
				// Classic graphics is showing (loading screens report it too)
				if (!mLoggedFrameHookInClassic)
				{
					mLoggedFrameHookInClassic = true;
					PLOG_INFO << "[H2AShaderPrewarm] the renderer's frame hook does fire while Classic is showing (thread " << GetCurrentThreadId() << ")";
				}
				onClassic(base, mgr);
				return;
			}
			uint8_t* const saber = *(uint8_t**)(base + kSaberSceneRva);
			if (!saber) return;
			const int frame = *(int*)(saber + kSaberFrame);
			const int level = mLevel.load(std::memory_order_relaxed);
			if (frame < 1 || level < 0) return;
			uint8_t* const table = mgr + kMgrTable;
			const int live = *(int*)(table + kTblLive);

			const bool sceneRestarted = saber != mLastSaber || frame < mLastFrame;
			if (sceneRestarted || level != mSetLevel)
			{
				// a level started (or the level number changed under a scene that is still up)
				// Going from one mission straight into the next keeps the scene object and its frame counter (seen live:
				// Delta Halo -> Regret arrived at scene frame 9318); only the variant table is emptied. So "the table
				// was emptied since we last looked" also marks a level start, and such a start gets the one-go pass too.
				const bool firstSight = mSetLevel == -2;
				const bool tableEmptied = live < 8 || live * 4 < mLastLive;
				mRecordHold = !sceneRestarted && !firstSight && !tableEmptied;
				ensureSet(base, level); // the Classic path may already have loaded this level's list
				queueSet(frame < kBlockingFrames || (!firstSight && !sceneRestarted && tableEmptied));
				PLOG_INFO << "[H2AShaderPrewarm] level " << level << ": " << mSet.size() << " recorded variants, scene frame " << frame
					<< (mBlocking ? " (one-go pass)" : " (spread out)");
			}
			else if (mQueue.empty() && mLastLive >= 64 && live * 4 < mLastLive)
			{
				// The game emptied the table under a level that is still up (seen on restarts / reverts from the pause
				// menu). It re-makes what is on screen itself; bring back the rest a little per frame.
				queueSet(false);
				PLOG_INFO << "[H2AShaderPrewarm] variants were purged (" << mLastLive << " -> " << live << "), re-creating " << mQueue.size() << " spread out";
			}
			if (sceneRestarted || live < 8) mRecordHold = false;
			mLastSaber = saber;
			mLastFrame = frame;

			if (!mQueue.empty())
				runQueue(base, mgr, mBlocking ? kBlockingBudgetMs : kSlicedBudgetMs);
			mLastLive = *(int*)(table + kTblLive);

			if (!mRecordHold && (++mTick % kScanEveryFrames) == 0)
				scan(mgr);
			if (mDirty && GetTickCount64() - mLastSaveTick > kSaveEveryMs)
				saveSet();
		}
		catch (...)
		{
			// file or allocation trouble must never reach the game's frame: switch ourselves off
			mEnabled = false;
			PLOG_ERROR << "[H2AShaderPrewarm] unexpected exception in the frame hook - switched off for this session";
		}
	}

	// ---------------------------------------------------------------- HCM events ----
	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "H2AShaderPrewarm onToggle, newValue: " << newValue;
		if (newValue)
		{
			const bool armed = tryArm();
			mEnabled = true;
			if (mArmRefused) return;
			say(armed ? "H2A Shader Pre-warm on - takes effect at the next level start; it also learns the level you are in"
				: "H2A Shader Pre-warm on - starts when Halo 2 loads");
		}
		else
		{
			mEnabled = false;
			std::scoped_lock lk(mStateMutex);
			try { saveSet(); } catch (...) {}
			mQueue.clear();
			mQueuePos = 0;
			say("H2A Shader Pre-warm off");
		}
	}

	void onMCCStateChanged(const MCCState& newState)
	{
		const bool inLevel = newState.currentGameState == mGame && newState.currentPlayState != PlayState::MainMenu;
		mLevel = inLevel ? (int)(uint8_t)newState.currentLevelID : -1;
		mIngame = newState.currentGameState == mGame && newState.currentPlayState == PlayState::Ingame;
		if (newState.currentGameState == mGame && mEnabled.load()) tryArm();
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2aShaderPrewarmToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mMCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
		mDir = std::filesystem::path(dicon.Resolve<DirPathContainer>().lock()->dirPath) / "H2AShaderPrewarm";
		auto target = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(mGame.toModuleName(), std::vector<int64_t>{ (int64_t)kFrameEndRva });
		mHook = ModuleInlineHook::make(mGame.toModuleName(), target, (void*)&frameEndDetour);
		auto modeTarget = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(mGame.toModuleName(), std::vector<int64_t>{ (int64_t)kModeTickRva });
		mModeHook = ModuleInlineHook::make(mGame.toModuleName(), modeTarget, (void*)&modeTickDetour);
		sSelf = this;

		if (auto mcc = mccStateHookWeak.lock())
		{
			try { onMCCStateChanged(mcc->getCurrentMCCState()); } catch (...) {}
		}
		if (auto s = settingsWeak.lock(); s && s->h2aShaderPrewarmToggle->GetValue())
		{
			bool on = true;
			onToggle(on);
		}
	}

	~Impl()
	{
		mEnabled = false;
		if (mHook) mHook->setWantsToBeAttached(false);
		if (mModeHook) mModeHook->setWantsToBeAttached(false);
		for (int i = 0; i < 400 && sInFlight.load() > 0; ++i) Sleep(1);
		{
			std::scoped_lock lk(mStateMutex);
			try { saveSet(); } catch (...) {}
		}
		sSelf = nullptr;
		mHook.reset();
		mModeHook.reset();
	}
};

H2AShaderPrewarm::H2AShaderPrewarm(GameState game, IDIContainer& dicon)
{
	switch (game)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<Impl>(game, dicon);
		break;

	default:
		throw HCMInitException("H2AShaderPrewarm not impl for this game");
	}
}

H2AShaderPrewarm::~H2AShaderPrewarm()
{
	PLOG_DEBUG << "~" << getName();
}
