#pragma once
#include "pch.h"
#include "CustomExceptions.h"
#include <string>
#include <vector>
#include <cstring>

// ============================================================================
//  Halo 2 checkpoints vs the session's gametype (2026-11-04, halo2.dll 1.3528 RE + live tests with the user)
//
//  A Halo 2 game state starts with a 0x1930-byte GAME OPTIONS block (+0 signature, +4 scenario path, +0x130 the game
//  variant: name UTF-16 @+0x3F8, BASE TYPE byte @+0x438 - 1 CTF, 2 Slayer, 8 Territories ...). The game also keeps a copy
//  of it per checkpoint slot (the SLOT HEADER, halo2+0x15A0B08 + 6448 * slot), and the revert (sub_1806AFFD0) reloads the
//  slot buffer until the loaded options equal that header. HCM injects only the buffer, so a checkpoint from another
//  gametype made the revert reload FOREVER (the game froze).
//
//  Fix: before injecting, the slot header's options replace the checkpoint's - the world loads under the SESSION's
//  gametype. Verified live: Slayer <-> Bounce Ghost (same base type) and CTF -> Slayer run normally. A different base
//  type into a richer engine crashes (Slayer -> CTF, Territories -> Slayer: that engine's setup and objective objects
//  exist only from its match start), so a cross-type load is REFUSED with a clear message.
//
//  Only acts when it can prove the layout: halo2.dll loaded, the target buffer is one of the two slot buffers, both
//  blocks carry the game's options signature and the same scenario. Otherwise it does nothing (the inject is unchanged).
// ============================================================================
namespace H2Gametype
{
	inline constexpr uintptr_t kSlotHeaders = 0x15A0B08, kSlotBuffers = 0x1652CF0, kSignature = 0x15A3D7C, kLiveOptions = 0x15A3D88;
	inline constexpr size_t kHeaderStride = 6448, kOptionsSize = 0x1930, kVariantStart = 0x130, kNameOff = 0x3F8, kTypeOff = 0x438;
	inline constexpr uintptr_t kVariantCopy = 0xE80A78;   // qword: game-state struct whose +8 is the second variant copy (sub_1806A6500)
	inline constexpr size_t kVariantSize = kOptionsSize - kVariantStart;   // 0x1800

	inline uintptr_t halo2() { return (uintptr_t)GetModuleHandleA("halo2.dll"); }

	inline const char* typeName(int t)
	{
		switch (t)
		{
		case 1: return "CTF"; case 2: return "Slayer"; case 3: return "Oddball"; case 4: return "King of the Hill";
		case 7: return "Juggernaut"; case 8: return "Territories"; case 9: return "Assault";
		default: return nullptr;
		}
	}
	inline std::string typeLabel(int t) { const char* n = typeName(t); return n ? std::string(n) : std::format("type {}", t); }

	// the variant's name in an options block ("$slayer" -> "slayer"); "" when there is none
	inline std::string variantName(const byte* opt)
	{
		std::string out;
		for (size_t i = 0; i < 48; ++i)
		{
			const wchar_t c = (wchar_t)(opt[kNameOff + 2 * i] | (opt[kNameOff + 2 * i + 1] << 8));
			if (!c) break;
			out.push_back(c < 0x80 ? (char)c : '?');
		}
		if (!out.empty() && out[0] == '$') out.erase(0, 1);
		return out;
	}

	// the live session's options block (nullptr when not readable / no Halo 2 game)
	inline const byte* liveOptions()
	{
		const uintptr_t b = halo2();
		if (!b || IsBadReadPtr((void*)(b + kLiveOptions), 8) || IsBadReadPtr((void*)(b + kSignature), 4)) return nullptr;
		const byte* o = *(const byte* const*)(b + kLiveOptions);
		if (!o || IsBadReadPtr(o, kOptionsSize)) return nullptr;
		if (*(const uint32_t*)o != *(const uint32_t*)(b + kSignature)) return nullptr;
		return o;
	}

