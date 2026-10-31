#pragma once
// Halo: Reach (haloreach.dll, MCC 1.3528) - just enough to drive the out-of-bounds timer bar. The Collision Viewer itself
// does not support Reach; this is read on the render thread with no hooks.
//
// OUT OF BOUNDS (player update, the block at haloreach.dll+0x47B28):
//   players data array = *(TLS + 0x18), elements 0x490 bytes from *(header + 0x50); player +0x28 = unit
//   counter byte player +0x85: +1 per tick the unit's test (sub_18048BFB0) says "out of bounds"; RESET to 0 on the first
//   tick it says no - like Halo 2, unlike Halo 3 / ODST
//   while the counter is 0 the test only runs on ticks where (tick + player index) % 16 == 0
//   kill when counter > round(0.5 s * ticks/s) (float @ +0xA8AD74, sub_18005CC74) -> sub_18047CAA8
//   game time *(TLS + 0xA0): ticks per second i16 @+6, tick @+0xC
// KILL VOLUMES: Reach no longer rotates through them - it looks up the volumes around the object and tests every one,
// every time (sub_180153360), so there is no countdown to show.
#include <cstdint>
#include <string>

namespace hr
{
	uintptr_t base();
	bool buildOk();               // PE timestamp + SizeOfImage of the 1.3528 build this was reversed on
	struct Death
	{
		bool valid = false;
		int counter = 0, limit = 15, ticksPerSec = 30, nextCheck = 0; uint32_t playerIndex = 0;
		std::string note;
	};
	bool readDeath(Death& out);
}
