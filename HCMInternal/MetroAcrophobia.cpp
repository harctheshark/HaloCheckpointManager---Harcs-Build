#include "pch.h"
#include "MetroAcrophobia.h"
#include "MetroMove.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "GlobalKill.h"

class MetroAcrophobia::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccWeak;
	std::weak_ptr<IMessagesGUI> messagesWeak;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;
	std::atomic<bool> mReady{ false };

	void onToggle(bool on)
	{
		metro::move::setAcrophobia(on);
		if (auto m = messagesWeak.lock()) m->addMessage(on ? "Acrophobia on: hold jump to fly, crouch to hover" : "Acrophobia off");
	}

	void onRender(SimpleMath::Vector2)
	{
		if (!mReady.load(std::memory_order_acquire) || GlobalKill::isKillSet()) return;
		auto s = settingsWeak.lock();
		auto m = mccWeak.lock();
		if (!s || !m || !s->h5AcrophobiaToggle->GetValue()) return;
		if (!m->isGameCurrentlyPlaying(mGame) || ImGui::GetIO().WantTextInput) { metro::move::setAcrophobiaInput(false, false); return; }
		const bool jump = ImGui::IsKeyDown(ImGuiKey_Space) || ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown);
		const bool crouch = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_GamepadL3);
		metro::move::setAcrophobiaInput(jump, crouch);
	}

	ScopedCallback<ToggleEvent> mToggleCallback;
	ScopedCallback<RenderEvent> mRenderCallback;

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccWeak(dicon.Resolve<IMCCStateHook>()),
		messagesWeak(dicon.Resolve<IMessagesGUI>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->h5AcrophobiaToggle->valueChangedEvent, [this](bool& on) { onToggle(on); }),
		mRenderCallback(dicon.Resolve<RenderEvent>().lock(), [this](SimpleMath::Vector2 ss) { onRender(ss); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::MetroRedux) throw HCMInitException("MetroAcrophobia is Metro Redux only");
		metro::move::init();
		if (auto s = settingsWeak.lock(); s && s->h5AcrophobiaToggle->GetValue()) metro::move::setAcrophobia(true);
		mReady.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mReady.store(false, std::memory_order_release);
		mToggleCallback.removeCallback();
		mRenderCallback.removeCallback();
		metro::move::setAcrophobia(false);
	}
};

MetroAcrophobia::MetroAcrophobia(GameState game, IDIContainer& dicon) : pimpl(std::make_unique<Impl>(game, dicon)) {}
MetroAcrophobia::~MetroAcrophobia() { PLOG_VERBOSE << "~" << getName(); }
