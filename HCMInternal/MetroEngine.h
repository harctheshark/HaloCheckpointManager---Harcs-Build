#pragma once
#include "pch.h"

// Metro 2033 Redux / Metro Last Light Redux (metro.exe, 4A Engine) - the engine addresses HCM reads (2026-11-24).
// Both games are ONE identical engine build after SteamStub's decryption, so these RVAs hold for both. Source: static RE,
// Documents\Halo Mod And Tools\Metro Redux\BINARY_SURVEY_20261124.md (IDB binary_survey\ll\metro_unpacked.exe.i64).
// Every read is fault-guarded: the level / player objects come and go with loads.
namespace metro
{
	constexpr uintptr_t kLevelRVA = 0xFBD820;      // level object*
	constexpr uintptr_t kGameRVA = 0xFBD7A8;       // game object* (gamesave = vtbl +272, continue = vtbl +280)
	constexpr uintptr_t kConsoleRVA = 0xFBD770;    // console object* (vtbl +64 execute now, +72 queue)
	constexpr uintptr_t kGodFlagsRVA = 0xF2E0F0;   // dword; & 2 = "global god mode" in the can't-take-damage predicate 0x2427E0
	constexpr uintptr_t kLevelPlayerOff = 56;      // level -> the controlled (player) entity*
	constexpr uintptr_t kPlayerTransformOff = 192; // 4x4 float matrix ("Base\Transform"); row 3 (+48) = position (INFERRED)
	constexpr uintptr_t kPlayerHealthOff = 776;    // float
	constexpr uintptr_t kPlayerStaminaOff = 784;   // float
	constexpr float kMetresPerHaloUnit = 3.048f;   // Halo world unit = 10 ft

	inline uintptr_t base() { static const uintptr_t b = (uintptr_t)GetModuleHandleW(nullptr); return b; }

	namespace detail
	{
		inline bool sehRead(void* dest, const void* src, size_t size)
		{
			__try { memcpy(dest, src, size); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		inline bool sehWrite(void* dest, const void* src, size_t size)
		{
			__try { memcpy(dest, src, size); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
	}
	inline bool okPtr(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFFFFFFull; }
	template <typename T> inline bool read(uintptr_t at, T& out) { return okPtr(at) && detail::sehRead(&out, (const void*)at, sizeof(T)); }
	template <typename T> inline bool write(uintptr_t at, const T& v) { return okPtr(at) && detail::sehWrite((void*)at, &v, sizeof(T)); }
	inline uintptr_t readPtr(uintptr_t at) { uintptr_t v = 0; return read(at, v) && okPtr(v) ? v : 0; }

	inline uintptr_t level() { return readPtr(base() + kLevelRVA); }
	inline uintptr_t player() { const uintptr_t l = level(); return l ? readPtr(l + kLevelPlayerOff) : 0; }

	struct Vec3 { float x = 0, y = 0, z = 0; };
	// the player's position in metres (engine axes: Y is up), or nothing
	inline std::optional<Vec3> playerPosition()
	{
		const uintptr_t p = player();
		Vec3 v;
		if (!p || !read(p + kPlayerTransformOff + 48, v)) return std::nullopt;
		if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return std::nullopt;
		return v;
	}
	// engine metres (Y up) -> Halo world units (Z up): x, z, y each / 3.048
	inline Vec3 toHalo(const Vec3& m) { return { m.x / kMetresPerHaloUnit, m.z / kMetresPerHaloUnit, m.y / kMetresPerHaloUnit }; }
	// Halo world units (Z up) -> engine metres (Y up)
	inline Vec3 fromHalo(const Vec3& h) { return { h.x * kMetresPerHaloUnit, h.z * kMetresPerHaloUnit, h.y * kMetresPerHaloUnit }; }
}
