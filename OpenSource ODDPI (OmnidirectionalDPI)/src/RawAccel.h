#pragma once

// Single include point for the vendored Raw Accel headers (third_party/rawaccel, MIT).
// Include this BEFORE anything that pulls in <Windows.h>: Raw Accel defines NOMINMAX itself.
//
// The driver copies settings structs byte-for-byte, so this app must use the exact
// struct layout of the installed driver. That layout is x64/ARM64 only.

#if !defined(_M_X64) && !defined(_M_ARM64)
#error "ODDPI talks to the 64-bit Raw Accel driver and must be built for x64 or ARM64 (not x86/Win32)."
#endif

#include "rawaccel-io.hpp"

namespace ra = rawaccel;

#include "rawaccel-validate.hpp"
