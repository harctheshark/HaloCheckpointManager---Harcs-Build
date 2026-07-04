#pragma once
#include "pch.h"
#include "IOptionalCheat.h"
#include "GameState.h"
#include "DIContainer.h"

class ISphereSpecularForceImpl { public: virtual ~ISphereSpecularForceImpl() = default; };

// Halo 2 only. Forces specular highlights ON for SPHERE (spherical / light_type 0) lights at all times.
// Stock, render_light zeroes a spherical light's specular colour unless a cinematic is in progress
// (`if (!cinematic_in_progress() && light_type_is_spherical(type)) specular_colour = 0;`) - so sphere
// lights only show specular during cutscenes. We flip the guard's `jnz` (skip the zero-out when cinematic)
// into an unconditional `jmp`, so the zero-out is always skipped and sphere lights keep specular in and out
// of cinematics. Takes effect live (read per light-render frame). Build 1.3528 only.
class SphereSpecularForce : public IOptionalCheat
{
private:
	std::unique_ptr<ISphereSpecularForceImpl> pimpl;

public:
	SphereSpecularForce(GameState gameImpl, IDIContainer& dicon);
	~SphereSpecularForce();

	std::string_view getName() override { return nameof(SphereSpecularForce); }
};
