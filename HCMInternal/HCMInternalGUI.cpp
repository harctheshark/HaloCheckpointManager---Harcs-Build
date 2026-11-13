#include "pch.h"
#include "HCMInternalGUI.h"
#include "GlobalKill.h"
#include "GUISimpleButton.h"
#include "MCCStateHook.h"
#include "imgui.h"
#include "imgui_internal.h" // ErrorRecoveryStoreState / TryToRecoverState: unwind ImGui stacks if the new layout throws mid-frame
#include "ImGuiManager.h"   // setSwallowGameInput fallback for games with no blockGameInputService
#include "Lapua.h"
#define addTooltip(x) if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(x)



// .x is only ever a sanity floor: "is the screen actually bigger than our window?", guarding against a junk
// getScreenSize. It tracks GUIWindowWidth so that question keeps meaning the same thing. .y is the real output -
// it becomes 2/3 of screen height and caps how tall the window may grow.
ImVec2 minimumWindowSize{ GUIWindowWidth, 500 };

void HCMInternalGUI::adjustWindowPosition()
{
	// aspect ratio adjustment to main gui position so that it appears over the top of the games ammo counter ui
	constexpr float ratio16to9 = 16.f / 9.f;
	float currentRatio = mFullScreenSize.x / mFullScreenSize.y;

	if (currentRatio > ratio16to9) // need to move HCM window pos to the right
	{
		// if the aspect radio were 16:9, how wide would the screen be?
		float widthIf16to9 = mFullScreenSize.y * ratio16to9;
		float currentExtraWidth = mFullScreenSize.x - widthIf16to9;
		PLOG_DEBUG << "currentExtraWidth: " << currentExtraWidth;

		constexpr float magicAdjustmentFactor = 0.6;


		float delta = currentExtraWidth * 0.5;// halve it since each side gets half the extra width
		delta = delta * magicAdjustmentFactor; // idk

		mWindowPos = SimpleMath::Vector2(10 + delta, 25);
	}
	else
	{
		mWindowPos = SimpleMath::Vector2(10, 25);
	}
}


void HCMInternalGUI::initializeHCMInternalGUI()
{
	ImGui::SetNextWindowCollapsed(false);
	windowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

	PLOG_VERBOSE << "actual screen size: " << mFullScreenSize.x << ", " << mFullScreenSize.y;
	if (mFullScreenSize.x > minimumWindowSize.x && mFullScreenSize.y > minimumWindowSize.y) // check for really small screens or getScreenSize returning a junk value, then we just use default 500, 500
	{
		// adjust vertical window height to be 2/3rds of screen
		minimumWindowSize.y = mFullScreenSize.y / 3 * 2;
	}

	adjustWindowPosition();

}



void HCMInternalGUI::onImGuiRenderEvent(SimpleMath::Vector2 screenSize)
{
	std::unique_lock<std::mutex> lock(mDestructionGuard);

	if (mFullScreenSize != screenSize) // recalculate our gui canvas size
	{
		mFullScreenSize = screenSize;
		if (screenSize.x > minimumWindowSize.x && screenSize.y > minimumWindowSize.y) // check for really small screens or getScreenSize returning a junk value, then we just use default 500, 500
		{
			// adjust vertical window height to be 2/3rds of screen
			minimumWindowSize.y = screenSize.y / 3 * 2;
		}

		adjustWindowPosition();
	}


	if (!m_HCMInternalGUIinitialized)
	{
		// This is the Present/render thread: a throw here is unhandled and takes the game down (it did, for every
		// Halo 2 Anniversary MP session, via GUIElementStore's .at(game)). Contain it and fall back to the
		// NoGame menu so the overlay still comes up.
		try
		{
			onGameStateChange(mccStateHook->getCurrentMCCState());
		}
		catch (const std::exception& ex)
		{
			PLOG_ERROR << "onGameStateChange threw on first frame (" << typeid(ex).name() << "): " << ex.what() << " - falling back to the NoGame menu";
			try
			{
				std::unique_lock<std::mutex> fallbackLock(currentGameGUIElementsMutex);
				p_currentGameGUIElements = &mGUIStore->getTopLevelGUIElements(GameState::Value::NoGame);
			}
			catch (const std::exception& ex2)
			{
				PLOG_ERROR << "NoGame menu fallback also failed: " << ex2.what() << " - keeping the previous menu";
			}
		}

		PLOG_INFO << "Initializing HCMInternalGUI";
		try
		{
			initializeHCMInternalGUI();
			m_HCMInternalGUIinitialized = true;
		}
		catch (HCMInitException& ex)
		{
			PLOG_FATAL << "Failed to initialize HCMInternalGUI, info: " << std::endl
				<< ex.what() << std::endl
				<< "HCM will now automatically close down";
			GlobalKill::killMe();
			return;
		}
	}


	renderErrorDialog();
	primaryRender();

}


