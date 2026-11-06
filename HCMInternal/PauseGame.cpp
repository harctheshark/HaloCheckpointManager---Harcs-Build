#include "pch.h"
#include "HCEAnchors.h"
#include "PauseGame.h"
#include "ModuleHook.h"
#include "MidhookContextInterpreter.h"
#include "MultilevelPointer.h"

// to avoid ambiguity of updateService, these intermediate classes rename those functions
class IPauseService : public TokenSharedRequestProvider
{ 
public: 
	~IPauseService() = default; 
	virtual void updateService() override { updatePauseService(); }
	virtual void updatePauseService() = 0;
};
class IOverridePauseService : public TokenSharedRequestProvider
{
public:
	~IOverridePauseService() = default;
	virtual void updateService() override { updateOverrideService(); }
	virtual void updateOverrideService() = 0;
};


class PauseGame::PauseGameImpl : public IPauseService, public IOverridePauseService
{
private:
	std::vector<std::unique_ptr<ModulePatch>> pauseGamePatches;

public:
	// ---- "Pause also freezes effects" (2026-11-05; Halo 3 / ODST) ----
	// HCM's pause skips the call that hands the frame its game ticks; MCC's own pause instead makes the GAME-TIME UPDATE
	// (halo3 sub_1800F3424) take its "paused" path: frame game dt 0, ticks 0, interpolation fraction 0, and the main loop
	// then raises frame flag 8 ("paused"). Particles, lift effects and decorator wind run on that game time, so they only
	// stop with that path - and in multiplayer MCC never pauses at all. While HCM's pause is in effect these code patches
	// make the game take the same path (nothing is written to game state, so a checkpoint made while paused is unaffected):
	//   A  game-time update  `call is_paused; test al,al; jnz paused_path` -> jmp paused_path
	//      halo3 RVA 0xF3524  0F 85 8B 01 00 00 -> E9 8C 01 00 00 90      halo3odst RVA 0x10ECB7  0F 85 95 01 00 00 -> E9 96 01 00 00 90
	// (Tested live on Valhalla 2026-11-05: A alone freezes particles, lift effects and grass; the camera still moves.
	//  Forcing the main loop's frame flag 8 was not needed.)
	// Sites found by HCM H3 Pause VFX\find_pause_sites.py (1.3528); the original bytes are checked before each attach and
	// another build is left alone. Advance ticks' override lifts them with the pause.
	struct FxTarget { GameState::Value game; int64_t rva; std::vector<byte> original, patched; std::unique_ptr<ModulePatch> patch; bool attached = false; };
	std::vector<FxTarget> mFx;
	std::mutex mFxMutex;
	std::atomic<bool> mFreezeEffects{ true };

