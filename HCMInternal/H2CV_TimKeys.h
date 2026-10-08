#pragma once
// Decoding of the Havok shape keys of the (biped, structure-BSP) welder's children - the engine's own decode in
// getChildShape sub_180792E90 (notes\W2-tim-key-mapping.md):
//   key >> 29 = category: 1 BSP polygon, 2 whole IG instance (rare, giant IG; transform agent + inner welder),
//                         3 baked scenery phmo, 4 baked scenery coll welder, 5 IG polygon
//   cat 1: surface = key & 0xFFFF in collision_bsp[0] of the resident structure BSP (no BSP index in the key)
//   cat 5: instance = key & 0xFFFF, surface = (key >> 16) & 0x1FFF in that instance's definition collision_bsp
//   cat 3/4: low 16 = scenery table index (halo2+0x165B440), bits 16-20 region, 21-28 permutation
#include <cstdint>

namespace h2cv
{
	struct KeyInfo
	{
		enum Kind { Unknown, WorldPolygon, IgPolygon, Other } kind = Unknown;
		int category = -1;
		uint32_t instance = 0, surface = 0;
	};

	inline KeyInfo decodeKey(uint32_t key)
	{
		KeyInfo k;
		k.category = (int)(key >> 29);
		switch (k.category)
		{
		case 1: k.kind = KeyInfo::WorldPolygon; k.surface = key & 0xFFFF; break;
		case 5: k.kind = KeyInfo::IgPolygon; k.instance = key & 0xFFFF; k.surface = (key >> 16) & 0x1FFF; break;
		default: k.kind = KeyInfo::Other; k.instance = key & 0xFFFF; break;
		}
		return k;
	}
}
