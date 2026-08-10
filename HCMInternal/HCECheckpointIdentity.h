#pragma once
#include "pch.h"
#include <cstdint>
#include <string>
#include <vector>

// ================================================================================================================
// Halo Campaign Evolved checkpoint PLAYER IDENTITY - what makes a checkpoint shareable between players.
//
// THE PROBLEM THIS SOLVES, MEASURED RATHER THAN REASONED
// -----------------------------------------------------
// A checkpoint from another player loads and then kills you within a second or two. It is not rejected - it
// passes every header test the engine applies (build string, state CRC, scenario path, game options, session
// block, SHA-1), which is why it spawns at all. It dies afterwards, in the simulation.
//
// The cause is that the blob carries THE SAVING PLAYER'S IDENTITY, and not only in the header - it is baked into
// the live player structures inside the game state as well. Three separate captures were diffed to establish it:
//
//     GOOD (ours)  vs FOREIGN : 315/3088 4K blocks differ  (10.20%)  - two genuinely different run states
//     OURS-relabelled vs FOREIGN :  2/3088 blocks differ  ( 0.06%)  - the same capture, header edited
//
// A community "patched" copy of that foreign checkpoint had rewritten exactly ONE of the identity fields (id8b
// in the second header copy) plus the digest, and still died. Rewriting ALL of them made it work.
//
// THE FIVE FIELDS, AND WHICH ARE ACTUALLY IDENTITY
// -----------------------------------------------
// Established by comparing two checkpoints from DIFFERENT LEVELS (d20 and b30) saved by the same player:
//
//     id4    4 bytes   @ 0x390    identical across levels  -> PLAYER IDENTITY   ( 9 occurrences)
//     id8    8 bytes   @ 0x412    identical across levels  -> PLAYER IDENTITY   ( 6 occurrences)
//     id8b   8 bytes   @ 0x440    identical across levels  -> PLAYER IDENTITY   ( 6 occurrences)
//     name  32 bytes   @ 0x420    identical across levels  -> PLAYER IDENTITY   ( 8 occurrences)
//     tag   12 bytes   @ 0x478    DIFFERS between levels   -> PER-SAVE TOKEN    ( 5 occurrences)
//
// ⚠ `tag` is NOT a player identifier. The same player produced "C648" on one level and "I879" on another, so it
// is generated per save. It is rewritten anyway, deliberately: the configuration that was PROVEN to work in game
// rewrote all five, and reproducing a verified-working transform exactly is worth more than shaving one field on
// a theory. If it ever needs to stop being rewritten, that is a one-line change here - but do not make it on
// reasoning alone, make it on a test.
//
// ⚠ THE OFFSETS ABOVE ARE ONLY USED TO *LEARN* THE TWO IDENTITIES. The rewrite itself is a whole-blob
// search-and-replace of the resulting byte patterns, so occurrences buried in the live player structures (around
// 0x6A0000, 0x6BF000, 0x6C4000 and 0xC00008 in the captures examined) are caught without hardcoding a single one
// of them. That is what makes this survive a game update that moves the state layout: only the five header
// offsets have to hold, and those sit inside the fixed session header the engine itself compares.
//
// WHOLE-FIELD MATCHING, NOT TEXT MATCHING. The name and tag are fixed-size NUL-padded UTF-16 fields, and the
// whole field is used as the search pattern. Measured: matching the full 32-byte name field finds exactly the
// same 8 sites as matching just the 14-character text, on every file tested - so nothing is lost, and a 32-byte
// pattern cannot collide with unrelated data the way a 4-character one could.
//
// THE CALLER MUST RE-SIGN. Every byte of the blob is covered by the SHA-1 at +0x1ED20, so a rewrite invalidates
// it. HCECheckpointBlob::applySHA1 is the engine's own recipe and HCEInjectCheckpoint already calls it
// unconditionally after this runs - see the comment at that call site.
// ================================================================================================================
namespace HCECheckpointIdentity
{
	// Offsets into the checkpoint header. Inside the 0x1ED38-byte session header, which means the LIVE identity
	// is readable from HCECheckpointBlob::readLiveHeader() with no extra address to resolve.
	static constexpr int64_t kId4Offset = 0x390;
	static constexpr int64_t kId8Offset = 0x412;   // NOTE: not 8-byte aligned. These are packed records.
	static constexpr int64_t kNameOffset = 0x420;
	static constexpr int64_t kId8bOffset = 0x440;
	static constexpr int64_t kTagOffset = 0x478;

	static constexpr size_t kId4Length = 4;
	static constexpr size_t kId8Length = 8;
	static constexpr size_t kNameLength = 0x20;   // 15 UTF-16 chars + terminator; id8b begins at +0x20
	static constexpr size_t kTagLength = 0x0C;

