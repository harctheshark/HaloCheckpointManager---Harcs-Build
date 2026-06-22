#include "pch.h"
#include "SunScaleFix.h"
#include "ModuleHook.h"
#include "SettingsStateAndEvents.h"
#include "PointerDataStore.h"

// Two ModulePatches (the two operand repoints). Driven by the shared-request token: attached while the
// sunScaleFixToggle is on and a Halo 2 game is playing, restored otherwise. Mirrors HideHUDImplDoublePatch.
template <GameState::Value gameT>
class SunScaleFixImpl : public TokenSharedRequestProvider
{
private:
	GameState mGame;

	static inline std::shared_ptr<ModulePatch> sunScaleFixPatch1;
	static inline std::shared_ptr<ModulePatch> sunScaleFixPatch2;

public:
	SunScaleFixImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl)
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();

		auto sunScaleFixFunction1 = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(sunScaleFixFunction1), gameImpl);
		auto sunScaleFixCode1 = ptr->getVectorData<byte>(nameof(sunScaleFixCode1), gameImpl);
		sunScaleFixPatch1 = ModulePatch::make(gameImpl.toModuleName(), sunScaleFixFunction1, *sunScaleFixCode1.get());

		auto sunScaleFixFunction2 = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(sunScaleFixFunction2), gameImpl);
		auto sunScaleFixCode2 = ptr->getVectorData<byte>(nameof(sunScaleFixCode2), gameImpl);
		sunScaleFixPatch2 = ModulePatch::make(gameImpl.toModuleName(), sunScaleFixFunction2, *sunScaleFixCode2.get());
	}

	virtual void updateService() override
	{
		bool newState = serviceIsRequested();
		sunScaleFixPatch1->setWantsToBeAttached(newState);
		sunScaleFixPatch2->setWantsToBeAttached(newState);
	}
};


SunScaleFix::SunScaleFix(GameState gameImpl, IDIContainer& dicon)
	:
	mGame(gameImpl),
	mSunScaleFixToggleCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->sunScaleFixToggle->valueChangedEvent, [this](bool& n) { onSunScaleFixToggleEvent(n); }),
	mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
	messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
	runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_shared<SunScaleFixImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;
	default:
		throw HCMInitException("SunScaleFix not impl for this game");
	}
}

SunScaleFix::~SunScaleFix()
{
	PLOG_DEBUG << "~" << getName();
}
