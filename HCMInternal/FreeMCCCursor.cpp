#include "pch.h"
#include "FreeMCCCursor.h"
#include "ModuleHook.h"
#include "MidhookFlagInterpreter.h"
#include "imgui.h"   // HCE fallback uses ImGui's software cursor




class FreeMCCCursorImpl : public TokenSharedRequestProvider
{
private:
	static inline FreeMCCCursorImpl* instance = nullptr;

	std::set<std::string> callersRequestingFreedCursor{};
	std::shared_ptr<ModuleMidHook> shouldCursorBeFreeHook;
	std::shared_ptr< MidhookFlagInterpreter> shouldCursorBeFreeFunctionFlagSetter;
	
	static void shouldCursorBeFreeHookFunction(SafetyHookContext& ctx)
	{
		instance->shouldCursorBeFreeFunctionFlagSetter->setFlag(ctx);
	}

public:
	FreeMCCCursorImpl(std::shared_ptr<PointerDataStore> ptr)
	{
		if (instance) throw HCMInitException("Cannot have more than one FreeMCCCursorImpl");

		auto shouldCursorBeFreeFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(shouldCursorBeFreeFunction));
		shouldCursorBeFreeFunctionFlagSetter = ptr->getData<std::shared_ptr<MidhookFlagInterpreter>>(nameof(shouldCursorBeFreeFunctionFlagSetter));
		shouldCursorBeFreeHook = ModuleMidHook::make(L"main", shouldCursorBeFreeFunction, shouldCursorBeFreeHookFunction, false);

		instance = this;
	}

	~FreeMCCCursorImpl() 
	{		PLOG_DEBUG << "~" << nameof(FreeMCCCursorImpl);  

		instance = nullptr;
	}

	virtual void updateService() override
	{
		bool requested = serviceIsRequested();
		PLOG_INFO << "FreeMCCCursor service is turning " << (requested ? "ON!" : "OFF!");
		shouldCursorBeFreeHook->setWantsToBeAttached(requested);
		PLOG_DEBUG << "FreeMCCCursor service hooks have been updated.";
	}

};








// Halo Campaign Evolved implementation. HCE is a different engine with no equivalent of MCC's
// shouldCursorBeFree function, so there is nothing to midhook. Instead we ask ImGui to draw its own software
// cursor while the service is requested - the game keeps its cursor hidden, but the user can see what they're
// pointing at. Same TokenSharedRequestProvider interface, so every consumer works unchanged, which also means
// the "Free cursor when GUI open" toggle finally constructs on HCE instead of failing.
class FreeCursorHCEImpl : public TokenSharedRequestProvider
{
public:
	FreeCursorHCEImpl() { PLOG_INFO << "FreeCursor: using Halo Campaign Evolved software-cursor implementation"; }
	virtual void updateService() override
	{
		bool requested = serviceIsRequested();
		PLOG_INFO << "FreeCursor (HCE) service is turning " << (requested ? "ON!" : "OFF!");
		if (ImGui::GetCurrentContext() != nullptr)
			ImGui::GetIO().MouseDrawCursor = requested;
	}
};

// Halo 5: Forge implementation.
//
// ⚠⚠⚠ THERE IS NO FUNCTION TO HOOK HERE, AND POINTING shouldCursorBeFreeFunction AT THIS WOULD CORRUPT THE
// GAME. halo5forge.exe imports NO user32 AT ALL - measured by parsing the PE import directory: 57 DLLs, not
// one cursor or input API. The 1x1 clip and the hidden cursor are the OS's response to
// ICoreWindow::SetPointerCapture, so there is no ClipCursor to xref and no predicate to midhook.
// FreeMCCCursorImpl does ModuleMidHook::make(...) on whatever shouldCursorBeFreeFunction resolves to, so
// naming this entry that would write a jmp trampoline into a live .data byte. It gets its own name.
//
// WHAT THIS IS: the engine's mouse-capture REQUEST byte. The platform dispatcher loop reads it every
// iteration at RVA 0x0060FFA4, compares it against the applied state at [obj+0x59], and calls
// SetMouseCaptured (0x00610390) when they disagree - whose disable path calls
// ICoreWindow::ReleasePointerCapture (vtable +0xB0) at 0x0061055D.
//
// So we write 0 and THE GAME releases the cursor itself, on its own thread, through its own code. That is
// the supported mechanism, and it is a pure data write - no hook, no engine call, no sim-thread pump.
//
// Correlation measured live over 1171 samples, with no counter-examples:
//     byte == 1  in 80/80    samples where the clip was 1x1 and CURSOR_SHOWING was clear
//     byte == 0  in 1091/1091 samples where the cursor was unclipped and visible
//
// ⚠ It is re-written every update rather than once on toggle. Game code writes this byte on UI transitions
// (0x00944C10 and the screen-stack sites), never per frame, so one write would usually hold - but a UI
// transition while the overlay is still up would re-request capture underneath us. Idempotent and cheap.
//
// ⚠ This byte is the master GAMEPLAY-vs-UI mouse mode switch, not merely "is the cursor free": at the
// MouseMoved delegate (0x0060CA80) a non-zero value feeds relative mouselook while zero takes the absolute
// pointer path. That is what we want while a menu is open, and it is exactly what the game itself does when
// it opens one - but it is a bigger lever than the name suggests, which is why the original value is saved
// and restored rather than assumed to be 1.
class FreeCursorH5Impl : public TokenSharedRequestProvider
{
private:
	std::shared_ptr<MultilevelPointer> mCaptureRequestFlag;
	std::optional<uint8_t> mSaved;

