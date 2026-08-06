#pragma once
#include "pch.h"
#include "HCEGetPlayerState.h"

// ================================================================================================================
// Byte-signature resolution for Halo Campaign Evolved.
//
// WHY THIS EXISTS AT ALL: HaloSimulation_tag_release.dll carries no version resource, so every build of it
// reports 0.0.0.0 and InternalPointerData.xml's Version attribute cannot tell two builds apart. Module globals
// DO move between builds (the tag address table was observed at 0x2C2DCC0 on one and 0x2C2CCC0 on another), and
// a stale global does not fail loudly - it resolves to whatever now occupies that address and produces confident
// nonsense. Byte signatures are the only update detector this title admits.
//
// THE CONTRACT EVERY CALLER MUST HONOUR: exactly one match is a resolution. Zero matches, or more than one,
// DISABLES the feature. Never fall back to a last-known address.
//
// ⚠ HCETriggerOverlay.cpp carries its own private copy of this logic and deliberately still does. It is a
// verified-working feature on a title with no version detection, and the cost of a regression there is drawing
// wrong geometry rather than a visible failure. New code uses this header; the trigger overlay is not worth
// re-pointing at it for tidiness alone.
//
// ⚠ Everything here is POD-only and takes no locks, because the scan body sits inside SEH: a function containing
// __try/__except cannot also require C++ object unwinding (C2712). That is the same constraint
// HCEGetPlayerState.cpp documents.
// ================================================================================================================
namespace HCESignatureScan
{
	constexpr int kMaxSigBytes = 64;

	struct Pattern
	{
		unsigned char bytes[kMaxSigBytes];
		bool wild[kMaxSigBytes];
		int length;
	};

	inline int hexNibble(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}

	// "48 8B 05 ?? ?? ?? ?? 3B 88" - hex bytes, ?? for a wildcard, whitespace ignored.
	inline bool parsePattern(const char* text, Pattern& out)
	{
		out.length = 0;
		while (*text && out.length < kMaxSigBytes)
		{
			if (*text == ' ' || *text == '\t') { ++text; continue; }
			if (*text == '?')
			{
				out.bytes[out.length] = 0;
				out.wild[out.length] = true;
				++out.length;
				++text;
				if (*text == '?') ++text;
				continue;
			}
			const int hi = hexNibble(text[0]); if (hi < 0) return false;
			const int lo = hexNibble(text[1]); if (lo < 0) return false;
			out.bytes[out.length] = (unsigned char)((hi << 4) | lo);
			out.wild[out.length] = false;
			++out.length;
			text += 2;
		}
		return out.length > 0;
	}

	// Scans every EXECUTABLE section of the module. Returns the match count, or -1 if the PE headers could not
	// be walked. POD-only, see the header note above.
	inline int scanExecutableSections(uintptr_t base, const unsigned char* pattern, const bool* wild, int length,
		uintptr_t* outHits, int maxHits)
	{
		int found = 0;
		__try
		{
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
			const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;

			const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
			for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
			{
				if (!(section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
				const DWORD virtualSize = section[s].Misc.VirtualSize ? section[s].Misc.VirtualSize : section[s].SizeOfRawData;
				if (virtualSize <= (DWORD)length) continue;

				const unsigned char* const bytes = (const unsigned char*)(base + section[s].VirtualAddress);
				const size_t span = (size_t)virtualSize - (size_t)length;
				const unsigned char firstByte = pattern[0];
				const bool firstWild = wild[0];

				for (size_t i = 0; i <= span; ++i)
				{
					if (!firstWild && bytes[i] != firstByte) continue;
					int k = 1;
					for (; k < length; ++k)
						if (!wild[k] && bytes[i + k] != pattern[k]) break;
					if (k != length) continue;
					if (found < maxHits) outHits[found] = (uintptr_t)(bytes + i);
					++found;
					if (found > maxHits) return found;   // caller only needs to know it is > 1
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		return found;
	}

	// Exactly one hit or nothing. See the contract in the header comment.
	inline uintptr_t resolveUnique(uintptr_t base, const char* patternText, int& outHits)
	{
		outHits = 0;
		Pattern pattern{};
		if (!parsePattern(patternText, pattern)) return 0;
		uintptr_t hits[4]{};
		const int count = scanExecutableSections(base, pattern.bytes, pattern.wild, pattern.length, hits, 4);
		outHits = count;
		return (count == 1) ? hits[0] : 0;
	}

	// Reads the disp32 of a rip-relative instruction and returns its target. insnLength is the length of the
	// WHOLE instruction, because rip is the address of the NEXT one.
	inline uintptr_t ripTarget(uintptr_t instruction, int dispOffset, int insnLength)
	{
		int32_t displacement = 0;
		if (!HCEGetPlayerState::tryReadRaw(instruction + dispOffset, &displacement, sizeof(displacement))) return 0;
		return instruction + insnLength + (intptr_t)displacement;
	}
}
