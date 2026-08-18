#include "pch.h"
#include "HCEDumpCheckpoint.h"
#include "HCECheckpointBlob.h"
#include "HCECheckpointDetours.h"
#include "HCEGetPlayerState.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "ISharedMemory.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "IMakeOrGetCheat.h"
#include "DirPathContainer.h"
#include "ModalDialogRenderer.h"
#include "ModalDialogFactory.h"
#include <atomic>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <ctime>

// See HCEDumpCheckpoint.h for why this is not a branch inside DumpCheckpoint.h, and HCECheckpointBlob.h for the
// checkpoint layout and the publication barrier this relies on.

namespace
{
	// THE FALLBACK folder, and where dumps used to go unconditionally. Dumps now land in the save folder
	// HCMExternal's Halo Campaign Evolved tab has selected - <HCM dir>\Saves\Halo Campaign Evolved\... - which is
	// the same convention all six MCC tabs use and, more to the point, the only folder the tab actually lists. HCM
	// falls back here whenever that folder is unavailable (no shared memory, HCMExternal on another game's tab, an
	// older HCMExternal with no HaloCER tab at all), which is exactly what this feature did before the tab existed.
	//
	// ⚠ EXISTING FILES ARE NOT MIGRATED. Anything already in this folder stays here, stays valid, and stays
	// injectable - HCEInjectCheckpoint's browse dialog deliberately opens here while it still holds .bin files and
	// the tab's folder does not. Dragging them into Saves\Halo Campaign Evolved\ makes them appear in the tab.
	constexpr const char* kLegacyDumpSubfolder = "HaloCER Checkpoints";

	// Long enough for "<level>_<timestamp>" plus room for a hand-typed name; short enough that the whole path
	// stays well inside MAX_PATH even under a deep HCM install.
	constexpr size_t kMaxNameLength = 96;

	// Anything Windows will not accept in a file name, plus the characters that would let a typed name escape the
	// dump folder. Control characters go too - the level name comes out of game memory.
	std::string sanitiseFileName(const std::string& in)
	{
		std::string out;
		out.reserve(in.size());
		for (unsigned char c : in)
		{
			if (c < 0x20 || c == 0x7F) continue;
			switch (c)
			{
			case '\\': case '/': case ':': case '*': case '?':
			case '"': case '<': case '>': case '|':
				continue;
			default:
				break;
			}
			out.push_back((char)c);
		}

		// Trailing dots and spaces are legal to type but not to create.
		while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
		while (!out.empty() && out.front() == ' ') out.erase(out.begin());

		if (out.size() > kMaxNameLength) out.resize(kMaxNameLength);
		while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
		return out;
	}

	// Local time, not UTC: this ends up in a file name the user reads, and MCC's UTC timestamps are a known
	// annoyance rather than something to reproduce.
	std::string timestamp()
	{
		std::time_t now = std::time(nullptr);
		std::tm local{};
		if (localtime_s(&local, &now) != 0) return "unknown_time";
		char buffer[32]{};
		if (std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &local) == 0) return "unknown_time";
		return std::string(buffer);
	}
}


class HCEDumpCheckpoint::HCEDumpCheckpointImpl
{
private:
	GameState mGame;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<ModalDialogRenderer> modalDialogsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// SOFT dependency. HCMExternal's Halo Campaign Evolved tab - specifically the save folder it has selected, so
	// a dump lands where the tab is looking. resolveDumpFolder() falls back to the legacy folder for every reason
	// this could be unusable, so a dump can never fail because of it.
	std::weak_ptr<ISharedMemory> sharedMemWeak;

	// HARD dependency, unlike the player state below. On the PROVIDER path - which is what the shipped game uses -
	// this is the only way to reach a checkpoint blob at all, so a dump without it could never work. Constructing
	// it patches nothing; the hooks still attach lazily on first use.
	std::weak_ptr<HCECheckpointDetours> checkpointDetoursWeak;

