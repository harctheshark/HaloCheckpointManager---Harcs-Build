#include "pch.h"
#include "SharedMemoryInternal.h"

#define nullCheck(x) 	if (!x) throw HCMRuntimeException(std::format("Could not find shm: {}", nameof(x)))

SelectedCheckpointData SharedMemoryInternal::getInjectInfo()
{
	auto* selectedCheckpointNull = segment.find<bool>("selectedCheckpointNull").first;
	auto* selectedCheckpointGame = segment.find<int>("selectedCheckpointGame").first;
	auto* selectedCheckpointName = segment.find<shm_string>("selectedCheckpointName").first;
	auto* selectedCheckpointFilePath = segment.find<shm_string>("selectedCheckpointFilePath").first;
	auto* selectedCheckpointLevelCode = segment.find<shm_string>("selectedCheckpointLevelCode").first;
	auto* selectedCheckpointGameVersion = segment.find<shm_string>("selectedCheckpointGameVersion").first;
	auto* selectedCheckpointDifficulty = segment.find<int>("selectedCheckpointDifficulty").first;

	nullCheck(selectedCheckpointNull);
	nullCheck(selectedCheckpointGame);
	nullCheck(selectedCheckpointName);
	nullCheck(selectedCheckpointFilePath);
	nullCheck(selectedCheckpointLevelCode);
	nullCheck(selectedCheckpointGameVersion);
	nullCheck(selectedCheckpointDifficulty);

	return SelectedCheckpointData
	{
		*selectedCheckpointNull,
		*selectedCheckpointGame,
		std::string(*selectedCheckpointName),
		std::string(*selectedCheckpointFilePath),
		std::string(*selectedCheckpointLevelCode),
		std::string(*selectedCheckpointGameVersion),
		*selectedCheckpointDifficulty,
	};


}

SelectedFolderData SharedMemoryInternal::getDumpInfo(GameState game)
{
	// ⚠ THE RUNNING GAME'S OWN FOLDER, WHATEVER TAB HCMEXTERNAL IS SHOWING. `game` is always the game that is actually
	// playing - every dump cheat is built per game and bails unless isGameCurrentlyPlaying(its game) - and HCMExternal
	// publishes every game's folder into these two arrays (see dumpFolderPathByGame in HCMInterproc's
	// SharedMemoryExternal.h). This used to demand that the VISIBLE tab match the running game, and failed with
	// "Wrong game tab selected" otherwise.
	// Slot index = the GameState int, which is what HCMExternal's HaloGame.ToInternalIndex() produces.
	const int gameIndex = (int)game;
	std::optional<SelectedFolderData> perGame;
	std::string lookupError;
	try
	{
		auto read = [&]()
			{
				auto names = segment.find<shm_string>("dumpFolderNameByGame");
				auto paths = segment.find<shm_string>("dumpFolderPathByGame");
				if (!names.first || !paths.first) return;   // an HCMExternal that predates the arrays
				if (gameIndex < 0 || (size_t)gameIndex >= names.second || (size_t)gameIndex >= paths.second) return;
				if (paths.first[gameIndex].empty()) return; // nothing published for this game yet
				perGame = SelectedFolderData{ std::string(names.first[gameIndex]), std::string(paths.first[gameIndex]) };
			};
		// Same lock HCMExternal's writer holds (setDumpFolderForGame), so the copy can never be torn by a reassign.
		segment.atomic_func(read);
	}
	catch (const std::exception& ex) { lookupError = ex.what(); }
	catch (...) { lookupError = "unknown error"; }

	if (perGame)
	{
		// Checked here rather than left to the file write: a remembered folder can be deleted in Explorer, and the MCC
		// dump otherwise only finds out as a vague "Could not create file". The path is UTF-8 (HCMExternal marshals
		// it that way), hence the wide conversion.
		std::error_code ec;
		if (!std::filesystem::is_directory(str_to_wstr(perGame->selectedFolderPath), ec) || ec)
			throw HCMRuntimeException(std::format("Cannot dump: the {} save folder does not exist:\n{}\n"
				"Pick a folder on HCMExternal's {} tab, or restart HCMExternal.",
				game.toString(), perGame->selectedFolderPath, game.toString()));
		return *perGame;
	}
	if (!lookupError.empty())
		PLOG_ERROR << "getDumpInfo: per-game folder lookup failed (" << lookupError << "), using HCMExternal's visible tab";

	// FALLBACK: the single visible-tab slot - only usable when that tab IS the running game. Reached only if
	// HCMExternal has not published this game (the moment between creating the segment and publishing every game).
	auto* selectedFolderGame = segment.find<int>("selectedFolderGame").first;
	auto* selectedFolderName = segment.find<shm_string>("selectedFolderName").first;
	auto* selectedFolderPath = segment.find<shm_string>("selectedFolderPath").first;

	nullCheck(selectedFolderGame);
	nullCheck(selectedFolderName);
	nullCheck(selectedFolderPath);

	if ((GameState)*selectedFolderGame != game)
		throw HCMRuntimeException(std::format("Cannot dump: HCMExternal has not published a save folder for {} (its visible tab is {}). "
			"Switch HCMExternal to the {} tab to dump there, or restart HCMExternal.",
			game.toString(), ((GameState)*selectedFolderGame).toString(), game.toString()));


	return SelectedFolderData
	{
		std::string(*selectedFolderName),
		std::string(*selectedFolderPath)
	};

}

void SharedMemoryInternal::setStatusFlag(HCMInternalStatus in) noexcept
{
	auto* HCMInternalStatusFlag = segment.find<int>("HCMInternalStatusFlag").first;

	if (!HCMInternalStatusFlag)
	{
		PLOG_ERROR << "HCMInternalStatusFlag was null";
		return;
	}

	*HCMInternalStatusFlag = (int)in;
}


bool SharedMemoryInternal::getAndClearInjectQueue()
{

	bool* injectCommandQueued = segment.find<bool>("injectCommandQueued").first;
	nullCheck(injectCommandQueued);

	if (*injectCommandQueued)
	{
		*injectCommandQueued = false;
		return true;
	}
	else
	{
		return false;
	}
}
