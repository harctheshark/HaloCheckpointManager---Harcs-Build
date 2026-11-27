#include "pch.h"
#include "H2PlayerModelSwap.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "DirPathContainer.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"
#include "H2CV_Halo2.h"
#include "H2CV_Mem.h"
#include <Windows.h>
#include <fstream>

// ---- How the engine loads a map's tags (halo2.dll 1.3528; STAGE_B_DESIGN.md, checked in the decompile 2026-11-24) -----
//   sub_1806C9970 (tag loader): one allocation of exactly "mask + meta size"; the tags header, the group table, the tag
//     instance array (16 B: group, datum, data offset i32, size) and all tag data live in it. Sets
//       [+0x15E4B58] tags header (tag count i32 at +24, scenario datum +12)     [+0x15E4B30] instance array
//       [+0x15E4B38] tag data base (instance data offsets are relative to it)  [+0xE80AB0] pool A = the same base
//       [+0x15E4B60] u32 name count, [+0x15E4B68] i32 name offset per tag index  (the ENGINE'S HEAP - freed by the unloader)
//       [+0x15E4B70] u32 name buffer size, [+0x15E4B78] name buffer              (same)
//       [+0xE6F768] scenario tag data, [+0xE6F790] globals (matg) tag data
//   sub_180678950 (tag postprocess, no arguments): the biped / collision / creature / physics / vehicle fix-ups over every tag
//   sub_180036710 (after it): loads every bitmap tag's pixels and every render model section's geometry, by tag count
//   ... the first player spawn comes later.
//   sub_1806C9DD0 (unloader): frees the two name tables with the engine's heap.
//
// ---- What this does -----------------------------------------------------------------------------------------------
// A hook on the ENTRY of the tag postprocess: the tables are final, nothing has used them yet. There it
//   1. copies the pack's tag data into a block of its own a little above the tag data base (offsets must be positive i32),
//      and relocates the pack's block pointers to it;
//   2. builds a longer instance array and longer name tables (old entries copied, new ones appended) and repoints the
//      engine's pointers at them, tag count += N;
//   3. writes each new tag's own datum where the pack says (references between new tags, a bitmap's own datum, a render
//      model section's owner datum). Bitmap pixels need nothing else: bitmaps are read from the shared texture file by
//      group. Render model sections come with the "read from single_player_shared.map" bits already set in the pack;
//   4. wires it up: globals player row 0 (hands, body, unit), every weapon's first-person entry 0, and the scenario's
//      player start type 1 -> 0, so the engine takes exactly the path it takes on a chief level.
// A hook on the unloader puts the engine's own name tables back before it frees them, and frees ours after.
// Tag memory is reloaded from the map on every level load, so nothing has to be undone when the option is switched off -
// it simply does not inject at the next load.
//
// "Swap Test Stage" stops early so each step can be tried in game by itself:
//   2 = only the longer tables (no new tags)   3 = + bitmaps and shaders   4 = + render models   5 = + animations (all tags)
//   6 = + the wiring (the visible result)
namespace
{
	constexpr uintptr_t kTagsHeaderRVA = 0x15E4B58, kInstancesRVA = 0x15E4B30, kTagBaseRVA = 0x15E4B38, kPoolARVA = 0xE80AB0;
	constexpr uintptr_t kNameCountRVA = 0x15E4B60, kNameIndexRVA = 0x15E4B68, kNameSizeRVA = 0x15E4B70, kNameBufferRVA = 0x15E4B78;
	constexpr uintptr_t kScenarioRVA = 0xE6F768, kGlobalsRVA = 0xE6F790;
	constexpr uint32_t kRepCountOff = 0x138, kRepBlockOff = 0x13C, kRepRowSize = 0xBC;
	constexpr uint32_t kWeaponFirstPersonOff = 0x2A8, kWeaponFirstPersonElement = 0x10;
	constexpr uint32_t kStartsCountOff = 0x100, kStartsBlockOff = 0x104, kStartElement = 0x34, kStartTypeOff = 0x2C;

	struct EngineFn { uintptr_t rva; uint8_t bytes[15]; };
	constexpr EngineFn kPostprocess{ 0x678950, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40 } };
	constexpr EngineFn kUnload{ 0x6C9DD0, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xDB, 0x48, 0x89, 0x1D, 0x71, 0xAD, 0xF1, 0x00 } };

	bool matches(uintptr_t base, const EngineFn& f)
	{
		uint8_t now[sizeof(f.bytes)];
		return mem::copy(now, base + f.rva, sizeof(now)) && memcmp(now, f.bytes, sizeof(now)) == 0;
	}

	// ---- the pack (made offline by stageB\packtool; format v1, little-endian) ----
