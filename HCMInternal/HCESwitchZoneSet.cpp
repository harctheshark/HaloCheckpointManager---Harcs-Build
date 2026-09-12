#include "pch.h"
#include "HCESwitchZoneSet.h"
#include "IMCCStateHook.h"
#include "IMessagesGUI.h"
#include "RuntimeExceptionHandler.h"
#include "SettingsStateAndEvents.h"
#include "IMakeOrGetCheat.h"
#include "HCEGetPlayerState.h"
#include "HCESignatureScan.h"
#include "HCEConsole.h"        // HCEConsoleBridge
#include "HCEAnchors.h"
#include "GlobalKill.h"
#include <mutex>

// See HCESwitchZoneSet.h for where the names come from and why the switch goes through HaloScript.

namespace
{
	// ---- bridge state. Guarded because the GUI reads it on the render thread while the cheat refreshes. ----
	std::mutex g_mutex;
	bool g_armed = false;
	std::vector<std::string> g_names;
	uintptr_t g_scenarioSlot = 0;
	uintptr_t g_tagAddressTable = 0;
	uintptr_t g_currentZoneSetSlot = 0;
	std::string g_anchorFailure;
	uintptr_t g_lastScenario = 0;
	bool g_dirty = true;
	int g_selection = 0;

	// Same field layout the trigger overlay reads; see the header.
	constexpr int64_t kZoneSetBlock = 0xD0;
	constexpr int64_t kZoneSetStride = 0x130;
	constexpr int64_t kZoneSetNameOffset = 0x004;   // char[256] literal
	constexpr int64_t kZoneSetFlagsOffset = 0x108;  // bit2 = internal
	constexpr int32_t kMaxZoneSets = 64;            // k_maximum_scenario_zone_set_count

