#pragma once
// SEH-guarded reads of game memory. Everything the overlay reads from Halo 2 / Havok goes through these:
// the render thread reads structures the simulation thread is mutating, so a pointer can die between two reads.
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace mem
{
	inline bool okp(uintptr_t p) { return p >= 0x10000 && p < 0x7FFFFFFFFFFFull; }

	// copy n bytes; false on any access violation
	inline bool copy(void* dst, uintptr_t src, size_t n)
	{
		if (!okp(src)) return false;
		__try { memcpy(dst, reinterpret_cast<const void*>(src), n); return true; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	template <class T> inline bool rd(uintptr_t a, T& out) { return copy(&out, a, sizeof(T)); }
	template <class T> inline T get(uintptr_t a, T def = T{}) { T v; return rd(a, v) ? v : def; }

	inline uintptr_t q(uintptr_t a) { return get<uintptr_t>(a, 0); }
	inline uint32_t u32(uintptr_t a, uint32_t def = 0xFFFFFFFFu) { return get<uint32_t>(a, def); }
	inline int32_t i32(uintptr_t a, int32_t def = 0) { return get<int32_t>(a, def); }
	inline uint16_t u16(uintptr_t a, uint16_t def = 0xFFFF) { return get<uint16_t>(a, def); }
	inline uint8_t u8(uintptr_t a, uint8_t def = 0) { return get<uint8_t>(a, def); }
	inline float f32(uintptr_t a, float def = 0.f) { return get<float>(a, def); }
}
