#include "pch.h"
#include "GetTagName.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "DynamicStructFactory.h"
#include "IMCCStateHook.h"
#include "MagicString.h"




// First gen (ie h1) stores an offset (nameOffset) to the tagNameString as part of the tagElementDataStructure. just add this offset to metaHeaderAddress (same thing as magicAddress in 1st gen) and boom there's ya string
class GetTagNameImplTagElement : public IGetTagNameImpl
{
private:

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	// callbacks
	ScopedCallback< eventpp::CallbackList<void(const MCCState&)>> MCCStateChangedCallback;
	// event to fire on mcc state change (just clear the caches)
	void onGameStateChange(const MCCState&)
	{
		PLOG_INFO << "clearing " << nameof(GetTagNameImplTagElement) << " caches";
		cacheValid = false;
	}

	//data
	enum class metaHeaderDataFields { tagTableOffset, numberOfTags, magic};
	std::shared_ptr<DynamicStruct<metaHeaderDataFields>> metaHeaderDataStruct;

	enum class tagElementDataFields { tagDatum, nameOffset};
	std::shared_ptr<StrideableDynamicStruct<tagElementDataFields>> tagElementDataStruct;

	std::shared_ptr<MultilevelPointer> metaHeaderAddress;
	int64_t tagBase;

	MagicString expectedMagic = MagicString("tags");

	// cache
	bool cacheValid = false;
	uintptr_t cachedMetaHeaderAddress;
	uintptr_t cachedTagTableAddress;
	int cachedTagCount;



public:
	GetTagNameImplTagElement(GameState game, IDIContainer& dicon)
		:
		MCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& n) { onGameStateChange(n); })
	{

		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		tagElementDataStruct = DynamicStructFactory::makeStrideable<tagElementDataFields>(ptr, game);
		metaHeaderDataStruct = DynamicStructFactory::make<metaHeaderDataFields>(ptr, game);
		metaHeaderAddress = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(metaHeaderAddress), game);
		tagBase = *ptr->getData<std::shared_ptr<int64_t>>(nameof(tagBase), game).get();
	}

	virtual const char* getTagName(Datum tagDatum) override  	// so get the tagElement of the tagDatum, lookup the nameOffset, add to the currentCacheAddress
	{  

		LOG_ONCE_CAPTURE(PLOG_DEBUG << "resolving tag name for tagDatum: " << tdatum, tdatum = tagDatum);

		if (!cacheValid)
		{
			LOG_ONCE(PLOG_DEBUG << "Resolving caches");

			// resolve meta header address
			if (!metaHeaderAddress->resolve(&cachedMetaHeaderAddress)) throw HCMRuntimeException(std::format("Could not resolve metaHeaderAddress, {}", MultilevelPointer::GetLastError()));
			metaHeaderDataStruct->currentBaseAddress = cachedMetaHeaderAddress;

			// validate magic
			MagicString observedMagic = *metaHeaderDataStruct->field<MagicString>(metaHeaderDataFields::magic);
			if (observedMagic != expectedMagic) throw HCMRuntimeException(std::format("metaHeaderTable magic mismatch! Expected: {}, observed: {}", expectedMagic.getString(), observedMagic.getString()));

			// resolve tag count
			cachedTagCount = *metaHeaderDataStruct->field<int32_t>(metaHeaderDataFields::numberOfTags);
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "cached tag count: " << tc, tc = cachedTagCount);

			// resolve tag table address
			int64_t tagTableOffset = *metaHeaderDataStruct->field<int32_t>(metaHeaderDataFields::tagTableOffset);
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "tagTableOffset: " << std::hex << tt, tt = tagTableOffset);
			tagTableOffset += tagBase;
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "tagTableOffset after adding tagbase: " << std::hex << tt, tt = tagTableOffset);
			cachedTagTableAddress = cachedMetaHeaderAddress + tagTableOffset;
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "cachedTagTableAddress: " << std::hex << tta, tta = cachedTagTableAddress);

			cacheValid = true;
		}

		if (tagDatum.index > cachedTagCount) throw HCMRuntimeException(std::format("tagDatumIndex out of range! index was {:X}, tagCount was {:X}", tagDatum.index, cachedTagCount));

		tagElementDataStruct->setIndex(cachedTagTableAddress, tagDatum.index);

		LOG_ONCE_CAPTURE(PLOG_DEBUG << "tagElementDataStruct at index: " << tdidx, tdidx = tagDatum.index); 
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "stride size is: " << ss, ss = tagElementDataStruct->StrideSize);
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "tagElement header address: 0x" << std::hex << taddy, taddy = tagElementDataStruct->currentBaseAddress);

		// datum at element should exactly match passed tagDatum, this is a safety check
		Datum* datumAtElement = tagElementDataStruct->field<Datum>(tagElementDataFields::tagDatum);
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "tag datum check address: " << std::hex << ptdatum, ptdatum = (uintptr_t)datumAtElement);
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "resolved datum: " << tdatum, tdatum = *datumAtElement);
		
		if (*datumAtElement != tagDatum) throw HCMRuntimeException(std::format("tagDatum mismatch! Expected {}, observed {}", tagDatum.toString(), datumAtElement->toString()));

		auto* nameOffset = tagElementDataStruct->field<uint32_t>(tagElementDataFields::nameOffset);
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "pNameOffset: " << std::hex << pno, pno = (uintptr_t)nameOffset);
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "nameOffset: " << no, no = *nameOffset);
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "tagBase: " << tb, tb = tagBase);
		uintptr_t nameAddress = cachedMetaHeaderAddress + *nameOffset + tagBase;
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "nameAddress: " << std::hex << na, na = nameAddress);

		if (IsBadReadPtr((void*)nameAddress, 8)) throw HCMRuntimeException(std::format("Bad read address for tagDatum {} at {}", tagDatum.toString(), nameAddress));

		return (const char*)nameAddress; // does this make a copy of the string? I don't want that. I don't think it does tho
	}
};







