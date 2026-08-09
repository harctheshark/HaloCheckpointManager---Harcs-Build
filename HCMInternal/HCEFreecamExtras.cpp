#include "pch.h"
#include "HCEFreecamExtras.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "HCESignatureScan.h"
#include "HCEGetPlayerState.h"   // tryReadRaw / tryWriteRaw - SEH-guarded raw access
#include <atomic>

// See HCEFreecamExtras.h for what these do and why the cave page is leaked.

namespace
{
	// ------------------------------------------------------------------------------------------------------------
	// Everything below is expressed as (rva, offBytes, onBytes). The OFF bytes are the guard: nothing is written
	// unless the site currently reads back exactly those, and nothing is restored unless it reads back the ON
	// bytes. Verified present in HaloSimulation_tag_release.dll at the time this was written.
	// ------------------------------------------------------------------------------------------------------------
	struct BytePatch
	{
		uint32_t    rva;
		const char* off;      // stock bytes - the guard
		const char* on;       // patched bytes
		const char* what;
	};

	// Gimbal lock bypass. Two clamp sites, four patches.
	constexpr BytePatch kGimbalPatches[] =
	{
		{ 0x3A61ED, "73 04",                                  "90 90",                                  "site1 >=max branch" },
		{ 0x3A61EF, "C5 F2 5F C2",                            "C5 F2 5F C1",                            "site1 vmaxss vs min" },
		{ 0x3A7082, "C5 FA 5F 15 C6 C0 54 00",                "C5 F8 28 D0 90 90 90 90",                "site2 vmaxss vs min" },
		{ 0x3A708A, "C5 EA C2 C1 01 C4 E3 71 4A C2 00",       "C5 F8 28 C2 90 90 90 90 90 90 90",       "site2 cmpltss+blendvps" },
	};

	// Theater camera collision distance. Not code - a float the camera reads.
	constexpr uint32_t kNoclipRva = 0x9E56F8;
	constexpr float    kNoclipOn  = -999999.f;
	constexpr float    kNoclipOff = 0.f;

	// The wrap-around cave. 144 bytes; the E9 at offset 123 is a placeholder whose rel32 is filled in once the
	// page is allocated. Everything else is position-independent: the rip-relative loads at the front all target
	// the four float constants in the tail (1/2pi, 2pi, pi/2, -pi/2), so the blob only has to be copied whole.
	constexpr size_t kCaveJmpBackOffset = 123;
	constexpr uint8_t kCaveBytes[] = {
		0xC5,0xFA,0x10,0x4E,0x28, 0xC5,0xF8,0x28,0xD1, 0xC5,0xEA,0x59,0x15,0x6F,0x00,0x00,0x00,
		0xC5,0xFA,0x2D,0xC2, 0xC5,0xEA,0x2A,0xD0, 0xC5,0xEA,0x59,0x15,0x63,0x00,0x00,0x00,
		0xC5,0xF2,0x5C,0xCA, 0xC5,0xF8,0x2F,0x0D,0x5B,0x00,0x00,0x00, 0x0F,0x87,0x13,0x00,0x00,0x00,
		0xC5,0xF8,0x2F,0x0D,0x51,0x00,0x00,0x00, 0x0F,0x82,0x05,0x00,0x00,0x00, 0xE9,0x30,0x00,0x00,0x00,
		0xC5,0xE8,0x57,0xD2, 0xC4,0xC1,0x6A,0x5C,0x57,0x38, 0xC4,0xC1,0x7A,0x11,0x57,0x38,
		0xC5,0xE8,0x57,0xD2, 0xC4,0xC1,0x6A,0x5C,0x57,0x3C, 0xC4,0xC1,0x7A,0x11,0x57,0x3C,
		0xC5,0xE8,0x57,0xD2, 0xC4,0xC1,0x6A,0x5C,0x57,0x40, 0xC4,0xC1,0x7A,0x11,0x57,0x40,
		0xC5,0xFA,0x10,0x46,0x2C,                     // the displaced instruction, re-executed
		0xE9,0x00,0x00,0x00,0x00,                     // <- offset 123: jmp back, patched at runtime
		0x83,0xF9,0x22,0x3E, 0xDB,0x0F,0xC9,0x40, 0xDB,0x0F,0xC9,0x3F, 0xDB,0x0F,0xC9,0xBF,
	};
	static_assert(sizeof(kCaveBytes) == 144, "cave length changed - re-check kCaveJmpBackOffset and the rip-relative constant offsets");
	static_assert(kCaveBytes[kCaveJmpBackOffset] == 0xE9, "kCaveJmpBackOffset must point at the jmp-back placeholder");

	// The camera-update site the cave hooks. Signature rather than a fixed address: the two rel32s are wildcarded,
	// so this survives anything that only moves code around. Exactly one match is required.
	constexpr const char* kCaveHookSignature =
		"C5 FA 10 46 2C E8 ?? ?? ?? ?? C4 E3 79 04 D8 01 C5 F8 28 D0";
	constexpr size_t kCaveHookPatchLength = 5;   // the displaced vmovss - exactly a jmp rel32

