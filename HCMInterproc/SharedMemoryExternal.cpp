#include "pch.h"
#include "SharedMemoryExternal.h"
#include <sddl.h>    // ConvertStringSecurityDescriptorToSecurityDescriptorW - see the DACL note below
#include <aclapi.h>  // SetNamedSecurityInfoW, SetEntriesInAclW - see grantAllApplicationPackages
#include <boost/interprocess/detail/shared_dir_helpers.hpp>   // get_shared_dir


// Add an ALL APPLICATION PACKAGES (S-1-15-2-1) allow ACE to a path, preserving whatever is already there.
//
// This is deliberately belt-and-braces on top of passing the right SECURITY_ATTRIBUTES to boost. The whole
// failure mode is INVISIBLE from the outside - HCMInternal cannot log, cannot show a message box, and dies
// inside an AppContainer with nothing but "Access is denied" - so it is worth paying a few milliseconds to
// make the grant explicit AND to say in the log whether it landed.
// Local, because HCMInterproc has no wstr_to_str of its own. Only ever used for log text.
static std::string narrowForLog(const std::wstring& w)
{
	if (w.empty()) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)(n > 0 ? n : 0), '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
	return out;
}

static bool grantAllApplicationPackages(const std::wstring& path)
{
	PSID acSid = nullptr;
	if (!ConvertStringSidToSidW(L"S-1-15-2-1", &acSid) || !acSid)
	{
		PLOG_ERROR << "grantAllApplicationPackages: could not build the ALL APPLICATION PACKAGES SID, error " << GetLastError();
		return false;
	}

	PACL oldDacl = nullptr;
	PSECURITY_DESCRIPTOR sd = nullptr;
	bool ok = false;

	if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
			nullptr, nullptr, &oldDacl, nullptr, &sd) == ERROR_SUCCESS)
	{
		EXPLICIT_ACCESSW ea{};
		ea.grfAccessPermissions = GENERIC_ALL;
		ea.grfAccessMode = GRANT_ACCESS;
		ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
		ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
		ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
		ea.Trustee.ptstrName = (LPWSTR)acSid;

		PACL newDacl = nullptr;
		if (SetEntriesInAclW(1, &ea, oldDacl, &newDacl) == ERROR_SUCCESS && newDacl)
		{
			const DWORD set = SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT,
				DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl, nullptr);
			ok = (set == ERROR_SUCCESS);
			if (!ok) PLOG_ERROR << "grantAllApplicationPackages: SetNamedSecurityInfo failed on " << narrowForLog(path) << ", error " << set;
			LocalFree(newDacl);
		}
	}
	else
	{
		PLOG_ERROR << "grantAllApplicationPackages: GetNamedSecurityInfo failed on " << narrowForLog(path) << ", error " << GetLastError();
	}

	if (sd) LocalFree(sd);
	LocalFree(acSid);
	return ok;
}




std::string getOwnProcessDirectory()
{
	CHAR buffer[MAX_PATH] = { 0 };
	GetModuleFileNameA(NULL, buffer, MAX_PATH);
	std::string::size_type pos = std::string(buffer).find_last_of("\\/");
	return std::string(std::string(buffer).substr(0, pos) + "\\");
}


