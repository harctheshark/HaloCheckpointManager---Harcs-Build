#pragma once
#include "pch.h"
#include "MetroEngine.h"

// Metro Redux (2026-11-25): moving the player - teleport, velocity, look direction, and the player snapshot behind
// HCM's instant Save Checkpoint / Revert. Shared by MetroForceTeleport, MetroForceLaunch and MetroCheckpoint.
//
// Live read-only probes (Documents\Halo Mod And Tools\Metro Redux\probe\):
//   velocity  vec3 m/s (Y up) at *(player+0x4F8)+0x66C, duplicated at +0x678; *(player+0x4F8) == player+0x3498 (the
//             movement component is embedded in the player). Matched d(position)/dt walking, jumping and falling.
//   look      unit vec3 at player+0x1244: the transform's forward row (+0xE0) plus pitch (INFERRED from one probe).
//   position  copies of the transform position (+0xF0): player+0xA40, +0x3AF8 (component+0x660, right before the
//             velocity), +0x3A84 (component+0x5EC, 0.025 m lower - the controller's own).
namespace metro::move
{
	void init();   // idempotent; installs whatever game-thread hook the moves need

	std::optional<Vec3> velocity();             // m/s, engine axes
	std::optional<Vec3> lookDirectionHalo();    // unit, Halo axes (Z up)

	// forward/right/up (Halo axes) relative to a Halo-axes look direction -> a Halo-axes offset
	Vec3 lookRelative(const Vec3& lookHalo, const SimpleMath::Vector3& forwardRightUp, bool ignoreVertical);

	// "" = queued for the player's next update on the game's thread, else why not
	std::string teleportTo(const Vec3& metres);
	// adds (or, replace = true, sets) HCM's launch velocity, m/s engine axes - on top of the game's own walking motion
	std::string launch(const Vec3& metresPerSecond, bool replace);

	// HCM's instant checkpoint: the player's own state only
	struct Snapshot
	{
		std::array<float, 16> transform{};
		std::array<float, 12> angles{};   // head / body / body unaffected / move angles
		float health = 0, stamina = 0;
		float fallSpeed = 0, airTime = 0; // the physics shell's own vertical state
		Vec3 airVel;                      // the shell's free-air velocity (used in its free-air modes)
		bool launchActive = false, launchAirborne = false; Vec3 launchV;
		uintptr_t player = 0;      // the entity it was taken from (a level load makes a new one)
		uintptr_t level = 0;
		bool valid = false;
	};
	std::string take(Snapshot& out);
	std::string restore(const Snapshot& s);   // returns kDeadReason while the player is dead (nothing is changed)
	inline constexpr const char* kDeadReason = "you are dead";
	bool isPlayerDead();

	// Acrophobia (the Halo 1 / CER flight model): on/off, and the held keys, fed every frame from the render thread
	void setAcrophobia(bool on);
	void setAcrophobiaInput(bool jump, bool crouch);
}