	std::vector<uint8_t> parseBytes(const char* text)
	{
		std::vector<uint8_t> out;
		for (const char* p = text; *p; )
		{
			if (*p == ' ') { ++p; continue; }
			out.push_back((uint8_t)strtoul(std::string(p, p + 2).c_str(), nullptr, 16));
			p += 2;
		}
		return out;
	}

	// Reads a site and compares. False means "not what we expected" - never write in that case.
	bool siteMatches(uintptr_t address, const std::vector<uint8_t>& expect)
	{
		std::vector<uint8_t> actual(expect.size());
		if (!HCEGetPlayerState::tryReadRaw(address, actual.data(), actual.size())) return false;
		return memcmp(actual.data(), expect.data(), expect.size()) == 0;
	}

	bool writeSite(uintptr_t address, const std::vector<uint8_t>& bytes)
	{
		DWORD old = 0;
		if (!VirtualProtect((void*)address, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
		const bool ok = HCEGetPlayerState::tryWriteRaw(address, bytes.data(), bytes.size());
		DWORD ignored = 0;
		VirtualProtect((void*)address, bytes.size(), old, &ignored);
		if (ok) FlushInstructionCache(GetCurrentProcess(), (void*)address, bytes.size());
		return ok;
	}

	// VirtualAlloc within +/-1.75 GB of target so an E9 rel32 reaches it. Same approach HCETriggerActivity uses
	// for its stub, and for the same reason.
	void* allocNear(uintptr_t target, size_t size)
	{
		SYSTEM_INFO si{}; GetSystemInfo(&si);
		const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
		const uintptr_t lo = target > 0x70000000ULL ? target - 0x70000000ULL : 0x10000ULL;
		const uintptr_t hi = target + 0x70000000ULL;
		const uintptr_t start = target & ~(gran - 1);

		for (uintptr_t p = start; p > lo; p -= gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
		for (uintptr_t p = start; p < hi; p += gran)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) return m;
		return nullptr;
	}
}


class HCEFreecamExtras::HCEFreecamExtrasImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::mutex mMutex;
	uintptr_t mSimBase = 0;

	bool mGimbalApplied = false;
	uintptr_t mCaveHookSite = 0;             // the signature match - where our jmp goes
	void* mCavePage = nullptr;               // NEVER freed, see the header
	std::vector<uint8_t> mCaveSiteOriginal;  // the displaced bytes, for restore

	uintptr_t simBase()
	{
		if (mSimBase) return mSimBase;
		mSimBase = (uintptr_t)GetModuleHandleW(L"HaloSimulation_tag_release.dll");
		if (!mSimBase) throw HCMRuntimeException("HaloSimulation_tag_release.dll is not loaded yet - load a level first");
		return mSimBase;
	}

	// ------------------------------------------------------------------------------------------------------
	// NOCLIP
	// ------------------------------------------------------------------------------------------------------
	void applyNoclip(bool enable)
	{
		const uintptr_t address = simBase() + kNoclipRva;
		const float value = enable ? kNoclipOn : kNoclipOff;
		if (!HCEGetPlayerState::tryWriteRaw(address, &value, sizeof(value)))
			throw HCMRuntimeException("Could not write the HaloCER camera collision distance");

		lockOrThrow(messagesGUIWeak, messagesGUI);
		messagesGUI->addMessage(enable ? "Camera noclip on." : "Camera noclip off.");
	}

	// ------------------------------------------------------------------------------------------------------
	// GIMBAL LOCK BYPASS
	// ------------------------------------------------------------------------------------------------------
	void applyGimbal(bool enable)
	{
		std::scoped_lock lock(mMutex);
		if (enable == mGimbalApplied) return;

		if (enable) applyGimbalOn();
		else        applyGimbalOff();
	}

	void applyGimbalOn()
	{
		const uintptr_t base = simBase();

		// ⚠ VERIFY EVERY SITE BEFORE WRITING ANY OF THEM. A half-applied set of clamp patches is worse than none:
		// the two sites cooperate, and leaving one patched would produce camera behaviour nobody has tested.
		for (const auto& p : kGimbalPatches)
			if (!siteMatches(base + p.rva, parseBytes(p.off)))
				throw HCMRuntimeException(std::format(
					"The HaloCER gimbal site '{}' (rva 0x{:X}) does not contain its expected original bytes - this "
					"build of Halo Campaign Evolved is not supported by this patch.", p.what, p.rva));

		// The cave hook, resolved by signature.
		int hits = 0;
		const uintptr_t site = HCESignatureScan::resolveUnique(base, kCaveHookSignature, hits);
		if (!site)
			throw HCMRuntimeException(std::format(
				"Could not locate the HaloCER camera update site by signature ({} matches, exactly 1 required). "
				"Gimbal lock bypass is unavailable on this build.", hits));

		if (!mCavePage)
		{
			mCavePage = allocNear(site, sizeof(kCaveBytes));
			if (!mCavePage) throw HCMRuntimeException("Could not allocate a code cave within reach of the camera site");

			memcpy(mCavePage, kCaveBytes, sizeof(kCaveBytes));

			// Fill in the jump back to just past the bytes we are about to displace. The cave has already
			// re-executed them by this point.
			const uintptr_t caveJmp = (uintptr_t)mCavePage + kCaveJmpBackOffset;
			const int32_t backRel = (int32_t)((intptr_t)(site + kCaveHookPatchLength) - (intptr_t)(caveJmp + 5));
			memcpy((void*)(caveJmp + 1), &backRel, sizeof(backRel));

			FlushInstructionCache(GetCurrentProcess(), mCavePage, sizeof(kCaveBytes));
			PLOG_INFO << "HCE gimbal bypass: cave at " << std::format("0x{:X}", (uintptr_t)mCavePage)
				<< " (never freed), hooking the camera site at " << std::format("0x{:X}", site);
		}

		// Save the displaced bytes so disable can put them back verbatim.
		mCaveSiteOriginal.resize(kCaveHookPatchLength);
		if (!HCEGetPlayerState::tryReadRaw(site, mCaveSiteOriginal.data(), mCaveSiteOriginal.size()))
			throw HCMRuntimeException("Could not read the HaloCER camera site before patching it");

		std::vector<uint8_t> jump(kCaveHookPatchLength, 0x90);
		jump[0] = 0xE9;
		const int32_t rel = (int32_t)((intptr_t)mCavePage - (intptr_t)(site + 5));
		memcpy(jump.data() + 1, &rel, sizeof(rel));

		// Byte patches first, then the jump - so the cave is never reachable while the clamps are still live.
		for (const auto& p : kGimbalPatches)
			if (!writeSite(base + p.rva, parseBytes(p.on)))
				throw HCMRuntimeException(std::format("Failed to patch the HaloCER gimbal site '{}'", p.what));

		if (!writeSite(site, jump))
			throw HCMRuntimeException("Failed to install the HaloCER camera cave hook");

		mCaveHookSite = site;
		mGimbalApplied = true;

		lockOrThrow(messagesGUIWeak, messagesGUI);
		messagesGUI->addMessage("Gimbal lock bypass on.");
	}

	void applyGimbalOff()
	{
		const uintptr_t base = simBase();

		// Jump out first: once the site is stock again no new thread can enter the cave. The page itself stays
		// mapped forever - a thread may be inside it right now, and this runs on the camera path every frame.
		if (mCaveHookSite && !mCaveSiteOriginal.empty())
		{
			std::vector<uint8_t> current(mCaveSiteOriginal.size());
			if (HCEGetPlayerState::tryReadRaw(mCaveHookSite, current.data(), current.size()) && current[0] == 0xE9)
				writeSite(mCaveHookSite, mCaveSiteOriginal);
			else
				PLOG_WARNING << "HCE gimbal bypass: the camera site no longer holds our jump - leaving it alone "
					"rather than overwriting whatever replaced it.";
		}

		for (const auto& p : kGimbalPatches)
		{
			const auto on = parseBytes(p.on);
			if (siteMatches(base + p.rva, on)) writeSite(base + p.rva, parseBytes(p.off));
			else PLOG_WARNING << "HCE gimbal bypass: site '" << p.what << "' was not as we left it; not restoring.";
		}

		mCaveHookSite = 0;
		mGimbalApplied = false;

		if (auto messagesGUI = messagesGUIWeak.lock()) messagesGUI->addMessage("Gimbal lock bypass off.");
	}

	void onNoclipToggle(bool& newValue)
	{
		try { applyNoclip(newValue); }
		catch (HCMRuntimeException ex) { runtimeExceptions->handleMessage(ex); }
	}

	void onGimbalToggle(bool& newValue)
	{
		try { applyGimbal(newValue); }
		catch (HCMRuntimeException ex)
		{
			// Leave nothing half-applied.
			try { applyGimbalOff(); } catch (...) {}
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mNoclipCallback;
	ScopedCallback<ToggleEvent> mGimbalCallback;

public:
	HCEFreecamExtrasImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mNoclipCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFreecamNoclipToggle->valueChangedEvent, [this](bool& n) { onNoclipToggle(n); }),
		mGimbalCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceFreecamGimbalBypassToggle->valueChangedEvent, [this](bool& n) { onGimbalToggle(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEFreecamExtras only supports Halo Campaign Evolved");
	}

	~HCEFreecamExtrasImpl()
	{
		mNoclipCallback.removeCallback();
		mGimbalCallback.removeCallback();

		// Put the game back. Both are game-code / game-state changes that would otherwise outlive HCM.
		try { if (mGimbalApplied) applyGimbalOff(); } catch (...) {}
		try { if (mSimBase) { const float off = kNoclipOff; HCEGetPlayerState::tryWriteRaw(mSimBase + kNoclipRva, &off, sizeof(off)); } } catch (...) {}
		// mCavePage is deliberately NOT freed. See the header.
	}
};


HCEFreecamExtras::HCEFreecamExtras(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEFreecamExtrasImpl>(game, dicon))
{
}

HCEFreecamExtras::~HCEFreecamExtras()
{
	PLOG_VERBOSE << "~" << getName();
}