	void makeFxPatches()
	{
		mFx.push_back({ GameState::Value::Halo3, 0xF3524, { 0x0F, 0x85, 0x8B, 0x01, 0x00, 0x00 }, { 0xE9, 0x8C, 0x01, 0x00, 0x00, 0x90 } });
		mFx.push_back({ GameState::Value::Halo3ODST, 0x10ECB7, { 0x0F, 0x85, 0x95, 0x01, 0x00, 0x00 }, { 0xE9, 0x96, 0x01, 0x00, 0x00, 0x90 } });
		for (auto& t : mFx)
		{
			try
			{
				auto ptr = std::make_shared<MultilevelPointerSpecialisation::ModuleOffset>(GameState(t.game).toModuleName(), std::vector<int64_t>{ t.rva });
				t.patch = ModulePatch::make(GameState(t.game).toModuleName(), ptr, t.patched, false);
			}
			catch (HCMInitException ex) { PLOG_ERROR << "PauseGame: effects patch not made for " << GameState(t.game).toString() << ": " << ex.what(); }
		}
	}
	static bool fxSiteMatches(const uint8_t* p, const byte* expect, size_t n)
	{
		__try { return memcmp(p, expect, n) == 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	// freeze = HCM's pause is in effect and the option is on
	void applyEffects(bool freeze)
	{
		std::scoped_lock lk(mFxMutex);
		for (auto& t : mFx)
		{
			if (!t.patch) continue;
			if (freeze && !t.attached)
			{
				const HMODULE mod = GetModuleHandleW(GameState(t.game).toModuleName().c_str());
				if (!mod) continue;                                         // that game is not loaded
				if (!fxSiteMatches((const uint8_t*)mod + t.rva, t.original.data(), t.original.size()))
				{
					PLOG_ERROR << "PauseGame: effects patch site 0x" << std::hex << t.rva << std::dec << " does not match in " << GameState(t.game).toString() << " (another game build?) - not applied";
					continue;
				}
				t.patch->setWantsToBeAttached(true);
				t.attached = true;
				PLOG_DEBUG << "PauseGame: effects patch 0x" << std::hex << t.rva << std::dec << " applied in " << GameState(t.game).toString();
			}
			else if (!freeze && t.attached)
			{
				t.patch->setWantsToBeAttached(false);
				t.attached = false;
			}
		}
	}
	bool pauseInEffect() { return this->IPauseService::serviceIsRequested() && !this->IOverridePauseService::serviceIsRequested(); }
	void setFreezeEffects(bool freeze) { mFreezeEffects = freeze; applyEffects(pauseInEffect() && freeze); }

	std::map<GameState, HCMInitException> serviceFailures;

	PauseGameImpl(std::shared_ptr<PointerDataStore> ptr)
	{
		for (auto game : AllSupportedGames)
		{
			try
			{
				auto pauseGameFunction = ptr->getData<std::shared_ptr<MultilevelPointer>>(nameof(pauseGameFunction), game);
				auto pauseGameCode = ptr->getVectorData<byte>(nameof(pauseGameCode), game);

				// ⚠ HALOCER ONLY, AND THIS IS THE ONE THAT ACTUALLY BIT. HaloCER reports version 0.0.0.0 on
				// every build, so a game update cannot be detected from the pointer data; the 2026-08-17 one
				// moved this site 0x10 and the patch landed mid-instruction, making pausing a reliable crash.
				// Cross-checking against an independent byte signature turns that into a refusal.
				// Throws only if the two derivations DISAGREE - see HCEAnchors.h for why an unresolved anchor
				// deliberately warns and permits instead.
				if (game.operator GameState::Value() == GameState::Value::HaloCER)
				{
					uintptr_t pauseSite = 0;
					if (pauseGameFunction && pauseGameFunction->resolve(&pauseSite) && pauseSite)
						HCEAnchors::crossCheck(HCEAnchors::Anchor::PauseGameFunction, pauseSite, "Pause Game");
				}

				std::unique_ptr<ModulePatch> patch = ModulePatch::make(game.toModuleName(), pauseGameFunction, *pauseGameCode.get(), false);

				pauseGamePatches.emplace_back(std::move(patch));
			}
			catch (HCMInitException ex)
			{
				serviceFailures.emplace(game, ex);
			}
		}

		makeFxPatches();
		if (pauseGamePatches.empty())
		{
			std::stringstream oss;
			for (auto& [game, ex] : serviceFailures)
			{
				oss << game.toString() << ": " << ex.what() << std::endl;
			}
			throw HCMInitException(std::format("Could not construct any game implementations\n{}", oss.str()));
		}

	}

	~PauseGameImpl()
	{
		PLOG_DEBUG << "~" << nameof(PauseGameImpl);
	}
	

	std::shared_ptr<SharedRequestToken> makeRegularScopedRequest() { return IPauseService::makeScopedRequest(); }
	std::shared_ptr<SharedRequestToken> makeOverrideScopedRequest() { return IOverridePauseService::makeScopedRequest(); }

	void combinedUpdateService()
	{

		bool requested = this->IPauseService::serviceIsRequested() && !this->IOverridePauseService::serviceIsRequested();

		PLOG_INFO << "PauseGame service is turning " << (requested ? "ON!" : "OFF!");
		PLOG_DEBUG << "this->IPauseService::serviceIsRequested(): " << (this->IPauseService::serviceIsRequested() ? "true" : "false");
		PLOG_DEBUG << "this->IOverridePauseService::serviceIsRequested(): " << (this->IOverridePauseService::serviceIsRequested() ? "true" : "false");

		for (auto& patch : pauseGamePatches)
		{
			patch->setWantsToBeAttached(requested);
		}

		PLOG_DEBUG << "PauseGame hooks have been updated.";
		applyEffects(requested && mFreezeEffects);

	}

	virtual void updatePauseService() override { combinedUpdateService(); }
	virtual void updateOverrideService() override { combinedUpdateService(); }
};







PauseGame::PauseGame(std::shared_ptr<PointerDataStore> ptr)
	: pimpl(std::make_shared< PauseGameImpl>(ptr))
{}

PauseGame::~PauseGame() = default;

std::shared_ptr<SharedRequestToken> PauseGame::makeScopedRequest() { return pimpl->makeRegularScopedRequest(); }
std::shared_ptr<SharedRequestToken> PauseGame::makeOverrideScopedRequest() { return pimpl->makeOverrideScopedRequest(); }

std::map<GameState, HCMInitException>& PauseGame::getServiceFailures() { return pimpl->serviceFailures; }
void PauseGame::setFreezeEffects(bool freeze) { pimpl->setFreezeEffects(freeze); }