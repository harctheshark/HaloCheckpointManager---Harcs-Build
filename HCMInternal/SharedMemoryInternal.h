#pragma once
#include <boost/interprocess/allocators/allocator.hpp>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/containers/string.hpp>
#include "ExternalInfo.h"
#include "ISharedMemory.h"
namespace bip = boost::interprocess;

template <typename T>
using Alloc = bip::allocator<T, bip::managed_shared_memory::segment_manager>;
using shm_string = bip::basic_string<char, std::char_traits<char>, Alloc<char>>;



class SharedMemoryInternal : public ISharedMemory
{
private:
	bip::managed_shared_memory segment;
public:
	SharedMemoryInternal()
	{
		try
		{
			segment = bip::managed_shared_memory(bip::open_only, "hcm_shm");
			auto* pdirPath = segment.find<shm_string>("HCMdirPath").first;
			if (!pdirPath) throw HCMInitException("Could not access HCMDirPath");

			HCMDirPath = *pdirPath;
			instance = this;
		}
		catch (bip::interprocess_exception ex)
		{
			throw HCMInitException(std::format("boost::interprocess::interprocess_exception: {}", ex.what()));
		}

	}
	~SharedMemoryInternal()
	{
		instance = nullptr;
	}

	virtual SelectedCheckpointData getInjectInfo() override;
	virtual SelectedFolderData getDumpInfo(GameState game) override;
	virtual bool getAndClearInjectQueue() override;
	virtual void setStatusFlag(HCMInternalStatus in) noexcept override;
	std::string HCMDirPath;

	// HCMExternal's liveness counter, bumped once per state machine tick (~1s).
	//
	// ⚠ This exists because the usual proof of life - OpenProcess + GetExitCodeProcess on HCMExternal -
	// is IMPOSSIBLE from a sandboxed host. An AppContainer (Halo 5: Forge is a UWP title) can neither
	// enumerate desktop processes nor open a handle to one, so HeartbeatTimer decided it was an orphan and
	// killed the session about three seconds in, every time.
	//
	// Returns nullopt when the field is absent, which means an OLDER HCMExternal that predates this
	// counter. Callers must treat that as "cannot tell" and fall back, never as "external is dead".
	std::optional<int> getExternalHeartbeat() noexcept
	{
		try
		{
			auto* p = segment.find<int>("externalHeartbeat").first;
			if (!p) return std::nullopt;
			return *p;
		}
		catch (...) { return std::nullopt; }
	}

	// Keyboard state forwarded by HCMExternal, one byte per virtual key.
	//
	// ⚠ THIS IS THE ONLY KEYBOARD SOURCE THAT WORKS IN AN APPCONTAINER. Inside the sandbox
	// GetAsyncKeyState returns 0x0000 for everything, and Raw Input - which registers successfully for
	// mouse AND keyboard - delivers mouse events only, never a single keystroke. Since HCM's hotkeys are
	// ImGui::IsKeyDown() checks, without this every hotkey on Halo 5: Forge is dead, not just text entry.
	//
	// Returns nullptr against an older HCMExternal that predates the array; callers must treat that as
	// "no keyboard available" rather than "all keys up".
	const unsigned char* getForwardedKeyStates() noexcept
	{
		try
		{
			auto found = segment.find<unsigned char>("hcmKeyStates");
			if (!found.first || found.second < 256) return nullptr;
			return found.first;
		}
		catch (...) { return nullptr; }
	}

	// Static, because ImGuiManager is constructed from the graphics hook and has no shared memory handle
	// of its own. Null when there is no shared memory or no forwarding external.
	static const unsigned char* forwardedKeyStates() noexcept
	{
		return instance ? instance->getForwardedKeyStates() : nullptr;
	}

	// Cumulative mouse motion collected by HCMExternal. ⚠ The game clips the cursor to 1x1 for mouse-look,
	// which freezes GetCursorPos, and raw input cannot be used inside the game without stealing the game's
	// own (it is registered per process). Diff these against what you last saw.
	bool getForwardedMouseMotion(int& x, int& y, int& wheel) noexcept
	{
		try
		{
			auto px = segment.find<int>("hcmMouseAccumX").first;
			auto py = segment.find<int>("hcmMouseAccumY").first;
			auto pw = segment.find<int>("hcmMouseAccumWheel").first;
			if (!px || !py || !pw) return false;
			x = *px; y = *py; wheel = *pw;
			return true;
		}
		catch (...) { return false; }
	}

	static bool forwardedMouseMotion(int& x, int& y, int& wheel) noexcept
	{
		return instance ? instance->getForwardedMouseMotion(x, y, wheel) : false;
	}

	// Ask HCMExternal to write the settings file on our behalf.
	//
	// ⚠⚠ ONLY REACHED WHEN OUR OWN WRITE FAILED. Halo 5: Forge runs this DLL inside an AppContainer, and
	// the HCM install directory grants ALL APPLICATION PACKAGES only ReadAndExecute - so the config can be
	// read but never written, and every Halo 5 setting was lost on exit. MCC and HaloCER write directly and
	// never come through here, so their behaviour is completely unchanged.
	//
	// Returns false when the fields are absent (an older HCMExternal), which the caller must report rather
	// than treat as a successful save - otherwise the user is told their settings saved when they did not.
	bool forwardConfigSave(const std::string& xml) noexcept
	{
		try
		{
			auto* text = segment.find<shm_string>("pendingConfigXml").first;
			auto* gen = segment.find<int>("pendingConfigGeneration").first;
			if (!text || !gen) return false;
			text->assign(xml.c_str(), xml.size());
			++(*gen);   // ⚠ bumped LAST: the generation is what tells the reader the text is complete.
			return true;
		}
		catch (...) { return false; }
	}

	static bool forwardConfigSaveStatic(const std::string& xml) noexcept
	{
		return instance ? instance->forwardConfigSave(xml) : false;
	}

	// setStatusFlag but static for access by UnhandledExceptionHandler in emergencies
	static void UnhandledExceptionSetStatusErrorFlag()
	{
		if (instance) instance->setStatusFlag(HCMInternalStatus::Error);
	}
private:
	static inline SharedMemoryInternal* instance = nullptr;
	
};

