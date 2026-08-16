#include "pch.h"
#include "ModuleHook.h"
#include "ModuleCache.h"
#include "ModuleHookManager.h"
#include "GlobalKill.h"
#include "HookGraveyard.h"

std::unique_ptr<ModuleInlineHook> ModuleInlineHook::make(const std::wstring associatedModule, std::shared_ptr<MultilevelPointer> original_func, void* new_func, bool startEnabled)
{
	auto ptr = std::unique_ptr< ModuleInlineHook>(new ModuleInlineHook(associatedModule, original_func, new_func, startEnabled));
	ModuleHookManager::addHook(associatedModule, ptr.get());
	return ptr;

}

std::unique_ptr<ModuleMidHook> ModuleMidHook::make(const std::wstring associatedModule, std::shared_ptr<MultilevelPointer> original_func, safetyhook::MidHookFn new_func, bool startEnabled)
{
	auto ptr = std::unique_ptr< ModuleMidHook>(new ModuleMidHook(associatedModule, original_func, new_func, startEnabled));
	ModuleHookManager::addHook(associatedModule, ptr.get());
	return ptr;
}

std::unique_ptr<ModulePatch> ModulePatch::make(const std::wstring associatedModule, std::shared_ptr<MultilevelPointer> original_func, std::vector<byte> patchedBytes, bool startEnabled)
{
	auto ptr = std::unique_ptr< ModulePatch>(new ModulePatch(associatedModule, original_func, patchedBytes, startEnabled));
	ModuleHookManager::addHook(associatedModule, ptr.get());
	return ptr;
}

std::unique_ptr<ModulePatch> ModulePatch::makeNOPCall(const std::wstring associatedModule, std::shared_ptr<MultilevelPointer> original_func, bool startEnabled)
{
	return ModulePatch::make(associatedModule, original_func, std::vector<byte>{0x90, 0x90, 0x90, 0x90, 0x90}, startEnabled);
}

const std::wstring& ModuleHookBase::getAssociatedModule() const
{
	return this->mAssociatedModule;
}

ModuleInlineHook::~ModuleInlineHook()
{
	detach();
	ModuleHookManager::removeHook(getAssociatedModule(), this);

}

ModuleMidHook::~ModuleMidHook()
{
	detach();
	ModuleHookManager::removeHook(getAssociatedModule(), this);
}

ModulePatch::~ModulePatch()
{
	detach();
	ModuleHookManager::removeHook(getAssociatedModule(), this);
}



void ModuleInlineHook::attach()
{
	PLOG_DEBUG << "inline_hook attempting attach: " << getAssociatedModule();
	PLOG_VERBOSE << "hookFunc " << this->mHookFunction;
	if (this->isHookInstalled()) {
		PLOG_DEBUG << "attach failed: hook already installed";
		return;
	}

	if (this->mOriginalFunction == nullptr)
	{
		PLOG_DEBUG << "attach failed: no pointer to original function";
		return;
	}

	uintptr_t pOriginalFunction;
	if (!this->mOriginalFunction->resolve(&pOriginalFunction))
	{
		PLOG_ERROR << "attach failed: pOriginalFunction pointer failed to resolve" << MultilevelPointer::GetLastError();
		return;
	}

	PLOG_VERBOSE << "pOriginalFunction " << std::hex << pOriginalFunction;

	this->mInlineHook = safetyhook::create_inline((void*)pOriginalFunction, this->mHookFunction);

	// ⚠ create_inline's result was previously never checked, so a hook that failed to install looked
	// EXACTLY like one that succeeded - including printing "successfully attached" below. That is how
	// the OBS bypass managed to be dead for years while its toggle lit up. Callers that care can now
	// ask isHookInstalled(); everyone else at least gets a log line naming the address that failed.
	if (!this->mInlineHook)
	{
		PLOG_ERROR << std::format("attach failed: safetyhook::create_inline could not hook 0x{:X} in ", pOriginalFunction)
			<< this->getAssociatedModule();
		return;
	}

	PLOG_DEBUG << "inline_hook successfully attached: " << this->getAssociatedModule();

}

