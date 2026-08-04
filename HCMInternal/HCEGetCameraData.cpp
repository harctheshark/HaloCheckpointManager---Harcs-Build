#include "pch.h"
#include "HCEGetCameraData.h"
#include "HCEGetPlayerState.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "ModuleHook.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>

// See HCEGetCameraData.h for the derivation of every offset and for the coordinate-frame rules.
// This file is a MOVE of the camera half of HCETriggerOverlay.cpp - the maths is byte-for-byte the
// same, only its home changed.

namespace
{
	// ---------------------------------------------------------------------------------------------------------
	// APlayerCameraManager::DoUpdateCamera is midhooked ONLY to capture `this` - RCX at the function entry,
	// before the prologue has moved anything. This runs on the game thread once per frame, so it does the
	// absolute minimum: one atomic store.
	//
	// File-scope (not a member) because safetyhook::MidHookFn is a plain function pointer. There is exactly one
	// HaloCER game and therefore at most one HCEGetCameraData, so a file-scope slot is unambiguous.
	// ---------------------------------------------------------------------------------------------------------
	std::atomic<uintptr_t> gCameraManager{ 0 };

	// PROOF OF LIFE FOR THE CALLBACK ITSELF.
	// Without this, "gCameraManager is 0" is ambiguous between "the callback is never reached" and "the callback
	// is reached but what it stored was later cleared/rejected" - two completely different faults that need
	// completely different fixes. One relaxed increment per frame is free.
	std::atomic<uint64_t> gCameraManagerHookFires{ 0 };

	// The POV payload, exactly as FMinimalViewInfo lays out its first 0x34 bytes.
	struct PovSnapshot { double location[3]; double rotation[3]; float fov; };

	// SNAPSHOT, NOT A POINTER. Two earlier designs both failed in game and both failed the same way - by leaving
	// the VALUES to be fetched later, on a frame of our choosing rather than the game's:
	//
	//   1. "store ctx.rcx every call"  -> the overlay FLICKERED AND SPUN. operator= fires for every view-info
	//      assignment in the process, so the pointer hopped between unrelated cameras frame to frame.
	//   2. "latch the first pointer and keep reading it" -> STILL FLICKERED, because that same object is
	//      rewritten by DIFFERENT cameras. Its contents oscillated between plausible and implausible, we
	//      rejected the implausible frames, and a rejected frame is a frame the overlay does not draw.
	//
	// So validate AT CAPTURE TIME and keep the last good VALUES. Every consumer then gets a camera that is
	// always valid and always the most recent one the engine actually assigned - no rejected frames, so the
	// overlay updates in lockstep with the game instead of stuttering whenever a foreign camera passed through.
	//
	// We read the SOURCE (RDX), not the destination: at function entry the destination still holds last frame's
	// data, whereas the source is the camera being assigned right now.
	std::atomic<uint32_t> gPovSequence{ 0 };   // even = stable, odd = mid-write. 0 = nothing captured yet.
	PovSnapshot gPovSnapshot{};

	void cameraManagerUpdateHook(SafetyHookContext& ctx)
	{
		gCameraManagerHookFires.fetch_add(1, std::memory_order_relaxed);

		const PovSnapshot* source = reinterpret_cast<const PovSnapshot*>(ctx.rdx);
		if (source == nullptr) return;

		const PovSnapshot candidate = *source;

		// Reject foreign/garbage view infos HERE, so a bad one is simply never adopted - rather than being
		// adopted and then rejected later, which is what cost us a drawn frame.
		if (!std::isfinite(candidate.fov) || candidate.fov <= 10.f || candidate.fov >= 170.f) return;
		for (double d : candidate.location) if (!std::isfinite(d)) return;
		for (double d : candidate.rotation) if (!std::isfinite(d)) return;

		// Seqlock: readers retry if they catch a torn write. Cheaper than a mutex on a per-frame game-thread
		// path, and a hook callback must never block.
		gPovSequence.fetch_add(1, std::memory_order_acq_rel);
		gPovSnapshot = candidate;
		gPovSequence.fetch_add(1, std::memory_order_release);

		gCameraManager.store(ctx.rdx, std::memory_order_release);   // kept for diagnostics only
	}

