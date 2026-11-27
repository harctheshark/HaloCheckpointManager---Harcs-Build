#pragma once
#include "pch.h"
#include "IMCCStateHook.h"
#include "ScopedCallback.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include <thread>

// Halo 2 (MCC 1.3528) player model swap, step 1 (2026-11-24): a READ-ONLY look at what the loaded map holds, written to
// the HCM log a few seconds after every Halo 2 level load. Nothing is written to the game and nothing is hooked; every
// read is fault-guarded (mem::copy). It answers, on the user's real game, the questions the tag-injection design depends
// on and that reading the code cannot: is tag pool B in use in campaign, how many tags / names the map has, what the
// player representation rows and each weapon's first-person entries point at, which bipeds exist.
// See Documents\Halo Mod And Tools\H2 Player Model Swap Research\REPORT.md. Log prefix: "H2 model probe:".
class H2TagProbe
{
private:
	// halo2.dll globals (REPORT.md; the pool / instance / header ones are also in H2CV_Halo2.h)
	static constexpr uintptr_t kMatgRVA = 0xE6F790;          // globals (matg) tag data*
	static constexpr uintptr_t kNameOffsetsRVA = 0x15E4B68;  // i32[tag index] -> offset into the name buffer (sub_1806C9EA0)
	static constexpr uintptr_t kNameBufferRVA = 0x15E4B78;
	static constexpr uint32_t kRepCountOff = 312, kRepBlockOff = 316, kRepRowSize = 188;   // sub_18069E580
	static constexpr uint32_t kWeaponFirstPersonBlockOff = 0x2A8;
	static constexpr uint32_t kPlayerSize = 548, kPlayerUnitOff = 44, kPlayerCharacterTypeOff = 0x80;

	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mMCCStateChangedCallback;
	std::jthread mWorker;

	struct Tags
	{
		uintptr_t base = 0, instances = 0, nameOffsets = 0, nameBuffer = 0;
		int32_t count = 0;
		h2::TagPools pools;

		bool read()
		{
			base = h2::base();
			if (!base) return false;
			const uintptr_t meta = mem::q(base + h2::rva::META_HEADER);
			instances = mem::q(base + h2::rva::TAG_INSTANCES);
			nameOffsets = mem::q(base + kNameOffsetsRVA);
			nameBuffer = mem::q(base + kNameBufferRVA);
			count = mem::okp(meta) ? mem::i32(meta + 24, 0) : 0;
			return mem::okp(instances) && count > 0 && count < 0x10000 && h2::tagPools(pools);
		}
		uint32_t group(int32_t index) const { return mem::u32(instances + (uintptr_t)index * 16, 0); }
		uint32_t datum(int32_t index) const { return mem::u32(instances + (uintptr_t)index * 16 + 4); }
		uintptr_t data(int32_t index) const { return h2::resolveTagOffset(mem::i32(instances + (uintptr_t)index * 16 + 8, -1), pools); }
		std::string name(int32_t index) const
		{
			if (index < 0 || index >= count || !mem::okp(nameOffsets) || !mem::okp(nameBuffer)) return "?";
			char buf[200]{};
			const int32_t off = mem::i32(nameOffsets + (uintptr_t)index * 4, -1);
			if (off < 0 || !mem::copy(buf, nameBuffer + (uintptr_t)off, sizeof(buf) - 1)) return "?";
			return std::string(buf);
		}
		// "group name" of a tag reference's datum (index = low word), or "(none)"
		std::string describe(uint32_t tagDatum) const
		{
			if (tagDatum == 0xFFFFFFFFu) return "(none)";
			const int32_t index = (int32_t)(tagDatum & 0xFFFF);
			if (index >= count) return std::format("(bad datum {:08X})", tagDatum);
			const uint32_t g = group(index);
			const char fourcc[5] = { (char)(g >> 24), (char)(g >> 16), (char)(g >> 8), (char)g, 0 };
			return std::format("{} {}{}", fourcc, name(index), datum(index) == tagDatum ? "" : " (salt differs)");
		}
	};

