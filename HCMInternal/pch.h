// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

// Windows 
#define NOMINMAX 
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <Psapi.h>
#include <winternl.h>
#include <Dbghelp.h>


// Standard library
#include <iostream>
//#include <memory>
#include <vector>
#include <cstdint>
#include <optional>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
//#include <format>
#include <string>
#include <unordered_map>
#include <functional>
#include <utility>
#include <set>
#include <fstream>
#include <source_location>
#include <algorithm>
#include <string_view>
#include <array>
#include <tuple>
#include <inttypes.h>
#include <ranges>
#include <math.h>
#include <any>
#include <type_traits>
#include <variant>
#include <concepts>
#include <numbers>
#include <bitset>
#include <expected>


// External Libraries
#include "gsl\gsl" //https://github.com/microsoft/GSL
#include "safetyhook.hpp"






// logging
#define PLOG_OMIT_LOG_DEFINES //both plog and rpc define these
#include <plog/Log.h>
#include <plog/Initializers/RollingFileInitializer.h>
#include <plog/Initializers/ConsoleInitializer.h>

//https://github.com/Neargye/magic_enum/blob/master/doc/limitations.md
// magic_enum can only reflect enumerators whose VALUE lands inside [MIN, MAX]; anything outside silently
// reflects as an empty name (enum_name -> "") rather than failing to compile. GUIElementEnum is by far the
// largest enum here - RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES1 (148) + ...2 (206 literal + 3 defFreeCameraInterpolator
// uses x2 = 212) = 360 enumerators, values 0..359 - so the old bound of 256 left ~104 of them nameless in every
// PLOG line, in the "you forgot a creation case label" message and in the GUIServiceInfo failure listings.
// If you add another ~150 gui elements, raise this again.
// NOTE: raising the bound is monotone - it can only ADD names, never renumber or remove them - and magic_enum
// clamps the range to the underlying type (see reflected_max() in magic_enum.hpp), so byte-backed enums such as
// LevelID and DifficultyEnum are pinned at 255 and are unaffected by this value.
#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 512
#include "magic_enum\magic_enum_all.hpp" // enum reflection https://github.com/Neargye/magic_enum



#include <eventpp/eventdispatcher.h>
#include "eventpp/callbacklist.h"
#include "eventpp/utilities/scopedremover.h"

// directx
#include <directxtk\SimpleMath.h>
using namespace DirectX;
#include "HCM_imconfig.h" 





// boost
#include "boost\stacktrace.hpp"
#include "boost\algorithm\string\predicate.hpp"
#include "boost\bimap.hpp"
#include "boost\assign.hpp"

// WARNING: these three raises DO NOT TAKE EFFECT. boost/stacktrace.hpp above already pulls in
// boost/preprocessor/config/limits.hpp, and its include guard makes these #defines a no-op. Measured by
// compiling this exact include order and printing the macros: VARIADIC=64, TUPLE=64, SEQ=256, MAG=256.
// (Even on a clean include, SEQ 512 would be clamped back to 256 because limits.hpp caps SEQ at
// BOOST_PP_LIMIT_MAG, which is left at 256.) Consequences worth knowing before you grow a macro list:
//   - BOOST_PP_LIMIT_VARIADIC = 64  -> ALLOPTIONALCHEATS1 is at exactly 64 and is FULL (see OptionalCheatEnum.h)
//   - BOOST_PP_LIMIT_SEQ      = 256 -> RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES2 is at 212 sequence elements
//     ...but boost's limit is NOT the one you hit first. MSVC's own preprocessor recursion limit fires earlier:
//     MEASURED, growing sequence 2 from 212 to 232 made every TU that includes GuiElementEnum.h fail with
//     C1009 "compiler limit: macros nested too deeply". That is why RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES3
//     exists. If you see C1009, split the sequence - do not try to raise a limit.
// To actually raise them, these #defines must come BEFORE the first boost include in this file.
#define BOOST_PP_LIMIT_TUPLE 128
#define BOOST_PP_LIMIT_VARIADIC 128
#define BOOST_PP_LIMIT_SEQ 512
#include <boost\preprocessor.hpp>

// Custom utilities
#include "CustomExceptions.h"
#include "CustomErrors.h"
#include "ControlDefs.h"
#include "WindowsUtilities.h"
#include "ScopedCallback.h"
#include "ScopedAtomicBool.h"

// Some gui constant definitions
constexpr int GUIFrameHeight = 19;
constexpr int GUISpacing = 4;
constexpr int GUIFrameHeightWithSpacing = GUIFrameHeight + GUISpacing;

// for logging
template <typename T, typename F>
void once(T t, F f) {
    static bool first = true;
    if (first) {
        f();
        first = false;
    }
}
#define LOG_ONCE(x)   once([](){},[](){ x; });
#define LOG_ONCE_THIS(x)   once([this](){},[this](){ x; });
#define LOG_ONCE_CAPTURE(x, y)   once([y](){},[y](){ x; });
//#define LOG_ONCE_CAPTURE(x, y, z)   once([y, z](){},[y, z](){ x; });





// it's a RECT but single-precision
struct RECTF
{
    float left, top, right, bottom;
};

typedef uint8_t bitOffsetT; // TODO: add constraint to this (0-7)

#endif //PCH_H