	// The raw bytes of each field, exactly as they sit in the blob. Deliberately NOT decoded into integers and
	// strings: the rewrite is a byte-for-byte substitution and decoding would only add ways to be wrong about
	// encoding, padding and endianness.
	struct Identity
	{
		std::vector<byte> id4, id8, id8b, name, tag;

		bool complete() const
		{
			return id4.size() == kId4Length && id8.size() == kId8Length && id8b.size() == kId8Length
				&& name.size() == kNameLength && tag.size() == kTagLength;
		}

		bool operator==(const Identity& other) const
		{
			return id4 == other.id4 && id8 == other.id8 && id8b == other.id8b
				&& name == other.name && tag == other.tag;
		}
		bool operator!=(const Identity& other) const { return !(*this == other); }

		// The player-visible name, for messages. Best effort: the field is UTF-16 and may hold anything.
		std::string displayName() const
		{
			std::string out;
			for (size_t i = 0; i + 1 < name.size(); i += 2)
			{
				const uint16_t ch = (uint16_t)((uint8_t)name[i] | ((uint16_t)(uint8_t)name[i + 1] << 8));
				if (ch == 0) break;
				out += (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
			}
			return out;
		}
	};

	namespace detail
	{
		inline bool slice(const std::vector<byte>& blob, int64_t offset, size_t length, std::vector<byte>& out)
		{
			if (offset < 0 || (uint64_t)offset + length > blob.size()) return false;
			out.assign(blob.begin() + (ptrdiff_t)offset, blob.begin() + (ptrdiff_t)offset + (ptrdiff_t)length);
			return true;
		}

		// A pattern that is all-zero (or all-0xFF) would match enormous stretches of a 12 MB blob and turn a
		// rewrite into corruption. An identity field reading as either means we are not looking at an identity -
		// refuse the whole operation rather than half-apply it.
		inline bool degenerate(const std::vector<byte>& field)
		{
			if (field.empty()) return true;
			bool allZero = true, allOnes = true;
			for (byte b : field)
			{
				if ((uint8_t)b != 0x00) allZero = false;
				if ((uint8_t)b != 0xFF) allOnes = false;
			}
			return allZero || allOnes;
		}

		inline size_t replaceAll(std::vector<byte>& blob, const std::vector<byte>& from, const std::vector<byte>& to)
		{
			if (from.size() != to.size() || from.empty() || from == to) return 0;
			if (degenerate(from)) return 0;

			size_t count = 0;
			auto it = blob.begin();
			while (true)
			{
				it = std::search(it, blob.end(), from.begin(), from.end());
				if (it == blob.end()) break;
				std::copy(to.begin(), to.end(), it);
				it += (ptrdiff_t)to.size();
				++count;
			}
			return count;
		}
	}

	// Reads the five fields out of a blob's header. `blob` may be a whole checkpoint or just the live header -
	// every offset used is below 0x1ED38. Returns an Identity whose complete() is false if the buffer is short.
	inline Identity read(const std::vector<byte>& blob)
	{
		Identity id;
		detail::slice(blob, kId4Offset, kId4Length, id.id4);
		detail::slice(blob, kId8Offset, kId8Length, id.id8);
		detail::slice(blob, kId8bOffset, kId8Length, id.id8b);
		detail::slice(blob, kNameOffset, kNameLength, id.name);
		detail::slice(blob, kTagOffset, kTagLength, id.tag);
		return id;
	}

	struct RewriteResult
	{
		size_t id4 = 0, id8 = 0, id8b = 0, name = 0, tag = 0;
		size_t total() const { return id4 + id8 + id8b + name + tag; }
	};

	// Replaces every occurrence of `from`'s field values with `to`'s, across the WHOLE blob. Fields that are
	// already equal are skipped, so re-running this is a no-op and injecting one's own checkpoint changes nothing.
	//
	// ⚠ The caller MUST recompute the SHA-1 afterwards - see the header comment.
	inline RewriteResult rewrite(std::vector<byte>& blob, const Identity& from, const Identity& to)
	{
		RewriteResult result;
		if (!from.complete() || !to.complete()) return result;

		result.id4 = detail::replaceAll(blob, from.id4, to.id4);
		result.id8 = detail::replaceAll(blob, from.id8, to.id8);
		result.id8b = detail::replaceAll(blob, from.id8b, to.id8b);
		result.name = detail::replaceAll(blob, from.name, to.name);
		result.tag = detail::replaceAll(blob, from.tag, to.tag);
		return result;
	}
}
