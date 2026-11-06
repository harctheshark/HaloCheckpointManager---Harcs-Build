#include "pch.h"
#include "SwitchBSP.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "SettingsStateAndEvents.h"
#include "RuntimeExceptionHandler.h"
#include "PointerDataStore.h"
#include "MultilevelPointer.h"
#include "GetTagName.h"
#include "IMakeOrGetCheat.h"
#include "H2CV_Halo2.h"
#include <functional>
#include <mutex>

namespace
{
	std::mutex g_bspNamesMutex;
	std::function<std::vector<std::string>()> g_bspNames;   // set while the Halo 2 implementation lives

	bool safeRead32(uintptr_t a, uint32_t& v)
	{
		__try { v = *(volatile uint32_t*)a; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}

namespace SwitchBSPNames
{
	static std::vector<std::string> names()
	{
		std::function<std::vector<std::string>()> f;
		{ std::scoped_lock lk(g_bspNamesMutex); f = g_bspNames; }
		return f ? f() : std::vector<std::string>{};
	}
	std::string describe(int index)
	{
		const auto n = names();
		if (index < 0 || index >= (int)n.size()) return "";
		const size_t cut = n[index].find_last_of("\\/");
		return cut == std::string::npos ? n[index] : n[index].substr(cut + 1);
	}
	std::string listAll()
	{
		const auto n = names();
		std::string s;
		for (size_t i = 0; i < n.size(); ++i) s += std::format("{}: {}\n", i, n[i]);
		return s;
	}
}

template <GameState::Value gameT>
class SwitchBSPImpl : public ISwitchBSPImpl
{
	// which game is this implementation for
	GameState mGame;

	// event callbacks
	ScopedCallback<ActionEvent> switchBSPEventCallbackHandle;

	// injected services
	std::weak_ptr<IMCCStateHook> mccStateHookWeak;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	std::weak_ptr<SettingsStateAndEvents> settingsWeak;

	// data
	std::shared_ptr<MultilevelPointer> switchBSPFunction;

	// Halo 2: the loaded scenario's BSP names for the switch control, cached per (scenario, block) - 2026-11-02
	std::weak_ptr<GetTagName> getTagNameWeak;
	std::mutex cacheMutex;
	uintptr_t cachedScenario = 0, cachedFirst = 0; uint32_t cachedCount = 0;
	std::vector<std::string> cachedNames;

	std::vector<std::string> readBspNames()
	{
		std::scoped_lock lk(cacheMutex);
		uint32_t lo = 0, hi = 0;
		const uintptr_t pscen = h2::base() + h2::rva::SCENARIO;
		if (!safeRead32(pscen, lo) || !safeRead32(pscen + 4, hi)) return {};
		const uintptr_t scen = ((uintptr_t)hi << 32) | lo;
		h2::TagPools p; uintptr_t first = 0; uint32_t count = 0;
		if (!scen || !h2::tagPools(p) || !h2::resolveBlock(scen + 0x210, p, first, count) || count == 0 || count > 64) return {};
		if (scen == cachedScenario && first == cachedFirst && count == cachedCount) return cachedNames;
		std::vector<std::string> names;
		auto getTagName = getTagNameWeak.lock();
		for (uint32_t i = 0; i < count; ++i)
		{
			uint32_t datum = 0xFFFFFFFF;
			std::string name;
			if (getTagName && safeRead32(first + (uintptr_t)i * 0x44 + 0x14, datum) && datum != 0xFFFFFFFF)
			{
				try { const char* s = getTagName->getTagName(Datum(datum)); if (s) name = s; }
				catch (HCMRuntimeException&) {}
			}
			names.push_back(name.empty() ? std::string("?") : name);
		}
		cachedScenario = scen; cachedFirst = first; cachedCount = count; cachedNames = names;
		return names;
	}

	// primary event callback
	void onSwitchBSPEvent()
	{
		try
		{
			PLOG_DEBUG << "onSwitchBSPEvent";
			lockOrThrow(mccStateHookWeak, mccStateHook);
			if (mccStateHook->isGameCurrentlyPlaying(mGame) == false) return;
			 
			lockOrThrow(settingsWeak, settings);
			lockOrThrow(messagesGUIWeak, messagesGUI);
			int64_t newBSPIndex = settings->switchBSPIndex->GetValue();

			uintptr_t pSwitchBSPFunction;
			if (!switchBSPFunction->resolve(&pSwitchBSPFunction)) throw HCMRuntimeException("Could not resolve pointer to switchBSPFunction function");      

			typedef int64_t(__fastcall *SwitchBSPFunction_t)(int64_t newIndex);
			SwitchBSPFunction_t switchBSPFunction_vptr = static_cast<SwitchBSPFunction_t>((void*)pSwitchBSPFunction);

			PLOG_DEBUG << "Calling switch BSP function at ptr " << (uint64_t)pSwitchBSPFunction << " with arg " << newBSPIndex;
			auto result = switchBSPFunction_vptr(newBSPIndex);

			PLOG_VERBOSE << "useless switchBSP return: " << result; // we get garbage data here cos we're calling a subroutine. would prob need to switch to a inline hook if we cared but it doesn't really matter

			// todo (maybe): get proper return (success value) and throw if failed
			constexpr const char* nameOfThing = (gameT == GameState::Value::Halo1 || gameT == GameState::Value::Halo2) ? "BSP" : "Zone Set";
			messagesGUI->addMessage(std::format("Switching {} to {}", nameOfThing, newBSPIndex));
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
		


	}

public:
	SwitchBSPImpl(GameState gameImpl, IDIContainer& dicon)
		: mGame(gameImpl),
		switchBSPEventCallbackHandle(dicon.Resolve<SettingsStateAndEvents>().lock()->switchBSPEvent, [this]() { onSwitchBSPEvent(); }),
		mccStateHookWeak(dicon.Resolve<IMCCStateHook>()),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		settingsWeak(dicon.Resolve<SettingsStateAndEvents>())

	{
		auto ptr = dicon.Resolve<PointerDataStore>().lock();
		switchBSPFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(switchBSPFunction), mGame);
		if constexpr (gameT == GameState::Value::Halo2)
		{
			// BSP names are a nicety: a missing tag-name service leaves the control index-only
			try { GameState game = gameImpl; getTagNameWeak = resolveDependentCheat(GetTagName); }
			catch (HCMInitException& ex) { PLOG_ERROR << "SwitchBSP: no tag names for the BSP list: " << ex.what(); }
			std::scoped_lock lk(g_bspNamesMutex);
			g_bspNames = [this]() { return readBspNames(); };
		}
	}
	~SwitchBSPImpl()
	{
		if constexpr (gameT == GameState::Value::Halo2) { std::scoped_lock lk(g_bspNamesMutex); g_bspNames = nullptr; }
	}
};





SwitchBSP::SwitchBSP(GameState gameImpl, IDIContainer& dicon)
{
	switch (gameImpl)
	{
		case GameState::Value::Halo1:
			pimpl = std::make_unique<SwitchBSPImpl<GameState::Value::Halo1>>(gameImpl, dicon);
			break;

		case GameState::Value::Halo2:
			pimpl = std::make_unique<SwitchBSPImpl<GameState::Value::Halo2>>(gameImpl, dicon);
			break;

		case GameState::Value::Halo3:
			throw HCMInitException("Not applicable, use MCCSwitchZoneSet (Switch Zone Set) instead.");
			break;

		case GameState::Value::Halo3ODST:
			throw HCMInitException("Not applicable, use MCCSwitchZoneSet (Switch Zone Set) instead.");
			break;

		case GameState::Value::HaloReach:
			throw HCMInitException("Not applicable, use MCCSwitchZoneSet (Switch Zone Set) instead.");
			break;

		case GameState::Value::Halo4:
			throw HCMInitException("Not applicable, use MCCSwitchZoneSet (Switch Zone Set) instead.");
			break;
	default:
		throw HCMInitException("not impl yet");
	}
}

SwitchBSP::~SwitchBSP()
{
	PLOG_DEBUG << "~" << getName();
}