	// OPTIONAL on purpose: the only thing it provides here is the level name in the auto-generated file name.
	// A dump must not become impossible because the TLS walk is unavailable.
	std::optional<std::weak_ptr<HCEGetPlayerState>> playerStateOptionalWeak;

	// Everything that knows the checkpoint layout. Constructing it is pure PointerDataStore lookups.
	HCECheckpointBlob mBlob;

	// <HCM dir>, from the DI container - the same string ReplayRecorder builds <HCM dir>\replays\ out of.
	std::string mHCMDirPath;

	// Two rapid hotkey presses must not race each other into the same file name (or run two 12 MB copies at once).
	std::mutex mDumpMutex;

	// Set as the last statement of the constructor, cleared as the first of the destructor. The ScopedCallback is
	// declared LAST so it subscribes after every member above exists, but the constructor body still runs
	// afterwards and createCheats builds cheats on detached threads while the hotkey thread is already live.
	std::atomic<bool> mReady{ false };

	std::string defaultCheckpointName()
	{
		std::string level;
		if (playerStateOptionalWeak.has_value())
		{
			// Best effort. Throws at a menu, mid load, and on any build where the level string has moved.
			try
			{
				if (auto playerState = playerStateOptionalWeak.value().lock())
					level = sanitiseFileName(playerState->getCurrentLevelName());
			}
			catch (HCMRuntimeException) {}
		}

		if (level.empty()) level = "HaloCER";
		return std::format("{}_{}", level, timestamp());
	}

	// PROVIDER PATH ONLY. Makes sure HCM is shadowing checkpoints, and refuses - with the reason - if there is
	// nothing shadowed to write. Runs BEFORE the name dialog, so the user is never asked to name a file that
	// cannot be produced.
	//
	// ⚠ THIS IS THE FUNCTION THAT REPLACED THE AUTO-FORCE. The old dump forced a checkpoint so a hand-off would
	// happen while it watched; that silently moved the player's checkpoint, which is the opposite of what a dump
	// is for. It now refuses and says what to do instead.
	void requireShadowedCheckpoint()
	{
		lockOrThrow(checkpointDetoursWeak, checkpointDetours);

		// Idempotent and cheap once running. It is here (rather than only on level load) so the FIRST dump also
		// works when HCM was injected mid-level, or when the user has only just switched shadowing on. It will
		// still report "nothing yet" below, because a checkpoint has to actually happen before there is one.
		checkpointDetours->armShadowCapture();

		const HCECheckpointDetours::ShadowStatus shadow = checkpointDetours->getShadowStatus();

		if (!shadow.wanted)
			throw HCMRuntimeException(
				"HCM is not keeping a copy of your checkpoints, so there is nothing to dump.\n"
				"Turn on \"Keep last checkpoint ready to dump\" under Dump Checkpoint Settings, then reach a checkpoint.");

		if (!shadow.armed)
			throw HCMRuntimeException(
				"HCM could not start watching Halo Campaign Evolved's checkpoints - load a level first, then try again.");

		if (shadow.overlongLength != 0)
			throw HCMRuntimeException(std::format(
				"Halo Campaign Evolved handed its storage provider 0x{:X} bytes, which is more than HCM had room for.\n"
				"Nothing was dumped - the game state size changed underneath HCM. Reloading the level will fix it.",
				shadow.overlongLength));

		if (!shadow.haveCheckpoint)
			throw HCMRuntimeException(
				"No checkpoint has occurred yet, so there is nothing to dump.\n"
				"Play to a checkpoint, or use Force Checkpoint, then dump.");
	}