void ModuleMidHook::attach()
{
	PLOG_DEBUG << "mid_hook attempting attach: " << this->getAssociatedModule();
	if (this->isHookInstalled()) {
		PLOG_DEBUG << "attach failed: hook already installed";
		return;
	}

	if (this->mOriginalFunction == nullptr)
	{
		PLOG_DEBUG << "attach failed: no pointer to original function";
		return;
	}

	uintptr_t pOriginalFunction;
	if (!this->mOriginalFunction->resolve(&pOriginalFunction))
	{
		PLOG_ERROR << "attach failed: pOriginalFunction pointer failed to resolve: " << MultilevelPointer::GetLastError();
		return;
	}

	PLOG_VERBOSE << "pOriginalFunction: " << std::hex << pOriginalFunction;
	PLOG_VERBOSE << "mHookFunction: " << std::hex << this->mHookFunction;
	PLOG_VERBOSE << "this->mMidhook: " << this->mMidHook.operator bool();


	this->mMidHook = safetyhook::create_mid((void*)pOriginalFunction, this->mHookFunction);


	PLOG_DEBUG << "mid_hook successfully attached: " << this->getAssociatedModule();
}

// ====================================================================================================================
// THE HOOK GRAVEYARD - why shutdown does not destroy hooks.
//
// Destroying a safetyhook hook does TWO things: it restores the target's original bytes, and it frees the
// trampoline page holding the relocated prologue plus (for a mid hook) the stub that saves context and calls our
// handler. The first is required. The second is what killed the game.
//
// MEASURED, not theorised. With first-chance logging armed, teardown produced:
//
//     [thread 41852] ~HCECheckpointDetours
//     [thread 41852] ModuleMidHook::detach: detaching hook: HaloSimulation_tag_release.dll
//     [thread 26268] FIRST-CHANCE EXCEPTION 0xC0000005 at <no module - unmapped or freed memory>
//                    - tried to EXECUTE 0x7FFDCBB00072
//     [thread 41852] successfully detached
//
// A GAME thread was executing inside the trampoline at the instant HCM's thread freed it. safetyhook freezes
// threads and rewrites an instruction pointer sitting in the relocated PROLOGUE, but a thread parked further in -
// in the stub, or returning through it - is not covered, and the page goes away underneath it.
//
// Downstream that reads as a crash with no HCM in the callstack: the access violation is swallowed by a __except
// above it (Streamline wraps the swapchain when DLSS/frame generation is on), Present returns E_ABORT, and UE
// raises LowLevelFatalError from D3D12Util.cpp. It is intermittent because it needs a thread to be inside the
// trampoline during the microsecond it is freed - which is also why it looked unrelated to whatever was toggled.
//
// So at SHUTDOWN we disable instead of destroy: disable() restores the original bytes, so no new thread can enter,
// and the hook object is then parked here forever so its trampoline stays mapped for anyone already inside. The
// cost is a few pages leaked in a process that is about to lose HCM anyway. Exactly the same trade
// HCETriggerActivity documents for its stub page, and the one ~D3D12Hook makes when it abandons GPU resources.
//
// ⚠ Normal (non-shutdown) detaches still destroy, because HCM stays alive to re-attach and must not leak a page
// per toggle. The risk window is identical either way, but a running HCM re-attaching is the common case and the
// game is not being torn down around it.
// ====================================================================================================================
void ModuleInlineHook::detach()
{
	PLOG_DEBUG << "detaching hook: " << this->getAssociatedModule();
	if (!this->isHookInstalled()) {
		PLOG_DEBUG << "already detached";
		return;
	}

	if (GlobalKill::isKillSet())
	{
		// Restore the bytes, keep the trampoline. See HookGraveyard.h.
		if (HookGraveyard::park(this->mInlineHook))
		{
			PLOG_DEBUG << "shutdown: disabled and parked inline hook on " << this->getAssociatedModule()
				<< " (trampoline deliberately leaked)";
			return;
		}
		PLOG_ERROR << "could not disable inline hook on " << this->getAssociatedModule()
			<< " during shutdown; destroying it instead, which risks freeing a trampoline a thread is inside";
	}

	this->mInlineHook = {};
	PLOG_DEBUG << "successfully detached " << this->getAssociatedModule();
}

