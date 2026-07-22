//
//  PicoCompat.h
//
//  The InfoNES core is not quite platform-free: InfoNES.cpp, K6502.cpp,
//  InfoNES_pAPU.cpp and one mapper wrap hot functions in the pico-sdk's
//  __not_in_flash_func(), which places code in RAM rather than in flash. On a
//  Raspberry Pi everything already runs from RAM, so it means nothing here.
//
//  Rather than touch the shared sources - which would make merging from
//  upstream harder - this header defines the macro away, and the Makefile
//  force-includes it into every emulator translation unit.
//
#pragma once

#ifndef __not_in_flash_func
#define __not_in_flash_func(funcname)		funcname
#endif

#ifndef __not_in_flash
#define __not_in_flash(group)
#endif

#ifndef __scratch_x
#define __scratch_x(group)
#endif

#ifndef __scratch_y
#define __scratch_y(group)
#endif
