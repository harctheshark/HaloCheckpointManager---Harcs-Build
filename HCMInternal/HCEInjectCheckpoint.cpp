#include "pch.h"
#include "HCEInjectCheckpoint.h"
#include "HCECheckpointBlob.h"
#include "HCECheckpointDetours.h"
#include "HCECheckpointIdentity.h"
#include "IMakeOrGetCheat.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "ISharedMemory.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "DirPathContainer.h"
#include "ModalDialogRenderer.h"
#include "ModalDialogFactory.h"
#include "ModalDialogGuard.h"
#include <atomic>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <optional>
#include <algorithm>
#include <Windows.h>
#include <commdlg.h>

// See HCEInjectCheckpoint.h for why this is not a branch inside InjectCheckpoint.h and for the level-restart
// hazard that shapes every decision here, and HCECheckpointBlob.h for the blob format, the publication barrier
// and the ported acceptance test.

namespace
{
	// The LEGACY dump folder, and now only a fallback. HaloCER dumps land in the save folder HCMExternal's Halo
	// Campaign Evolved tab has selected (<HCM dir>\Saves\Halo Campaign Evolved\...), the same convention the six
	// MCC tabs use; this folder is what HCM wrote to before that tab existed. Files already in it are NOT migrated
	// and are still perfectly valid - the browse dialog below deliberately opens here when it still holds .bin
	// files and the tab's folder does not, so an upgrading user's first browse lands on their existing dumps.
	// Kept as its own constant rather than shared through a header, exactly as before: a wrong folder here is a
	// harmless "no file selected", and coupling inject to dump for one string literal buys nothing.
	constexpr const char* kLegacyDumpSubfolder = "HaloCER Checkpoints";

	// How long the provider-path injection waits for its forced checkpoint to reach the hand-off. Same reasoning
	// as HCEDumpCheckpoint's copy of this: sized for a bad frame, not for a loading screen.
	constexpr int kHandoffTimeoutMilliseconds = 4000;
}


class HCEInjectCheckpoint::HCEInjectCheckpointImpl
{
private:
	GameState mGame;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::weak_ptr<ModalDialogRenderer> modalDialogsWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// SOFT dependency, and that is the whole design. This is HCMExternal's Halo Campaign Evolved tab - which
	// checkpoint the user has highlighted and which save folder they are in. Every path that reads it degrades to
	// the file dialog this class shipped with, so HCM keeps working with an old HCMExternal, with HCMExternal on a
	// different game's tab, and with nothing selected at all.
	std::weak_ptr<ISharedMemory> sharedMemWeak;

	// HARD dependency. On the PROVIDER path - what the shipped game uses - redirecting the storage provider's
	// argument at the hand-off is the only way to inject anything at all.
	std::weak_ptr<HCECheckpointDetours> checkpointDetoursWeak;

	// Everything that knows the checkpoint layout. Constructing it is pure PointerDataStore lookups.
	HCECheckpointBlob mBlob;

	// <HCM dir>, from the DI container - the same string ReplayRecorder builds <HCM dir>\replays\ out of.
	std::string mHCMDirPath;

	// One injection at a time. Both the file dialog and the warning dialogs are blocking, so a second press must
	// drop rather than queue a second 12 MB write behind the first.
	std::mutex mInjectMutex;

	// Set as the last statement of the constructor, cleared as the first of the destructor. The ScopedCallback is
	// declared LAST so it subscribes after every member above exists, but the constructor body still runs
	// afterwards and createCheats builds cheats on detached threads while the hotkey thread is already live.
	std::atomic<bool> mReady{ false };


	// How the candidate compares to THE LIVE SESSION. Taken twice - once to decide what to warn about, once
	// immediately before the injection - because the file dialog and every warning dialog is blocking, and the
	// player can reach a new BSP (or leave the level entirely) while one is up.
	//
	// The live side is the header at the front of the game state block, which is the engine's own `live` argument
	// to sub_18019EB50 and is maintained at all times. That is why this needs no checkpoint to exist and works
	// identically on both storage paths.
	HCECheckpointBlob::HeaderComparison compareAgainstLiveSession(const std::vector<byte>& candidate)
	{
		const std::vector<byte> liveHeader = mBlob.readLiveHeader();
		return mBlob.compareHeaders(candidate, liveHeader);
	}

