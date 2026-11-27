#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "DIContainer.h"
#include "GameState.h"

// Halo 2 (MCC 1.3528): supersampling through the engine's own render scale (2026-11-21, user: "yes build it, try 2x" after
// DLDSR was ruled out by DSC). See H2Supersampling.cpp.
class H2Supersampling : public IOptionalCheat
{
private:
	class H2SupersamplingImpl;
	std::unique_ptr<H2SupersamplingImpl> pimpl;

public:
	H2Supersampling(GameState gameImpl, IDIContainer& dicon);
	~H2Supersampling();

	virtual std::string_view getName() override { return nameof(H2Supersampling); }
};
