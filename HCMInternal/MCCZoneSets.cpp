#include "pch.h"
#include "MCCZoneSets.h"
#include "GetCurrentZoneSet.h"
#include "GetCurrentBSPSet.h"
#include "GetScenarioAddress.h"
#include "TagBlockReader.h"
#include "DynamicStructFactory.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "IMakeOrGetCheat.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// See MCCZoneSets.h for where every value comes from.

namespace
{
	// ⚠ SEH-guarded reads. The string map and its nodes are heap memory the engine frees and rebuilds on every map load,
	// so a read can race a level change. These functions hold no C++ objects (SEH and unwinding do not mix).
	bool guardedCopy(void* dst, uintptr_t src, size_t len) noexcept
	{
		__try { memcpy(dst, (const void*)src, len); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// NUL-terminated string of at most maxLen chars. False if unreadable or not terminated in range.
	bool guardedCString(uintptr_t src, char* out, size_t maxLen) noexcept
	{
		__try
		{
			for (size_t i = 0; i < maxLen; ++i)
			{
				const char c = ((const char*)src)[i];
				out[i] = c;
				if (c == '\0') return true;
			}
			out[maxLen - 1] = '\0';
			return false;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	struct MapNode { int64_t key; uint32_t hash; uint32_t pad; uint64_t next; int32_t value; };
	static_assert(offsetof(MapNode, key) == 0x00 && offsetof(MapNode, next) == 0x10 && offsetof(MapNode, value) == 0x18);
}

class MCCZoneSets::Impl
{
private:
	GameState mGame;

	std::weak_ptr<GetCurrentZoneSet> getCurrentZoneSetWeak;
	std::weak_ptr<GetCurrentBSPSet> getCurrentBSPSetWeak;
	std::weak_ptr<GetScenarioAddress> getScenarioAddressWeak;
	std::weak_ptr<TagBlockReader> tagBlockReaderWeak;

	enum class scenarioTagDataFields { ZoneSetTagBlock };
	std::shared_ptr<DynamicStruct<scenarioTagDataFields>> scenarioStruct;

	enum class mccZoneSetFields { nameStringID, bspMask };
	std::shared_ptr<StrideableDynamicStruct<mccZoneSetFields>> zoneSetStruct;

	// Optional - names fall back to "zone set N" without them (a build whose string map was never located).
	enum class mccStringIDMapFields { bucketCount, buckets };
	std::shared_ptr<DynamicStruct<mccStringIDMapFields>> stringMapStruct;
	std::shared_ptr<MultilevelPointer> mccStringIDMap;    // resolves to the map object
	std::shared_ptr<MultilevelPointer> mccStringIDPool;   // resolves to the string pool base

	// ---- cache, rebuilt when the scenario changes ----
	std::mutex mMutex;
	uintptr_t mCachedFirstElement = 0;
	size_t mCachedCount = 0;
	uint32_t mCachedFirstNameId = 0;
	std::vector<std::string> mNames;
	std::vector<uint32_t> mBspMasks;

	struct Block { uintptr_t firstElement; size_t count; };

	Block readZoneSetBlock()
	{
		lockOrThrow(getScenarioAddressWeak, getScenarioAddress);
		auto scenario = getScenarioAddress->getScenarioAddress();
		if (!scenario) throw scenario.error();
		scenarioStruct->currentBaseAddress = scenario.value();

		auto* pBlock = scenarioStruct->field<uint32_t>(scenarioTagDataFields::ZoneSetTagBlock);
		if (IsBadReadPtr(pBlock, 8)) throw HCMRuntimeException(std::format("Bad read of the zone set tag block at {:X}", (uintptr_t)pBlock));

		lockOrThrow(tagBlockReaderWeak, tagBlockReader);
		auto block = tagBlockReader->read((uintptr_t)pBlock);
		if (!block) throw block.error();
		// Scenarios declare a handful (HCE ships at most 10, the tag limit is 64). Anything wild is a bad read.
		if (block.value().elementCount == 0 || block.value().elementCount > 128)
			throw HCMRuntimeException(std::format("Implausible zone set count {}", block.value().elementCount));
		return { block.value().firstElement, block.value().elementCount };
	}

	uint32_t readNameId(const Block& b, size_t i)
	{
		zoneSetStruct->setIndex(b.firstElement, (int)i);
		uint32_t id = 0;
		if (!guardedCopy(&id, (uintptr_t)zoneSetStruct->field<uint32_t>(mccZoneSetFields::nameStringID), 4))
			throw HCMRuntimeException(std::format("Bad read of zone set {}'s name", i));
		return id;
	}

	// Walk the engine's string_id -> offset map once and resolve every id in `wanted`. See the header for why it walks
	// rather than hashes. Missing ids are simply absent from the result.
	std::unordered_map<uint32_t, std::string> resolveNames(const std::unordered_set<uint32_t>& wanted)
	{
		std::unordered_map<uint32_t, std::string> out;
		if (!mccStringIDMap || !mccStringIDPool || !stringMapStruct || wanted.empty()) return out;

		uintptr_t map = 0, pool = 0;
		if (!mccStringIDMap->resolve(&map) || !map) return out;
		if (!mccStringIDPool->resolve(&pool) || !pool) return out;

		stringMapStruct->currentBaseAddress = map;
		uint32_t bucketCount = 0;
		if (!guardedCopy(&bucketCount, (uintptr_t)stringMapStruct->field<uint32_t>(mccStringIDMapFields::bucketCount), 4)) return out;
		// All four engines create it with 0xFF800 buckets. Refuse anything implausible rather than copy megabytes of garbage.
		if (bucketCount == 0 || bucketCount > 0x200000) { PLOG_ERROR << "MCCZoneSets: implausible string map bucket count " << bucketCount; return out; }

		std::vector<uint64_t> heads(bucketCount);
		if (!guardedCopy(heads.data(), (uintptr_t)stringMapStruct->field<uint64_t>(mccStringIDMapFields::buckets), (size_t)bucketCount * 8))
			return out;

		size_t visited = 0;
		constexpr size_t kMaxNodes = 0x200000;   // node pools are 0x7FC00 / 0x7FC00 entries; this is just a runaway guard
		for (uint64_t node : heads)
		{
			for (int depth = 0; node && depth < 256; ++depth)
			{
				if (++visited > kMaxNodes) return out;
				MapNode n{};
				if (!guardedCopy(&n, (uintptr_t)node, sizeof(n))) break;
				const uint32_t id = (uint32_t)n.key;
				if (n.value != -1 && wanted.contains(id) && !out.contains(id))
				{
					char buf[256];
					if (guardedCString(pool + (int64_t)n.value, buf, sizeof(buf)) && buf[0])
					{
						out.emplace(id, std::string(buf));
						if (out.size() == wanted.size()) return out;
					}
				}
				node = n.next;
			}
		}
		return out;
	}

	// Caller holds mMutex.
	void refreshLocked()
	{
		const Block b = readZoneSetBlock();
		const uint32_t firstId = readNameId(b, 0);
		if (b.firstElement == mCachedFirstElement && b.count == mCachedCount && firstId == mCachedFirstNameId && !mNames.empty())
			return;   // same scenario

		std::vector<uint32_t> ids(b.count);
		std::vector<uint32_t> masks(b.count);
		for (size_t i = 0; i < b.count; ++i)
		{
			ids[i] = readNameId(b, i);
			uint32_t mask = 0;
			if (!guardedCopy(&mask, (uintptr_t)zoneSetStruct->field<uint32_t>(mccZoneSetFields::bspMask), 4))
				throw HCMRuntimeException(std::format("Bad read of zone set {}'s BSP mask", i));
			masks[i] = mask;
		}

		const auto resolved = resolveNames(std::unordered_set<uint32_t>(ids.begin(), ids.end()));
		std::vector<std::string> names(b.count);
		for (size_t i = 0; i < b.count; ++i)
		{
			auto it = resolved.find(ids[i]);
			names[i] = it != resolved.end() ? it->second : std::format("zone set {}", i);
		}

		mNames = std::move(names);
		mBspMasks = std::move(masks);
		mCachedFirstElement = b.firstElement;
		mCachedCount = b.count;
		mCachedFirstNameId = firstId;
		PLOG_DEBUG << "MCCZoneSets(" << mGame.toString() << "): " << mNames.size() << " zone sets, "
			<< resolved.size() << " names resolved, first '" << mNames[0] << "'";
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		getCurrentZoneSetWeak(resolveDependentCheat(GetCurrentZoneSet)),
		getCurrentBSPSetWeak(resolveDependentCheat(GetCurrentBSPSet)),
		getScenarioAddressWeak(resolveDependentCheat(GetScenarioAddress)),
		tagBlockReaderWeak(resolveDependentCheat(TagBlockReader))
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		scenarioStruct = DynamicStructFactory::make<scenarioTagDataFields>(ptr, game);
		zoneSetStruct = DynamicStructFactory::makeStrideable<mccZoneSetFields>(ptr, game);

		try
		{
			stringMapStruct = DynamicStructFactory::make<mccStringIDMapFields>(ptr, game);
			mccStringIDMap = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(mccStringIDMap), game);
			mccStringIDPool = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(mccStringIDPool), game);
		}
		catch (const std::exception& ex)
		{
			stringMapStruct.reset(); mccStringIDMap.reset(); mccStringIDPool.reset();
			PLOG_INFO << "MCCZoneSets(" << game.toString() << "): no string map for this build, zone sets will be listed by index (" << ex.what() << ")";
		}
	}

	std::vector<std::string> getZoneSetNames()
	{
		std::scoped_lock lock(mMutex);
		refreshLocked();
		return mNames;
	}

	int getCurrentZoneSet()
	{
		lockOrThrow(getCurrentZoneSetWeak, getCurrentZoneSet);
		return (int32_t)getCurrentZoneSet->getCurrentZoneSet();
	}

	bool isZoneSetFullyLoaded(int index)
	{
		uint32_t mask = 0;
		{
			std::scoped_lock lock(mMutex);
			refreshLocked();
			if (index < 0 || (size_t)index >= mBspMasks.size()) return false;
			mask = mBspMasks[(size_t)index];
		}
		lockOrThrow(getCurrentBSPSetWeak, getCurrentBSPSet);
		const uint32_t loaded = (uint32_t)getCurrentBSPSet->getCurrentBSPSet().to_ulong();
		return (mask & ~loaded) == 0;
	}

	std::string describeCurrentZoneSet()
	{
		const int current = getCurrentZoneSet();
		if (current < 0) return "none";
		std::vector<std::string> names;
		try { names = getZoneSetNames(); }
		catch (HCMRuntimeException&) { return std::to_string(current); }
		if ((size_t)current >= names.size()) return std::to_string(current);
		std::string out = std::format("{}: {}", current, names[(size_t)current]);
		try { if (!isZoneSetFullyLoaded(current)) out += " (loading)"; }
		catch (HCMRuntimeException&) {}
		return out;
	}
};


MCCZoneSets::MCCZoneSets(GameState game, IDIContainer& dicon)
{
	switch (game)
	{
	case GameState::Value::Halo3:
	case GameState::Value::Halo3ODST:
	case GameState::Value::HaloReach:
	case GameState::Value::Halo4:
		pimpl = std::make_unique<Impl>(game, dicon);
		break;
	default:
		throw HCMInitException("MCCZoneSets is only for Halo 3, ODST, Reach and Halo 4");
	}
}
MCCZoneSets::~MCCZoneSets() = default;

std::vector<std::string> MCCZoneSets::getZoneSetNames() { return pimpl->getZoneSetNames(); }
int MCCZoneSets::getCurrentZoneSet() { return pimpl->getCurrentZoneSet(); }
bool MCCZoneSets::isZoneSetFullyLoaded(int index) { return pimpl->isZoneSetFullyLoaded(index); }
std::string MCCZoneSets::describeCurrentZoneSet() { return pimpl->describeCurrentZoneSet(); }


// ---- bridge -----------------------------------------------------------------------------------------------------
namespace
{
	struct BridgeEntry { std::weak_ptr<MCCZoneSets> info; bool switchRegistered = false; int selection = 0; };
	std::mutex g_bridgeMutex;
	std::map<GameState::Value, BridgeEntry> g_bridge;

	std::shared_ptr<MCCZoneSets> lockInfo(GameState game)
	{
		std::scoped_lock lock(g_bridgeMutex);
		auto it = g_bridge.find(game);
		if (it == g_bridge.end() || !it->second.switchRegistered) return nullptr;
		return it->second.info.lock();
	}
}

namespace MCCZoneSetBridge
{
	bool isUsable(GameState game) { return lockInfo(game) != nullptr; }

	std::vector<std::string> names(GameState game)
	{
		auto info = lockInfo(game);
		if (!info) return {};
		try { return info->getZoneSetNames(); }
		catch (HCMRuntimeException&) { return {}; }   // no level loaded - normal at a menu or mid-load
	}

	int currentIndex(GameState game)
	{
		auto info = lockInfo(game);
		if (!info) return -1;
		try { return info->getCurrentZoneSet(); }
		catch (HCMRuntimeException&) { return -1; }
	}

	int selection(GameState game)
	{
		std::scoped_lock lock(g_bridgeMutex);
		return g_bridge[game].selection;
	}

	void setSelection(GameState game, int index)
	{
		std::scoped_lock lock(g_bridgeMutex);
		g_bridge[game].selection = index;
	}

	void registerSwitch(GameState game, std::weak_ptr<MCCZoneSets> info)
	{
		std::scoped_lock lock(g_bridgeMutex);
		auto& e = g_bridge[game];
		e.info = info;
		e.switchRegistered = true;
	}

	void unregisterSwitch(GameState game)
	{
		std::scoped_lock lock(g_bridgeMutex);
		auto& e = g_bridge[game];
		e.info.reset();
		e.switchRegistered = false;
	}
}