#pragma pack(push, 1)
	struct PackHeader
	{
		char magic[4]; uint32_t version;
		char targetMap[32];
		uint32_t targetTagCount, targetSignature;
		uint32_t tagCount, dataSize, namesSize;
		uint32_t blockFixupCount, selfRefCount, wireWeaponCount, reserved;
	};
	struct PackTag { uint32_t group, dataOffset, dataSize, nameOffset, flags, reserved; };
	struct PackSelfRef { uint32_t offset, tagIndex; };
	struct PackRep { uint32_t handsTag, bodyTag, unitDatum, variantStringId; };
	struct PackWireWeapon { uint32_t weaponDatum, fpModelDatum, animTag, animDatum; };
#pragma pack(pop)
	static_assert(sizeof(PackHeader) == 76 && sizeof(PackTag) == 24 && sizeof(PackRep) == 16 && sizeof(PackWireWeapon) == 16);
	// the pack's targetSignature = the u32 at offset 0x2F8 of the map file's header; the engine keeps a copy of the header
	constexpr uintptr_t kHeaderCopyRVA = 0xE70DB8;
	constexpr uint32_t kHeaderSignatureOff = 0x2F8;

	struct Pack
	{
		PackHeader header{};
		std::vector<PackTag> tags;
		std::vector<uint32_t> blockFixups;
		std::vector<PackSelfRef> selfRefs;
		PackRep rep{};
		std::vector<PackWireWeapon> weapons;
		std::vector<uint8_t> data;
		std::vector<char> names;

		// "" = fine, otherwise why the file is refused
		std::string load(const std::filesystem::path& path)
		{
			std::ifstream f(path, std::ios::binary);
			if (!f) return "the file could not be opened";
			std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			size_t at = 0;
			auto take = [&](void* dst, size_t n) { if (at + n > all.size()) return false; memcpy(dst, all.data() + at, n); at += n; return true; };
			if (!take(&header, sizeof(header)) || memcmp(header.magic, "H2SP", 4) != 0) return "it is not a player model swap pack";
			if (header.version != 1) return std::format("it is pack version {} (this build reads version 1)", header.version);
			if (header.tagCount == 0 || header.tagCount > 512 || header.dataSize > 64u * 1024 * 1024 || header.namesSize > 1024 * 1024
				|| header.blockFixupCount > 4'000'000 || header.selfRefCount > 1'000'000 || header.wireWeaponCount > 256) return "its header is out of range";
			tags.resize(header.tagCount); blockFixups.resize(header.blockFixupCount); selfRefs.resize(header.selfRefCount);
			weapons.resize(header.wireWeaponCount); data.resize(header.dataSize); names.resize(header.namesSize);
			if (!take(tags.data(), tags.size() * sizeof(PackTag)) || !take(blockFixups.data(), blockFixups.size() * 4)
				|| !take(selfRefs.data(), selfRefs.size() * sizeof(PackSelfRef)) || !take(&rep, sizeof(rep))
				|| !take(weapons.data(), weapons.size() * sizeof(PackWireWeapon)) || !take(data.data(), data.size())
				|| !take(names.data(), names.size())) return "it is cut short";
			if (names.empty() || names.back() != 0) return "its name table is not terminated";
			for (const auto& t : tags)
				if ((uint64_t)t.dataOffset + t.dataSize > data.size() || t.nameOffset >= names.size() || (t.dataOffset & 3)) return "a tag entry points outside the pack";
			for (uint32_t o : blockFixups)
			{
				if ((uint64_t)o + 4 > data.size()) return "a pointer fix-up is outside the pack";
				int32_t v; memcpy(&v, data.data() + o, 4);
				if (v < 0 || (uint32_t)v > data.size()) return "a pointer in the pack points outside it";
			}
			for (const auto& s : selfRefs)
				if ((uint64_t)s.offset + 4 > data.size() || s.tagIndex >= tags.size()) return "a datum fix-up is outside the pack";
			if (rep.handsTag >= tags.size() || rep.bodyTag >= tags.size()) return "its player row names a tag it does not hold";
			for (const auto& w : weapons)
				if (w.animTag != 0xFFFFFFFFu && w.animTag >= tags.size()) return "a weapon entry names a tag it does not hold";
			header.targetMap[31] = 0;
			return {};
		}
	};

	// which stage first includes a tag of this group
	int stageOfGroup(uint32_t group)
	{
		if (group == 'bitm' || group == 'shad') return 3;
		if (group == 'mode') return 4;
		return 5;
	}

	// memory at a positive 32-bit distance above `base` (tag data offsets are i32 added to the tag data base)
	void* allocAbove(uintptr_t base, size_t size)
	{
		for (uintptr_t p = (base + 0x01000000ull) & ~0xFFFFull; p + size < base + 0x7F000000ull; p += 0x00100000ull)
			if (void* m = VirtualAlloc((void*)p, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) return m;
		return nullptr;
	}
}