	static void dump()
	{
		Tags t;
		if (!t.read()) { PLOG_INFO << "H2 model probe: no tag table to read"; return; }

		int32_t used = 0, maxIndex = -1, weapons = 0, bipeds = 0;
		for (int32_t i = 0; i < t.count; ++i)
			if (t.datum(i) != 0xFFFFFFFFu && t.group(i) != 0xFFFFFFFFu) { ++used; maxIndex = i; }
		PLOG_INFO << std::format("H2 model probe: {} tag slots, {} used, highest index {}; instances at {:X}, tag data base {:X}, pool A {:X}, pool B {:X} ({}), names {:X} / {:X}",
			t.count, used, maxIndex, t.instances, mem::q(t.base + h2::rva::TAG_DATA_BASE), t.pools.a, t.pools.b, t.pools.b ? "IN USE" : "null",
			t.nameOffsets, t.nameBuffer);

		// player representation rows (matg): first-person hands, first-person body, third-person unit, variant
		const uintptr_t matg = mem::q(t.base + kMatgRVA);
		if (mem::okp(matg))
		{
			const int32_t rows = mem::i32(matg + kRepCountOff, 0);
			const uintptr_t first = h2::resolveTagOffset(mem::i32(matg + kRepBlockOff, -1), t.pools);
			PLOG_INFO << std::format("H2 model probe: {} player representation row(s) at {:X}", rows, first);
			for (int32_t r = 0; r < rows && r < 8 && first; ++r)
			{
				const uintptr_t row = first + (uintptr_t)r * kRepRowSize;
				PLOG_INFO << std::format("H2 model probe:   row {}: hands [{}], body [{}], unit [{}], variant {:08X}", r,
					t.describe(mem::u32(row + 4)), t.describe(mem::u32(row + 12)), t.describe(mem::u32(row + 180)), mem::u32(row + 184, 0));
			}
		}
		else PLOG_INFO << "H2 model probe: no globals tag pointer";

		// the players: character type and unit
		{
			const uintptr_t hdr = mem::q(t.base + h2::rva::PLAYERS_ARRAY);
			const int32_t size = mem::okp(hdr) ? mem::i32(hdr + 36, 0) : 0, bound = mem::okp(hdr) ? mem::i32(hdr + 60, 0) : 0;
			const int64_t off = mem::okp(hdr) ? mem::get<int64_t>(hdr + 72, 0) : 0;
			if (size == (int32_t)kPlayerSize && bound > 0 && bound <= 64 && off)
				for (int32_t i = 0; i < bound; ++i)
				{
					const uintptr_t p = hdr + (uintptr_t)off + (uintptr_t)i * kPlayerSize;
					if (mem::u16(p, 0) == 0) continue;
					const uint32_t unit = mem::u32(p + kPlayerUnitOff);
					std::string unitTag = "(no unit)";
					if (const uintptr_t obj = h2::objectFromDatum(unit)) unitTag = t.describe(mem::u32(obj));
					PLOG_INFO << std::format("H2 model probe:   player {}: character type {}, unit {:08X} = [{}]", i, (int)mem::u8(p + kPlayerCharacterTypeOff, 0xFF), unit, unitTag);
				}
		}

		// every biped tag, and every weapon's first-person block (two tag references a row: model, animations)
		for (int32_t i = 0; i < t.count; ++i)
		{
			const uint32_t g = t.group(i);
			if (g == 'bipd') { ++bipeds; PLOG_INFO << std::format("H2 model probe:   biped tag {} = {}", i, t.name(i)); }
			if (g != 'weap') continue;
			++weapons;
			const uintptr_t data = t.data(i);
			uintptr_t first = 0; uint32_t rows = 0;
			std::string line = std::format("H2 model probe:   weapon {} = {}:", i, t.name(i));
			if (!data || !h2::resolveBlock(data + kWeaponFirstPersonBlockOff, t.pools, first, rows) || rows == 0 || rows > 8) line += " no first-person rows";
			else
				for (uint32_t r = 0; r < rows; ++r)
					line += std::format(" fp{} [{}] + [{}]", r, t.describe(mem::u32(first + (uintptr_t)r * 16 + 4)), t.describe(mem::u32(first + (uintptr_t)r * 16 + 12)));
			PLOG_INFO << line;
		}
		PLOG_INFO << std::format("H2 model probe: done ({} biped tags, {} weapon tags)", bipeds, weapons);
	}

	void onMCCStateChanged(const MCCState& s)
	{
		if (s.currentGameState.operator GameState::Value() != GameState::Value::Halo2 || s.currentPlayState != PlayState::Ingame) return;
		// a few seconds after the level is in play, off the event's thread; a newer level load replaces a pending one
		mWorker = std::jthread([](std::stop_token stop)
			{
				for (int i = 0; i < 40 && !stop.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (stop.stop_requested()) return;
				try { dump(); }
				catch (...) { PLOG_ERROR << "H2 model probe: stopped by an exception"; }
			});
	}

public:
	explicit H2TagProbe(std::shared_ptr<IMCCStateHook> mccStateHook)
		: mMCCStateChangedCallback(mccStateHook->getMCCStateChangedEvent(), [this](const MCCState& s) { onMCCStateChanged(s); })
	{
	}
};
