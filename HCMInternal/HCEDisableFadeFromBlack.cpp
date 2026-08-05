#include "pch.h"
#include "HCEDisableFadeFromBlack.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "PointerDataStore.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"

// See HCEDisableFadeFromBlack.h for the full derivation.

namespace
{
	std::string bytesToString(const std::vector<byte>& bytes)
	{
		std::string out;
		for (auto b : bytes)
		{
			if (!out.empty()) out += ' ';
			out += std::format("{:02X}", (uint32_t)(unsigned char)b);
		}
		return out;
	}
}

class HCEDisableFadeFromBlack::HCEDisableFadeFromBlackImpl
{
private:
	GameState mGame;
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;

	// The patch target is the IMMEDIATE (one byte), the guard covers the WHOLE instruction that contains it -
	// so a match proves we are looking at `mov r9d, 3Ch` and not at whatever a game update might have moved
	// into that address.
	std::shared_ptr<MultilevelPointer> mPatchTarget;    // 0x2117CA, the imm8
	std::shared_ptr<MultilevelPointer> mGuardSite;      // 0x2117C8, the instruction start
	std::shared_ptr<std::vector<byte>> mExpectedBytes;  // 41 B9 3C 00 00 00
	std::shared_ptr<std::vector<byte>> mPatchCode;      // 00

	std::unique_ptr<ModulePatch> mPatch;

	std::mutex mAttachMutex;
	bool mVerified = false;   // sticky - once patched, the site no longer reads as the original

	std::atomic<bool> mReady{ false };

	// ⚠ HCE has no version resource (every build reports 0.0.0.0), so this is the ONLY thing preventing a game
	// update from having us write 0x00 over an unrelated instruction's operand. DO NOT REMOVE IT.
	void verifyOriginalBytes()
	{
		if (!mGuardSite) throw HCMRuntimeException("No guard pointer data for the HaloCER fade patch");
		if (!mExpectedBytes || mExpectedBytes->empty()) throw HCMRuntimeException("No expected original bytes for the HaloCER fade patch");

		const std::vector<byte>& expected = *mExpectedBytes;
		std::vector<byte> actual(expected.size());
		if (!mGuardSite->readArrayData(actual.data(), actual.size()))
			throw HCMRuntimeException(std::format("Could not read the HaloCER fade patch site: {}", MultilevelPointer::GetLastError()));

		if (actual != expected)
			throw HCMRuntimeException(std::format(
				"The HaloCER fade patch site did not match its expected original bytes - this build of Halo "
				"Campaign Evolved is not supported. Expected [{}], found [{}]",
				bytesToString(expected), bytesToString(actual)));
	}

	void onToggle(bool& newValue)
	{
		if (!mReady.load(std::memory_order_acquire)) return;

		try
		{
			lockOrThrow(mccStateHookWeak, mccStateHook);
			lockOrThrow(messagesGUIWeak, messagesGUI);

			if (newValue)
			{
				std::scoped_lock lock(mAttachMutex);
				if (!mVerified)
				{
					verifyOriginalBytes();
					mVerified = true;
					PLOG_INFO << "HaloCER fade patch site matched its expected original bytes";
				}
				mPatch->setWantsToBeAttached(true);
			}
			else
			{
				mPatch->setWantsToBeAttached(false);
			}

			if (mccStateHook->isGameCurrentlyPlaying(mGame))
				messagesGUI->addMessage(newValue
					? "Fade from black disabled - reverts come back instantly."
					: "Fade from black restored.");
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

	// Declared LAST - a ScopedCallback subscribes inside its own constructor.
	ScopedCallback<ToggleEvent> mToggleCallback;

public:
	HCEDisableFadeFromBlackImpl(GameState game, IDIContainer& dicon)
		: mGame(game),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mToggleCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceDisableFadeFromBlackToggle->valueChangedEvent, [this](bool& n) { onToggle(n); })
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEDisableFadeFromBlack only supports Halo Campaign Evolved");

		// Pure pointer-data lookups; no game memory is touched here (the sim dll may not even be loaded yet).
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		mPatchTarget = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceDisableFadeFromBlackFunction), mGame);
		mGuardSite = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(hceDisableFadeFromBlackGuardSite), mGame);
		mExpectedBytes = ptr->getVectorData<byte>(nameof(hceDisableFadeFromBlackOriginalBytes), mGame);
		mPatchCode = ptr->getVectorData<byte>(nameof(hceDisableFadeFromBlackCode), mGame);

		// startEnabled = false. Nothing is written until the user asks for it.
		mPatch = ModulePatch::make(mGame.toModuleName(), mPatchTarget, *mPatchCode, false);

		mReady.store(true, std::memory_order_release);
	}

	~HCEDisableFadeFromBlackImpl()
	{
		mReady.store(false, std::memory_order_release);
		if (mPatch) mPatch->setWantsToBeAttached(false);
		mToggleCallback.removeCallback();
	}
};


HCEDisableFadeFromBlack::HCEDisableFadeFromBlack(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<HCEDisableFadeFromBlackImpl>(game, dicon))
{
}

HCEDisableFadeFromBlack::~HCEDisableFadeFromBlack()
{
	PLOG_VERBOSE << "~" << getName();
}