	// Latched failure reporting for getUeCamera. Re-latches whenever the REASON changes, so a transition from
	// "nothing captured yet" to "captured but rejected" IS visible, while any steady state stays silent.
	// This replaces a single LOG_ONCE that was spent on frame ~6 - before the hook had had a chance to fire -
	// and therefore proved nothing.
	enum class UeCameraFailure { None, NotCaptured, ReadFailed, ImplausibleFov, NonFinite };
	std::atomic<UeCameraFailure> gLastReportedFailure{ UeCameraFailure::None };

	void reportUeCameraFailure(UeCameraFailure failure, uintptr_t cameraManager, float fov)
	{
		if (gLastReportedFailure.exchange(failure, std::memory_order_relaxed) == failure) return;

		const uint64_t fires = gCameraManagerHookFires.load(std::memory_order_relaxed);
		switch (failure)
		{
		case UeCameraFailure::NotCaptured:
			PLOG_WARNING << "HCEGetCameraData: no APlayerCameraManager captured yet. DoUpdateCamera's midhook "
				"callback has fired " << fires << " time(s). ZERO means the callback is not being reached AT ALL "
				"(not installed, detached behind our back, or that address is not executed on this build); "
				"NON-ZERO means it was reached and the pointer was cleared afterwards. Consumers are using their "
				"fallback camera.";
			break;
		case UeCameraFailure::ReadFailed:
			PLOG_WARNING << "HCEGetCameraData: captured APlayerCameraManager 0x" << std::hex << cameraManager
				<< std::dec << " but the POV read faulted (hook fires: " << fires
				<< "). The captured pointer is stale, or the POV offset is wrong for this build.";
			break;
		case UeCameraFailure::ImplausibleFov:
			PLOG_WARNING << "HCEGetCameraData: POV read back an implausible horizontal FOV of " << fov
				<< " from APlayerCameraManager 0x" << std::hex << cameraManager << std::dec
				<< " (hook fires: " << fires << "). This is exactly what a WRONG POV OFFSET looks like: the "
				"midhook is working, the struct layout is not.";
			break;
		case UeCameraFailure::NonFinite:
			PLOG_WARNING << "HCEGetCameraData: POV location/rotation read back non-finite from "
				"APlayerCameraManager 0x" << std::hex << cameraManager << std::dec
				<< " (hook fires: " << fires << ")";
			break;
		default:
			break;
		}
	}

	std::string bytesToString(const std::vector<byte>& bytes)
	{
		std::string out;
		for (auto b : bytes)
		{
			if (!out.empty()) out += ' ';
			out += std::format("{:02X}", (uint32_t)(unsigned char)b);
		}
		return out;
	}
}


class HCEGetCameraData::HCEGetCameraDataImpl
{
public:
	std::shared_ptr<MultilevelPointer> mCameraManagerFunction;
	std::shared_ptr<std::vector<byte>> mCameraManagerOriginalBytes;
	std::shared_ptr<ModuleMidHook> mCameraManagerHook;
	int64_t mPovFovOffset = 0x14E0;

	// Guards mRequesters, mCameraHookVerified and every attach/detach. Never taken on the render thread.
	std::mutex mHookMutex;
	std::set<const void*> mRequesters;
	bool mCameraHookVerified = false;   // sticky once the site's bytes match

	// Last FOV that read back as plausible. Seeded with a sane default so a consumer still draws on the very
	// first frames, before DoUpdateCamera has run even once.
	std::atomic<float> mLastGoodFov{ 78.f };

