//
//  pico.h
//
//  A stand-in for the pico-sdk umbrella header.
//
//  InfoNES.cpp, K6502.cpp and InfoNES_Mapper.cpp include <pico.h> directly,
//  for the sake of __not_in_flash_func(). This build is not on a pico, so this
//  header takes that name on the include path and supplies the same macros as
//  no-ops. It means the shared sources stay untouched and still merge from
//  upstream.
//
#pragma once

// The real pico.h drags in the fixed width integer types, and the core relies
// on that: without them uint16_t and int8_t are undeclared in InfoNES.cpp and
// K6502.cpp.
#include <stdint.h>
#include <stddef.h>

#include "PicoCompat.h"