	// ============================================================================================================
	// WHERE THE FILE GOES.
	//
	// The save folder HCMExternal's Halo Campaign Evolved tab has selected, so the dump appears in the tab the
	// instant it is written (HCMExternal watches Saves\ with a FileSystemWatcher) and subfolders work exactly like
	// they do for the MCC games. Falls back to <HCM dir>\HaloCER Checkpoints\ - what this feature used
	// unconditionally before the tab existed - for every reason the tab's folder is unavailable.
	//
	// ⚠ getDumpInfo THROWS when a different game's tab is selected. That is its job, not a bug, and it is the
	// common case when the user is looking at Halo 2 with HaloCER running. Catching it and falling back is what
	// makes this an improvement rather than a new way for a dump to fail.
	// ============================================================================================================
	// outUsedLegacyFolder tells the caller to SAY SO in the dump message. Without that, a dump that fell back is
	// indistinguishable from one that worked right up until the user goes looking for it on the tab and it is not
	// there - which is the whole reason this used to be a fixed folder.
	std::filesystem::path resolveDumpFolder(bool& outUsedLegacyFolder)
	{
		outUsedLegacyFolder = true;
		try
		{
			auto sharedMem = sharedMemWeak.lock();
			if (sharedMem)
			{
				const SelectedFolderData folder = sharedMem->getDumpInfo(mGame);
				if (!folder.selectedFolderPath.empty())
				{
					const std::filesystem::path path(folder.selectedFolderPath);
					std::error_code ec;
					if (std::filesystem::is_directory(path, ec) && !ec)
					{
						PLOG_DEBUG << "Dumping into HCMExternal's selected save folder: " << path.string();
						outUsedLegacyFolder = false;
						return path;
					}
					PLOG_WARNING << "HCMExternal's selected save folder does not exist: " << path.string();
				}
			}
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_DEBUG << "No HaloCER save folder from HCMExternal (" << ex.what() << "), using the legacy dump folder";
		}
		catch (const std::exception& ex)
		{
			PLOG_DEBUG << "No HaloCER save folder from HCMExternal: " << ex.what() << ", using the legacy dump folder";
		}

		return std::filesystem::path(mHCMDirPath) / kLegacyDumpSubfolder;
	}

