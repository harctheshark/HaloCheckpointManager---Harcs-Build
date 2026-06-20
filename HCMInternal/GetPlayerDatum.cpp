#include "pch.h"
#include "GetPlayerDatum.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
class SimpleGetPlayerDatum : public GetPlayerDatum::IGetPlayerDatumImpl
{
private:
	GameState mGame; 

	//data
	std::shared_ptr<MultilevelPointer> playerDatumPointer;

public:
	SimpleGetPlayerDatum(GameState game, IDIContainer& dicon) : mGame(game)
	{
		playerDatumPointer = dicon.Resolve< PointerDataStore>().lock()->getData<std::shared_ptr<MultilevelPointer>>("playerDatum", mGame);
	}

	virtual Datum getPlayerDatum() override
	{
		Datum playerDatum; // inits to null datum
		if (!playerDatumPointer->readData(&playerDatum)) PLOG_ERROR << "Failed to read playerDatum: " << MultilevelPointer::GetLastError();
		return playerDatum;
	}
};

// Halo 2: the "playerDatum" pointer resolves to field +0x04 of entry[0] in a 16-entry, 0x44-stride
// player-control tracking array (its resolved address is the array base). entry[i].unit is the unit
// controlled by player index i. In campaign the local player is always player index 0, so entry[0] is
// correct - but in multiplayer the local player sits at its own network player index, so we must read
// entry[localPlayerIndex] instead. localPlayerIndex comes from the "localPlayerIndex" pointer
// (players-globals + 0x1C = engine sub_18069CFF0(0)). If that pointer is unavailable (build other than the
// verified one) or unreadable (not in a game yet), we fall back to index 0 == the original campaign behaviour.
class Halo2GetPlayerDatum : public GetPlayerDatum::IGetPlayerDatumImpl
{
private:
	GameState mGame;
	std::shared_ptr<MultilevelPointer> playerDatumPointer; // resolves to &trackingArray[0].unit
	std::optional<std::shared_ptr<MultilevelPointer>> localPlayerIndexPointer; // reads uint32 local->player index

	static constexpr uintptr_t kTrackingEntryStride = 0x44; // 68 bytes per player-control tracking entry
	static constexpr uint32_t kMaxPlayers = 16;

public:
	Halo2GetPlayerDatum(GameState game, IDIContainer& dicon) : mGame(game)
	{
		auto pointerStore = dicon.Resolve<PointerDataStore>().lock();
		playerDatumPointer = pointerStore->getData<std::shared_ptr<MultilevelPointer>>("playerDatum", mGame);

		// optional: only wired for verified builds. Missing -> fall back to index 0 (campaign-correct).
		try
		{
			localPlayerIndexPointer = pointerStore->getData<std::shared_ptr<MultilevelPointer>>("localPlayerIndex", mGame);
		}
		catch (HCMInitException& ex)
		{
			PLOG_INFO << "localPlayerIndex pointer unavailable for this Halo2 build; player info will assume local player index 0 (campaign only). " << ex.what();
			localPlayerIndexPointer = std::nullopt;
		}
	}

	virtual Datum getPlayerDatum() override
	{
		// resolve the base address of the tracking array (== address of entry[0].unit)
		uintptr_t trackingArrayBase;
		if (!playerDatumPointer->resolve(&trackingArrayBase))
		{
			PLOG_ERROR << "Failed to resolve playerDatum: " << MultilevelPointer::GetLastError();
			return Datum(); // null datum
		}

		// determine which player slot is the local player (0 in campaign, network index in multiplayer)
		uint32_t localPlayerIndex = 0;
		if (localPlayerIndexPointer.has_value())
		{
			uint32_t readIndex;
			if (localPlayerIndexPointer.value()->readData(&readIndex) && readIndex < kMaxPlayers)
				localPlayerIndex = readIndex;
			// else: leave at 0 (no local player yet / out of range) - campaign-correct fallback
		}

		uintptr_t datumAddress = trackingArrayBase + (kTrackingEntryStride * (uintptr_t)localPlayerIndex);
		if (IsBadReadPtr((void*)datumAddress, sizeof(Datum)))
		{
			PLOG_ERROR << std::format("Bad read of playerDatum at 0x{:X} (localPlayerIndex {})", datumAddress, localPlayerIndex);
			return Datum(); // null datum
		}

		return *(Datum*)datumAddress;
	}
};

GetPlayerDatum::GetPlayerDatum(GameState game, IDIContainer& dicon)
{
	switch (game)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<Halo2GetPlayerDatum>(game, dicon);
		return;

	case GameState::Value::Halo1:
	case GameState::Value::Halo3:
	case GameState::Value::Halo3ODST:
	case GameState::Value::HaloReach:
	case GameState::Value::Halo4:
		pimpl = std::make_unique<SimpleGetPlayerDatum>(game, dicon);
		return;



	default:
		throw HCMInitException("GetPlayerDatum not impl for this game yet");
	}
}

GetPlayerDatum::~GetPlayerDatum() = default;