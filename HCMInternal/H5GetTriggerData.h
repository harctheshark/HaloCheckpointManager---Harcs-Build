#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"
#include "IModel.h"   // VertexCollection / IndexCollection - the renderer's own geometry types
#include "H5TriggerVolumeNames.h"   // ScriptTier
#include "H5SpeedrunTriggers.h"     // the per-level minimum completion path

// ================================================================================================================
// Halo 5: Forge - scenario trigger volume READER.
//
// Snapshots every trigger volume the loaded scenario declares: geometry, category and (where recoverable) name.
// Draws nothing and patches nothing; H5TriggerOverlay renders what this returns.
//
// WHERE THE DATA IS  (all verified live against a 379-volume campaign scenario)
//     globals = [exe + 0x05A62538]          the same scenario singleton the zone sets use
//     array   = [globals + 0x840]           count = [globals + 0x850]        stride 0xC8
//
//     volume + 0x00  u32   name string id (hash)
//     volume + 0x10  u32   shape: 0 = box, 1 = sector
//     volume + 0x14  3f    forward (unit)
//     volume + 0x20  3f    up (unit; (0,0,1) for upright volumes - Blam is Z-up)
//     volume + 0x2C  3f    CENTRE
//     volume + 0x40  3f    EXTENTS
//     volume + 0xA4  f     bounding radius
//
// ⚠⚠ THE EXTENTS ARE FULL DIMENSIONS, NOT HALF-EXTENTS. Getting this wrong draws every box at double size,
// and it is not obvious by eye. Proof, and the way to re-check it after any game update: +0xA4 equals
// 0.5 * |extents| on every volume tested - 13.921/19.775/4.105 -> 12.265 (field reads 12.265),
// 8.028/14.039/7.879 -> 8.996 (8.995), 110.359/113.423/37.185 -> 81.28 (81.281). buildCorners halves them.
//
// ⚠⚠⚠ SECTOR VOLUMES ARE EXTRUDED POLYGONS AND THEIR extents FIELD IS JUNK - NEVER DRAW ONE AS A BOX.
// 173 of the 379 are shape type 1. Their real geometry is a point loop plus a set of convex prisms; see
// buildSector in the .cpp for the full layout and the engine functions it came from. `extents` for a
// sector is stale (9 distinct values across the 173 - NOT the (1,1,h) placeholder an earlier pass
// assumed), and +0xA4 is derived from it, so both are meaningless for sectors.
//
// ⚠ And the shape flag is a **u16** at +0x10: the halfword at +0x12 is a constant 0xBCBC, so a u32 read
// gives 0xBCBC0000/0xBCBC0001 and silently classifies everything as a box. That exact bug shipped once.
//
// ---------------------------------------------------------------------------------------------------------------
// CATEGORIES COME FROM SIBLING INDEX BLOCKS, NEVER FROM THE NAME
// ---------------------------------------------------------------------------------------------------------------
// The tag documentation says the type is a name PREFIX ('zone_set:', 'kill', 'safe_zone' ...). That is true of
// the tag and useless at runtime, because retail cannot turn a string id back into text - see
// H5TriggerVolumeNames.h. The scenario instead keeps separate blocks that reference volumes BY INDEX:
//
//     [globals+0x8B0] count [globals+0x8C0]   stride 8    zone set switch volumes
//         +0x00 u16 flags   +0x02 u16 BEGIN zone set   +0x04 u16 volume index   +0x06 u16 COMMIT zone set
//         Exactly one of begin/commit is set; the other is 0xFFFF.
//         ✅ Begin-vs-commit is PROVEN, 8/8 with no counterexamples: once names were resolved, every entry's
//         zone set index matched the zone set named inside the volume's own name - volume 299 has begin=5 and
//         is named "begin_zone_set:zs_03" (zone set 5 IS zs_03); volume 303 has commit=8 and is named
//         "zone_set:zs_05" (zone set 8 IS zs_05).
//
// KILL volumes do NOT need an index block: the engine keeps the answer ON the volume, as a u16 at
// volume+0xA8 where 0xFFFF means "not a kill trigger". kill_volume_enable / kill_volume_disable
// (exe+0x00A7E710 / exe+0x00A7E7C0) load exactly that field. The sibling block at [globals+0xBD8]
// (count +0xBE8, stride 4) lists the same volumes and agrees exactly - 30 rows, identical set - but it
// is redundant, so it is not read.
// ⚠ Those 30 are referenced by NO script, so any "inert vs live" colouring that looks only at the script
// corpus paints thirty volumes that actively kill the player as inert. isKillVolume exists for that.
// ================================================================================================================
class H5GetTriggerData : public IOptionalCheat
{
public:
	enum class Category
	{
		Regular,        // anything not claimed by a category block
		Kill,
		BeginZoneSet,   // entering it starts PREPARING a zone set
		CommitZoneSet,  // entering it COMMITS the switch
	};