void ModuleMidHook::detach()
{
	PLOG_DEBUG << "detaching hook: " << this->getAssociatedModule();
	if (!this->isHookInstalled()) {
		PLOG_DEBUG << "already detached";
		return;
	}

	if (GlobalKill::isKillSet())
	{
		if (HookGraveyard::park(this->mMidHook))
		{
			PLOG_DEBUG << "shutdown: disabled and parked mid hook on " << this->getAssociatedModule()
				<< " (trampoline deliberately leaked)";
			return;
		}
		PLOG_ERROR << "could not disable mid hook on " << this->getAssociatedModule()
			<< " during shutdown; destroying it instead, which risks freeing a trampoline a thread is inside";
	}

	this->mMidHook = {};

	PLOG_DEBUG << "successfully detached " << this->getAssociatedModule();
}



void ModulePatch::attach()
{
	PLOG_VERBOSE << " ModulePatch::attach()";
#define logErrorReturn(x, y) if (x) { PLOG_ERROR << y; return; }

	uintptr_t addy;
	mOriginalFunction->resolve(&addy);
	logErrorReturn(IsBadReadPtr((void*)addy, mPatchedBytes.size()), std::format("Bad read ptr at {:x}", addy));
	//PLOG_DEBUG << "mOriginalFunction loc: " << std::hex << (uint64_t)addy;
	//PLOG_DEBUG << "mPatchedBytes size: " << mPatchedBytes.size();


	if (mOriginalBytes.empty())
	{
		mOriginalBytes.resize(mPatchedBytes.size());
		logErrorReturn(mOriginalFunction->readArrayData(mOriginalBytes.data(), mPatchedBytes.size()) == false, std::format("Could not resolve original function: {}", MultilevelPointer::GetLastError()));
	}

	std::vector<byte> currentBytes;
	currentBytes.resize(mPatchedBytes.size());
	logErrorReturn(mOriginalFunction->readArrayData(currentBytes.data(), mPatchedBytes.size()) == false, std::format("Could not resolve original function: {}", MultilevelPointer::GetLastError()));

	logErrorReturn(currentBytes != mOriginalBytes, "Current bytes did not match original bytes");


	if (mOriginalFunction->writeArrayData(mPatchedBytes.data(), mPatchedBytes.size(), true) == false) 
	{
			PLOG_ERROR << std::format("Failed to patch new bytes: {}", MultilevelPointer::GetLastError());
	};


}

void ModulePatch::detach()
{

	PLOG_VERBOSE << " ModulePatch::detach()";
#define logErrorReturn(x, y) if (x) { PLOG_ERROR << y; return; }
	logErrorReturn(mOriginalBytes.empty(), "No original bytes saved to restore");

	PLOG_VERBOSE << "mOriginalBytes size: " << mOriginalBytes.size();

	std::vector<byte> currentBytes;
	currentBytes.resize(mPatchedBytes.size());
	logErrorReturn(mOriginalFunction->readArrayData(currentBytes.data(), mPatchedBytes.size()) == false, "Failed to read current bytes");

	logErrorReturn(currentBytes != mPatchedBytes, "Current bytes did not match patched bytes")


	if (mOriginalFunction->writeArrayData(mOriginalBytes.data(), mOriginalBytes.size(), true) == false) 
	{
		PLOG_ERROR << std::format("Failed to restore original bytes: {}", MultilevelPointer::GetLastError());
	};



}





void ModuleHookBase::updateHookState()
{
	if (this->mWantsToBeAttached)
	{
		this->attach();
	}
	else
	{
		this->detach();
	}
}


// Getter/setter for mWantsToBeAttached
void ModuleHookBase::setWantsToBeAttached(bool value)
{
	if (this->mWantsToBeAttached != value)
	{
		this->mWantsToBeAttached = value;
		this->updateHookState();
	}
}


bool ModuleHookBase::getWantsToBeAttached() const
{
	return this->mWantsToBeAttached;
}

// Gets a ref to the safetyhook object, mainly used for calling the original function from within the new function
safetyhook::InlineHook& ModuleInlineHook::getInlineHook()
{
	return this->mInlineHook;
}



bool ModuleInlineHook::isHookInstalled() const
{
	return this->mInlineHook.operator bool();
}

bool ModuleMidHook::isHookInstalled() const
{
	return this->mMidHook.operator bool();
}

bool ModulePatch::isHookInstalled() const
{
	std::vector<byte> currentBytes;
	if (!mOriginalFunction->readArrayData(&currentBytes, mOriginalBytes.size())) return false;
	return currentBytes == mPatchedBytes;
}