	static bool sehWrite8(void* dest, uint8_t v)
	{
		__try { *(uint8_t*)dest = v; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static bool sehRead8(const void* src, uint8_t& out)
	{
		__try { out = *(const uint8_t*)src; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

public:
	explicit FreeCursorH5Impl(std::shared_ptr<PointerDataStore> ptr)
	{
		mCaptureRequestFlag = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(h5MouseCaptureRequestedFlag));
		PLOG_INFO << "FreeCursor: using Halo 5: Forge capture-request implementation";
	}

	virtual void updateService() override
	{
		const bool requested = serviceIsRequested();
		try
		{
			uintptr_t addr = 0;
			if (!mCaptureRequestFlag->resolve(&addr) || !addr)
			{
				PLOG_ERROR << "FreeCursor (H5): could not resolve the capture request flag";
				return;
			}

			if (requested)
			{
				// Save the FIRST time only - repeated updates must not capture our own zero as "original".
				if (!mSaved.has_value())
				{
					uint8_t original = 1;
					if (sehRead8((const void*)addr, original)) mSaved = original;
				}
				sehWrite8((void*)addr, 0);   // idempotent; re-asserted in case a UI transition re-requested
			}
			else if (mSaved.has_value())
			{
				sehWrite8((void*)addr, mSaved.value());
				mSaved.reset();
				PLOG_INFO << "FreeCursor (H5) service OFF, capture request restored";
			}
		}
		catch (HCMRuntimeException& ex)
		{
			PLOG_ERROR << "FreeCursor (H5) failed: " << ex.what();
		}
	}

	~FreeCursorH5Impl()
	{
		// ⚠ Leaving the game in absolute-pointer mode would break mouselook for the rest of the session,
		// and HCM stays resident across sessions. Put it back.
		try
		{
			uintptr_t addr = 0;
			if (mSaved.has_value() && mCaptureRequestFlag
				&& mCaptureRequestFlag->resolve(&addr) && addr)
				sehWrite8((void*)addr, mSaved.value());
		}
		catch (...) {}
	}
};

// true when we're inside halo5forge.exe.
bool hostIsHalo5Forge()
{
	char path[MAX_PATH]{};
	if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, sizeof(path))) return false;
	std::string exe(path);
	auto slash = exe.find_last_of("\\/");
	if (slash != std::string::npos) exe = exe.substr(slash + 1);
	return boost::iequals(exe, "halo5forge.exe");
}

// true when we're inside HaloCampaignEvolved.exe rather than MCC.
bool hostIsCampaignEvolved()
{
	char path[MAX_PATH]{};
	if (!GetModuleFileNameA(GetModuleHandleA(NULL), path, sizeof(path))) return false;
	std::string exe(path);
	auto slash = exe.find_last_of("\\/");
	if (slash != std::string::npos) exe = exe.substr(slash + 1);
	return boost::iequals(exe, "HaloCampaignEvolved.exe");
}

// ⚠ Three titles, three completely different mechanisms - MCC midhooks a predicate, HaloCER has none so it
// falls back to drawing a software cursor, and Halo 5 writes the engine's own capture-request byte. Halo 5
// used to fall through to the MCC implementation, which could not find its pointer data, so the whole service
// failed to construct and took Toggle Free Cursor, Toggle Pause and Toggle Block Input down with it.
static std::shared_ptr<TokenSharedRequestProvider> makeCursorImpl(std::shared_ptr<PointerDataStore> ptr)
{
	if (hostIsHalo5Forge())
		return std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<FreeCursorH5Impl>(ptr));
	if (hostIsCampaignEvolved())
		return std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<FreeCursorHCEImpl>());
	return std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<FreeMCCCursorImpl>(ptr));
}

FreeMCCCursor::FreeMCCCursor(std::shared_ptr<PointerDataStore> ptr)
	: pimpl(makeCursorImpl(ptr)) {}

FreeMCCCursor::~FreeMCCCursor() = default;