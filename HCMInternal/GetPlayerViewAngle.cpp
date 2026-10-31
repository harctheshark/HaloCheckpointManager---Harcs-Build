#include "pch.h"
#include "GetPlayerViewAngle.h"
#include "MultilevelPointer.h"
#include "PointerDataStore.h"


class GetPlayerViewAngleImpl : public GetPlayerViewAngle::IGetPlayerViewAngleImpl
{
private:
	GameState mGame;

	//data
	std::shared_ptr<MultilevelPointer> playerViewAngle;

public:
	GetPlayerViewAngleImpl(GameState game, IDIContainer& dicon) : mGame(game)
	{
		playerViewAngle = dicon.Resolve< PointerDataStore>().lock()->getData<std::shared_ptr<MultilevelPointer>>(nameof(playerViewAngle), mGame);
	}

	virtual const SimpleMath::Vector2 getPlayerViewAngle() override
	{
		SimpleMath::Vector2 outAngles; // inits to null datum
		if (!playerViewAngle->readData(&outAngles)) throw HCMRuntimeException(std::format("Failed to read playerViewAngle: {}", MultilevelPointer::GetLastError()));
		return outAngles;
	}

	virtual void setPlayerViewAngle(SimpleMath::Vector2 newAngles) override
	{
		if (!playerViewAngle->writeData(&newAngles)) throw HCMRuntimeException(std::format("Failed to write playerViewAngle: {}", MultilevelPointer::GetLastError()));
	}

	virtual bool setPlayerViewAngleFast(SimpleMath::Vector2 newAngles) override
	{
		uintptr_t address = 0;
		if (!playerViewAngle->resolve(&address) || !address) return false;
		return guardedWrite(address, newAngles);
	}

	static bool guardedWrite(uintptr_t address, SimpleMath::Vector2 value)
	{
		__try { *reinterpret_cast<SimpleMath::Vector2*>(address) = value; return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
};

GetPlayerViewAngle::GetPlayerViewAngle(GameState game, IDIContainer& dicon)
{
	pimpl = std::make_unique<GetPlayerViewAngleImpl>(game, dicon);
}

GetPlayerViewAngle::~GetPlayerViewAngle() = default;