static bool errorDialogShowing = false;
void HCMInternalGUI::renderErrorDialog()
{
	if (errorsToDisplay.empty())
	{
		return;
	}

	if (!errorDialogShowing)
	{
		ImGui::OpenPopup("Error!");
		errorDialogShowing = true;
	}

	SimpleMath::Vector2 size = SimpleMath::Vector2(300, 0);
	SimpleMath::Vector2 pos = SimpleMath::Vector2(mFullScreenSize / 2.f) - (size / 2.f);

	ImGui::SetNextWindowSize(size);
	ImGui::SetNextWindowPos(pos);

	if (ImGui::BeginPopupModal("Error!", NULL, ImGuiWindowFlags_NoResize))
	{

		ImGui::TextWrapped("HCM encountered an error!");
		ImGui::TextWrapped(errorsToDisplay.front().what());
		ImGui::TextWrapped("");
		ImGui::TextWrapped("Nerdy info for Burnt: ");

		ImGui::BeginChild("trace", ImVec2(ImGui::GetWindowSize().x - 15, 150), NULL);
		ImGui::TextWrapped("The error occured here:");
		ImGui::TextWrapped(errorsToDisplay.front().source().c_str());
		ImGui::TextWrapped("");
		ImGui::TextWrapped("Full stack trace:");
		ImGui::TextWrapped(errorsToDisplay.front().trace().c_str());
		ImGui::EndChild();

		if (ImGui::Button("OK"))
		{
			errorDialogShowing = false;
			errorsToDisplay.erase(errorsToDisplay.begin());
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		if (ImGui::Button("Copy to clipboard"))
		{
			std::string copyText = std::format("{}\n\n{}\n\n{}", errorsToDisplay.front().what(), errorsToDisplay.front().source(), errorsToDisplay.front().trace());
			ImGui::SetClipboardText(copyText.c_str());
		}
		ImGui::EndPopup();
	}

}

//https://www.modernescpp.com/index.php/dealing-with-mutation-locking/


void HCMInternalGUI::onGameStateChange(const MCCState& newState)
{

	LOG_ONCE_CAPTURE(PLOG_VERBOSE << "onGameStateChange locking currentGameGUIElementsMutex @ 0x" << std::hex << pMutex, pMutex = &currentGameGUIElementsMutex);
	std::unique_lock<std::mutex> lock(currentGameGUIElementsMutex);

	p_currentGameGUIElements = &mGUIStore->getTopLevelGUIElements(newState.currentGameState);
	mCurrentGame = newState.currentGameState; // the new layout indexes the element tree of this game
	LOG_ONCE(PLOG_VERBOSE << "unlocking currentGameGUIElementsMutex");
}





void HCMInternalGUI::setClassicLayout(bool classic)
{
	mSettings->classicLayout->GetValueDisplay() = classic;
	mSettings->classicLayout->UpdateValueWithInput(); // persisted (it is in allSerialisableOptions)
}

// Picks the layout. The new one is the default; the classic one is the fallback for every failure mode:
// the user asked for it (persisted "Classic layout" setting), the new one could not be constructed, or its
// render threw. A throw is contained HERE because this is the Present/render thread, where an escaping
// exception takes the game down.
void HCMInternalGUI::primaryRender()
{
	if (mPalette && !mPaletteFailed && !mSettings->classicLayout->GetValue())
	{
		ImGuiErrorRecoveryState recovery;
		ImGui::ErrorRecoveryStoreState(&recovery);
		try
		{
			primaryRenderPalette();
			return;
		}
		catch (const std::exception& ex)
		{
			PLOG_ERROR << "The new HCM layout threw while rendering (" << typeid(ex).name() << "): " << ex.what()
				<< " - switching to the classic layout for the rest of this session";
		}
		catch (...)
		{
			PLOG_ERROR << "The new HCM layout threw an unknown exception while rendering - switching to the classic layout for the rest of this session";
		}
		// It may have thrown between a Begin and its End, or with style / ID pushes outstanding. Put ImGui's
		// stacks back the way they were before this frame's attempt, then draw nothing more this frame; the
		// classic layout takes over from the next one.
		ImGui::ErrorRecoveryTryToRecoverState(&recovery);
		mPaletteFailed = true;
		mPalette->onWindowHidden(); // hand back the keyboard if the search box held it
		return;
	}
	primaryRenderClassic();
}

void HCMInternalGUI::primaryRenderPalette()
{
	std::unique_lock<std::mutex> lock(currentGameGUIElementsMutex);

	// Height follows content, up to the same cap the classic layout uses (2/3 of the screen).
	mWindowSize.y = std::clamp(mPaletteDesiredHeight, 60.f, std::max(60.f, minimumWindowSize.y));

	if (!m_WindowOpen && mSettings->hideWatermark->GetValue() && Lapua::lapuaGood && Lapua::lapuaGood2)
	{
		// collapsed and the watermark is hidden: draw nothing (same rule as the classic layout)
	}
	else
	{
		ImGui::SetNextWindowSize(mWindowSize);
		ImGui::SetNextWindowPos(mWindowPos);

		if (m_WindowOpen)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.80f, 0.40f, 1.00f));
		else
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.60f, 0.25f, 1.00f));

		ImGuiWindowFlags blockInputsIfNoCursor = *mIsCursorShowing == true ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs & ImGuiWindowFlags_NoMouseInputs;

		ImGui::SetNextWindowCollapsed(!m_WindowOpen);
		// Same window name / ID as the classic layout, so collapse state and the toggle hotkey behave identically.
		// The body scrolls on its own, so the window itself never needs a scrollbar.
		m_WindowOpen = ImGui::Begin(m_WindowOpen ? "Halo Checkpoint Manager###HCM" : "HCM###HCM", nullptr,
			windowFlags | blockInputsIfNoCursor | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		ImGuiManager::setMenuWantsCursor(m_WindowOpen);

		ImGui::PopStyleColor();

		if (m_WindowOpen)
		{
			const auto result = mPalette->render(mCurrentGame, [this]() { mGUIMCCState.render(); }, minimumWindowSize.y, *mIsCursorShowing == true);
			mPaletteDesiredHeight = result.desiredWindowHeight;
			if (result.switchToClassic) setClassicLayout(true);
		}

		ImGui::End(); // end main window
	}

	if (!m_WindowOpen) mPalette->onWindowHidden();

	// do stuff if window just opened
	if (m_WindowOpen && !m_windowOpenLastFrame)
	{
		onWindowJustOpened();
	}
	// do stuff if window just closed
	else if (!m_WindowOpen && m_windowOpenLastFrame)
	{
		onWindowJustClosed();
	}

	m_windowOpenLastFrame = m_WindowOpen;
}