	static bool sameVerdict(const HCECheckpointBlob::HeaderComparison& a, const HCECheckpointBlob::HeaderComparison& b)
	{
		return a.scenarioMatches == b.scenarioMatches
			&& a.sessionBlockMatches == b.sessionBlockMatches
			&& a.optionsMatch == b.optionsMatch
			&& a.signatureMatches == b.signatureMatches
			&& a.engineBuildMatches == b.engineBuildMatches
			&& a.stateIdentityMatches == b.stateIdentityMatches;
	}


	// ============================================================================================================
	// HCMEXTERNAL'S HALO CAMPAIGN EVOLVED TAB.
	//
	// Both helpers below return nullopt for EVERY reason, and the caller falls back to the file dialog this class
	// shipped with. That is deliberate and it is requirement number one of this integration: the tab is an
	// improvement on top of a feature that already worked, so it must never be able to break it. No shared memory,
	// an older HCMExternal, HCMExternal closed, a different game's tab open, nothing selected, a file that has been
	// deleted since it was selected - all of them land on the dialog, which is exactly today's behaviour.
	// ============================================================================================================

	// The save folder the HaloCER tab currently has selected. getDumpInfo THROWS when another game's tab is
	// selected (that is its entire job - see SharedMemoryInternal.cpp), which is the common case and not an error.
	std::optional<std::filesystem::path> externalSaveFolder()
	{
		try
		{
			auto sharedMem = sharedMemWeak.lock();
			if (!sharedMem) return std::nullopt;

			const SelectedFolderData folder = sharedMem->getDumpInfo(mGame);
			if (folder.selectedFolderPath.empty()) return std::nullopt;

			const std::filesystem::path path(folder.selectedFolderPath);
			std::error_code ec;
			if (!std::filesystem::is_directory(path, ec) || ec) return std::nullopt;
			return path;
		}
		catch (HCMRuntimeException& ex)
		{
			// Overwhelmingly "wrong game tab selected". DEBUG, not ERROR - it is the normal state of the world
			// while the user is looking at Halo 2.
			PLOG_DEBUG << "No HaloCER save folder from HCMExternal (" << ex.what() << ")";
			return std::nullopt;
		}
		catch (const std::exception& ex)
		{
			PLOG_DEBUG << "No HaloCER save folder from HCMExternal: " << ex.what();
			return std::nullopt;
		}
	}

	// The checkpoint the user has highlighted on the HaloCER tab. This is what makes the tab's "Inject" menu item
	// (and the in-game hotkey) inject THAT file instead of opening a dialog - the same contract MCC's
	// InjectCheckpoint has had all along.
	std::optional<std::filesystem::path> externalSelectedCheckpoint()
	{
		try
		{
			auto sharedMem = sharedMemWeak.lock();
			if (!sharedMem) { PLOG_DEBUG << "No shared memory, falling back to the inject file dialog"; return std::nullopt; }

			const SelectedCheckpointData selected = sharedMem->getInjectInfo();
			if (selected.selectedCheckpointNull) { PLOG_DEBUG << "HCMExternal has no checkpoint selected"; return std::nullopt; }

			// ⚠⚠ THE GAME GUARD, AND IT IS NOT OPTIONAL. HCMExternal publishes exactly ONE selection for whichever
			// tab is open, so with an MCC tab open this is an MCC checkpoint - a file of the wrong length, from the
			// wrong engine entirely. HCM would refuse it a moment later on length anyway, but silently substituting
			// somebody's Halo 3 checkpoint for the file dialog they expected is the wrong behaviour on its own.
			if ((GameState)selected.selectedCheckpointGame != mGame)
			{
				PLOG_DEBUG << "HCMExternal's selected checkpoint is for "
					<< ((GameState)selected.selectedCheckpointGame).toString()
					<< ", not " << mGame.toString() << " - falling back to the inject file dialog";
				return std::nullopt;
			}

			if (selected.selectedCheckpointFilePath.empty()) return std::nullopt;

			const std::filesystem::path path(selected.selectedCheckpointFilePath);
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec) || ec)
			{
				// Deleted or renamed between selecting and injecting. Falling back to the dialog is friendlier
				// than an error, and the dialog opens in the folder it went missing from.
				PLOG_WARNING << "HCMExternal's selected checkpoint is not there any more: " << path.string();
				return std::nullopt;
			}