	// CALLER MUST HOLD mHookMutex.
	void applyHookState()
	{
		if (!mCameraManagerHook) return;

		const bool wanted = !mRequesters.empty();
		if (!wanted)
		{
			mCameraManagerHook->setWantsToBeAttached(false);
			// The captured pointer belongs to the level that is unloading. Drop it so a stale
			// APlayerCameraManager is never read back on the next level.
			gCameraManager.store(0, std::memory_order_release);
			return;
		}

		// Refuse to hook a build we do not recognise, exactly as HCEInvulnerability does.
		if (!mCameraHookVerified)
		{
			if (!mCameraManagerOriginalBytes || mCameraManagerOriginalBytes->empty())
				throw HCMRuntimeException("No expected original bytes for the HaloCER camera manager");

			const std::vector<byte>& expected = *mCameraManagerOriginalBytes;
			std::vector<byte> actual(expected.size());
			if (!mCameraManagerFunction->readArrayData(actual.data(), actual.size()))
				throw HCMRuntimeException(std::format("Could not read the HaloCER camera manager site: {}",
					MultilevelPointer::GetLastError()));

			if (actual != expected)
				throw HCMRuntimeException(std::format(
					"The HaloCER camera manager site did not match its expected original bytes - this build of "
					"Halo Campaign Evolved is not supported. Expected [{}], found [{}]",
					bytesToString(expected), bytesToString(actual)));

			mCameraHookVerified = true;
			PLOG_INFO << "HaloCER camera manager site matched its expected original bytes";
		}

		mCameraManagerHook->setWantsToBeAttached(true);
		if (!mCameraManagerHook->isHookInstalled())
		{
			mCameraManagerHook->setWantsToBeAttached(false);
			throw HCMRuntimeException("Failed to install the Halo Campaign Evolved camera manager hook");
		}

		// PROOF THE PATCH PHYSICALLY LANDED. isHookInstalled() only reports safetyhook's own bookkeeping - it
		// does not prove the bytes at the site changed. A hook that reports success while leaving the prologue
		// intact would never fire, which is precisely the symptom being chased (callback fire count stuck at 0
		// while everything upstream reports healthy). Reading the site back is the only way to tell those apart.
		{
			// Explicit latch, not LOG_ONCE: LOG_ONCE wraps its argument in a NON-CAPTURING lambda, so it cannot
			// reference a local (C3493).
			static bool loggedSiteBytes = false;
			if (!loggedSiteBytes)
			{
				loggedSiteBytes = true;
				std::vector<byte> afterAttach(8);
				if (mCameraManagerFunction->readArrayData(afterAttach.data(), afterAttach.size()))
				{
					PLOG_INFO << "HCEGetCameraData: site bytes AFTER attach: [" << bytesToString(afterAttach)
						<< "]. The unpatched prologue starts 48 89 5C 24 20 - if it still reads that, the patch "
						"did NOT land and the callback can never fire. An E9/FF25 or any other change means it "
						"did land, and a zero fire count then means the callback address or the call site is "
						"the problem.";
				}
				else
				{
					PLOG_ERROR << "HCEGetCameraData: could not read the site back after attach";
				}
			}
		}

		// Latched, not per-call: this is the one line that proves the render camera is actually available, and
		// its absence is the difference between "tracks your view" and "silently anchored somewhere else".
		LOG_ONCE(PLOG_INFO << "HCEGetCameraData: DoUpdateCamera midhook installed; the UE render camera is live");
	}
};


HCEGetCameraData::HCEGetCameraData(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEGetCameraDataImpl>())
{
	if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
		throw HCMInitException("HCEGetCameraData only supports Halo Campaign Evolved");

	// Pure PointerDataStore lookups. NOTHING here touches game memory - the exe's camera manager has certainly
	// not run yet when cheats are constructed, and the sim dll may not even be loaded.
	auto ptr = dicon.Resolve<PointerDataStore>().lock();
	pimpl->mPovFovOffset = *ptr->getData<std::shared_ptr<int64_t>>(nameof(hceCameraManagerPovFovOffset), game);
	pimpl->mCameraManagerFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceCameraManagerUpdateFunction), game);
	pimpl->mCameraManagerOriginalBytes = ptr->getVectorData<byte>(nameof(hceCameraManagerUpdateOriginalBytes), game);

	// startEnabled = false, and the hook is on the EXE (L"main"), not the sim dll. Nothing is patched until a
	// consumer actually asks for the camera.
	pimpl->mCameraManagerHook = ModuleMidHook::make(L"main", pimpl->mCameraManagerFunction, &cameraManagerUpdateHook, false);
}

