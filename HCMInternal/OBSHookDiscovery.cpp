#include "pch.h"
#include "OBSHookDiscovery.h"
#include "ModuleCache.h"

// See OBSHookDiscovery.h for what this is and why it replaced a hard-coded RVA.

namespace
{
	constexpr wchar_t kOBSModule[] = L"graphics-hook64.dll";
	constexpr wchar_t kDxgiModule[] = L"dxgi.dll";

	// Detours copies at most ~20 bytes of the target's prologue into its trampoline, then jumps back
	// to just past what it copied. So a trampoline for `dxgiEntry` always lands in this window - and
	// nothing else in the process has any reason to.
	constexpr uintptr_t kDetourJumpBackWindow = 0x20;

	// How far into the trampoline we look for the jump back. Detours' relocated prologue is short;
	// 32 bytes covers it with room to spare.
	constexpr size_t kTrampolineScanBytes = 32;

	// Anything below this is a small integer / flag / enum stored in .data, not an address.
	constexpr uintptr_t kMinimumPlausibleAddress = 0x10000;

	// A trampoline page is at least this big. Rules out pointers into tiny reserved slivers.
	constexpr SIZE_T kMinimumRegionSize = 64;

	std::weak_ptr<IMessagesGUI> gMessageSink;

	bool protectIsExecutable(DWORD protect)
	{
		// Deliberately NOT accepting PAGE_EXECUTE_READ|PAGE_GUARD etc: the low bits are the access
		// class, the high bits (GUARD/NOCACHE/WRITECOMBINE) are modifiers we don't want to trip over.
		switch (protect & 0xFF)
		{
		case PAGE_EXECUTE:
		case PAGE_EXECUTE_READ:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			return true;
		default:
			return false;
		}
	}

	bool protectIsReadable(DWORD protect)
	{
		if (protect & (PAGE_GUARD | PAGE_NOACCESS))
			return false;

		switch (protect & 0xFF)
		{
		case PAGE_READONLY:
		case PAGE_READWRITE:
		case PAGE_WRITECOPY:
		case PAGE_EXECUTE_READ:
		case PAGE_EXECUTE_READWRITE:
		case PAGE_EXECUTE_WRITECOPY:
			return true;
		default:
			return false;
		}
	}

	// Every read below this file's discovery walk goes through here. We are scanning raw qwords out
	// of another product's data section and following them wherever they point, so a bad guess must
	// cost us a `false`, never an access violation inside the game's render thread.
	bool isReadableRange(uintptr_t address, size_t size)
	{
		if (!address || !size) return false;

		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery((LPCVOID)address, &mbi, sizeof(mbi)) != sizeof(mbi))
			return false;
		if (mbi.State != MEM_COMMIT || !protectIsReadable(mbi.Protect))
			return false;

		const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		return (address + size) <= regionEnd;
	}

	// The one real test. `candidateValue` is a qword we found in graphics-hook64's data; if it is a
	// Detours trampoline for `dxgiEntry`, its first instructions jump back into that function.
	//
	// Two encodings, because Detours emits either depending on how far it has to go:
	//    FF 25 rel32   jmp qword ptr [rip+rel32]   -> the destination is READ FROM a slot
	//    E9    rel32   jmp rel32                   -> the destination is the target itself
	bool trampolineJumpsBackTo(uintptr_t candidateValue, uintptr_t dxgiEntry)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (VirtualQuery((LPCVOID)candidateValue, &mbi, sizeof(mbi)) != sizeof(mbi))
			return false;
		if (mbi.State != MEM_COMMIT) return false;
		if (!protectIsExecutable(mbi.Protect)) return false;
		if (mbi.RegionSize < kMinimumRegionSize) return false;

		// Never read past the end of the region we just validated.
		const uintptr_t regionEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
		const size_t available = (size_t)(regionEnd - candidateValue);
		const size_t scanBytes = available < kTrampolineScanBytes ? available : kTrampolineScanBytes;
		if (scanBytes < 6) return false;

		const uint8_t* code = (const uint8_t*)candidateValue;

		for (size_t i = 0; i + 6 <= scanBytes; i++)
		{
			uintptr_t landsAt = 0;

			if (code[i] == 0xFF && code[i + 1] == 0x25)
			{
				int32_t rel = 0;
				memcpy(&rel, code + i + 2, sizeof(rel));
				const uintptr_t slot = candidateValue + i + 6 + (intptr_t)rel;
				if (!isReadableRange(slot, sizeof(uintptr_t)))
					continue;
				memcpy(&landsAt, (const void*)slot, sizeof(landsAt));
			}
			else if (code[i] == 0xE9 && (i + 5) <= scanBytes)
			{
				int32_t rel = 0;
				memcpy(&rel, code + i + 1, sizeof(rel));
				landsAt = candidateValue + i + 5 + (intptr_t)rel;
			}
			else
			{
				continue;
			}

			if (landsAt >= dxgiEntry && landsAt < dxgiEntry + kDetourJumpBackWindow)
				return true;
		}

		return false;
	}
}


