#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

// only impl for h1/h2. Third gen switches ZONE SETS instead - see MCCSwitchZoneSet

class ISwitchBSPImpl { public: virtual ~ISwitchBSPImpl() = default; };
class SwitchBSP : public IOptionalCheat
{
private:

	std::unique_ptr<ISwitchBSPImpl> pimpl;

public:
	SwitchBSP(GameState gameImpl, IDIContainer& dicon);
	~SwitchBSP();

	std::string_view getName() override { return nameof(SwitchBSP); }

};

// the BSP names of the loaded scenario, for the switch control (index -> tag name; empty when unknown). Filled by the Halo 2
// implementation (2026-11-02, user: the control showed only an index): scenario +0x210 count / +0x214 data, 0x44-byte entries,
// structure BSP tag datum at entry +0x14 (halo2.dll sub_18067AA50 / sub_180036710).
namespace SwitchBSPNames
{
	std::string describe(int index);   // short name (last path part), "" when unknown
	std::string listAll();             // "0: name\n1: name ..." for the tooltip, "" when unknown
}