	uint32_t readU32(uintptr_t a) { uint32_t v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	int32_t  readI32(uintptr_t a) { int32_t  v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	uintptr_t readPtr(uintptr_t a) { uintptr_t v = 0; HCEGetPlayerState::tryReadRaw(a, &v, sizeof(v)); return v; }
	bool plausible(uintptr_t p) { return p >= 0x10000ull && p < 0x7FFFFFFFFFFFull; }

	uintptr_t resolveTagBlock(uint32_t encoded)
	{
		if (encoded == 0 || encoded == 0xFFFFFFFFu) return 0;
		const uintptr_t regionBase = readPtr(g_tagAddressTable + 8ull * (encoded >> 28));
		if (!plausible(regionBase)) return 0;
		return regionBase + 4ull * encoded;
	}

	// Same two signatures HCETriggerOverlay anchors on. Kept as a fail-closed SET: if either is not a UNIQUE
	// match we resolve nothing at all, rather than reading a block out of whatever the second-best hit was.
	void resolveAnchors()
	{
		g_scenarioSlot = 0;
		g_tagAddressTable = 0;
		g_anchorFailure.clear();

		const uintptr_t simBase = (uintptr_t)GetModuleHandleW(L"HaloSimulation_tag_release.dll");
		if (!simBase) { g_anchorFailure = "HaloSimulation_tag_release.dll is not loaded"; return; }

		int hits = 0;
		const uintptr_t scenarioInsn = HCESignatureScan::resolveUnique(
			simBase, "48 8B 05 ?? ?? ?? ?? 3B 88 78 02 00 00", hits);
		if (!scenarioInsn) { g_anchorFailure = std::format("scenario data pointer ({} matches)", hits); return; }
		const uintptr_t scenarioSlot = HCESignatureScan::ripTarget(scenarioInsn, 3, 7);

		const uintptr_t tableInsn = HCESignatureScan::resolveUnique(
			simBase, "48 8B D9 4C 8D 05 ?? ?? ?? ??", hits);
		if (!tableInsn) { g_anchorFailure = std::format("tag address table ({} matches)", hits); return; }
		const uintptr_t tagTable = HCESignatureScan::ripTarget(tableInsn, 6, 10);

		if (!scenarioSlot || !tagTable) { g_anchorFailure = "rip-relative operand read failed"; return; }
		g_scenarioSlot = scenarioSlot;
		g_tagAddressTable = tagTable;
	}

	// Re-read the scenario's zone set block. Cheap enough to do on a level change, not per frame.
	void refresh()
	{
		if (!g_scenarioSlot || !g_tagAddressTable) return;
		const uintptr_t scenario = readPtr(g_scenarioSlot);
		if (!plausible(scenario)) { g_names.clear(); g_lastScenario = 0; return; }
		if (!g_dirty && scenario == g_lastScenario) return;

		g_lastScenario = scenario;
		g_dirty = false;
		g_names.clear();

		const int32_t count = readI32(scenario + kZoneSetBlock);
		const uint32_t encoded = readU32(scenario + kZoneSetBlock + 4);
		// A count of 0 means the block pointer is wrong: every shipped level has at least 4, and always at
		// least one internal set.
		if (count <= 0 || count > kMaxZoneSets || encoded == 0 || encoded == 0xFFFFFFFFu) return;

		const uintptr_t base = resolveTagBlock(encoded);
		if (!plausible(base)) return;

		char buffer[257]{};
		for (int32_t i = 0; i < count; ++i)
		{
			const uintptr_t element = base + (uintptr_t)kZoneSetStride * i;
			std::memset(buffer, 0, sizeof(buffer));
			HCEGetPlayerState::tryReadRaw(element + kZoneSetNameOffset, buffer, 256);

			std::string name;
			for (int c = 0; c < 256; ++c)
			{
				const char ch = buffer[c];
				if (ch == '\0') break;
				if (ch < 0x20 || ch > 0x7E) break;     // stop at the first non-printable, never show garbage
				name.push_back(ch);
			}
			// ⚠ An empty name is EXPECTED for the one internal zone set every level ships, not a failure.
			if (name.empty())
				name = (readU32(element + kZoneSetFlagsOffset) & 0x4u)
				? std::format("(internal zone set {})", i)
				: std::format("zone set {}", i);

			g_names.push_back(std::move(name));
		}
	}
}

namespace HCEZoneSetBridge
{
	bool isUsable()
	{
		std::scoped_lock lock(g_mutex);
		return g_armed && g_scenarioSlot != 0 && g_tagAddressTable != 0;
	}

	std::vector<std::string> names()
	{
		std::scoped_lock lock(g_mutex);
		if (!g_armed) return {};
		refresh();
		return g_names;
	}

	int currentIndex()
	{
		std::scoped_lock lock(g_mutex);
		if (!g_armed || !g_currentZoneSetSlot) return -1;
		const int32_t v = readI32(g_currentZoneSetSlot);
		return (v < 0 || v > kMaxZoneSets) ? -1 : (int)v;
	}

	void invalidate()
	{
		std::scoped_lock lock(g_mutex);
		g_dirty = true;
	}

	int selection()
	{
		std::scoped_lock lock(g_mutex);
		return g_selection;
	}

	void setSelection(int index)
	{
		std::scoped_lock lock(g_mutex);
		g_selection = index;
	}

	bool switchTo(int index, std::string& outWhy)
	{
		std::string name;
		{
			std::scoped_lock lock(g_mutex);
			if (!g_armed) { outWhy = "zone set service is not running"; return false; }
			refresh();
			if (g_names.empty())
			{
				outWhy = g_anchorFailure.empty()
					? "no zone sets readable - is a level loaded?"
					: std::format("anchors unresolved: {}", g_anchorFailure);
				return false;
			}
			if (index < 0 || (size_t)index >= g_names.size())
			{
				outWhy = std::format("zone set {} is out of range (0..{})", index, g_names.size() - 1);
				return false;
			}
			name = g_names[(size_t)index];
		}

		// The internal set has no real name, so it cannot be named to the script parser. Nothing stops the
		// engine switching to it, but we have no token for it - say so rather than queue a bad command.
		if (!name.empty() && name.front() == '(')
		{
			outWhy = "that zone set has no name in the tag, so HaloScript cannot address it";
			return false;
		}
		return HCEConsoleBridge::queue(std::format("switch_zone_set {}", name), outWhy);
	}
}


class HCESwitchZoneSet::Impl
{
private:
	GameState mGame;
	std::weak_ptr<IMessagesGUI> messagesGUIWeak;
	std::shared_ptr<RuntimeExceptionHandler> runtimeExceptions;
	ScopedCallback<ActionEvent> mSwitchCallback;
	ScopedCallback<eventpp::CallbackList<void(const MCCState&)>> mGameStateChangedCallback;

	void onGameStateChanged(const MCCState&)
	{
		HCEZoneSetBridge::invalidate();    // a level change moves the scenario; force a re-read
	}

	void onSwitch()
	{
		try
		{
			lockOrThrow(messagesGUIWeak, messagesGUI);
			const int index = HCEZoneSetBridge::selection();
			std::string why;
            if (HCEZoneSetBridge::switchTo(index, why))
            {
                auto list = HCEZoneSetBridge::names();
                messagesGUI->addMessage(std::format("Switching to zone set: {}",
                    (index >= 0 && (size_t)index < list.size()) ? list[(size_t)index] : std::to_string(index)));
            }
            else
            {
                messagesGUI->addMessage(std::format("Switch Zone Set failed: {}", why));
            }
		}
		catch (HCMRuntimeException ex)
		{
			runtimeExceptions->handleMessage(ex);
		}
	}

public:
	Impl(GameState game, IDIContainer& dicon)
		: mGame(game),
		messagesGUIWeak(dicon.Resolve<IMessagesGUI>()),
		runtimeExceptions(dicon.Resolve<RuntimeExceptionHandler>()),
		mSwitchCallback(dicon.Resolve<SettingsStateAndEvents>().lock()->hceSwitchZoneSetEvent,
			[this]() { onSwitch(); }),
		mGameStateChangedCallback(dicon.Resolve<IMCCStateHook>().lock()->getMCCStateChangedEvent(),
			[this](const MCCState& s) { onGameStateChanged(s); })
	{
		std::scoped_lock lock(g_mutex);
		resolveAnchors();
		// Reuse HCM's already-validated anchor for the current zone set rather than scanning for it again.
		// ⚠ It is called CurrentBSP for config compatibility, but it IS the zone set index - see the header.
		try { g_currentZoneSetSlot = HCEAnchors::get(HCEAnchors::Anchor::CurrentBSP); }
		catch (...) { g_currentZoneSetSlot = 0; }
		g_dirty = true;
		g_armed = true;
		if (!g_anchorFailure.empty())
			PLOG_ERROR << "HCESwitchZoneSet anchors unresolved: " << g_anchorFailure;
	}

	~Impl()
	{
		std::scoped_lock lock(g_mutex);
		g_armed = false;
		g_names.clear();
		g_scenarioSlot = 0;
		g_tagAddressTable = 0;
	}
};


HCESwitchZoneSet::HCESwitchZoneSet(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}

HCESwitchZoneSet::~HCESwitchZoneSet()
{
	PLOG_VERBOSE << "~" << getName();
}
