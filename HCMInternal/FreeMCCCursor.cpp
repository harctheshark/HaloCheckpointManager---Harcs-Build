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

FreeMCCCursor::FreeMCCCursor(std::shared_ptr<PointerDataStore> ptr)
	: pimpl(hostIsCampaignEvolved()
		? std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<FreeCursorHCEImpl>())
		: std::static_pointer_cast<TokenSharedRequestProvider>(std::make_shared<FreeMCCCursorImpl>(ptr))) {}

FreeMCCCursor::~FreeMCCCursor() = default;