class H2PlayerModelSwap::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::filesystem::path mPackDir;

	ScopedCallback<eventpp::CallbackList<void(bool&)>> mToggleCallback;
	std::unique_ptr<ModuleInlineHook> mPostprocessHook, mUnloadHook;
	std::mutex mToggleMutex;
	bool mAutotest = false;   // player_model_swap\autotest_stage.txt existed when HCM started (see the constructor)

	// what is injected into the currently loaded map (all touched on the engine's map-load thread only)
	struct Injected
	{
		bool active = false;
		uintptr_t base = 0;
		void* window = nullptr;          // the new tags' data
		void* instances = nullptr;       // the longer instance array
		void* nameIndex = nullptr; void* nameBuffer = nullptr;
		uintptr_t oldNameIndex = 0, oldNameBuffer = 0; uint32_t oldNameCount = 0, oldNameSize = 0;
	} mInjected;

	static inline std::atomic<Impl*> sSelf{ nullptr };
	static inline std::atomic<bool> sEnabled{ false };

	void say(const std::string& s) { if (auto m = messagesGUIWeak.lock()) m->addMessage(s); PLOG_INFO << s; }

	static std::string tagName(uintptr_t base, int32_t index)
	{
		const uintptr_t idx = mem::q(base + kNameIndexRVA), buf = mem::q(base + kNameBufferRVA);
		if (index < 0 || (uint32_t)index >= mem::u32(base + kNameCountRVA, 0) || !mem::okp(idx) || !mem::okp(buf)) return {};
		char text[256]{};
		const int32_t off = mem::i32(idx + (uintptr_t)index * 4, -1);
		if (off < 0 || (uint32_t)off >= mem::u32(base + kNameSizeRVA, 0)) return {};
		const size_t n = std::min<size_t>(sizeof(text) - 1, mem::u32(base + kNameSizeRVA, 0) - (uint32_t)off);
		if (!mem::copy(text, buf + (uintptr_t)off, n)) return {};
		return std::string(text);
	}

	void freeInjected()
	{
		if (mInjected.window) VirtualFree(mInjected.window, 0, MEM_RELEASE);
		if (mInjected.instances) VirtualFree(mInjected.instances, 0, MEM_RELEASE);
		if (mInjected.nameIndex) VirtualFree(mInjected.nameIndex, 0, MEM_RELEASE);
		if (mInjected.nameBuffer) VirtualFree(mInjected.nameBuffer, 0, MEM_RELEASE);
		mInjected = {};
	}

	// The engine's map-load thread, at the entry of the tag postprocess. Returns a line for the log / the screen.
	std::string inject(int stage)
	{
		const uintptr_t base = h2::base();
		if (!base) return "Halo 2 is not loaded";
		const uintptr_t tagsHeader = mem::q(base + kTagsHeaderRVA), instances = mem::q(base + kInstancesRVA);
		const uintptr_t tagBase = mem::q(base + kTagBaseRVA), poolA = mem::q(base + kPoolARVA);
		if (!mem::okp(tagsHeader) || !mem::okp(instances) || !mem::okp(tagBase)) return "the map's tag table is not readable";
		if (mInjected.active && instances == (uintptr_t)mInjected.instances) return {};   // already done for this load
		if (mInjected.active) { PLOG_ERROR << "Player Model Swap: a previous map's injection was never unloaded; dropping it"; freeInjected(); }
		if (poolA != tagBase) return std::format("tag pool A ({:X}) is not the tag data base ({:X}); not touching this map", poolA, tagBase);

		const int32_t oldCount = mem::i32(tagsHeader + 24, 0);
		const uint32_t nameCount = mem::u32(base + kNameCountRVA, 0), nameSize = mem::u32(base + kNameSizeRVA, 0);
		const uintptr_t nameIndex = mem::q(base + kNameIndexRVA), nameBuffer = mem::q(base + kNameBufferRVA);
		if (oldCount <= 0 || oldCount > 0xF000 || (int32_t)nameCount != oldCount || !mem::okp(nameIndex) || !mem::okp(nameBuffer) || nameSize == 0 || nameSize > 64u * 1024 * 1024)
			return std::format("unexpected tag / name tables ({} tags, {} names, {} name bytes); not touching this map", oldCount, nameCount, nameSize);

		// which level: the scenario tag's name, last path part ("scenarios\solo\04a_gasgiant\04a_gasgiant")
		const uint32_t scenarioDatum = mem::u32(tagsHeader + 12);
		std::string scenario = tagName(base, (int32_t)(scenarioDatum & 0xFFFF));
		if (const auto slash = scenario.find_last_of('\\'); slash != std::string::npos) scenario = scenario.substr(slash + 1);
		if (scenario.empty()) return "the level's name could not be read";

		const auto packPath = mPackDir / (scenario + ".h2swap");
		std::error_code ec;
		if (!std::filesystem::exists(packPath, ec)) return std::format("no pack for this level ({}): it is left as it is", scenario);
		Pack pack;
		if (const std::string why = pack.load(packPath); !why.empty()) return std::format("the pack for {} was refused: {}", scenario, why);
		if ((int32_t)pack.header.targetTagCount != oldCount)
			return std::format("the pack for {} was made for a map with {} tags, this one has {}: not used", scenario, pack.header.targetTagCount, oldCount);
		// the datums baked into the pack are this exact map file's
		const uint32_t signature = mem::u32(base + kHeaderCopyRVA + kHeaderSignatureOff, 0);
		if (signature != pack.header.targetSignature)
			return std::format("the pack for {} was made for another build of the map (its signature {:08X}, this map's {:08X}): not used", scenario, pack.header.targetSignature, signature);

		// the tags this stage takes, and their indices / datums
		std::vector<int32_t> newIndexOfPackTag(pack.tags.size(), -1);
		std::vector<uint32_t> datumOfPackTag(pack.tags.size(), 0xFFFFFFFFu);
		int32_t added = 0;
		const uint32_t lastSalt = mem::u32(instances + (uintptr_t)(oldCount - 1) * 16 + 4) >> 16;
		for (size_t i = 0; i < pack.tags.size(); ++i)
		{
			if (stage < stageOfGroup(pack.tags[i].group)) continue;
			const int32_t index = oldCount + added++;
			const uint32_t salt = (lastSalt + (uint32_t)(index - (oldCount - 1))) & 0xFFFF;
			newIndexOfPackTag[i] = index;
			datumOfPackTag[i] = ((salt == 0 || salt == 0xFFFF ? 0x7A00u : salt) << 16) | (uint32_t)index;
		}
		if (oldCount + added > 0xFFF0) return "the map has no room for more tags";

		// ---- build everything aside first; the engine's pointers change only at the very end ----
		Injected inj;
		inj.base = base;
		inj.oldNameIndex = nameIndex; inj.oldNameBuffer = nameBuffer; inj.oldNameCount = nameCount; inj.oldNameSize = nameSize;
		const size_t windowSize = std::max<size_t>(pack.data.size(), 16);
		inj.window = allocAbove(tagBase, windowSize);
		inj.instances = VirtualAlloc(nullptr, (size_t)(oldCount + added) * 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		inj.nameIndex = VirtualAlloc(nullptr, (size_t)(oldCount + added) * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		inj.nameBuffer = VirtualAlloc(nullptr, (size_t)nameSize + pack.names.size() + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		auto fail = [&](std::string why) { std::swap(mInjected, inj); freeInjected(); return why; };
		if (!inj.window || !inj.instances || !inj.nameIndex || !inj.nameBuffer) return fail("no memory near the map's tags");
		const uintptr_t window = (uintptr_t)inj.window;
		const int64_t windowOffset = (int64_t)window - (int64_t)tagBase;   // positive, < 2 GB by construction

		// 1. the tag data, relocated
		memcpy(inj.window, pack.data.data(), pack.data.size());
		for (uint32_t o : pack.blockFixups)
		{
			int32_t v; memcpy(&v, (void*)(window + o), 4);
			v = (int32_t)(v + windowOffset);
			memcpy((void*)(window + o), &v, 4);
		}
		// 3. the new tags' own datums (a tag this stage leaves out = no tag)
		for (const auto& s : pack.selfRefs)
			memcpy((void*)(window + s.offset), &datumOfPackTag[s.tagIndex], 4);

		// 2. the longer instance array and name tables
		if (!mem::copy(inj.instances, instances, (size_t)oldCount * 16) || !mem::copy(inj.nameIndex, nameIndex, (size_t)oldCount * 4)
			|| !mem::copy(inj.nameBuffer, nameBuffer, nameSize)) return fail("the map's tables could not be copied");
		for (size_t i = 0; i < pack.tags.size(); ++i)
		{
			if (newIndexOfPackTag[i] < 0) continue;
			const PackTag& t = pack.tags[i];
			uint32_t entry[4] = { t.group, datumOfPackTag[i], (uint32_t)(int32_t)(windowOffset + t.dataOffset), t.dataSize };
			memcpy((uint8_t*)inj.instances + (size_t)newIndexOfPackTag[i] * 16, entry, 16);
			const int32_t nameOff = (int32_t)(nameSize + t.nameOffset);
			memcpy((uint8_t*)inj.nameIndex + (size_t)newIndexOfPackTag[i] * 4, &nameOff, 4);
		}
		memcpy((uint8_t*)inj.nameBuffer + nameSize, pack.names.data(), pack.names.size());

		// ---- commit: the engine's pointers ----
		const int32_t newCount = oldCount + added;
		const uint32_t newNameCount = (uint32_t)newCount, newNameSize = nameSize + (uint32_t)pack.names.size();
		bool ok = mem::put(base + kInstancesRVA, &inj.instances, 8);
		ok = ok && mem::put(base + kNameIndexRVA, &inj.nameIndex, 8) && mem::put(base + kNameBufferRVA, &inj.nameBuffer, 8);
		ok = ok && mem::put(base + kNameCountRVA, &newNameCount, 4) && mem::put(base + kNameSizeRVA, &newNameSize, 4);
		ok = ok && mem::put(tagsHeader + 24, &newCount, 4);
		inj.active = true;
		mInjected = inj;
		if (!ok) return "a write to the map's table pointers failed (the level may be unstable - restart it)";

		// 4. the wiring
		int weaponsWired = 0, startsChanged = 0; bool rowWritten = false;
		if (stage >= 6)
		{
			h2::TagPools pools;
			const uintptr_t matg = mem::q(base + kGlobalsRVA), scnr = mem::q(base + kScenarioRVA);
			if (h2::tagPools(pools) && mem::okp(matg) && mem::okp(scnr))
			{
				uintptr_t rows = 0; uint32_t rowCount = 0;
				if (h2::resolveBlock(matg + kRepCountOff, pools, rows, rowCount) && rowCount >= 1 && rows)
				{
					const uint32_t hands = datumOfPackTag[pack.rep.handsTag], body = datumOfPackTag[pack.rep.bodyTag];
					const uint32_t groupMode = 'mode', groupBiped = 'bipd';
					rowWritten = mem::put(rows + 0x00, &groupMode, 4) && mem::put(rows + 0x04, &hands, 4)
						&& mem::put(rows + 0x08, &groupMode, 4) && mem::put(rows + 0x0C, &body, 4)
						&& mem::put(rows + 0xB0, &groupBiped, 4) && mem::put(rows + 0xB4, &pack.rep.unitDatum, 4)
						&& mem::put(rows + 0xB8, &pack.rep.variantStringId, 4);
				}
				for (const auto& w : pack.weapons)
				{
					const int32_t index = (int32_t)(w.weaponDatum & 0xFFFF);
					if (index >= oldCount || mem::u32((uintptr_t)inj.instances + (size_t)index * 16 + 4) != w.weaponDatum) continue;
					const uintptr_t weapon = h2::resolveTagOffset(mem::i32((uintptr_t)inj.instances + (size_t)index * 16 + 8, -1), pools);
					uintptr_t first = 0; uint32_t count = 0;
					if (!weapon || !h2::resolveBlock(weapon + kWeaponFirstPersonOff, pools, first, count) || count < 1 || !first) continue;
					const uint32_t anim = w.animTag != 0xFFFFFFFFu ? datumOfPackTag[w.animTag] : w.animDatum;
					const uint32_t groupMode = 'mode', groupAnim = 'jmad';
					if (mem::put(first + 0x00, &groupMode, 4) && mem::put(first + 0x04, &w.fpModelDatum, 4)
						&& mem::put(first + 0x08, &groupAnim, 4) && mem::put(first + 0x0C, &anim, 4)) ++weaponsWired;
				}
				uintptr_t starts = 0; uint32_t startCount = 0;
				if (rowWritten && h2::resolveBlock(scnr + kStartsCountOff, pools, starts, startCount) && starts && startCount <= 256)
					for (uint32_t i = 0; i < startCount; ++i)
					{
						const uintptr_t type = starts + (uintptr_t)i * kStartElement + kStartTypeOff;
						const int16_t zero = 0;
						if (mem::get<int16_t>(type, -1) == 1 && mem::put(type, &zero, 2)) ++startsChanged;
					}
			}
		}
		return std::format("{}: {} tag(s) added at stage {} ({} -> {} tags, data at +{:X}){}", scenario, added, stage, oldCount, newCount, (uint64_t)windowOffset,
			stage >= 6 ? std::format("; player row {}, {} of {} weapons, {} start(s) set to the chief", rowWritten ? "written" : "NOT written", weaponsWired, pack.weapons.size(), startsChanged) : std::string("; nothing wired at this stage"));
	}

	static char postprocessDetour()
	{
		Impl* self = sSelf.load();
		// autotest: the file is read again at every level load (0 or anything else = leave this load alone), so the stage
		// can be changed between loads without restarting HCM
		int fileStage = -1;
		if (self && self->mAutotest)
		{
			std::ifstream f(self->mPackDir / "autotest_stage.txt");
			if (!(f >> fileStage)) fileStage = 0;
			PLOG_INFO << "Player Model Swap: autotest file says stage " << fileStage << " for this level load";
		}
		if (self && sEnabled.load() && (fileStage == -1 || (fileStage >= 2 && fileStage <= 6)))
		{
			int stage = 6;
			if (fileStage != -1) stage = fileStage;
			else if (auto settings = self->settingsWeak.lock()) stage = std::clamp(settings->h2PlayerModelSwapStage->GetValue(), 2, 6);
			std::string result;
			try { result = self->inject(stage); }
			catch (const std::exception& ex) { result = std::string("stopped by an error: ") + ex.what(); }
			if (!result.empty()) self->say("Player Model Swap: " + result);
		}
		return self && self->mPostprocessHook ? self->mPostprocessHook->getInlineHook().call<char>() : 0;
	}

	// the engine frees its name tables with its own heap: give it its own pointers back first, free ours after
	static int64_t unloadDetour()
	{
		Impl* self = sSelf.load();
		const bool mine = self && self->mInjected.active;
		if (mine)
		{
			const Injected& inj = self->mInjected;
			if (mem::q(inj.base + kNameIndexRVA) == (uintptr_t)inj.nameIndex)
			{
				mem::put(inj.base + kNameIndexRVA, &inj.oldNameIndex, 8);
				mem::put(inj.base + kNameCountRVA, &inj.oldNameCount, 4);
			}
			if (mem::q(inj.base + kNameBufferRVA) == (uintptr_t)inj.nameBuffer)
			{
				mem::put(inj.base + kNameBufferRVA, &inj.oldNameBuffer, 8);
				mem::put(inj.base + kNameSizeRVA, &inj.oldNameSize, 4);
			}
		}
		const int64_t r = self && self->mUnloadHook ? self->mUnloadHook->getInlineHook().call<int64_t>() : 0;
		if (mine) { self->freeInjected(); PLOG_INFO << "Player Model Swap: map unloaded, injected tags released"; }
		return r;
	}

	void onToggle(bool& on)
	{
		std::scoped_lock lk(mToggleMutex);
		auto settings = settingsWeak.lock();
		auto untick = [&]() { if (settings) { settings->h2PlayerModelSwapToggle->GetValue() = false; settings->h2PlayerModelSwapToggle->GetValueDisplay() = false; } };
		if (!on)
		{
			sEnabled = false;   // the hooks stay: a map that was injected must still be unloaded through ours
			say("Player Model Swap off - the level goes back to normal the next time it is loaded (restart the mission).");
			return;
		}
		const uintptr_t base = h2::base();
		if (!base) { say("Player Model Swap: load Halo 2 first."); untick(); return; }
		const bool installed = mPostprocessHook && mPostprocessHook->isHookInstalled() && mUnloadHook && mUnloadHook->isHookInstalled();
		if (!installed && (!matches(base, kPostprocess) || !matches(base, kUnload)))
		{
			say("Player Model Swap: this Halo 2 build is not supported (made for MCC 1.3528).");
			untick();
			return;
		}
		mPostprocessHook->setWantsToBeAttached(true);
		mUnloadHook->setWantsToBeAttached(true);
		sEnabled = true;
		std::error_code ec;
		const bool havePacks = std::filesystem::exists(mPackDir, ec);
		say(havePacks ? "Player Model Swap on (experimental) - it takes effect when a level is loaded: restart the mission. Levels without a pack are left as they are."
			: "Player Model Swap on, but the pack folder is missing: " + mPackDir.string());
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mPackDir(std::filesystem::path(dicon.Resolve<DirPathContainer>().lock()->dirPath) / "player_model_swap"),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h2PlayerModelSwapToggle->valueChangedEvent, [this](bool& on) { onToggle(on); })
	{
		if (game.operator GameState::Value() != GameState::Value::Halo2) throw HCMInitException("Player Model Swap is Halo 2 only");
		if (sSelf.load()) throw HCMInitException("Player Model Swap already exists");
		auto at = [](uintptr_t rva) { return std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(std::wstring_view(L"halo2.dll"), std::vector<int64_t>{ (int64_t)rva }); };
		mPostprocessHook = ModuleInlineHook::make(L"halo2.dll", at(kPostprocess.rva), (void*)&postprocessDetour, false);
		mUnloadHook = ModuleInlineHook::make(L"halo2.dll", at(kUnload.rva), (void*)&unloadDetour, false);
		sSelf = this;

		// Unattended testing (2026-11-24, the user asleep: "You likely need to start the game"): if
		// <HCM folder>\player_model_swap\autotest_stage.txt exists and holds a stage number 2..6, the option starts ON at
		// that stage with no click in the menu. Delete the file for normal use. (The build check of the toggle is skipped
		// here - halo2.dll is not loaded yet when HCM starts at the main menu - so only for the build this was made for.)
		std::error_code ec;
		const auto autotest = mPackDir / "autotest_stage.txt";
		if (std::filesystem::exists(autotest, ec))
		{
			mAutotest = true;
			if (auto settings = settingsWeak.lock())
			{
				settings->h2PlayerModelSwapToggle->GetValue() = true; settings->h2PlayerModelSwapToggle->GetValueDisplay() = true;
			}
			mPostprocessHook->setWantsToBeAttached(true);
			mUnloadHook->setWantsToBeAttached(true);
			sEnabled = true;
			PLOG_INFO << "Player Model Swap: AUTOTEST file found - hooks on from the start, the stage is read from the file at every level load";
		}
	}

	// a level is loaded with our tables in it
	bool holdsMap() const { return mInjected.active; }
	void disable() { sEnabled = false; }

	~Impl()
	{
		sEnabled = false;
		if (mPostprocessHook) mPostprocessHook->setWantsToBeAttached(false);
		if (mUnloadHook) mUnloadHook->setWantsToBeAttached(false);
		sSelf = nullptr;
	}
};

H2PlayerModelSwap::H2PlayerModelSwap(GameState gameImpl, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(gameImpl, dicon))
{
}

H2PlayerModelSwap::~H2PlayerModelSwap()
{
	// If a level is still loaded with injected tags in it, the engine still points at our tables and our unload hook is
	// what hands its own name tables back before it frees them. Taking the hooks or the memory away now would crash the
	// game at the next level change, so in that case this object is deliberately left alive (HCM's DLL stays loaded):
	// switched off, hooks in place, cleaning up by itself when the map unloads.
	if (pimpl && pimpl->holdsMap())
	{
		PLOG_ERROR << "Player Model Swap: HCM is closing while a level holds injected tags - the hooks and the tags are left in place until that level unloads";
		pimpl->disable();
		(void)pimpl.release();
	}
}