// game has a stringMetaTable that stores offsets into a stringTable. Index of the tagDatum is the index into stringMetaTable.
// We still check the metaHeader just to safety check that the tagIndex is less than the total tag count.
class GetTagNameImplStringMetaTableDirectPointer : public IGetTagNameImpl
{
private:
	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	// callbacks
	ScopedCallback< eventpp::CallbackList<void(const MCCState&)>> MCCStateChangedCallback;
	// event to fire on mcc state change (just clear the caches)
	void onGameStateChange(const MCCState&)
	{
		PLOG_INFO << "clearing " << nameof(GetTagNameImplTagElement) << " caches";
		cacheValid = false;
	}


	//data
	std::shared_ptr<MultilevelPointer> stringTable;
	std::shared_ptr<MultilevelPointer> stringMetaTable;
	std::shared_ptr<MultilevelPointer> metaHeaderAddress;

	enum class metaHeaderDataFields { numberOfTags, magic };
	std::shared_ptr<DynamicStruct<metaHeaderDataFields>> metaHeaderDataStruct;

	MagicString expectedMagic = MagicString("tags");

	// cache
	uint32_t cachedTagCount;
	uintptr_t cachedStringTable;
	uintptr_t cachedStringMetaTable;
	bool cacheValid = false;

public:
	GetTagNameImplStringMetaTableDirectPointer(GameState game, IDIContainer& dicon)
		:
		MCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& n) { onGameStateChange(n); })
	{

		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		metaHeaderDataStruct = DynamicStructFactory::make<metaHeaderDataFields>(ptr, game);
		metaHeaderAddress = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(metaHeaderAddress), game);
		stringTable = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(stringTable), game);
		stringMetaTable = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(stringMetaTable), game);
	}

	virtual const char* getTagName(Datum tagDatum) override  	// so get the tagElement of the tagDatum, lookup the nameOffset, add to the currentCacheAddress
	{
		if (!cacheValid)
		{
			LOG_ONCE(PLOG_DEBUG << "Resolving caches");

			// resolve meta header address
			uintptr_t pMetaHeader;
			if (!metaHeaderAddress->resolve(&pMetaHeader)) throw HCMRuntimeException(std::format("Could not resolve metaHeaderAddress, {}", MultilevelPointer::GetLastError()));
			metaHeaderDataStruct->currentBaseAddress = pMetaHeader;

			LOG_ONCE_CAPTURE(PLOG_DEBUG << "metaHeader resolved as: " << std::hex << pmh, pmh = pMetaHeader);
			// validate magic
			MagicString observedMagic = *metaHeaderDataStruct->field<MagicString>(metaHeaderDataFields::magic);
			if (observedMagic != expectedMagic) throw HCMRuntimeException(std::format("metaHeaderTable magic mismatch! Expected: {}, observed: {} at address {:X}", expectedMagic.getString(), observedMagic.getString(), (uintptr_t)metaHeaderDataStruct->field<MagicString>(metaHeaderDataFields::magic)));

			// resolve tag count
			cachedTagCount = *metaHeaderDataStruct->field<int32_t>(metaHeaderDataFields::numberOfTags);
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "cached tag count: " << tc, tc = cachedTagCount);  

			// resolve string table
			if (!stringTable->resolve(&cachedStringTable)) throw HCMRuntimeException(std::format("Could not resolve stringTable, {}", MultilevelPointer::GetLastError()));
			if (!stringMetaTable->resolve(&cachedStringMetaTable)) throw HCMRuntimeException(std::format("Could not resolve stringMetaTable, {}", MultilevelPointer::GetLastError()));
		}
	

		if (tagDatum.index > cachedTagCount) throw HCMRuntimeException(std::format("tagDatumIndex out of range! index was {:X}, tagCount was {:X}", tagDatum.index, cachedTagCount));

		uint32_t* pStringOffset = (uint32_t*)(cachedStringMetaTable + (4 * tagDatum.index));
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "reading string offset at " << std::hex << pso, pso = (uintptr_t)pStringOffset);
		uint32_t stringOffset = *pStringOffset;
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "string offset is " << std::hex << so, so = stringOffset);
		return (const char*)(cachedStringTable + stringOffset);
	}
};