HCEGetCameraData::~HCEGetCameraData()
{
	PLOG_VERBOSE << "~" << getName();
	// Detach before the hook object dies, and drop the captured pointer so nothing can read it afterwards.
	if (pimpl && pimpl->mCameraManagerHook)
	{
		std::scoped_lock lock(pimpl->mHookMutex);
		pimpl->mRequesters.clear();
		pimpl->mCameraManagerHook->setWantsToBeAttached(false);
	}
	gCameraManager.store(0, std::memory_order_release);
}

void HCEGetCameraData::setHookWanted(const void* requester, bool wanted)
{
	std::scoped_lock lock(pimpl->mHookMutex);
	if (wanted) pimpl->mRequesters.insert(requester);
	else        pimpl->mRequesters.erase(requester);
	pimpl->applyHookState();   // may throw HCMRuntimeException; the caller decides what that means
}

float HCEGetCameraData::getLastGoodHorizontalFov() const
{
	return pimpl->mLastGoodFov.load(std::memory_order_acquire);
}

uint64_t HCEGetCameraData::getHookFireCount() const
{
	return gCameraManagerHookFires.load(std::memory_order_relaxed);
}

bool HCEGetCameraData::isHookInstalled() const
{
	return pimpl->mCameraManagerHook && pimpl->mCameraManagerHook->isHookInstalled();
}

// Self-heal. The midhook is the single point of failure for the whole render-camera path, and if anything ever
// detaches it (a module reload, an unbalanced request, a teardown that ran early) NOTHING re-attaches it and every
// consumer silently drops to the sim's player-aim camera for the rest of the session - which is precisely the
// failure that was reported. Cheap, idempotent, and never throws: the caller is a state-change handler, not the
// place to surface a hook failure.
void HCEGetCameraData::ensureHookLive()
{
	try
	{
		std::scoped_lock lock(pimpl->mHookMutex);
		if (pimpl->mRequesters.empty()) return;              // nothing wants it - correctly detached
		if (!pimpl->mCameraManagerHook) return;
		if (pimpl->mCameraManagerHook->isHookInstalled()) return;   // already live

		// attach() DIRECTLY, not applyHookState(): ModuleHookBase::setWantsToBeAttached only acts when the flag
		// CHANGES, so if the hook was detached behind our back while the flag stayed true, asking for it again
		// is a silent no-op. That is exactly the sort of gap this function exists to close.
		PLOG_WARNING << "HCEGetCameraData: the DoUpdateCamera midhook is wanted but is NOT installed; re-attaching";
		pimpl->mCameraManagerHook->attach();
		if (pimpl->mCameraManagerHook->isHookInstalled())
			PLOG_INFO << "HCEGetCameraData: the DoUpdateCamera midhook was successfully re-attached";
		else
			PLOG_ERROR << "HCEGetCameraData: re-attaching the DoUpdateCamera midhook FAILED; the render camera "
				"stays unavailable and consumers will use their fallback camera";
	}
	catch (HCMRuntimeException& ex)
	{
		PLOG_ERROR << "HCEGetCameraData::ensureHookLive could not re-attach the camera midhook: " << ex.what();
	}
	catch (...)
	{
		PLOG_ERROR << "HCEGetCameraData::ensureHookLive could not re-attach the camera midhook (unknown error)";
	}
}