	// "Bounce Ghost" / "slayer" for file names; "" outside a multiplayer game (no variant name)
	inline std::string sessionGametypeForName()
	{
		const byte* o = liveOptions();
		if (!o) return "";
		std::string n = variantName(o);
		for (char& c : n) if (std::strchr("<>:\"/\\|?*", c) || (unsigned char)c < 0x20) c = '_';
		while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
		return n;
	}

	// ---- skulls follow the CHECKPOINT (user, 2026-11-04: "HCMTest2Skulls session + a Slayer checkpoint -> skulls OFF;
	// Slayer session + an HCMTest2Skulls checkpoint -> skulls ON") ----
	// The ACTIVE skulls are a byte per skull at halo2+0x15F25F0 (35, the array HCM's skull toggles write; set at match start
	// from MCC, never touched by a checkpoint load). The variant's skull bits are options +0x4D4 (= +0xE0 of the gametype
	// file MCC saves in LocalFiles/<id>/Halo2/GameType/*.bin). Bit -> skull slot, in order onto the 13 skulls Halo 2's MP
	// enable mask (halo2+0xDFC898 = 0x15DB0542) allows: confirmed by the user's gametypes - Bounce Sput (bit 5) = Sputnik,
	// Bounce Boom (bit 6) = Boom, HCMTest2 (bits 1,3-12,14) = slots 1,6,8,10,16,17,19,20,22,23,24,28; bit 13 = Scarab (26),
	// the one allowed skull HCMTest2 leaves off. Other bits would be skulls MP does not allow (the game's setter drops them).
	inline constexpr uintptr_t kSkullArray = 0x15F25F0;
	inline constexpr size_t kSkullCount = 35, kSkullOff = 0x4D4;
	inline constexpr int kSkullOfBit[16] = { -1, 1, -1, 6, 8, 10, 16, 17, 19, 20, 22, 23, 24, 26, 28, -1 };
	inline byte* skullArray() { const uintptr_t b = halo2(); byte* p = b ? (byte*)(b + kSkullArray) : nullptr; return (p && !IsBadWritePtr(p, kSkullCount)) ? p : nullptr; }
	// set the active skulls to the ones a checkpoint's variant asks for; returns a note ("" = nothing changed)
	inline std::string applyCheckpointSkulls(uint32_t checkpointBits)
	{
		byte* arr = skullArray();
		if (!arr) return "";
		std::vector<byte> want(kSkullCount, 0);
		for (int bit = 0; bit < 16; ++bit)
			if (((checkpointBits >> bit) & 1) && kSkullOfBit[bit] >= 0) want[(size_t)kSkullOfBit[bit]] = 1;
		if (std::memcmp(arr, want.data(), kSkullCount) == 0) return "";   // already the checkpoint's skulls
		std::memcpy(arr, want.data(), kSkullCount);
		int n = 0; for (byte x : want) n += x != 0;
		PLOG_INFO << std::format("H2Gametype: skulls set from the checkpoint's gametype (bits {:08X}): {} on", checkpointBits, n);
		return n ? std::format("skulls: the checkpoint's {} on", n) : "skulls: off, as in the checkpoint";
	}