SharedMemoryExternal::SharedMemoryExternal(bool CPnullData,
	int CPgame, const char* CPname, const char* CPpath, const char* CPlevelcode, const char* CPgameVersion, int CPdifficulty,
	int SFgame, const char* SFname, const char* SFpath)
{
	try
	{
		// remove shared memory if it already exists
		bip::shared_memory_object::remove("hcm_shm");

		// Allow a process without our privileges (HCMInternal, inside the game) to reach this segment
		// even when HCMExternal is privileged.
		//
		// ⚠⚠⚠ DO NOT GO BACK TO permissions::set_unrestricted(). It sets a **NULL DACL**, which means
		// "no security, everyone gets access" for an ordinary token - but an APPCONTAINER (LowBox) token
		// is NOT covered by that. A LowBox access check requires an ACE explicitly granting the package
		// SID or ALL APPLICATION PACKAGES; a NULL DACL grants it nothing. Worse, writing an explicit NULL
		// DACL on the backing file OVERRIDES any inherited ACE, so granting the parent directory does not
		// help either - the file is recreated here on every run and loses it.
		//
		// That is not theoretical: Halo 5: Forge is a UWP title, and with set_unrestricted() HCMInternal
		// injected fine and then died with "boost::interprocess_exception: Access is denied" before it
		// could open a log - invisibly, because a sandboxed process cannot show a MessageBox either. A
		// probe DLL run inside the sandbox confirmed it: the boost DIRECTORY (explicit ACE) was readable
		// while the hcm_shm FILE (NULL DACL) returned ERROR_ACCESS_DENIED.
		//
		// So: an EXPLICIT DACL granting Everyone (WD) and ALL APPLICATION PACKAGES (AC), inheritable.
		// Same practical openness as before for normal hosts, and it actually works for sandboxed ones.
		// ⚠⚠⚠ boost::permissions WANTS A **SECURITY_ATTRIBUTES\***, NOT A SECURITY_DESCRIPTOR*.
		// Its own header says so ("a SECURITY_ATTRIBUTES pointer in windows"), set_unrestricted() stores an
		// interprocess_all_access_security (a SECURITY_ATTRIBUTES), and os_file_functions.hpp casts the value
		// straight through:
		//     winapi::create_file(..., (winapi::interprocess_security_attributes*)perm.get_permissions())
		//
		// I got this wrong the first time and handed it the raw descriptor. Windows then read a
		// SECURITY_DESCRIPTOR as though it were a SECURITY_ATTRIBUTES - nLength garbage, lpSecurityDescriptor
		// garbage - and SILENTLY created the file with the token's DEFAULT DACL instead:
		//     hurri:(F)  SYSTEM:(F)  LogonSessionId_0_xxxx:(RX)      <- no ALL APPLICATION PACKAGES
		// No error anywhere; shared memory creation still logged "Success!". The only symptom was
		// HCMInternal dying in the sandbox with "interprocess_exception: Access is denied" before it could
		// open a log, which reads like an injection failure and is nothing of the sort.
		//
		// It appeared to work for a while only because a manual `icacls /T` had stamped the then-current
		// boost folder; boost later made a NEW session directory with a NULL DACL (which blocks inheritance),
		// and the grant was gone.
		PSECURITY_DESCRIPTOR pSD = nullptr;
		SECURITY_ATTRIBUTES secAttrs{};   // ⚠ must stay alive until the segment below is constructed
		bip::permissions unrestricted_permissions;
		if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
				L"D:(A;OICI;GA;;;WD)(A;OICI;GA;;;AC)", SDDL_REVISION_1, &pSD, nullptr) && pSD)
		{
			secAttrs.nLength = sizeof(secAttrs);
			secAttrs.lpSecurityDescriptor = pSD;
			secAttrs.bInheritHandle = FALSE;
			unrestricted_permissions.set_permissions(&secAttrs);
		}
		else
		{
			// Could not build the descriptor - fall back to the old behaviour rather than failing to
			// start. Non-sandboxed hosts (MCC, HaloCER) are unaffected by the difference.
			PLOG_ERROR << "Could not build the shared memory security descriptor (error "
				<< GetLastError() << "); falling back to an unrestricted NULL DACL. "
				"Sandboxed hosts such as Halo 5: Forge will not be able to attach.";
			unrestricted_permissions.set_unrestricted();
		}

		// create segment with those permissions
		segment = bip::managed_shared_memory(bip::create_only, "hcm_shm", 65536, 0, unrestricted_permissions);

		if (pSD) LocalFree(pSD);   // boost has copied what it needs by now

		// Now make it explicit, and SAY whether it worked. boost creates the per-session directory itself
		// with a NULL DACL, and a NULL DACL carries no inheritable ACE - so the file underneath it inherits
		// nothing, which is how the AppContainer grant went missing in the first place. Stamp both.
		{
			std::wstring sharedDir;
			bip::ipcdetail::get_shared_dir(sharedDir);
			const std::wstring shmFile = sharedDir + L"\\hcm_shm";

			const bool dirOk = grantAllApplicationPackages(sharedDir);
			const bool fileOk = grantAllApplicationPackages(shmFile);

			if (dirOk && fileOk)
				PLOG_INFO << "Shared memory is reachable from an AppContainer: ALL APPLICATION PACKAGES granted on "
					<< narrowForLog(shmFile);
			else
				PLOG_ERROR << "COULD NOT grant ALL APPLICATION PACKAGES on the shared memory (dir: " << dirOk
					<< ", file: " << fileOk << "). Sandboxed games (Halo 5: Forge) will fail to attach with "
					"\"Access is denied\" and will not be able to log it.";
		}

		auto* segmentManager = segment.get_segment_manager();
		shm_string::allocator_type sa(segmentManager);

		auto HCMdirPath = segment.construct<shm_string>("HCMdirPath")(sa);
		HCMdirPath->assign(getOwnProcessDirectory().c_str());

		// See the note on externalHeartbeat in the header: this is how a sandboxed HCMInternal knows we
		// are still alive, since it cannot open our process.
		externalHeartbeat = segment.construct<int>("externalHeartbeat")(0);

		// Keyboard state forwarded into the sandbox - see the note in the header.
		sharedKeyStates = segment.construct<unsigned char>("hcmKeyStates")[kKeyStateCount](0);
		sharedKeyStatesGeneration = segment.construct<int>("hcmKeyStatesGeneration")(0);

		// Mouse motion, for the clipped-cursor case - see publishMouseMotion.
		sharedMouseAccumX = segment.construct<int>("hcmMouseAccumX")(0);
		sharedMouseAccumY = segment.construct<int>("hcmMouseAccumY")(0);
		sharedMouseAccumWheel = segment.construct<int>("hcmMouseAccumWheel")(0);

		injectCommandQueued = segment.construct<bool>("injectCommandQueued")(false);
		HCMInternalStatusFlag = segment.construct<int>("HCMInternalStatusFlag")((int)HCMInternalStatus::Initialising);

		// selectedCheckpointInfo
		selectedCheckpointNull = segment.construct<bool>("selectedCheckpointNull")(CPnullData);
		selectedCheckpointGame = segment.construct<int>("selectedCheckpointGame")(CPgame);
		selectedCheckpointName = segment.construct<shm_string>("selectedCheckpointName")(sa);
		selectedCheckpointName->assign(CPname);
		selectedCheckpointFilePath = segment.construct<shm_string>("selectedCheckpointFilePath")(sa);
		selectedCheckpointFilePath->assign(CPpath);
		selectedCheckpointLevelCode = segment.construct<shm_string>("selectedCheckpointLevelCode")(sa);
		selectedCheckpointLevelCode->assign(CPlevelcode);
		selectedCheckpointGameVersion = segment.construct<shm_string>("selectedCheckpointGameVersion")(sa);
		selectedCheckpointGameVersion->assign(CPgameVersion);
		selectedCheckpointDifficulty = segment.construct<int>("selectedCheckpointDifficulty")(CPdifficulty);

		PLOG_DEBUG << "SharedMemoryExternal::SharedMemoryExternal: SFname: " << SFname;

		// selected folder info
		selectedFolderGame = segment.construct<int>("selectedFolderGame")(SFgame);
		selectedFolderName = segment.construct<shm_string>("selectedFolderName")(sa);
		selectedFolderName->assign(SFname);
		selectedFolderPath = segment.construct<shm_string>("selectedFolderPath")(sa);
		selectedFolderPath->assign(SFpath);

	}
	catch (bip::interprocess_exception ex)
	{
		std::string errorMessage = std::format("HCM Interproc failed to create shared memory, error:\n{}", ex.what());
		PLOG_FATAL << errorMessage;
		int msgboxID = MessageBoxA(
			NULL,
			errorMessage.c_str(),
			"Halo checkpoint manager error",
			MB_OK
		);
		
	}

}