	enum class Shape
	{
		Box,
		Sector,         // arbitrary geometry; we only know its bounding box
	};

	struct Volume
	{
		int32_t index = -1;
		uint32_t nameHash = 0;
		std::string name;            // empty when the hash is not in the corpus
		Category category = Category::Regular;

		// ---- minimum completion path ---------------------------------------------------------------
		// True when this volume is one of the level's goal `gotoVolume`s (or its end trigger) - i.e. a
		// volume a speedrun MUST touch. See H5SpeedrunTriggers.h. speedrunStep is 1-based, 0 when not.
		// ⚠ false does NOT mean "skippable in practice" - a door may still be in the way. It means
		// "the mission does not advance on this one".
		bool isSpeedrun = false;
		int speedrunStep = 0;
		Shape shape = Shape::Box;

		// ⚠⚠ origin is what the engine STORES at +0x2C and it is a CORNER, not a centre: the box spans
		// [0, extents] along EVERY local axis. That is the engine's own containment test, not a guess -
		// see buildCorners. Treating it as a centre (on any axis) offsets the volume by half its extents
		// on that axis, which is subtle enough to survive two rounds of eyeballing. `centre` is the
		// derived true centre and is what labels and distance culling should use.
		SimpleMath::Vector3 origin{};
		SimpleMath::Vector3 centre{};
		SimpleMath::Vector3 extents{};   // FULL dimensions
		SimpleMath::Vector3 forward{};
		SimpleMath::Vector3 up{};
		float boundingRadius = 0.f;

		// Only meaningful for the two zone set categories; -1 otherwise.
		int32_t zoneSetIndex = -1;
		std::string zoneSetName;

		// The eight corners in world space, already built from centre/extents/forward/up.
		// ⚠ BOX VOLUMES ONLY. Sectors are not boxes; use the mesh below for those.
		std::array<SimpleMath::Vector3, 8> corners{};

		// ---- renderable geometry, in WORLD space, for BOTH shapes ----------------------------------
		// Built by the reader so the overlay never has to know the difference between a box and a sector.
		// Triangles are the filled surface; edges are the wireframe. For a sector the wireframe is the
		// POLYGON OUTLINE only (top loop, bottom loop, verticals) - drawing every prism's edges would
		// scribble the internal triangulation across the volume.
		VertexCollection meshVerts;
		IndexCollection meshTriangles;
		IndexCollection meshEdges;

		// ---- sector-only ---------------------------------------------------------------------------
		// The convex prisms the engine actually tests against, already in world space, each with its 5
		// planes. Containment is an OR over these, after the AABB early-out.
		struct Prism { std::array<SimpleMath::Vector4, 5> planesLocal{}; };
		std::vector<Prism> prisms;
		// Local-space AABB at volume+0x8C, field order x0,x1,y0,y1,z0,z1. ⚠ Not merely an optimisation -
		// on 23 of 173 sectors it is TIGHTER than the prism union, so it changes the answer and must be
		// applied.
		std::array<float, 6> localAabb{};
		bool hasLocalAabb = false;

