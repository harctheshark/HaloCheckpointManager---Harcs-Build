#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

class GetPlayerViewAngle : public IOptionalCheat
{
public:
	class IGetPlayerViewAngleImpl {
	public:
		virtual const SimpleMath::Vector2 getPlayerViewAngle() = 0;
		virtual void setPlayerViewAngle(SimpleMath::Vector2 newAngle) = 0;
		virtual bool setPlayerViewAngleFast(SimpleMath::Vector2 newAngle) = 0;
	};

private:
	std::unique_ptr<IGetPlayerViewAngleImpl> pimpl;

public:
	GetPlayerViewAngle(GameState game, IDIContainer& dicon);
	~GetPlayerViewAngle();

	const SimpleMath::Vector2 getPlayerViewAngle() { return pimpl->getPlayerViewAngle(); }

	void setPlayerViewAngle(SimpleMath::Vector2 newAngle) { return pimpl->setPlayerViewAngle(newAngle); }

	// For per-tick callers (the replay): the same write without MultilevelPointer::writeData's writability check, whose
	// VirtualQuery sizes the whole region - milliseconds when the angle sits in a game's huge game-state allocation (Reach:
	// ~6.5 ms per call, 60 calls a second during playback). Guarded instead; false when the pointer or the write fails.
	bool setPlayerViewAngleFast(SimpleMath::Vector2 newAngle) { return pimpl->setPlayerViewAngleFast(newAngle); }

	virtual std::string_view getName() override { return nameof(GetPlayerViewAngle); }
};

