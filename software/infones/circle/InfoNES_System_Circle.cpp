//
//  InfoNES_System_Circle.cpp
//
//  The platform layer: the functions InfoNES_System.h declares, implemented
//  for Circle on a Raspberry Pi. Modelled on linux/InfoNES_System_Linux.cpp,
//  which is the reference port in this tree.
//
//  The emulator core is scanline based: it calls InfoNES_PreDrawLine() before
//  drawing each line and expects a buffer to write it into. This port points
//  that buffer straight at the matching row of a full 256x240 frame, so a
//  finished frame accumulates with no copying, and the whole frame goes out in
//  one asynchronous transfer at InfoNES_LoadFrame().
//
//  That is deliberately simpler than the pico build, which ping-pongs two
//  scanline buffers and DMAs each line as it is produced. It has to: it has
//  264 KB of RAM. A Pi has 512 MB, and ST7789DMADisplay already returns while
//  the frame is still going out, so the emulation of the next frame overlaps
//  the transfer of the current one anyway.
//
#include <circle/util.h>
#include <circle/timer.h>
#include <circle/alloc.h>
#include <fatfs/ff.h>

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

#include "GamePi20.h"

//
// The NES palette, in the panel's pixel format.
//
// The values are from the pico build (main.cpp) and are big endian RGB565,
// which is why ST7789_SWAP_COLOR_BYTES is TRUE for this port and FALSE for
// circle-arcade, whose LMI assets are plain RGB565.
//
// The pico build masks each entry with 32767. That is dropped here: checked
// against the real 2C02 palette, masking makes the colours worse, roughly
// doubling the error. Read straight, entry 0x21 comes out (57, 190, 255)
// against a true sky blue of (63, 191, 255).
//
#define CC(x)	((WORD) (x))

const WORD NesPalette[64] =
{
	CC(0xAE73), CC(0xD120), CC(0x1500), CC(0x1340), CC(0x0E88), CC(0x02A8), CC(0x00A0), CC(0x4078),
	CC(0x6041), CC(0x2002), CC(0x8002), CC(0xE201), CC(0xEB19), CC(0x0000), CC(0x0000), CC(0x0000),
	CC(0xF7BD), CC(0x9D03), CC(0xDD21), CC(0x1E80), CC(0x17B8), CC(0x0BE0), CC(0x40D9), CC(0x61CA),
	CC(0x808B), CC(0xA004), CC(0x4005), CC(0x8704), CC(0x1104), CC(0x0000), CC(0x0000), CC(0x0000),
	CC(0xFFFF), CC(0xFF3D), CC(0xBF5C), CC(0x5FA4), CC(0xDFF3), CC(0xB6FB), CC(0xACFB), CC(0xC7FC),
	CC(0xE7F5), CC(0x8286), CC(0xE94E), CC(0xD35F), CC(0x5B07), CC(0x0000), CC(0x0000), CC(0x0000),
	CC(0xFFFF), CC(0x3FAF), CC(0xBFC6), CC(0x5FD6), CC(0x3FFE), CC(0x3BFE), CC(0xF6FD), CC(0xD5FE),
	CC(0x34FF), CC(0xF4E7), CC(0x97AF), CC(0xF9B7), CC(0xFE9F), CC(0x0000), CC(0x0000), CC(0x0000)
};

//
// The core prints progress messages. There is nowhere to print to yet, so they
// are swallowed; wiring this to Circle's CLogger would be the way to get them.
//
extern "C" int printf (const char *pFormat, ...)
{
	return 0;
}

//
// Video
//

// The frame the core draws into, handed over whole once per frame.
static WORD s_Frame[NES_DISP_WIDTH * NES_DISP_HEIGHT];

void InfoNES_PreDrawLine (int line)
{
	// Point the core's line buffer at this row of the frame. NES_FIRST_SCANLINE
	// is 0 for this port, so line indexes rows directly.
	InfoNES_SetLineBuffer (&s_Frame[line * NES_DISP_WIDTH], NES_DISP_WIDTH);
}

void InfoNES_PostDrawLine (int line)
{
	// Nothing to do: the core wrote straight into the frame.
}

int InfoNES_LoadFrame (void)
{
	GamePi20_PresentFrame (s_Frame);

	// After presenting, not before: the frame is going out over DMA while this
	// waits, so the transfer costs nothing extra as long as it fits inside the
	// frame period.
	GamePi20_WaitForNextFrame ();

	return 0;
}

//
// Input
//

void InfoNES_PadState (DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
	*pdwPad1 = GamePi20_ReadPad ();
	*pdwPad2 = 0;
	*pdwSystem = 0;
}

//
// ROM loading
//

