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
// magic_enum can only reflect enumerators whose VALUE lands inside [MIN, MAX]; anything outside SILENTLY
// reflects as an empty name (enum_name -> "") rather than failing to compile. That silence is the whole
// hazard: the symptom is not a build error, it is nameless entries in PLOG lines, in the "you forgot a
// creation case label" message, and in the GUIServiceInfo failure listings - i.e. exactly the diagnostics
// you would be reading while trying to work out what went wrong.
//
// ⚠ THE COUNT IN THIS COMMENT USED TO BE WRONG BY 132. It claimed 360 enumerators and "if you add another
// ~150, raise this again", both written before RELEASEGUIELEMENTS_ANDSUPPORTEDGAMES3 existed and never
// updated. Measured 2026-08-17: list1 148 + list2 206 + list3 134 = 488 literal, plus 4
// defFreeCameraInterpolator lines that each expand to two = 492 enumerators, values 0..491. Against the old
// bound of 512 that was 20 spare, not the comfortable margin the comment implied - one ordinary feature
// away from silently losing names.
//
// ⚠⚠ DO NOT RAISE THIS TO FIT GUIElementEnum. Every reflected enum in the process pays for this number -
// magic_enum instantiates a __FUNCSIG__ probe per candidate VALUE, per enum - so a global big enough for the
// biggest enum taxes the ~30 small ones too. GUIElementEnum gets its own range instead, specialised right
// under its definition in GuiElementEnum.h. 256 here covers every other enum with room to spare
// (OptionalCheatEnum 141, HotkeysEnum 66) and is HALF what this used to be, so the small enums got cheaper.
//
// ⚠⚠⚠ AND DO NOT LOWER IT BELOW 256 TO SAVE MORE. That is not free, and the reason is not obvious:
// magic_enum clamps each enum's range to its UNDERLYING TYPE (reflected_max() in magic_enum.hpp), so
// byte-backed enums are pinned at 255 and are currently riding on that clamp. LevelID in MCCState.h is
// `enum class LevelID : byte` with 229 enumerators spanning 0..255. Drop this to, say, 142 and 86 of them
// go nameless - and that is NOT cosmetic, because PointerDataParserInstantiators.h:544 does
// enum_cast<LevelID>(levelIDString) to parse level names out of the pointer data. A nameless enumerator
// cannot be cast FROM its string either, so pointer data for those 86 levels would silently stop loading
// and their cheats would break with no error anywhere. 256 is the floor, not a preference.
//
// NOTE: raising a bound is monotone - it can only ADD names, never renumber or remove them.
#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 256
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

// Width of the main HCM window, and therefore of every container widget inside it.
//
// The main window is NoResize and its .x is never recalculated (only .y is, to 2/3 of screen height), so this one
// number is the whole story - it used to be the literal 500 repeated across HCMInternalGUI and six container
// widgets, which is why they all had to agree by hand. Change it here and everything follows.
//
// Widget CONTENT does not scale with it: SetNextItemWidth values (sliders, int boxes, combos) are deliberately
// fixed, so widening the window gives them more trailing room rather than stretching them. Two-column tables like
// the skull lists do get the extra space, split evenly.
constexpr float GUIWindowWidth = 600.f;

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