		// ---- engine-authoritative kill flag --------------------------------------------------------
		// u16 at volume+0xA8; 0xFFFF means "not a kill trigger". Read directly by the engine's own
		// kill_volume_enable / kill_volume_disable (exe+0x00A7E710 / exe+0x00A7E7C0), so this is the
		// engine's answer, not an inference from a sibling index block.
		bool isKillVolume = false;
		uint16_t killIndex = 0xFFFF;

		// ---- "can a script hit or wake this?" ------------------------------------------------------
		// From the compiled script corpus, matched by name hash. See H5TriggerVolumeNames.h.
		// ⚠ Unknown does NOT mean inert - kill volumes and zone set volumes are engine-driven and appear
		// in no script. Use scriptReachableOrEngineDriven(), never the tier alone.
		H5TriggerVolumeNames::ScriptTier scriptTier = H5TriggerVolumeNames::ScriptTier::Unknown;

		bool scriptReachableOrEngineDriven() const noexcept
		{
			return H5TriggerVolumeNames::isScriptReachable(scriptTier)
				|| scriptTier == H5TriggerVolumeNames::ScriptTier::EngineZoneSet
				|| isKillVolume
				|| category == Category::BeginZoneSet
				|| category == Category::CommitZoneSet;
		}

		// The volume's orthonormal frame, kept so containment can be tested without rebuilding it.
		SimpleMath::Vector3 axisF{}, axisR{}, axisU{};
		SimpleMath::Vector3 halfExtents{};

		// ⚠ Box volumes only - always false for a sector, because a sector's extents are placeholders and a
		// box test against them would report nonsense. See the header block.
		// Exactly the engine's own predicate, for BOTH shapes. See buildCorners / buildSector for the
		// disassembly each branch comes from - do not "fix" the box test into a symmetric +-half test.
		bool containsPoint(const SimpleMath::Vector3& p) const noexcept
		{
			const SimpleMath::Vector3 d = p - origin;
			const float lx = d.Dot(axisF), ly = d.Dot(axisR), lz = d.Dot(axisU);

			if (shape == Shape::Box)
			{
				// exe+0x00A858B4: strict at both ends on all three axes.
				return lx > 0.f && lx < extents.x
					&& ly > 0.f && ly < extents.y
					&& lz > 0.f && lz < extents.z;
			}

			// SECTOR. exe+0x00A857C6: an INCLUSIVE AABB gate first (NaN fails it), then an OR over the
			// convex prisms, each of which is an AND over its 5 planes with dot3(n, p) <= d.
			if (hasLocalAabb)
			{
				if (!(lx >= localAabb[0] && lx <= localAabb[1])) return false;
				if (!(ly >= localAabb[2] && ly <= localAabb[3])) return false;
				if (!(lz >= localAabb[4] && lz <= localAabb[5])) return false;
			}
			for (const auto& pr : prisms)
			{
				bool inside = true;
				for (const auto& pl : pr.planesLocal)
				{
					if (pl.x * lx + pl.y * ly + pl.z * lz > pl.w) { inside = false; break; }
				}
				if (inside) return true;
			}
			return false;
		}
	};

	H5GetTriggerData(GameState game, IDIContainer& dicon);
	~H5GetTriggerData();
	std::string_view getName() override { return nameof(H5GetTriggerData); }

	// Re-reads only when the scenario has changed (cheap to call every frame; it compares the globals
	// pointer and the count first). Throws when no scenario is loaded.
	const std::vector<Volume>& getVolumes();

	// Forget the snapshot; the next getVolumes re-reads. Call on a level change.
	void invalidate() noexcept;

	static std::string_view categoryName(Category c) noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> pimpl;
};