int InfoNES_ReadRom (const char *pszFileName)
{
	FIL File;
	UINT nRead;

	if (f_open (&File, pszFileName, FA_READ) != FR_OK)
	{
		return -1;
	}

	if (   f_read (&File, &NesHeader, sizeof NesHeader, &nRead) != FR_OK
	    || nRead != sizeof NesHeader
	    || memcmp (NesHeader.byID, "NES\x1a", 4) != 0)
	{
		f_close (&File);

		return -1;
	}

	memset (SRAM, 0, SRAM_SIZE);

	// A trainer, if present, sits at 0x7000-0x71ff.
	if (NesHeader.byInfo1 & 4)
	{
		f_read (&File, &SRAM[0x1000], 512, &nRead);
	}

	ROM = (BYTE *) malloc (NesHeader.byRomSize * 0x4000);
	if (ROM == 0)
	{
		f_close (&File);

		return -1;
	}

	f_read (&File, ROM, NesHeader.byRomSize * 0x4000, &nRead);

	if (NesHeader.byVRomSize > 0)
	{
		VROM = (BYTE *) malloc (NesHeader.byVRomSize * 0x2000);
		if (VROM == 0)
		{
			free (ROM);
			ROM = 0;
			f_close (&File);

			return -1;
		}

		f_read (&File, VROM, NesHeader.byVRomSize * 0x2000, &nRead);
	}

	f_close (&File);

	return 0;
}

void InfoNES_ReleaseRom (void)
{
	if (ROM != 0)
	{
		free (ROM);
		ROM = 0;
	}

	if (VROM != 0)
	{
		free (VROM);
		VROM = 0;
	}
}

//
// Menu
//

int InfoNES_Menu (void)
{
	// 0 keeps the emulator running. A ROM browser would go here; for now the
	// ROM to load is decided in kernel.cpp.
	return 0;
}

void RomSelect_PreDrawLine (int line)
{
	// Only used by the ROM browser, which this port does not have yet.
}

//
// Sound.
//
// The APU hands over five channels of 8 bit unsigned mono - two pulse, one
// triangle, one noise, one DPCM - and expects them mixed. pAPU_QUALITY is 2 in
// InfoNES_pAPU.h, so the rate is 22050 Hz.
//

// A frame's worth at 22050 Hz is around 367 samples; this leaves plenty of
// headroom for a long one.
#define SOUND_CHUNK	1024

static BYTE s_MixBuffer[SOUND_CHUNK];

void InfoNES_SoundInit (void)
{
}

int InfoNES_SoundOpen (int samples_per_sync, int sample_rate)
{
	int nResult = GamePi20_SoundOpen (sample_rate);

	// The core starts muted (APU_Mute is 1 in InfoNES.cpp) and leaves it to the
	// platform layer to decide otherwise, which the pico build does in main.cpp
	// too. While it is set, InfoNES_pAPUHsync() zeroes all five wave buffers
	// and K6502_rw.h drops APU register writes, so everything downstream works
	// perfectly and produces silence.
	if (nResult == 0)
	{
		APU_Mute = 0;
	}

	return nResult;
}

void InfoNES_SoundClose (void)
{
	GamePi20_SoundClose ();
}

void InfoNES_SoundOutput (int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3,
			  BYTE *wave4, BYTE *wave5)
{
	// The five channels are summed rather than averaged, and then scaled by
	// the volume. A divisor of 250 puts unity at volume 50, which is the plain
	// average the reference ports use (linux/InfoNES_System_Linux.cpp) - quiet,
	// because a single channel only ever reaches a fifth of full scale. Above
	// that the mix is amplified and has to be clamped, which is the trade the
	// volume control exists to let you make.
	unsigned nVolume = GamePi20_GetVolume ();

	int nDone = 0;

	while (nDone < samples)
	{
		int nChunk = samples - nDone;
		if (nChunk > SOUND_CHUNK)
		{
			nChunk = SOUND_CHUNK;
		}

		for (int i = 0; i < nChunk; i++)
		{
			int j = nDone + i;

			int nSum = wave1[j] + wave2[j] + wave3[j] + wave4[j] + wave5[j];
			int nValue = (int) ((nSum * nVolume) / 250);

			s_MixBuffer[i] = (BYTE) (nValue > 255 ? 255 : nValue);
		}

		GamePi20_SoundWrite (s_MixBuffer, nChunk);

		nDone += nChunk;
	}
}

int InfoNES_GetSoundBufferSize (void)
{
	return GamePi20_SoundBufferAvail ();
}

//
// Diagnostics
//

void InfoNES_DebugPrint (const char *pszMsg)
{
}

void InfoNES_MessageBox (const char *pszMsg, ...)
{
}