void HCMInternalGUI::primaryRenderClassic()
{
	LOG_ONCE_CAPTURE(PLOG_VERBOSE << "onGameStateChange locking currentGameGUIElementsMutex @ 0x" << std::hex << pMutex, pMutex = &currentGameGUIElementsMutex);
	std::unique_lock<std::mutex> lock(currentGameGUIElementsMutex);
	// Calculate height of everything
	int totalContentHeight = 0;
	for (auto& element : *p_currentGameGUIElements)
	{
		totalContentHeight += element->getCurrentHeight();
	}

	totalContentHeight += 25; // some padding for.. actually I'm not sure why
	totalContentHeight += GUIFrameHeightWithSpacing; // for header


	mWindowSize.y = (minimumWindowSize.y > totalContentHeight ? totalContentHeight : minimumWindowSize.y);

	// check for hide-watermark toggle (only if window is collapsed)
	if (!m_WindowOpen && mSettings->hideWatermark->GetValue() && Lapua::lapuaGood && Lapua::lapuaGood2)
	{
		// do nothing
		LOG_ONCE(PLOG_DEBUG << "skipping rendering collapsed window due to hideWatermark setting");
	}
	else
	{
		// render
		ImGui::SetNextWindowSize(mWindowSize);
		ImGui::SetNextWindowPos(mWindowPos);

		if (m_WindowOpen)
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.80f, 0.40f, 1.00f));
		else
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.60f, 0.25f, 1.00f));



		ImGuiWindowFlags blockInputsIfNoCursor = *mIsCursorShowing == true ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs & ImGuiWindowFlags_NoMouseInputs;

		ImGui::SetNextWindowCollapsed(!m_WindowOpen);
		m_WindowOpen = ImGui::Begin(m_WindowOpen ? "Halo Checkpoint Manager###HCM" : "HCM###HCM", nullptr, windowFlags | blockInputsIfNoCursor); // Create window. Returns false when window collapsed

		// Windowless targets (Halo 5) have to wrestle the cursor away from the game to be usable, and must
		// hand it straight back when the menu closes or the player cannot aim. This is the authoritative
		// signal for that - see ImGuiManager::setMenuWantsCursor. No-op on every other title.
		ImGuiManager::setMenuWantsCursor(m_WindowOpen);

		ImGui::PopStyleColor();

		if (m_WindowOpen)  //only bother rendering children if it's not collapsed
		{	
			mGUIMCCState.render(); // render header

			// Way back to the new layout. A SmallButton on the header line itself: it is no taller than the text,
			// so the height bookkeeping above is unaffected.
			if (mPalette && !mPaletteFailed)
			{
				ImGui::SameLine(GUIWindowWidth - 100.f);
				if (ImGui::SmallButton("New layout")) setClassicLayout(false);
				addTooltip("Switch to the new window: search bar, favourites and tabs. The Classic layout button there switches back.");
			}

			// Tell all the current-games GUI elements to render themselves
			for (auto& element : *p_currentGameGUIElements)
			{
				element->render(*mHotkeyRenderer.get());
			}
		}

		ImGui::End(); // end main window

	}

	// do stuff if window just opened
	if (m_WindowOpen && !m_windowOpenLastFrame)
	{
		onWindowJustOpened();
	}
	// do stuff if window just closed
	else if (!m_WindowOpen && m_windowOpenLastFrame)
	{
		onWindowJustClosed();
	}

	m_windowOpenLastFrame = m_WindowOpen;

	LOG_ONCE(PLOG_VERBOSE << "unlocking currentGameGUIElementsMutex");

	//// little colour picker for gui stuff
	//ImGuiStyle* style = &ImGui::GetStyle();
	//ImVec4 newCol = style->Colors[ImGuiCol_ChildBg];
	//if (ImGui::ColorPicker4("col", &newCol.x))
	//{
	//	style->Colors[ImGuiCol_ChildBg] = newCol;
	//}

}