// game has a stringMetaTable that stores offsets into a stringTable. Index of the tagDatum is the index into stringMetaTable.
// We still check the metaHeader just to safety check that the tagIndex is less than the total tag count.
class GetTagNameImplStringMetaTableAddOffset : public IGetTagNameImpl
{
private:
	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	// callbacks
	ScopedCallback< eventpp::CallbackList<void(const MCCState&)>> MCCStateChangedCallback;
	// event to fire on mcc state change (just clear the caches)
	void onGameStateChange(const MCCState&)
	{
		PLOG_INFO << "clearing " << nameof(GetTagNameImplTagElement) << " caches";
		cacheValid = false;
	}


	//data
	std::shared_ptr<MultilevelPointer> stringTable;
	std::shared_ptr<MultilevelPointer> stringMetaTableOffset;
	std::shared_ptr<MultilevelPointer> metaHeaderAddress;

	enum class metaHeaderDataFields { numberOfTags, magic };
	std::shared_ptr<DynamicStruct<metaHeaderDataFields>> metaHeaderDataStruct;

	MagicString expectedMagic = MagicString("tags");

	// cache
	uint32_t cachedTagCount;
	uintptr_t cachedStringTable;
	uintptr_t cachedStringMetaTable;
	bool cacheValid = false;

public:
	GetTagNameImplStringMetaTableAddOffset(GameState game, IDIContainer& dicon)
		:
		MCCStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(), [this](const MCCState& n) { onGameStateChange(n); })
	{

		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		metaHeaderDataStruct = DynamicStructFactory::make<metaHeaderDataFields>(ptr, game);
		metaHeaderAddress = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(metaHeaderAddress), game);
		stringTable = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(stringTable), game);
		stringMetaTableOffset = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(stringMetaTableOffset), game);
	}

	virtual const char* getTagName(Datum tagDatum) override  	// so get the tagElement of the tagDatum, lookup the nameOffset, add to the currentCacheAddress
	{
		if (!cacheValid)
		{
			LOG_ONCE(PLOG_DEBUG << "Resolving caches");

			// resolve meta header address
			uintptr_t pMetaHeader;
			if (!metaHeaderAddress->resolve(&pMetaHeader)) throw HCMRuntimeException(std::format("Could not resolve metaHeaderAddress, {}", MultilevelPointer::GetLastError()));
			metaHeaderDataStruct->currentBaseAddress = pMetaHeader;

			LOG_ONCE_CAPTURE(PLOG_DEBUG << "metaHeader resolved as: " << std::hex << pmh, pmh = pMetaHeader);
			// This runs from the Display 2D Info game-tick midhook, on the game's own thread, where an access
			// violation cannot be caught. Every raw read below is therefore probed first and turned into an
			// HCMRuntimeException (which the overlay treats as transient) if the memory is not readable - this is
			// what stands between a wrong/new game's pointer data (e.g. Halo 2 Anniversary MP) and a dead game.
			auto* pObservedMagic = metaHeaderDataStruct->field<MagicString>(metaHeaderDataFields::magic);
			if (IsBadReadPtr((void*)pObservedMagic, sizeof(MagicString))) throw HCMRuntimeException(std::format("metaHeaderTable magic not readable at {:X}", (uintptr_t)pObservedMagic));

			// validate magic
			MagicString observedMagic = *pObservedMagic;
			if (observedMagic != expectedMagic) throw HCMRuntimeException(std::format("metaHeaderTable magic mismatch! Expected: {}, observed: {} at address {:X}", expectedMagic.getString(), observedMagic.getString(), (uintptr_t)metaHeaderDataStruct->field<MagicString>(metaHeaderDataFields::magic)));

			// resolve tag count
			auto* pTagCount = metaHeaderDataStruct->field<int32_t>(metaHeaderDataFields::numberOfTags);
			if (IsBadReadPtr((void*)pTagCount, sizeof(int32_t))) throw HCMRuntimeException(std::format("metaHeaderTable tag count not readable at {:X}", (uintptr_t)pTagCount));
			cachedTagCount = *pTagCount;
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "cached tag count: " << tc, tc = cachedTagCount);

			// resolve string table
			if (!stringTable->resolve(&cachedStringTable)) throw HCMRuntimeException(std::format("Could not resolve stringTable, {}", MultilevelPointer::GetLastError()));
			uint32_t stringMetaTableOffsetResolved;
			if (!stringMetaTableOffset->readData(&stringMetaTableOffsetResolved)) throw HCMRuntimeException(std::format("Could not resolve stringMetaTable, {}", MultilevelPointer::GetLastError()));
			cachedStringMetaTable = cachedStringTable + stringMetaTableOffsetResolved;
			LOG_ONCE_CAPTURE(PLOG_DEBUG << "resolved cachedStringMetaTable: " << csmt, csmt = cachedStringMetaTable);
		}


		if (tagDatum.index > cachedTagCount) throw HCMRuntimeException(std::format("tagDatumIndex out of range! index was {:X}, tagCount was {:X}", tagDatum.index, cachedTagCount));

		uint32_t* pStringOffset = (uint32_t*)(cachedStringMetaTable + (4 * tagDatum.index));
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "reading string offset at " << std::hex << pso, pso = (uintptr_t)pStringOffset);
		if (IsBadReadPtr((void*)pStringOffset, sizeof(uint32_t))) throw HCMRuntimeException(std::format("tag string offset not readable at {:X}", (uintptr_t)pStringOffset));
		uint32_t stringOffset = *pStringOffset;
		LOG_ONCE_CAPTURE(PLOG_DEBUG << "string offset is " << std::hex << so, so = stringOffset);
		const char* tagName = (const char*)(cachedStringTable + stringOffset);
		// Bounded: the caller streams this string (strlen) on the game tick thread, so a wrong offset must not let
		// that walk off the end of a committed region. Require a NUL within the READABLE part of the first 256
		// bytes, measured page by page - demanding all 256 bytes up front rejected genuine short names that sit
		// near the end of their region.
		constexpr size_t kMaxTagName = 256;
		size_t readable = 0;
		while (readable < kMaxTagName)
		{
			const uintptr_t at = (uintptr_t)tagName + readable;
			const size_t chunk = (std::min)((size_t)(0x1000 - (at & 0xFFF)), kMaxTagName - readable);
			if (IsBadReadPtr((void*)at, chunk)) break;
			readable += chunk;
		}
		if (readable == 0) throw HCMRuntimeException(std::format("tag name string not readable at {:X}", (uintptr_t)tagName));
		if (strnlen(tagName, readable) == readable) throw HCMRuntimeException(std::format("tag name at {:X} is not terminated within {} readable bytes", (uintptr_t)tagName, readable));
		return tagName;
	}
};


