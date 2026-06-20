#include "pch.h"
#include "Season7Physics.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"
#include <array>

// The patched site is a `mulss xmm6, dword ptr [rip+disp32]` that multiplies by a float constant.
// Only the rip displacement (byte index 4) differs between the two constants; we rewrite the whole
// 8-byte instruction for clarity. ON = Season 7 constant, OFF = the game's default constant.
template <GameState::Value gameT>
class Season7PhysicsImpl : public ISeason7PhysicsImpl
{
private:
	GameState mGame;

	ScopedCallback<ToggleEvent> mToggleCallback;

	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	std::shared_ptr<MultilevelPointer> season7PhysicsPatch;

	static constexpr std::array<uint8_t, 8> kSeason7Bytes{ 0xF3, 0x0F, 0x59, 0x35, 0xCA, 0x4E, 0x52, 0x00 };
	static constexpr std::array<uint8_t, 8> kDefaultBytes{ 0xF3, 0x0F, 0x59, 0x35, 0x9E, 0x4E, 0x52, 0x00 };

	void writePatch(const std::array<uint8_t, 8>& bytes)
	{
		// protectedMemory = true: target is executable .text, so this goes through VirtualProtect.
		if (!season7PhysicsPatch->writeArrayData(const_cast<uint8_t*>(bytes.data()), bytes.size(), true))
			throw HCMRuntimeException(std::format("Failed to write Season7Physics patch: {}", MultilevelPointer::GetLastError()));
	}

	void onToggle(bool& newValue)
	{
		PLOG_DEBUG << "Season7Physics onToggle, newValue: " << newValue;
		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;

			writePatch(newValue ? kSeason7Bytes : kDefaultBytes);
			messagesGUI->addMessage(newValue ? "Season 7 Physics enabled." : "Season 7 Physics disabled.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	Season7PhysicsImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->season7PhysicsToggle->valueChangedEvent, [this](bool& n) { onToggle(n); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>())
	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		season7PhysicsPatch = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(season7PhysicsPatch), mGame);
	}

	// restore the default scalar so we never leave the game patched after HCM unloads
	~Season7PhysicsImpl()
	{
		try { writePatch(kDefaultBytes); }
		catch (HCMRuntimeException& ex) { PLOG_ERROR << ex.what(); }
	}
};


Season7Physics::Season7Physics(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
	case GameState::Value::Halo2:
		pimpl = std::make_unique<Season7PhysicsImpl<GameState::Value::Halo2>>(gameImpl, dicon);
		break;

	default:
		throw HCMInitException("Season7Physics not impl for this game");
	}
}

Season7Physics::~Season7Physics()
{
	PLOG_DEBUG << "~" << getName();
}
