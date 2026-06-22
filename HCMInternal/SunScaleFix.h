#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "SharedRequestProvider.h"
#include "IMCCStateHook.h"
#include "RuntimeExceptionHandler.h"
#include "IMessagesGUI.h"

// Halo 2: anchors the sun glow + its occlusion query at the fixed corona distance (1023.875) instead of
// the live far-clip plane, so raising Far Clip Distance no longer shrinks/squares the sun. Implemented as
// two 4-byte RIP-relative operand repoints (ModulePatch), driven by sunScaleFixToggle. Mirrors HideHUD.
class SunScaleFix : public IOptionalCheat
{
private:
	GameState mGame;
	std::shared_ptr<TokenSharedRequestProvider> pimpl; // shared because of shared_from_this

	ScopedCallback<ToggleEvent> mSunScaleFixToggleCallbackHandle;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::optional<std::shared_ptr<SharedRequestToken>> currentServiceRequest = std::nullopt;

	void onSunScaleFixToggleEvent(bool& newValue)
	{
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);

			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false)
			{
				currentServiceRequest = std::nullopt;
				return;
			}

			if (newValue)
				currentServiceRequest = pimpl->makeScopedRequest();
			else
				currentServiceRequest = std::nullopt;
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	SunScaleFix(GameState gameImpl, IDIContainer& dicon);

	~SunScaleFix();

	std::string_view getName() override { return nameof(SunScaleFix); }

	std::shared_ptr<SharedRequestToken> makeScopedRequest() { return pimpl->makeScopedRequest(); }
};