	// primary event callback
	void onDump()
	{
		if (!mReady.load(std::memory_order_acquire))
		{
			PLOG_DEBUG << "HCEDumpCheckpoint not ready yet, ignoring dump request";
			return;
		}

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(settingsWeak, settings);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			PLOG_DEBUG << "onDump called for game: " << mGame.toString();

			// try_lock, not lock: the name dialog below is a BLOCKING call, so a second press while it is up must
			// drop rather than queue up another dialog (or a second 12 MB copy) behind it.
			std::unique_lock<std::mutex> lock(mDumpMutex, std::try_to_lock);
			if (!lock.owns_lock()) { PLOG_DEBUG << "A HaloCER checkpoint dump is already in progress, ignoring"; return; }

			// Validate BEFORE asking the user for a name - being told "there is nothing to dump" after typing one
			// would be miserable. This is a cheap read of a handful of globals, not the 12 MB copy.
			const HCECheckpointBlob::StorageMode storage = mBlob.detectStorageMode();
			const bool providerPath = (storage == HCECheckpointBlob::StorageMode::Provider);

			// Both paths need this, and on the provider path it is also the size the shadow must agree with.
			const int32_t stateLength = mBlob.readStateLength();

			if (providerPath)
			{
				// There is no ring to interrogate. The one precondition is that a game state exists at all,
				// which readStateBlockAddress reports as "load a level first".
				(void)mBlob.readStateBlockAddress();
				requireShadowedCheckpoint();
			}
			else
			{
				const HCECheckpointBlob::Snapshot probe = mBlob.readSnapshot();
				if (!probe.systemInitialised)
					throw HCMRuntimeException("Halo Campaign Evolved's checkpoint system is not running yet - load a level first.");
				if (!probe.anyCheckpointExists)
					throw HCMRuntimeException("Halo Campaign Evolved has no checkpoint yet - reach one (or use Force Checkpoint) first.");
			}

			std::string checkpointName = defaultCheckpointName();

			// Same setting (and so the same preset entry) as the MCC dump - HaloCER and the MCC games can never be
			// the same process, so only one of the two features exists at a time.
			if (settings->autonameCheckpoints->GetValue() == false)
			{
				lockOrThrow(modalDialogsWeak, modalDialogs);
				PLOG_DEBUG << "calling blocking func showSaveDumpNameDialog";
				auto modalReturn = modalDialogs->showReturningDialog(
					ModalDialogFactory::makeCheckpointDumpNameDialog("Name dumped checkpoint", checkpointName)); // blocking
				PLOG_DEBUG << "showSaveDumpNameDialog returned";

				if (!std::get<bool>(modalReturn)) { PLOG_DEBUG << "User cancelled dump"; return; }
				checkpointName = sanitiseFileName(std::get<std::string>(modalReturn));
			}

			if (checkpointName.empty())
				throw HCMRuntimeException("That checkpoint name has no characters in it that Windows will accept in a file name.");

			// ---- THE COPY ----
			std::vector<byte> checkpointData;
			std::string origin;

			if (providerPath)
			{
				// THE SHADOW. There is no buffer sitting around to read - the blob only exists in this process for
				// the duration of one call at 0x19D51A - so HCM copies it as it goes past, on every checkpoint,
				// and this reads the newest copy. NOTHING IS FORCED: the file is the checkpoint the player is
				// actually on. See HCECheckpointDetours.cpp.
				lockOrThrow(checkpointDetoursWeak, checkpointDetours);
				checkpointData = checkpointDetours->readShadowCheckpoint();

				// The size can only differ if the engine re-sized its game state between the shadowed checkpoint
				// and now, which would make the file un-injectable into THIS session. Worth knowing about, not
				// worth refusing over - the file is still a faithful copy of a real checkpoint.
				if ((int64_t)checkpointData.size() != (int64_t)stateLength)
					PLOG_WARNING << std::format(
						"HaloCER shadowed checkpoint is 0x{:X} bytes but the live game state is now 0x{:X}",
						(uint64_t)checkpointData.size(), (uint32_t)stateLength);

				// SIGN IT. What the shadow holds is the live game state as the engine handed it over, whose header
				// digest field is whatever the last revert left behind - the engine only signs the copy it gives
				// the provider, through the sub_1803274D0 callback, which runs on bytes we do not own. Re-signing
				// here is deterministic and covers the whole blob, so the FILE verifies on its own terms.
				mBlob.applySHA1(checkpointData);
				origin = "shadowed at the storage hand-off";
			}
			else
			{
				// Gated on the engine's publication barrier and re-checked afterwards - see
				// HCECheckpointBlob::readCurrentCheckpoint. Verbatim: the engine has already signed this one.
				HCECheckpointBlob::Snapshot captured{};
				checkpointData = mBlob.readCurrentCheckpoint(captured);
				origin = std::format("slot {} buffer 0x{:X}", captured.ringIndex, (uint64_t)captured.buffer);
			}

			bool usedLegacyFolder = false;
			const std::filesystem::path dumpFolder = resolveDumpFolder(usedLegacyFolder);
			const std::filesystem::path dumpPath = dumpFolder / (checkpointName + ".bin");

			std::error_code ec;
			std::filesystem::create_directories(dumpFolder, ec);
			if (ec) throw HCMRuntimeException(std::format("Could not create the dump folder {}: {}", dumpFolder.string(), ec.message()));

			if (std::filesystem::exists(dumpPath, ec))
				throw HCMRuntimeException(std::format("A checkpoint file already exists at {}", dumpPath.string()));

			// std::filesystem::path carries the name as wide characters, so unicode in a typed name survives
			// without the str_to_wstr dance the MCC dump does.
			std::ofstream dumpFile(dumpPath, std::ios::binary | std::ios::trunc);
			if (!dumpFile) throw HCMRuntimeException(std::format("Could not create a file at {}", dumpPath.string()));

			// VERBATIM. Nothing is appended and nothing is overwritten - the engine's SHA-1 covers the whole blob.
			dumpFile.write((const char*)checkpointData.data(), (std::streamsize)checkpointData.size());
			dumpFile.flush();
			const bool wrote = dumpFile.good();
			dumpFile.close();

			if (!wrote)
			{
				std::error_code removeEc;
				std::filesystem::remove(dumpPath, removeEc); // do not leave a truncated file that would restart a level
				throw HCMRuntimeException(std::format("Failed writing the checkpoint to {} (is the drive full?)", dumpPath.string()));
			}

			// Identical on both storage paths now: nothing was forced and the player's checkpoint did not move.
			// The only thing that varies is WHERE it went, and the user is told when that is not the tab.
			messagesGUI->addMessage(std::format("Dumped checkpoint: {}.bin{}", checkpointName,
				usedLegacyFolder ? " (to HaloCER Checkpoints - open HCM's Halo Campaign Evolved tab to dump there instead)" : ""));

			PLOG_INFO << std::format(
				"Dumped a HaloCER checkpoint ({}), 0x{:X} bytes, to {}",
				origin, (uint64_t)checkpointData.size(), dumpPath.string());
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
		catch (const std::exception& ex)
		{
			// std::filesystem and the stream can both throw on their own. A dump failing must never take HCM
			// (or the user's run) with it. handleMessage takes a non-const lvalue reference, hence the named local.
			HCMRuntimeException converted(std::format("Checkpoint dump failed: {}", ex.what()));
			runtimeExceptions->handleMessage(converted);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor. See HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mDumpCheckpointCallback;

public:
	HCEDumpCheckpointImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		modalDialogsWeak(dicon.Resolve<ModalDialogRenderer>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		// A DI-container service, not an optional cheat, so resolving it cannot fail - and everything that USES it
		// tolerates its absence anyway. See the member declaration.
		sharedMemWeak(dicon.Resolve<ISharedMemory>()),
		// Hard dependency - on the provider path (what the shipped game uses) the hand-off hook is the ONLY way
		// to reach a checkpoint. resolveDependentCheat throws HCMInitException, which is what createCheats
		// catches, and the shared instance is cached per game+cheat pair so the force/natural toggles get the
		// same one.
		checkpointDetoursWeak(resolveDependentCheat(HCECheckpointDetours)),
		mBlob(game, dicon),
		// initialiser order must match declaration order - this is the last member of the class
		mDumpCheckpointCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->dumpCheckpointEvent, [this]() { onDump(); })
	{
		// explicit cast: GameState has both operator==(GameState) and operator Value(), so comparing a GameState
		// straight against a Value is ambiguous (C2666).
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEDumpCheckpoint only supports Halo Campaign Evolved");

		auto dirPathContainer = dicon.Resolve<DirPathContainer>().lock();
		if (!dirPathContainer) throw HCMInitException("HCEDumpCheckpoint could not resolve the HCM directory path");
		mHCMDirPath = dirPathContainer->dirPath;

		// Cosmetic dependency only (the level name in the default file name), so a failure here must not fail the
		// whole feature. Same pattern InjectCheckpoint uses for GetCurrentLevelCode.
		try
		{
			playerStateOptionalWeak = resolveDependentCheat(HCEGetPlayerState);
		}
		catch (HCMInitException ex)
		{
			PLOG_ERROR << "HCEDumpCheckpoint couldn't acquire HCEGetPlayerState (dumps will not be named after the level): " << ex.what();
		}

		// Last statement: the callback above is already subscribed, this is what lets it actually run.
		mReady.store(true, std::memory_order_release);
	}

	~HCEDumpCheckpointImpl()
	{
		mReady.store(false, std::memory_order_release);
		mDumpCheckpointCallback.removeCallback();
	}
};


HCEDumpCheckpoint::HCEDumpCheckpoint(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEDumpCheckpointImpl>(game, dicon))
{
}

HCEDumpCheckpoint::~HCEDumpCheckpoint()
{
	PLOG_VERBOSE << "~" << getName();
}