void setOBSDiscoveryMessageSink(std::weak_ptr<IMessagesGUI> sink)
{
	gMessageSink = std::move(sink);
}


bool looksInlineHooked(uintptr_t functionEntry)
{
	if (!isReadableRange(functionEntry, 1)) return false;
	const uint8_t first = *(const uint8_t*)functionEntry;
	return first == 0xE9 || first == 0xEB || first == 0xFF;
}


OBSDiscoveryStatus discoverRealFnRva(uintptr_t dxgiEntry, uintptr_t& rvaOut, OBSDiscoveryDiagnostics* diagnosticsOut)
{
	OBSDiscoveryDiagnostics diagnostics{};
	diagnostics.dxgiEntry = dxgiEntry;

	const auto publish = [&](OBSDiscoveryStatus status)
	{
		if (diagnosticsOut) *diagnosticsOut = diagnostics;
		return status;
	};

	if (!dxgiEntry)
		return publish(OBSDiscoveryStatus::NoDetourFound);

	const auto obsModule = ModuleCache::getModuleInfo(kOBSModule);
	if (!obsModule.has_value() || !obsModule->lpBaseOfDll)
		return publish(OBSDiscoveryStatus::ModuleNotLoaded);

	const uintptr_t base = (uintptr_t)obsModule->lpBaseOfDll;
	diagnostics.moduleBase = base;
	diagnostics.moduleSize = obsModule->SizeOfImage;

	// dxgi's own image. A qword pointing INTO dxgi is the un-detoured function address, i.e. the
	// value RealPresent holds when OBS has not (or no longer) attached. Hooking that would put our
	// overlay before the capture, which is exactly the bug we are fixing.
	uintptr_t dxgiBase = 0, dxgiEnd = 0;
	if (const auto dxgiModule = ModuleCache::getModuleInfo(kDxgiModule); dxgiModule.has_value() && dxgiModule->lpBaseOfDll)
	{
		dxgiBase = (uintptr_t)dxgiModule->lpBaseOfDll;
		dxgiEnd = dxgiBase + dxgiModule->SizeOfImage;
	}

	if (!isReadableRange(base, sizeof(IMAGE_DOS_HEADER)))
		return publish(OBSDiscoveryStatus::NoDetourFound);

	const auto* dosHeader = (const IMAGE_DOS_HEADER*)base;
	if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		return publish(OBSDiscoveryStatus::NoDetourFound);

	const uintptr_t ntHeaderAddress = base + (uintptr_t)dosHeader->e_lfanew;
	if (!isReadableRange(ntHeaderAddress, sizeof(IMAGE_NT_HEADERS64)))
		return publish(OBSDiscoveryStatus::NoDetourFound);

	const auto* ntHeaders = (const IMAGE_NT_HEADERS64*)ntHeaderAddress;
	if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		return publish(OBSDiscoveryStatus::NoDetourFound);

	const WORD sectionCount = ntHeaders->FileHeader.NumberOfSections;
	const auto* sections = IMAGE_FIRST_SECTION(ntHeaders);
	if (!isReadableRange((uintptr_t)sections, sizeof(IMAGE_SECTION_HEADER) * sectionCount))
		return publish(OBSDiscoveryStatus::NoDetourFound);

	std::string scannedSections;

	for (WORD s = 0; s < sectionCount; s++)
	{
		const IMAGE_SECTION_HEADER& section = sections[s];

		// Writable initialised data - ".data" in every build of graphics-hook64 so far, but matched by
		// characteristics rather than by name so a renamed or split section still gets scanned.
		if (!(section.Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
		if (!(section.Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)) continue;
		if (section.Misc.VirtualSize == 0) continue;

		char sectionName[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
		memcpy(sectionName, section.Name, IMAGE_SIZEOF_SHORT_NAME);

		if (!scannedSections.empty()) scannedSections += " ";
		scannedSections += std::format("{}(0x{:x})", sectionName, (uint32_t)section.Misc.VirtualSize);

		const uintptr_t sectionStart = base + section.VirtualAddress;
		const uintptr_t sectionEnd = sectionStart + section.Misc.VirtualSize;

		for (uintptr_t cursor = (sectionStart + 7) & ~(uintptr_t)7; cursor + sizeof(uintptr_t) <= sectionEnd; cursor += 8)
		{
			if (!isReadableRange(cursor, sizeof(uintptr_t)))
				continue;

			diagnostics.qwordsScanned++;

			uintptr_t candidateValue = 0;
			memcpy(&candidateValue, (const void*)cursor, sizeof(candidateValue));

			if (candidateValue < kMinimumPlausibleAddress) continue;
			// Inside graphics-hook64 itself: that's its own data/code, not a Detours trampoline.
			if (candidateValue >= base && candidateValue < base + obsModule->SizeOfImage) continue;
			// Inside dxgi: the pointer is un-detoured. See above.
			if (dxgiBase && candidateValue >= dxgiBase && candidateValue < dxgiEnd) continue;

			diagnostics.candidatesProbed++;

			if (!trampolineJumpsBackTo(candidateValue, dxgiEntry))
				continue;

			rvaOut = cursor - base;
			diagnostics.sectionsScanned = scannedSections;
			PLOG_INFO << std::format(
				"OBS hook discovery: found a trampoline pointer for dxgi entry 0x{:X} at graphics-hook64.dll+0x{:X} (value 0x{:X})",
				dxgiEntry, rvaOut, candidateValue);
			return publish(OBSDiscoveryStatus::Ok);
		}
	}

	diagnostics.sectionsScanned = scannedSections.empty() ? "<none>" : scannedSections;
	return publish(OBSDiscoveryStatus::NoDetourFound);
}


bool OBSRealFnPointer::resolve(uintptr_t* resolvedOut) const
{
	uintptr_t rva = 0;
	OBSDiscoveryDiagnostics diagnostics{};
	const OBSDiscoveryStatus status = discoverRealFnRva(mDxgiEntry, rva, &diagnostics);

	if (status == OBSDiscoveryStatus::ModuleNotLoaded)
	{
		// Not an error. OBS isn't capturing (yet); ModuleHookManager will call us again when
		// graphics-hook64.dll loads.
		*SetLastErrorByRef() << "graphics-hook64.dll is not loaded, so OBS's " << mFunctionName
			<< " hook cannot be located yet" << std::endl;
		return false;
	}

	if (status == OBSDiscoveryStatus::NoDetourFound)
	{
		*SetLastErrorByRef() << "graphics-hook64.dll is loaded but no trampoline pointer for " << mFunctionName
			<< " could be found in it" << std::endl;

		// Report ONCE. attach() is retried on every module load, and the deferred path (inside
		// LoadLibrary) has no way to throw, so this log line plus the message is all the user gets.
		if (!mReportedFailure.exchange(true))
		{
			PLOG_ERROR << std::format(
				"OBS hook discovery FAILED for {}. graphics-hook64.dll base 0x{:X} size 0x{:X}; sections scanned: {}; "
				"{} aligned qwords examined, {} probed as candidates; dxgi entry used 0x{:X} (first byte 0x{:02X}, "
				"looks inline-hooked: {}). This means HCM does not understand your OBS version's Game Capture hook.",
				mFunctionName, diagnostics.moduleBase, (uint64_t)diagnostics.moduleSize, diagnostics.sectionsScanned,
				diagnostics.qwordsScanned, diagnostics.candidatesProbed, diagnostics.dxgiEntry,
				isReadableRange(diagnostics.dxgiEntry, 1) ? *(const uint8_t*)diagnostics.dxgiEntry : 0,
				looksInlineHooked(diagnostics.dxgiEntry) ? "yes" : "no");

			if (auto messages = gMessageSink.lock())
			{
				messages->addMessage(std::format(
					"OBS Game Capture was detected, but HCM could not locate its {} hook (unsupported OBS version).\n"
					"The OBS bypass will not engage. Please report this along with your OBS version.", mFunctionName));
			}

			// Deferred attaches happen inside LoadLibrary and cannot throw. Let the owner stop asking.
			if (mOnPermanentFailure)
			{
				try { mOnPermanentFailure(); }
				catch (...) { PLOG_ERROR << "OBS hook discovery: the permanent-failure callback threw; ignoring"; }
			}
		}
		return false;
	}

	const auto obsModule = ModuleCache::getModuleInfo(kOBSModule);
	if (!obsModule.has_value() || !obsModule->lpBaseOfDll)
	{
		*SetLastErrorByRef() << "graphics-hook64.dll unloaded between discovery and dereference" << std::endl;
		return false;
	}

	const uintptr_t variableAddress = (uintptr_t)obsModule->lpBaseOfDll + rva;
	if (!isReadableRange(variableAddress, sizeof(uintptr_t)))
	{
		*SetLastErrorByRef() << std::format("OBS trampoline pointer variable at 0x{:X} became unreadable", variableAddress) << std::endl;
		return false;
	}

	uintptr_t trampoline = 0;
	memcpy(&trampoline, (const void*)variableAddress, sizeof(trampoline));
	if (!trampoline)
	{
		*SetLastErrorByRef() << "OBS trampoline pointer variable held null" << std::endl;
		return false;
	}

	// A successful discovery already proved this, but resolve() is called again on every re-attach
	// and OBS could have restored the pointer in between.
	if (!trampolineJumpsBackTo(trampoline, mDxgiEntry))
	{
		*SetLastErrorByRef() << std::format(
			"OBS trampoline pointer at 0x{:X} no longer points at a trampoline for 0x{:X}", variableAddress, mDxgiEntry) << std::endl;
		return false;
	}

	mReportedFailure.store(false); // a later failure is a new event worth reporting
	*resolvedOut = trampoline;
	return true;
}
