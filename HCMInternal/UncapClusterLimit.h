#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class IUncapClusterLimitImpl { public: virtual ~IUncapClusterLimitImpl() = default; };

// Halo 2 only. Beats the 128 region-clusters-per-visibility-region wall (the "overflowed region clusters
// during region building" limit that makes half of large maps stop rendering from a high vantage). Proven
// end-to-end in sapien; this ports the fix to MCC halo2.dll (build 1.3528).
//
// STAGE 1 (this build): relocates the region buffer's index + volume arrays to the projbuf's own tail
//   ("in-slack": index 0x2460 / volume-count 0x2860 / volumes 0x2864, volume cap 512->496) so they fit inside
//   every stock projbuf (incl. the tiny 0x22978 shadow one). Cluster cap stays 128 => render-identical to
//   stock; this only validates the relocation + toggle machinery with zero crash risk.
// STAGE 2 (todo): subpart-mask pool relocation, item-bitvector widen (128->256 bits), unsigned cluster map,
//   traversal walker 4->8 dwords, cluster cap 128->255.
//
// Toggle-off is drained in a crash-safe order (caps to stock first, live counts zeroed, then code reverted),
// so flipping off never leaves stock code reading a >128 count against 128-sized arrays. Build 1.3528 only.
class UncapClusterLimit : public IOptionalCheat
{
private:
	std::unique_ptr<IUncapClusterLimitImpl> pimpl;

public:
	UncapClusterLimit(GameState gameImpl, IDIContainer& dicon);
	~UncapClusterLimit();

	std::string_view getName() override { return nameof(UncapClusterLimit); }
};
