#include "pch.h"
#include "GetCurrentBSPSet.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include "TagBlockReader.h"
#include "GetScenarioAddress.h"
#include "DynamicStructFactory.h"
#include "IMakeOrGetCheat.h"


class GetCurrentBSPSet::GetCurrentBSPSetImpl
{
private:
	std::shared_ptr<MultilevelPointer> currentBSPSet;

	enum class scenarioTagDataFields { StructureBSPsTagBlock };
	std::shared_ptr<DynamicStruct<scenarioTagDataFields>> scenarioTagDataStruct;

	std::weak_ptr< TagBlockReader> tagBlockReaderWeak;
	std::weak_ptr<GetScenarioAddress> getScenarioAddressWeak;

	// Why the max-index plumbing is resolved OPTIONALLY: getCurrentBSPSet() - the only thing the Display 2D Info
	// "BSP Set" row calls - needs nothing but the currentBSPSet pointer. TagBlockReader, GetScenarioAddress and
	// scenarioTagDataFields are only used by getMaxBSPIndex() (Switch BSP Set). Hard-resolving them meant a game
	// that has the currentBSPSet pointer but not the whole tag-block reader (Halo 2 Anniversary MP) lost the BSP
	// Set readout entirely. getMaxBSPIndex() now throws a runtime error in that case instead.
	std::optional<std::string> maxBSPIndexUnavailableReason;
public:

	GetCurrentBSPSetImpl(GameState game, IDIContainer& dicon)
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		currentBSPSet = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(currentBSPSet), game);

		try
		{
			tagBlockReaderWeak = resolveDependentCheat(TagBlockReader);
			getScenarioAddressWeak = resolveDependentCheat(GetScenarioAddress);
			scenarioTagDataStruct = DynamicStructFactory::make< scenarioTagDataFields>(ptr, game);
		}
		catch (HCMInitException& ex)
		{
			maxBSPIndexUnavailableReason = ex.what();
			PLOG_INFO << "GetCurrentBSPSet for " << game.toString() << ": BSP set readout available, max BSP index is not (" << ex.what() << ")";
		}
	}
	const BSPSet getCurrentBSPSet()
	{
		BSPSet bspSet(0);
		if (!currentBSPSet->readData(&bspSet)) 
			throw HCMRuntimeException(std::format("Could not read currentBSPSet, {}", MultilevelPointer::GetLastError()));

		return bspSet;
	}

	const int getMaxBSPIndex()
	{
		if (maxBSPIndexUnavailableReason.has_value() || !scenarioTagDataStruct)
			throw HCMRuntimeException(std::format("Max BSP index is not available for this game: {}", maxBSPIndexUnavailableReason.value_or("scenario tag data not resolved")));

		lockOrThrow(getScenarioAddressWeak, getScenarioAddress);
		auto scenAddress = getScenarioAddress->getScenarioAddress();
		if (!scenAddress)
			throw scenAddress.error();

		scenarioTagDataStruct->currentBaseAddress = scenAddress.value();
		auto* pStructureBSPsTagBlock = scenarioTagDataStruct->field<uint32_t>(scenarioTagDataFields::StructureBSPsTagBlock);

		if (IsBadReadPtr(pStructureBSPsTagBlock, sizeof(uint32_t)))
			throw HCMRuntimeException(std::format("Bad read of pStructureBSPsTagBlock at 0x{:X}", (uintptr_t)pStructureBSPsTagBlock));

		lockOrThrow(tagBlockReaderWeak, tagBlockReader);
		auto StructureBSPsTagBlock = tagBlockReader->read((uintptr_t)pStructureBSPsTagBlock);

		if (!StructureBSPsTagBlock)
			throw StructureBSPsTagBlock.error();

		return StructureBSPsTagBlock.value().elementCount - 1;

	}
};




GetCurrentBSPSet::GetCurrentBSPSet(GameState game, IDIContainer& dicon)
{
	pimpl = std::make_unique< GetCurrentBSPSetImpl>(game, dicon);
}

GetCurrentBSPSet::~GetCurrentBSPSet() = default;

const BSPSet GetCurrentBSPSet::getCurrentBSPSet() { return pimpl->getCurrentBSPSet(); }
const int GetCurrentBSPSet::getMaxBSPIndex() { return pimpl->getMaxBSPIndex(); }