void HCMInternalGUI::onGUIShowingFreesMCCCursorChanged(bool& newval)
{
	if (newval && m_WindowOpen && mControlServices->freeMCCSCursorService.has_value() && !freeCursorRequest)
	{
		freeCursorRequest = mControlServices->freeMCCSCursorService.value()->makeScopedRequest();
	}
	else if (!newval && m_WindowOpen && freeCursorRequest)
	{
		freeCursorRequest.reset();
	}
}


void HCMInternalGUI::onGUIShowingBlocksGameInputChanged(bool& newval)
{
	if (newval && m_WindowOpen && mControlServices->blockGameInputService.has_value() && !blockGameInputRequest)
	{
		blockGameInputRequest = mControlServices->blockGameInputService.value()->makeScopedRequest();
	}
	else if (!newval && m_WindowOpen && blockGameInputRequest)
	{
		blockGameInputRequest.reset();
	}
}

void HCMInternalGUI::onGUIShowingPausesGameChanged(bool& newval)
{
	if (newval && m_WindowOpen && mControlServices->pauseGameService.has_value() && !pauseGameRequest)
	{
		pauseGameRequest = mControlServices->pauseGameService.value()->makeScopedRequest();
	}
	else if (!newval && m_WindowOpen && pauseGameRequest)
	{
		pauseGameRequest.reset();
	}
}

void HCMInternalGUI::onWindowJustOpened()
{
	mSettings->GUIWindowOpen->GetValueDisplay() = m_WindowOpen;
	mSettings->GUIWindowOpen->UpdateValueWithInput();

	// NB: on Halo Campaign Evolved these two services exist but use HCE-specific implementations (software
	// cursor / WndProc message filtering) - see FreeMCCCursor.cpp and BlockGameInput.cpp. Nothing game-specific
	// is needed here.
	if (mSettings->GUIShowingFreesCursor->GetValue() && mControlServices->freeMCCSCursorService.has_value())
		freeCursorRequest = mControlServices->freeMCCSCursorService.value()->makeScopedRequest();

	if (mSettings->GUIShowingBlocksInput->GetValue() && mControlServices->blockGameInputService.has_value())
		blockGameInputRequest = mControlServices->blockGameInputService.value()->makeScopedRequest();

	if (mSettings->GUIShowingPausesGame->GetValue() && mControlServices->pauseGameService.has_value())
		pauseGameRequest = mControlServices->pauseGameService.value()->makeScopedRequest();
}

void HCMInternalGUI::onWindowJustClosed()
{
	mSettings->GUIWindowOpen->GetValueDisplay() = m_WindowOpen;
	mSettings->GUIWindowOpen->UpdateValueWithInput();

	// releasing the request tokens turns the services back off (including the HCE implementations)
	if (freeCursorRequest)
		freeCursorRequest.reset();

	if (blockGameInputRequest)
		blockGameInputRequest.reset();

	if (pauseGameRequest)
		pauseGameRequest.reset();
}

