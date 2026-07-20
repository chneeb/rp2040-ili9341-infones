//
//  GamePi20.h
//
//  The narrow bridge between the InfoNES platform layer and the Circle kernel.
//
//  InfoNES_System_Circle.cpp has to include the emulator's headers, and the
//  kernel has to include Circle's; keeping the two apart behind these two
//  functions avoids dragging either set into the other.
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Hand a finished 256x240 RGB565 frame to the panel. Returns as soon as the
// transfer has started.
void GamePi20_PresentFrame (const unsigned short *pFrame);

// Current state of the board's buttons, in InfoNES pad bits:
// A=1, B=2, SELECT=4, START=8, UP=0x10, DOWN=0x20, LEFT=0x40, RIGHT=0x80.
unsigned GamePi20_ReadPad (void);

// Block until the next frame is due. Called once per frame, after the picture
// has been handed over, so the transfer runs during the wait.
void GamePi20_WaitForNextFrame (void);

#ifdef __cplusplus
}
#endif