bool HCEGetCameraData::getUeCamera(UeCamera& out) const
{
	// Make failure UNAMBIGUOUS: a caller can never be handed a half-filled or stale camera. In particular a
	// zero-filled UeCamera returned as success would put the viewer at the world origin, which is exactly the
	// kind of silent failure this whole class exists to make visible.
	out = UeCamera{};

	// Diagnostics only - the VALUES come from the snapshot below, never from this pointer.
	const uintptr_t cameraManager = gCameraManager.load(std::memory_order_acquire);

	// Seqlock read of the snapshot the hook captured. Everything here was already validated at capture time, so
	// there is nothing left to reject - which is the entire point: no frame is ever skipped for a bad camera.
	PovSnapshot raw{};
	bool gotSnapshot = false;
	for (int attempt = 0; attempt < 8 && !gotSnapshot; ++attempt)
	{
		const uint32_t before = gPovSequence.load(std::memory_order_acquire);
		if (before == 0 || (before & 1u)) continue;          // never captured, or mid-write
		raw = gPovSnapshot;
		gotSnapshot = (gPovSequence.load(std::memory_order_acquire) == before);
	}

	if (!gotSnapshot)
	{
		// Torn every attempt, or nothing captured yet. Both are transient by nature.
		reportUeCameraFailure(UeCameraFailure::NotCaptured, cameraManager, 0.f);
		return false;
	}

	// Success: clear the latch so a LATER failure re-reports instead of being swallowed as "same as before",
	// and announce recovery once.
	if (gLastReportedFailure.exchange(UeCameraFailure::None, std::memory_order_relaxed) != UeCameraFailure::None)
		PLOG_INFO << "HCEGetCameraData: UE render camera is now readable (APlayerCameraManager 0x"
			<< std::hex << cameraManager << std::dec << ", hook fires: "
			<< gCameraManagerHookFires.load(std::memory_order_relaxed) << ")";

	out.positionBlam = SimpleMath::Vector3(
		(float)(raw.location[0] / kUeCmPerWorldUnit),
		-(float)(raw.location[1] / kUeCmPerWorldUnit),   // UE +Y is RIGHT, Blam +Y is LEFT
		(float)(raw.location[2] / kUeCmPerWorldUnit));

	out.pitchDegrees = (float)raw.rotation[0];
	out.yawDegrees = (float)raw.rotation[1];
	out.rollDegrees = (float)raw.rotation[2];
	out.horizontalFovDegrees = raw.fov;

	pimpl->mLastGoodFov.store(raw.fov, std::memory_order_release);
	return true;
}


// static
void HCEGetCameraData::ueRotationToBlamBasis(float pitchDegrees, float yawDegrees, float rollDegrees,
	SimpleMath::Vector3& outForward, SimpleMath::Vector3& outRight, SimpleMath::Vector3& outUp)
{
	const float pitch = DirectX::XMConvertToRadians(pitchDegrees);
	const float yaw = DirectX::XMConvertToRadians(yawDegrees);
	const float roll = DirectX::XMConvertToRadians(rollDegrees);
	const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);
	const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);

	SimpleMath::Vector3 forwardUe(cosPitch * cosYaw, cosPitch * sinYaw, sinPitch);
	SimpleMath::Vector3 rightUe(-sinYaw, cosYaw, 0.f);
	SimpleMath::Vector3 upUe(-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch);

	if (std::abs(roll) > 1e-6f)
	{
		const float cosRoll = std::cos(roll), sinRoll = std::sin(roll);
		const SimpleMath::Vector3 rolledRight = rightUe * cosRoll + upUe * sinRoll;
		const SimpleMath::Vector3 rolledUp = upUe * cosRoll - rightUe * sinRoll;
		rightUe = rolledRight;
		upUe = rolledUp;
	}

	// UE -> Blam is a single Y negation. See the header: dropping it mirrors the overlay horizontally.
	outForward = SimpleMath::Vector3(forwardUe.x, -forwardUe.y, forwardUe.z);
	outRight = SimpleMath::Vector3(rightUe.x, -rightUe.y, rightUe.z);
	outUp = SimpleMath::Vector3(upUe.x, -upUe.y, upUe.z);
}

// static
void HCEGetCameraData::simViewAngleToBlamBasis(const SimpleMath::Vector2& viewAngle,
	SimpleMath::Vector3& outForward, SimpleMath::Vector3& outRight, SimpleMath::Vector3& outUp)
{
	const float yaw = viewAngle.x;
	const float pitch = viewAngle.y;
	const float cosYaw = std::cos(yaw), sinYaw = std::sin(yaw);
	const float cosPitch = std::cos(pitch), sinPitch = std::sin(pitch);

	outForward = SimpleMath::Vector3(cosPitch * cosYaw, cosPitch * sinYaw, sinPitch);
	// forward x UnitZ. Blam's +Y is LEFT, so the camera's right is -Y at yaw 0 - the opposite of the external
	// CER tool's UE-frame (-sin, cos, 0). Do not "restore" that; it mirrors the whole overlay.
	outRight = SimpleMath::Vector3(sinYaw, -cosYaw, 0.f);
	outUp = SimpleMath::Vector3(-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch);
}