GetTagName::GetTagName(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo1:
		pimpl = std::make_unique<GetTagNameImplTagElement>(gameImpl, dicon);
		break;

	case GameState::Value::Halo2:
		pimpl = std::make_unique<GetTagNameImplStringMetaTableDirectPointer>(gameImpl, dicon);
		break;

	case GameState::Value::Halo3:
		pimpl = std::make_unique<GetTagNameImplStringMetaTableDirectPointer>(gameImpl, dicon);
		break;

	case GameState::Value::Halo3ODST:
		pimpl = std::make_unique<GetTagNameImplStringMetaTableDirectPointer>(gameImpl, dicon);
		break;

	case GameState::Value::HaloReach:
		pimpl = std::make_unique<GetTagNameImplStringMetaTableAddOffset>(gameImpl, dicon);
		break;

	case GameState::Value::Halo4:
		pimpl = std::make_unique<GetTagNameImplStringMetaTableAddOffset>(gameImpl, dicon);
		break;

	// Halo 2 Anniversary MP (groundhog): the tag cache / string loader code matches Halo 4's strictly.
	case GameState::Value::Halo2MP:
		pimpl = std::make_unique<GetTagNameImplStringMetaTableAddOffset>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("Not impl yet");
	}
}

GetTagName::~GetTagName()
{
	PLOG_DEBUG << "~" << getName();
}


const char* GetTagName::getTagName(Datum tagDatum) { return pimpl->getTagName(tagDatum); }