			PLOG_DEBUG << "Injecting HCMExternal's selected checkpoint: " << path.string();
			return path;
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_DEBUG << "Could not read HCMExternal's selection (" << ex.what() << "), falling back to the inject file dialog";
			return std::nullopt;
		}
		catch (const std::exception& ex)
		{
			PLOG_DEBUG << "Could not read HCMExternal's selection: " << ex.what() << ", falling back to the inject file dialog";
			return std::nullopt;
		}
	}

	static bool folderHoldsCheckpoints(const std::filesystem::path& folder)
	{
		std::error_code ec;
		if (!std::filesystem::is_directory(folder, ec) || ec) return false;
		for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
		{
			if (it->is_regular_file(ec) && !ec && it->path().extension() == ".bin") return true;
		}
		return false;
	}

	// Where the file dialog opens. The tab's folder normally, because that is where dumps go now - EXCEPT when the
	// legacy folder still holds .bin files and the tab's folder holds none, which is precisely the state of an
	// install that dumped checkpoints before the tab existed. Nothing is moved; the user is just shown where their
	// files actually are, and can drag them into the tab's folder whenever they like.
	std::filesystem::path browseStartFolder()
	{
		const std::filesystem::path legacyFolder = std::filesystem::path(mHCMDirPath) / kLegacyDumpSubfolder;
		const auto tabFolder = externalSaveFolder();

		if (!tabFolder) return legacyFolder;
		if (!folderHoldsCheckpoints(tabFolder.value()) && folderHoldsCheckpoints(legacyFolder))
		{
			PLOG_DEBUG << "Opening the inject dialog on the legacy folder - it still has dumps and "
				<< tabFolder->string() << " does not";
			return legacyFolder;
		}
		return tabFolder.value();
	}


	// The Win32 open dialog, initialised to the dump folder. ModalDialogGuard is the same global one-dialog-at-a-
	// time token PresetManager uses, so a stray click cannot stack two file dialogs.
	std::optional<std::filesystem::path> browseForCheckpoint()
	{
		auto claim = ModalDialogGuard::tryClaim();
		if (!claim) { PLOG_DEBUG << "A file dialog is already open, ignoring this inject press"; return std::nullopt; }

		const std::filesystem::path dumpFolder = browseStartFolder();
		std::error_code ec;
		std::filesystem::create_directories(dumpFolder, ec); // best effort; the dialog copes with a missing folder

		const std::wstring initialDir = dumpFolder.wstring();
		wchar_t fileBuffer[MAX_PATH]{};

		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetForegroundWindow();
		ofn.lpstrFilter = L"HCM Checkpoint (*.bin)\0*.bin\0All Files (*.*)\0*.*\0";
		ofn.lpstrFile = fileBuffer;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrInitialDir = initialDir.c_str();
		ofn.lpstrDefExt = L"bin";
		ofn.lpstrTitle = L"Inject Halo Campaign Evolved checkpoint";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

		if (!GetOpenFileNameW(&ofn)) return std::nullopt; // cancelled
		return std::filesystem::path(fileBuffer);
	}


	static std::vector<byte> readFile(const std::filesystem::path& path)
	{
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) throw HCMRuntimeException(std::format("Could not open {}", path.string()));

		const std::streamoff size = file.tellg();
		if (size <= 0) throw HCMRuntimeException(std::format("{} is empty.", path.string()));
		if (size > HCECheckpointBlob::kMaxPlausibleLength)
			throw HCMRuntimeException(std::format(
				"{} is 0x{:X} bytes - far too big to be a checkpoint.", path.string(), (uint64_t)size));

		file.seekg(0, std::ios::beg);
		std::vector<byte> data;
		data.resize((size_t)size);
		file.read((char*)data.data(), (std::streamsize)size);
		if (!file) throw HCMRuntimeException(std::format("Failed reading {}", path.string()));
		return data;
	}


	// A blocking "are you sure?" over the game. Returns true only if the user explicitly chose Continue.
	// ⚠ If the dialog cannot be shown at all, this REFUSES. That direction is not arbitrary: silently continuing
	// would push an injection the user never approved into a game that answers a rejected checkpoint with a level
	// restart, whereas refusing costs them nothing but the toggle they can turn off themselves.
	bool userConfirms(const std::string& title, const std::string& body)
	{
		auto modalDialogs = modalDialogsWeak.lock();
		if (!modalDialogs)
			throw HCMRuntimeException(std::format(
				"{}\n\nHCM could not show the confirmation dialog, so nothing was injected.\n"
				"(Turn the matching warning off in Inject Checkpoint Settings if you want to inject anyway.)", body));

		PLOG_DEBUG << "showing HaloCER injection warning: " << title;
		return modalDialogs->showReturningDialog(ModalDialogFactory::makeInjectionWarningDialog(title, body)); // blocking
	}


	// primary event callback
	void onInject()
	{
		if (!mReady.load(std::memory_order_acquire))
		{
			PLOG_DEBUG << "HCEInjectCheckpoint not ready yet, ignoring inject request";
			return;
		}

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			lockOrThrow(settingsWeak, settings);

			if (!mccStateHook->isGameCurrentlyPlaying(mGame)) return;
			PLOG_DEBUG << "onInject called for game: " << mGame.toString();

			// try_lock, not lock: every dialog below is blocking, so a second press must drop rather than queue.
			std::unique_lock<std::mutex> lock(mInjectMutex, std::try_to_lock);
			if (!lock.owns_lock()) { PLOG_DEBUG << "A HaloCER checkpoint injection is already in progress, ignoring"; return; }

			// Fail fast, BEFORE the file dialog, on everything the user cannot fix by picking a better file.
			// This is a read of a handful of globals, not the 12 MB copy.
			const HCECheckpointBlob::StorageMode storage = mBlob.detectStorageMode();
			const bool providerPath = (storage == HCECheckpointBlob::StorageMode::Provider);

			const int32_t stateLength = mBlob.readStateLength();
			(void)mBlob.readStateBlockAddress(); // reports "load a level first" if there is no session

			if (!providerPath)
			{
				// The local path overwrites the checkpoint that is there, so there has to be one.
				const HCECheckpointBlob::Snapshot probe = mBlob.readSnapshot();
				if (!probe.systemInitialised)
					throw HCMRuntimeException("Halo Campaign Evolved's checkpoint system is not running yet - load a level first.");
				if (!probe.anyCheckpointExists)
					throw HCMRuntimeException(
						"Halo Campaign Evolved has no checkpoint yet, so there is nothing to inject over.\n"
						"Reach a checkpoint (or use Force Checkpoint) first.");
			}

			// THE TAB FIRST, THE DIALOG SECOND. If HCMExternal's Halo Campaign Evolved tab has a checkpoint
			// highlighted, that is the file - no dialog, which is how every MCC game has always behaved. Anything
			// at all wrong with the selection (see externalSelectedCheckpoint) drops through to the dialog, which
			// is the behaviour this feature shipped with and is still the only way to inject a file that is not in
			// the tab's folder.
			bool fromExternalSelection = false;
			auto chosen = externalSelectedCheckpoint();
			if (chosen)
			{
				fromExternalSelection = true;
			}
			else
			{
				chosen = browseForCheckpoint();
				if (!chosen) { PLOG_DEBUG << "User cancelled the inject file dialog"; return; }
			}

			std::vector<byte> checkpointData = readFile(chosen.value());

			// ---- VALIDATE. Nothing below this point reaches the game until every check has passed. ----

			// REQUIREMENT: the file must be exactly the length the engine itself reports for the game state
			// (*(int*)(simBase + 0x142AA1C)). Hard refusal with no toggle - a short file is meaningless and a long
			// one is not a checkpoint. On the provider path the hand-off re-checks it against the length the
			// engine actually passes, so a size that changes underneath us cannot slip through either.
			if (checkpointData.size() != (size_t)stateLength)
				throw HCMRuntimeException(std::format(
					"That file is not a checkpoint for this build of Halo Campaign Evolved.\n"
					"It is 0x{:X} bytes; a checkpoint is 0x{:X}.",
					(uint64_t)checkpointData.size(), (uint32_t)stateLength));

			// ---- MAKE A FOREIGN CHECKPOINT OURS -------------------------------------------------------------
			// A checkpoint saved by someone else passes every header test the engine applies - which is why it
			// loads at all - and then kills the player a second or two later, because THE SAVING PLAYER'S
			// IDENTITY is baked through the live player structures inside the state, not just the header.
			// Rewriting it to the local player's identity is what makes shared checkpoints work; see
			// HCECheckpointIdentity.h for how the fields were established and why the per-save tag is included.
			//
			// Done BEFORE the header comparison below, so the verdict describes the bytes that will actually be
			// injected rather than the bytes as they arrived. No re-signing is needed here: applySHA1 further
			// down runs unconditionally on every injection.
			//
			// Injecting one's OWN checkpoint is a no-op - every field already matches and rewrite() skips fields
			// that are equal, so this costs a comparison and nothing else.
			if (settings->hceInjectCheckpointRewriteIdentity->GetValue())
			{
				const std::vector<byte> liveHeaderForIdentity = mBlob.readLiveHeader();
				const auto localIdentity = HCECheckpointIdentity::read(liveHeaderForIdentity);
				const auto fileIdentity = HCECheckpointIdentity::read(checkpointData);

				if (!localIdentity.complete() || !fileIdentity.complete())
				{
					// Not fatal: the injection can still go ahead, it just will not be re-identified.
					PLOG_WARNING << "HaloCER: could not read the player identity from "
						<< (localIdentity.complete() ? "the checkpoint file" : "the live session")
						<< "; injecting without rewriting it. A checkpoint from another player may kill you "
						"shortly after it loads.";
				}
				else if (!fileIdentity.samePlayerAs(localIdentity))
				{
					const auto rewritten = HCECheckpointIdentity::rewrite(checkpointData, fileIdentity, localIdentity);
					PLOG_INFO << std::format(
						"HaloCER: rewrote the checkpoint's player identity from \"{}\" to \"{}\" - {} occurrence(s) "
						"(id4 {}, id8 {}, id8b {}, name {}, tag {})",
						fileIdentity.displayName(), localIdentity.displayName(), rewritten.total(),
						rewritten.id4, rewritten.id8, rewritten.id8b, rewritten.name, rewritten.tag);

					messagesGUI->addMessage(std::format("Checkpoint from \"{}\" re-identified as yours.",
						fileIdentity.displayName()));
				}
			}

			const HCECheckpointBlob::HeaderComparison verdict = compareAgainstLiveSession(checkpointData);

			if (verdict.allOK())
			{
				PLOG_DEBUG << "HaloCER injection candidate matches the live session on every compared field";
			}
			else
			{
				PLOG_WARNING << std::format(
					"HaloCER injection candidate differs from the live session: scenario={} sessionBlock={} options={} signature={} build={} stateIdentity={}",
					verdict.scenarioMatches, verdict.sessionBlockMatches, verdict.optionsMatch,
					verdict.signatureMatches, verdict.engineBuildMatches, verdict.stateIdentityMatches);
			}

			// LEVEL. The scenario path at +0x008 is strcmp'd by the engine; the 28-byte block at +0x1ECE0 is
			// compared exactly by sub_18019CD70 and sits right beside the BSP index, so a checkpoint from another
			// part of the same level trips it.
			if (!verdict.levelOK() && settings->injectCheckpointLevelCheck->GetValue())
			{
				std::string body = std::format(
					"This checkpoint was not made where you are now.\n\n"
					"Checkpoint level: {}\nCurrent level:    {}\n",
					verdict.fileScenario.empty() ? "(unreadable)" : verdict.fileScenario,
					verdict.liveScenario.empty() ? "(unreadable)" : verdict.liveScenario);

				if (!verdict.sessionBlockMatches)
					body += std::format(
						"The map/session block also differs (checkpoint BSP {}, current BSP {}).\n",
						verdict.fileBSP, verdict.liveBSP);

				body +=
					"\nHalo Campaign Evolved does NOT fail an unacceptable checkpoint quietly - the next revert\n"
					"RESTARTS THE LEVEL and your run is gone. Inject anyway?";

				if (!userConfirms("Injection: wrong level!", body)) { PLOG_DEBUG << "User cancelled at the level warning"; return; }
			}

			// DIFFICULTY / SKULLS. sub_1802096E0 - see HCECheckpointBlob::compareGameOptions.
			if (!verdict.optionsOK() && settings->injectCheckpointDifficultyCheck->GetValue())
			{
				const std::string body = std::format(
					"This checkpoint's game options do not match the ones you are playing.\n\n"
					"The engine compares these field by field, and the first one that differs is: {}.\n"
					"\nHalo Campaign Evolved does NOT fail an unacceptable checkpoint quietly - the next revert\n"
					"RESTARTS THE LEVEL and your run is gone. Inject anyway?",
					verdict.optionsDetail.empty() ? "the game options block" : verdict.optionsDetail);

				if (!userConfirms("Injection: mismatched difficulty/skulls!", body)) { PLOG_DEBUG << "User cancelled at the options warning"; return; }
			}

			// BUILD / STATE SIGNATURE. The state-layout CRC at +0x000, the engine build string at +0x108 and the
			// state identity dword at +0x128. Reusing MCC's "wrong game version" toggle, which is the same idea.
			if (!verdict.buildOK() && settings->injectCheckpointVersionCheck->GetValue())
			{
				const std::string body = std::format(
					"This checkpoint was made by a different build of Halo Campaign Evolved.\n\n"
					"Checkpoint build: {}\nCurrent build:    {}\n"
					"State signature match: {}\nState identity match:  {}\n"
					"\nThe game state layout changes between builds, so this is the mismatch most likely to\n"
					"RESTART THE LEVEL (or worse) on the next revert. Inject anyway?",
					verdict.fileEngineBuild.empty() ? "(unreadable)" : verdict.fileEngineBuild,
					verdict.liveEngineBuild.empty() ? "(unreadable)" : verdict.liveEngineBuild,
					verdict.signatureMatches ? "yes" : "NO",
					verdict.stateIdentityMatches ? "yes" : "NO");

				if (!userConfirms("Injection: wrong game build!", body)) { PLOG_DEBUG << "User cancelled at the build warning"; return; }
			}

			// RE-TAKE THE VERDICT. Every dialog above is blocking, so the player may have moved to another BSP or
			// left the level while one was up. If the verdict moved at all, the user answered a question about a
			// session that no longer exists - abort rather than silently apply their answer to a different one.
			if (!sameVerdict(verdict, compareAgainstLiveSession(checkpointData)))
				throw HCMRuntimeException(
					"Halo Campaign Evolved's session changed while HCM was checking this file.\n"
					"NOTHING was injected - press inject again.");

			// ---- THE DIGEST. Always recomputed, never trusted. ----
			// Deterministic, so an untouched dump comes out byte-identical; a hand-edited or truncated-and-padded
			// one gets a correct digest instead of the rejection (i.e. the level restart) it would otherwise earn.
			// Deliberately NOT a toggle: there is no situation in which shipping a stale digest is what the user
			// wants, and the engine has no checksum bypass on the revert path to fall back on (the revert calls the
			// verifier with a2 = 0, so the 0xBB x 20 sentinel it honours is unreachable).
			if (mBlob.shaOffset() < 0 || mBlob.shaLength() <= 0
				|| (uint64_t)(mBlob.shaOffset() + mBlob.shaLength()) > checkpointData.size())
				throw HCMRuntimeException("That file is too small to hold a checkpoint checksum.");

			const std::vector<byte> originalDigest(
				checkpointData.begin() + mBlob.shaOffset(),
				checkpointData.begin() + mBlob.shaOffset() + mBlob.shaLength());

			mBlob.applySHA1(checkpointData);

			// A file whose stored digest did not match its own contents was edited (or is damaged). Re-signing it
			// is still the right move - a wrong digest is a guaranteed level restart - but the user should be told,
			// because it means the bytes they are injecting are not the bytes HCM dumped.
			const bool digestWasStale = !std::equal(originalDigest.begin(), originalDigest.end(),
				checkpointData.begin() + mBlob.shaOffset());
			if (digestWasStale)
				PLOG_WARNING << "The injected file's SHA-1 did not match its contents and has been recomputed "
				"(the file was edited, or was not dumped by HCM).";

			// ---- THE INJECTION ----
			std::string destination;

			if (providerPath)
			{
				// ONE SHOT. Stages the blob, arms the hand-off, forces a checkpoint, waits for the game thread to
				// consume the arming, and disarms whatever happens. The provider is handed OUR pointer instead of
				// the engine's live state block; the game's own memory is never written.
				lockOrThrow(checkpointDetoursWeak, checkpointDetours);
				checkpointDetours->injectCheckpointAtHandoff(checkpointData, kHandoffTimeoutMilliseconds);
				destination = "the external storage provider";
			}
			else
			{
				// The local path overwrites the ring slot in place, so it needs a fresh barrier reading and a
				// rollback image. Taken now rather than earlier: it is ~12 MB that would otherwise sit around for
				// however long the user spent in the blocking dialogs above, and it would be stale by now anyway.
				HCECheckpointBlob::Snapshot slot{};
				const std::vector<byte> rollback = mBlob.readCurrentCheckpoint(slot);

				if (checkpointData.size() != (size_t)slot.length)
					throw HCMRuntimeException(std::format(
						"Halo Campaign Evolved's checkpoint size changed while HCM was preparing the injection "
						"(0x{:X} -> 0x{:X}). NOTHING was injected.",
						(uint64_t)checkpointData.size(), (uint32_t)slot.length));

				// Barrier-gated on both sides, rolls `rollback` back if it is interrupted.
				mBlob.writeCheckpoint(slot, checkpointData, rollback);
				destination = std::format("slot {} buffer 0x{:X}", slot.ringIndex, (uint64_t)slot.buffer);
			}

			const std::string fileName = chosen->filename().string();
			// The provider path had to force a checkpoint to inject, so say so - the player's checkpoint moved.
			messagesGUI->addMessage(std::format("{} {}{}",
				providerPath ? "Checkpoint forced and injected:" : "Injected checkpoint",
				fileName,
				digestWasStale ? " (checksum repaired - this file was edited)" : ""));

			PLOG_INFO << std::format(
				"Injected a HaloCER checkpoint from {} ({}) into {} (0x{:X} bytes, digest {})",
				chosen->string(),
				fromExternalSelection ? "selected in HCMExternal" : "picked in the file dialog",
				destination,
				(uint64_t)checkpointData.size(), digestWasStale ? "recomputed" : "unchanged");

			if (settings->injectCheckpointForcesRevert->GetValue())
				settings->forceRevertEvent->operator()();
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
		catch (const std::exception& ex)
		{
			// std::filesystem and the stream can both throw on their own. handleMessage takes a non-const lvalue
			// reference, hence the named local.
			HCMRuntimeException converted(std::format("Checkpoint injection failed: {}", ex.what()));
			runtimeExceptions->handleMessage(converted);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor. See HCECheckpointDetours.cpp.
	ScopedCallback<ActionEvent> mInjectCheckpointCallback;

public:
	HCEInjectCheckpointImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		modalDialogsWeak(dicon.Resolve<ModalDialogRenderer>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		// Resolving it cannot fail (it is a DI-container service, not an optional cheat) and costs nothing;
		// everything that USES it tolerates its absence. See the member declaration.
		sharedMemWeak(dicon.Resolve<ISharedMemory>()),
		// Hard dependency - see the member declaration. Cached per game+cheat pair, so this is the same instance
		// the force/natural checkpoint toggles and HCEDumpCheckpoint use.
		checkpointDetoursWeak(resolveDependentCheat(HCECheckpointDetours)),
		mBlob(game, dicon),
		// initialiser order must match declaration order - this is the last member of the class
		mInjectCheckpointCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->injectCheckpointEvent, [this]() { onInject(); })
	{
		// explicit cast: GameState has both operator==(GameState) and operator Value(), so comparing a GameState
		// straight against a Value is ambiguous (C2666).
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEInjectCheckpoint only supports Halo Campaign Evolved");

		auto dirPathContainer = dicon.Resolve<DirPathContainer>().lock();
		if (!dirPathContainer) throw HCMInitException("HCEInjectCheckpoint could not resolve the HCM directory path");
		mHCMDirPath = dirPathContainer->dirPath;

		// Last statement: the callback above is already subscribed, this is what lets it actually run.
		mReady.store(true, std::memory_order_release);
	}

	~HCEInjectCheckpointImpl()
	{
		mReady.store(false, std::memory_order_release);
		mInjectCheckpointCallback.removeCallback();
	}
};


HCEInjectCheckpoint::HCEInjectCheckpoint(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEInjectCheckpointImpl>(game, dicon))
{
}

HCEInjectCheckpoint::~HCEInjectCheckpoint()
{
	PLOG_VERBOSE << "~" << getName();
}
