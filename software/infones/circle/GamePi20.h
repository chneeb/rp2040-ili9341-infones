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

// Show the ROM menu and let the player choose. Returns the full path, or
// null if there is nothing to play.
const char *GamePi20_ChooseRom (void);

// Is the previous frame still going out over SPI? Used to drop a frame rather
// than wait for the bus, so the game keeps its speed whatever the bus rate
// turns out to be.
int GamePi20_DisplayBusy (void);

// Block until the next frame is due. Called once per frame, after the picture
// has been handed over, so the transfer runs during the wait.
void GamePi20_WaitForNextFrame (void);

// Start PWM audio at the rate the emulator asks for. Mono, 8 bit unsigned,
// which is what the APU produces. Returns 0 on success.
int GamePi20_SoundOpen (int nSampleRate);

void GamePi20_SoundClose (void);

// Queue mono 8 bit samples. Returns how many were taken; anything not taken is
// dropped rather than waited for, so audio never holds up a frame.
int GamePi20_SoundWrite (const unsigned char *pSamples, int nCount);

// Current volume as a percentage, worked by the shoulder buttons. 50 is the
// plain average of the APU channels; above that the mix is amplified.
unsigned GamePi20_GetVolume (void);

// The level as set, and whether it is muted - what the menu shows, as opposed
// to the effective volume above, which is 0 while muted.
// Frames per second actually achieved, measured over the last second of play.
unsigned GamePi20_GetMeasuredFPS (void);

// The core clock as measured when the SPI divisor was fixed.
unsigned GamePi20_GetCoreClockAtInit (void);

unsigned GamePi20_GetVolumeLevel (void);
int GamePi20_IsMuted (void);

// Room left in the queue, in samples. The APU uses this to decide how much to
// generate.
int GamePi20_SoundBufferAvail (void);

#ifdef __cplusplus
}
#endif
