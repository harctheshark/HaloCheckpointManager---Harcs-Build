#pragma once
// H2FadeLists.h - Halo 2's per-view "fade lists" for dynamic lights and drop shadows, rewritten so a list can hold more than
// the engine's 32 entries.  2026-11-19.  Self-contained (no HCM headers): the offline test harness includes it too.
//
// THE ENGINE (halo2.dll 1.3528; the same code as Xbox 0x4a7c0 / 0x4ac30 / 0x4b160 in the decomp drop of 2026-11-18,
// src\unknown_0350e0.cpp):
//   a list   = 32 entries of 36 bytes + a count (0x484 bytes), one per view (4), for lights (qword_18165FA60) and for
//              drop shadows (qword_181660C70).
//   add      sub_1807F6FE0(list, inputs, n): entries whose id is in this frame's inputs are refreshed, the others are
//              REMOVED, new inputs are appended while there is room. Which inputs were taken is kept in a 32-BIT mask
//              (`1 << n`), so an input past the 32nd can never be appended - a second wall behind the list's size.
//   process  sub_1807F7400(list, out, limit, step): orders the list (first-person entries first, then by key, then by
//              score), lets the first `limit` entries fade IN towards their target and makes the rest fade OUT, and
//              copies every entry to `out` with its faded value. limit = 3 for lights, 4 for shadows.
// So a view shows 3 lights and 4 shadows whatever else is raised, and the list code itself stops at 32.
//
// THIS FILE is that logic with the capacity and the limit as parameters and a mask as wide as the inputs. With
// capacity 32 and `engineMask` it reproduces the engine's two functions bit for bit - proven by running the engine's own
// code against it on random frames (H2 Live Probe\uncap_harness\fade_diff.cpp).
#include <cstdint>
#include <cstring>

#pragma fp_contract(off)   // the engine's plain float adds: never fused

namespace h2fade
{
	struct Input   // 20 bytes: what the gatherer hands over for one light / one shadow caster
	{
		uint8_t flag[4];   // [0] first-person owner, [1] / [2] / [3] definition and object flags (see process)
		uint32_t id;       // the light index / the caster's object index
		int32_t key;       // the owning (root) object, or -1
		float target;      // the value to fade towards (its own brightness / alpha)
		float score;       // how big it is on screen
	};
	struct Entry   // 36 bytes
	{
		Input in;
		float elapsed;       // the fade's running step
		float value;         // the faded value (the engine's "initial")
		uint32_t lastSeen;   // frame stamp of the last add that had it
		uint32_t created;    // frame stamp of the add that appended it
	};
	static_assert(sizeof(Input) == 20 && sizeof(Entry) == 36, "engine layout");

	inline void swapEntries(Entry& a, Entry& b) { const Entry t = a; a = b; b = t; }
	inline void clampValue(Entry& e) { e.value = e.value < 0.0f ? 0.0f : e.value > e.in.target ? e.in.target : e.value; }

	// 0x4b160: the engine's ordering pass over [first, last] - mode 1 by key, mode 2 by score, both descending
	inline void order(Entry* e, int first, int last, int mode)
	{
		for (int i = first; i <= last; ++i)
			for (int j = first + 1; j <= last; ++j)
			{
				Entry& prev = e[j - 1]; Entry& cur = e[j];
				const bool swap = mode == 1 ? prev.in.key < cur.in.key : cur.in.score > prev.in.score;
				if (swap) swapEntries(prev, cur);
			}
	}

	// add. capacity = how many entries the list may hold. engineMask: keep the engine's 32-bit "taken" mask (inputs
	// past the 32nd share a bit with an earlier one) - only for comparing against the engine; false = every input has
	// its own bit. taken: scratch, at least (n + 31) / 32 dwords when !engineMask.
	inline void add(Entry* e, int& count, int capacity, const Input* in, int n, uint32_t stamp, bool engineMask, uint32_t* taken)
	{
		uint32_t one = 0;
		if (!engineMask) std::memset(taken, 0, sizeof(uint32_t) * (size_t)((n + 31) / 32));
		const auto mark = [&](int j) { if (engineMask) one |= 1u << (j & 31); else taken[j >> 5] |= 1u << (j & 31); };
		const auto marked = [&](int j) { return engineMask ? (one >> (j & 31)) & 1u : (taken[j >> 5] >> (j & 31)) & 1u; };

		for (int i = 0; i < count; ++i)
			for (int j = 0; j < n; ++j)
				if (e[i].in.id == in[j].id) { e[i].in = in[j]; e[i].lastSeen = stamp; mark(j); break; }
		for (int k = 0; k < count; ++k)
			if (count > 0 && e[k].lastSeen != stamp) { e[k] = e[count - 1]; --count; --k; }
		for (int j = 0; j < n; ++j)
			if (count < capacity && !marked(j))
			{
				Entry& x = e[count];
				x.in = in[j]; x.lastSeen = stamp; x.created = stamp; x.value = in[j].target; x.elapsed = 0.0f;
				++count; mark(j);
			}
	}

	// process. paused = the engine's own "do not advance the fades" test (its caller reads it). Returns count.
	inline int process(Entry* e, int& count, Input* out, int limit, float step, uint32_t stamp, bool paused)
	{
		if (count > 0 && !paused)
		{
			int first = 0, last = count - 1;
			const int requested = limit;
			if (limit > count) limit = count;
			bool found = false;
			for (int i = 0; last >= i; ++i)
				if (last > i && e[i].in.flag[0] && e[i].in.flag[1]) { swapEntries(e[i], e[0]); ++first; found = true; }
			if (!found)
				for (int i = first; i <= last; ++i)
					if (e[i].in.flag[0]) { swapEntries(e[i], e[first]); ++first; break; }
			if (count > requested)
				for (int i = first; last >= i; ++i)
					if (e[i].in.flag[2] && e[i].created == stamp) { e[i].value = 0.0f; swapEntries(e[i], e[last--]); --i; }
			order(e, first, last, 1);
			for (int i = first; i <= last; ++i)
				if (i > 0 && e[i].in.key != -1 && e[i].in.key == e[i - 1].in.key && !e[i].in.flag[3])
				{
					e[i].value = 0.0f; e[i].elapsed = 0.0f - step;
					swapEntries(e[i], e[last--]);
				}
			order(e, first, last, 2);
			if (count > 0 && requested < count)
				for (int i = count - 1; i >= limit; --i)
				{
					if (!e[i].in.flag[3] && e[i].value > 0.0f) { e[i].elapsed -= step; e[i].value = e[i].elapsed + e[i].value; clampValue(e[i]); }
					else e[i].elapsed = 0.0f;
				}
			for (int i = 0; i < limit; ++i)
			{
				if (e[i].in.target > e[i].value) { e[i].elapsed += step; e[i].value = e[i].value + e[i].elapsed; clampValue(e[i]); }
				else e[i].elapsed = 0.0f;
			}
		}
		for (int i = 0; i < count; ++i) { out[i] = e[i].in; out[i].target = e[i].value; }
		return count;
	}
}
