#pragma once

// Deliberately dependency-free: no pch.h, no STL, no Windows headers.
//
// HCEHavokDebuggerImpl.cpp compiles WITHOUT HCM's precompiled header - C++14, against the Havok 2007 SDK, with
// warnings off (see its ClCompile entry in the vcxproj). Including ImageResidencyGuard.h there would drag in
// pch.h and the whole HCM/STL surface with it, into a translation unit specifically isolated from all of that.
//
// So that file gets these two free functions instead. They are implemented in ImageResidencyShim.cpp, which DOES
// include the real header, and they drive exactly the same shared_mutex and thread_local depth counter that
// ScopedImageResidency uses - so a foreign TU registering through here is indistinguishable, to
// ImageResidency::drain(), from any other HCM entry point.
//
// Prefer ScopedImageResidency directly in any TU that can see the real header. This exists for the one that cannot.
namespace ImageResidency
{
	void enterFromForeignTU() noexcept;
	void leaveFromForeignTU() noexcept;
}