// Poll the real keyboard and publish it for the sandboxed HCMInternal. See the header for why this has to
// happen out here rather than in the game.
//
// ⚠ When the game is NOT foreground we publish ALL-KEYS-UP rather than simply skipping. Skipping would
// leave whatever was last written frozen in shared memory, so a key held as the user alt-tabbed would look
// permanently held to HCM - a hotkey stuck on forever. Clearing is the safe state.
void SharedMemoryExternal::publishKeyboardState(bool gameIsForeground) noexcept
{
	if (!sharedKeyStates || !sharedKeyStatesGeneration) return;

	if (!gameIsForeground)
	{
		memset(sharedKeyStates, 0, kKeyStateCount);
	}
	else
	{
		for (int vk = 0; vk < kKeyStateCount; ++vk)
		{
			// ⚠ HIGH bit only. The LOW bit ("pressed since last call") is consumed by whoever reads it
			// first, so using it here would race with anything else on the system polling the same key.
			sharedKeyStates[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
		}
	}

	++(*sharedKeyStatesGeneration);
}


void SharedMemoryExternal::publishMouseMotion(int dx, int dy, int wheel) noexcept
{
	if (sharedMouseAccumX) *sharedMouseAccumX += dx;
	if (sharedMouseAccumY) *sharedMouseAccumY += dy;
	if (sharedMouseAccumWheel) *sharedMouseAccumWheel += wheel;
}


std::unique_ptr<SharedMemoryExternal> g_SharedMemoryExternal;