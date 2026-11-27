#pragma once
#include <type_traits>

namespace SettingsEnums
{ 
	enum class FreeCameraObjectTrackEnum
	{
		Player,
		CustomObject
	};

	enum class FreeCameraObjectTrackEnumPlusAbsolute
	{
		Player,
		CustomObject,
		ManualPosition
	};

	enum class FreeCameraInterpolationTypesEnum
	{
		None,
		Linear,
		// Carries a velocity between frames - see MomentumSmoother.h. Eases in from rest and coasts to a
		// stop instead of parking the instant input ends, which is what makes camera moves read as
		// cinematic rather than stiff. Appended, never inserted: the value is serialised by NUMBER, so
		// putting it before Linear would silently reinterpret every saved config.
		Momentum
	};

	enum class TriggerInteriorStyle { Normal, Patterned, DontRender };

	enum class TriggerRenderStyle { Solid, Wireframe, SolidAndWireframe, None };

	enum class TriggerLabelStyle { None, Center, Corner };


	enum class SoftCeilingRenderTypes { Bipeds, Vehicles, BipedsOrVehicles };

	enum class SoftCeilingRenderDirection { Front, Back, Both };

	enum class ScreenAnchorEnum
	{
		TopLeft,
		TopRight,
		BottomRight,
		BottomLeft
	};

	// Halo 2 dynamic/projected shadow-buffer resolution. Retail = leave the engine's per-detail default
	// (256/512/1024) alone; the rest force target_shadow_buffer to that square resolution. Order matters:
	// the combo maps enum index -> label, and H2ShadowResolution.cpp maps each value to a pixel size.
	// Retail = engine default, no changes. RetailLOD = ~1024 shadow + force L6 cinematic LOD + disable object
	// distance-culling. 2048/4096/8192 = that shadow res + L6 LOD + distance-cull-off. i.e. everything except
	// Retail forces LOD + kills distance culling. Order matters: index -> label (combo) + size (cpp).
	enum class H2ShadowResolution { Retail, RetailLOD, x2048, x4096, x8192 };
	// Halo 2 supersampling (H2Supersampling.cpp): the engine's render scale x this
	enum class H2Supersampling { Off, x1_5, x2 };
	// SMAA (SMAAPostProcess.cpp): SMAA's own quality presets, in order (index = SMAAShaderSets[] in SMAA\Generated)
	enum class SMAAQuality { Low, Medium, High, Ultra };
	// SMAA's edge detector (2026-11-21): Luma = brightness differences only (SMAA's default), Colour = the largest R / G / B
	// difference (also catches edges between colours of similar brightness)
	enum class SMAAEdgeDetection { Luma, Colour };
	// the post-process anti-aliasing method (2026-11-23): SMAA, or Intel's CMAA2 (more conservative: changes far fewer pixels,
	// so fewer false edges / less "inverse" aliasing; quality = SMAA Quality's preset)
	// 2026-11-24 (user: "TAA goes under AA method"): TAA = Halo 2's temporal AA (H2TAA.cpp), Halo 2 only - in the other
	// games it does nothing. Saved as its index, so the order must not change.
	enum class PostAAMethod { SMAA, CMAA2, TAA };
	// Halo 2 Infinite Ammo (2026-11-24): which grenades the cheat keeps infinite (InfiniteAmmo.cpp). Weapons are not affected.
	enum class InfiniteAmmoGrenades { All, Frags, Plasmas };
	enum class CollisionViewerOcclusionEnum { InFrame_NoBloom, PostBloom_KeepsBloom, OwnCollisionDepth, XRay };




	template<class T, typename = std::enable_if_t<std::is_enum_v<T>>>
	inline std::ostream& operator<< (std::ostream& out, const T& enumValue)
	{
		return out << (int)enumValue;
	}
}