	// Before a checkpoint is written into the slot buffer at 'loc': match its options to the session (see above).
	// Throws on a cross-type load; returns a note for the message line when it changed the gametype ("" otherwise).
	inline std::string prepareInject(std::vector<byte>& data, uintptr_t loc)
	{
		const uintptr_t b = halo2();
		if (!b || data.size() < kOptionsSize) return "";
		int slot = -1;
		for (int s = 0; s < 2; ++s)
		{
			const uintptr_t p = b + kSlotBuffers + 16 * (uintptr_t)s;
			if (!IsBadReadPtr((void*)p, 8) && *(const uintptr_t*)p == loc) slot = s;
		}
		if (slot < 0) { PLOG_DEBUG << "H2Gametype: target buffer is not a slot buffer - options left as they are"; return ""; }
		const byte* hdr = (const byte*)(b + kSlotHeaders + kHeaderStride * (uintptr_t)slot);
		if (IsBadReadPtr(hdr, kOptionsSize) || IsBadReadPtr((void*)(b + kSignature), 4)) return "";
		const uint32_t sig = *(const uint32_t*)(b + kSignature);
		if (*(const uint32_t*)hdr != sig || *(const uint32_t*)data.data() != sig) { PLOG_DEBUG << "H2Gametype: options signature mismatch - left as they are"; return ""; }
		if (std::strncmp((const char*)hdr + 4, (const char*)data.data() + 4, 256) != 0) { PLOG_DEBUG << "H2Gametype: different scenario - left as they are"; return ""; }
		const bool sameVariant = std::memcmp(hdr + kVariantStart, data.data() + kVariantStart, kOptionsSize - kVariantStart) == 0;
		const int tc = data[kTypeOff], ts = hdr[kTypeOff];
		const std::string nc = variantName(data.data()), ns = variantName(hdr);
		if (!sameVariant && tc != ts)
			throw HCMRuntimeException(std::format(
				"Can't load this checkpoint into this session: it is from a {} game (\"{}\"), this session is {} (\"{}\").\n"
				"Halo 2 only runs a checkpoint under another gametype of the SAME type (e.g. Slayer into a Slayer-based variant); "
				"across types it freezes or crashes. Start a {} session to load it.",
				typeLabel(tc), nc, typeLabel(ts), ns, typeLabel(tc)));
		// skulls: the checkpoint's (read before its options are replaced)
		const std::string skullNote = applyCheckpointSkulls(*(const uint32_t*)(data.data() + kSkullOff));
		if (sameVariant) return skullNote;   // same gametype: the options already match
		std::memcpy(data.data(), hdr, kOptionsSize);
		// The SECOND copy of the variant (2026-11-04, user: after an inject the next NEW checkpoint's revert blue-screened and
		// restarted the match): the revert also compares the slot header's variant with sub_1806A6500() = qword_180E80A78 + 8,
		// a game-state copy (state +0x1DA0 = a near-copy of the options block at +0x1C70, its variant at +0x130, 0x1800 B;
		// flag at qword+0x1808 = the block's end). Left at the checkpoint's old gametype it failed every later revert
		// (-> sub_18067A8D0, the restart flag). Its offset is taken from the live game and range-checked.
		{
			const uintptr_t pLive = b + kLiveOptions, pVar = b + kVariantCopy;
			if (!IsBadReadPtr((void*)pLive, 8) && !IsBadReadPtr((void*)pVar, 8))
			{
				const uintptr_t live = *(const uintptr_t*)pLive, g = *(const uintptr_t*)pVar;
				const size_t off = (live && g > live) ? (size_t)(g + 8 - live) : 0;
				// in range, and it really is the checkpoint's variant (its name = the one the first copy had)
				if (off >= kVariantStart && off + kVariantSize <= data.size() && variantName(data.data() + off - kVariantStart) == nc)
				{
					std::memcpy(data.data() + off, hdr + kVariantStart, kVariantSize);
					PLOG_DEBUG << "H2Gametype: second variant copy at state +0x" << std::hex << off << " matched too";
				}
				else PLOG_WARNING << "H2Gametype: second variant copy not found - a later revert may restart the match";
			}
		}
		PLOG_INFO << "H2Gametype: checkpoint gametype \"" << nc << "\" matched to the session's \"" << ns << "\"";
		return skullNote.empty() ? std::format("gametype \"{}\" -> this session's \"{}\"", nc, ns)
			: std::format("gametype \"{}\" -> this session's \"{}\"; {}", nc, ns, skullNote);
	}
}
