#pragma once
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/containers/string.hpp>
#include "ExternalInfo.h"

namespace bip = boost::interprocess;

template <typename T>
using Alloc = bip::allocator<T, bip::managed_shared_memory::segment_manager>;
using shm_string = bip::basic_string<char, std::char_traits<char>, Alloc<char>>;


enum class HCMInternalStatus
{
	Initialising = 0,
	AllGood = 1,
	Error = 2,
	Shutdown = 3
};


class SharedMemoryExternal
{

private:
	bip::managed_shared_memory segment;

public:
	bool* selectedCheckpointNull;
	int* selectedCheckpointGame;
	shm_string* selectedCheckpointName;
	shm_string* selectedCheckpointFilePath;
	shm_string* selectedCheckpointLevelCode;
	shm_string* selectedCheckpointGameVersion;
	int* selectedCheckpointDifficulty;

	int* selectedFolderGame;
	shm_string* selectedFolderName;
	shm_string* selectedFolderPath;


	bool* injectCommandQueued;





	SharedMemoryExternal(bool CPnullData, 
		int CPgame, const char* CPname, const char* CPpath, const char* CPlevelcode, const char* CPgameVersion, int CPdifficulty,
		int SFgame, const char* SFname, const char* SFpath);




	int* HCMInternalStatusFlag;

	// ⚠ LIVENESS THAT CROSSES AN APPCONTAINER BOUNDARY.
	// HCMInternal normally proves HCMExternal is alive by OpenProcess + GetExitCodeProcess on it. That is
	// impossible from a sandboxed host: Halo 5: Forge is a UWP title, and an AppContainer can neither
	// enumerate desktop processes nor open a handle to one (findProcess returned 0 and OpenProcess failed
	// with error 87). HCMInternal therefore concluded it was an ORPHAN and killed itself ~3s into every
	// session - it initialised fully and then shut straight back down.
	//
	// So the external bumps this counter on every state machine tick (~1s) and HCMInternal watches it
	// change. Works identically for sandboxed and normal hosts; the process handle stays the preferred
	// mechanism where it is obtainable, because it detects a hard kill instantly.
	int* externalHeartbeat;

	void bumpHeartbeat() noexcept { if (externalHeartbeat) ++(*externalHeartbeat); }

	// ⚠⚠⚠ KEYBOARD STATE, FORWARDED FROM OUTSIDE THE SANDBOX.
	// A game running in an AppContainer has NO way to read the keyboard:
	//   * GetAsyncKeyState returns 0x0000 for every key (measured over minutes of typing);
	//   * Raw Input registers successfully for both mouse and keyboard, and then delivers MOUSE ONLY -
	//     zero keyboard events ever arrive. RIDEV_INPUTSINK on a keyboard is a keylogger primitive, so
	//     Windows withholding it from a LowBox token is entirely reasonable.
	// That is not just "Enter does not close a dialog": HCM's hotkeys are ImGui::IsKeyDown() checks, so
	// with no keyboard source EVERY hotkey on Halo 5 is dead - checkpoint, revert, the lot.
	//
	// HCMExternal is an ordinary desktop process where GetAsyncKeyState works fine, so it polls there and
	// publishes the state here for HCMInternal to read. One byte per virtual key, 0 or 1.
	//
	// ⚠ GATED ON THE GAME BEING FOREGROUND - see publishKeyboardState. Without that gate HCM would react
	// to keys typed into other applications, which is both a hotkey-misfire bug and a thing no tool should
	// be doing while the user is in their browser.
	static constexpr int kKeyStateCount = 256;
	unsigned char* sharedKeyStates = nullptr;
	int* sharedKeyStatesGeneration = nullptr;   // bumped on every publish; lets the reader spot a stale block

	void publishKeyboardState(bool gameIsForeground) noexcept;

	// ⚠⚠⚠ MOUSE MOTION MUST ALSO COME FROM OUT HERE, AND FOR A DIFFERENT REASON THAN THE KEYBOARD.
	// The game clips the cursor to 1x1 for mouse-look, so GetCursorPos inside the game is FROZEN and the
	// overlay's pointer cannot be moved at all. Relative motion via Raw Input is the only thing that still
	// works while clipped - but registering raw input INSIDE the game replaces the game's own registration
	// (it is scoped per process) and destroys the player's ability to aim and move. Registering it HERE, in
	// a separate process, has no such effect.
	//
	// Cumulative counters rather than per-tick deltas, so a reader that misses a tick loses nothing: the
	// internal side simply diffs against what it saw last.
	int* sharedMouseAccumX = nullptr;
	int* sharedMouseAccumY = nullptr;
	int* sharedMouseAccumWheel = nullptr;

	void publishMouseMotion(int dx, int dy, int wheel) noexcept;

};


extern std::unique_ptr<SharedMemoryExternal> g_SharedMemoryExternal; // starts uninitialised, created in inititialiseInterproc

static HCMInternalStatus getHCMInternalStatusFlag()
{
	if (!g_SharedMemoryExternal.get())
	{
		PLOG_ERROR << "g_SharedMemoryExternal not initialised! t.getHCMInternalStatusFlag";
		return HCMInternalStatus::Error;
	}

	if (!g_SharedMemoryExternal->HCMInternalStatusFlag)
	{
		PLOG_ERROR << "g_SharedMemoryExternal->HCMInternalStatusFlag was null! t.getHCMInternalStatusFlag";
		return HCMInternalStatus::Error;
	}

	int readFlag = *g_SharedMemoryExternal->HCMInternalStatusFlag;

	if (!magic_enum::enum_contains<HCMInternalStatus>(readFlag))
	{
		PLOG_ERROR << "invalid HCMInternalStatus by readFlag: " << readFlag;
		return HCMInternalStatus::Error;
	}

	return (HCMInternalStatus)readFlag; 
}