#include "pch.h"
#include "HCEGameThreadPump.h"
#include "HCEGetPlayerState.h"
#include "HCESignatureScan.h"
#include "IMakeOrGetCheat.h"
#include "RuntimeExceptionHandler.h"
#include "ModuleHook.h"
#include "MultilevelPointer.h"

// See HCEGameThreadPump.h for the site, why it was chosen, and the callback contract.

namespace
{
	// 16 bytes, ZERO wildcards, exactly one match in the module:
	//   41 BF E8 00 00 00   mov  r15d, 0xE8
	//   4A 8B 04 3F         mov  rax, [rdi+r15]
	//   F7 00 FF 3F 00 00   test dword ptr [rax], 0x3FFF
	// The match address IS the hook site. ⚠ Do not shorten this - the TLS-load idiom immediately before it
	// matches 22 places on its own, and uniqueness only arrives here in the tail.
	constexpr const char* kPumpSignature =
		"41 BF E8 00 00 00 4A 8B 04 3F F7 00 FF 3F 00 00";

	void gameThreadPumpHook(SafetyHookContext& ctx) noexcept
	{
		// RDI is the sim TLS base, live. Never write ctx.rdi / ctx.rsp / ctx.rip - see the header.
		HCEGameThreadPump::run((uintptr_t)ctx.rdi);
	}
}

class HCEGameThreadPumpHost::Impl
{
public:
	GameState mGame;
	std::shared_ptr<HCEGetPlayerState> mPlayerState;
	std::unique_ptr<ModuleMidHook> mHook;
	std::atomic<bool> mRunning{ false };

	Impl(GameState game, IDIContainer& dicon) : mGame(game)
	{
		if (static_cast<GameState::Value>(game) != GameState::Value::HaloCER)
			throw HCMInitException("HCEGameThreadPumpHost only supports Halo Campaign Evolved");

		mPlayerState = resolveDependentCheat(HCEGetPlayerState);

		uintptr_t simBase = 0;
		try { simBase = mPlayerState->getSimModuleBase(); }
		catch (HCMRuntimeException& ex)
		{
			throw HCMInitException(std::format("Game thread pump: the simulation module is not loaded ({})", ex.what()));
		}

		int hits = 0;
		const uintptr_t site = HCESignatureScan::resolveUnique(simBase, kPumpSignature, hits);
		if (!site)
		{
			// Fail closed and say why. Zero matches means this build moved the site; more than one means the
			// pattern stopped being unique. Either way, attaching would be a guess.
			throw HCMInitException(std::format(
				"Game thread pump is unavailable on this build of Halo Campaign Evolved: its site signature "
				"matched {} times, expected exactly 1. Features that need the simulation thread will be off.",
				hits));
		}

		PLOG_INFO << "HCEGameThreadPump: site resolved at 0x" << std::hex << site << std::dec
			<< " (sim+0x" << std::hex << (site - simBase) << std::dec << ")";

		auto sitePointer = std::make_shared<MultilevelPointerSpecialisation::Resolved>((void*)site);
		mHook = ModuleMidHook::make(mGame.toModuleName(), sitePointer, &gameThreadPumpHook, true);
		mRunning.store(true, std::memory_order_release);
	}

	~Impl()
	{
		mRunning.store(false, std::memory_order_release);
		// The hook object's destructor detaches. Callers are responsible for having removed their own PumpFn
		// before their objects died - see the contract in the header.
		if (mHook) mHook->setWantsToBeAttached(false);
	}
};


HCEGameThreadPumpHost::HCEGameThreadPumpHost(GameState game, IDIContainer& dicon)
	: pimpl(std::make_unique<Impl>(game, dicon))
{
}

HCEGameThreadPumpHost::~HCEGameThreadPumpHost()
{
	PLOG_VERBOSE << "~" << getName();
}

bool HCEGameThreadPumpHost::isRunning() const { return pimpl && pimpl->mRunning.load(std::memory_order_acquire); }
