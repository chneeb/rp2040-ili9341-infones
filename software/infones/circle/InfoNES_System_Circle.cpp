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

// Not optional: NES_FILL_WIDTH, DISPLAY_FRAME_SKIP and SKIP_DISPLAY are all
// tested with #if below, and an undefined name evaluates to 0 there silently.
// Without this the scaling was compiled out while kernel.cpp still set a 320
// wide window, so SetArea read a 256 wide buffer past its end.
#include "DisplayConfig.h"

// The shared region reader, also used by the ROM menu.
#include "NesRegion.h"

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

#if NES_FILL_WIDTH

// 256 to 320 is exactly 4:5, so this is a clean nearest neighbour: four source
// pixels become five, with one of them doubled, and nothing else moves. The
// source column for each output column never changes, so it is worked out once.
static WORD s_Scaled[NES_OUT_WIDTH * NES_HEIGHT];
static unsigned s_SourceColumn[NES_OUT_WIDTH];
static boolean s_bScaleReady = FALSE;

static void ScaleFrame (void)
{
	if (!s_bScaleReady)
	{
		for (unsigned x = 0; x < NES_OUT_WIDTH; x++)
		{
			s_SourceColumn[x] = x * NES_WIDTH / NES_OUT_WIDTH;
		}

		s_bScaleReady = TRUE;
	}

	const WORD *pIn = s_Frame;
	WORD *pOut = s_Scaled;

	for (unsigned y = 0; y < NES_HEIGHT; y++)
	{
		for (unsigned x = 0; x < NES_OUT_WIDTH; x++)
		{
			pOut[x] = pIn[s_SourceColumn[x]];
		}

		pIn += NES_WIDTH;
		pOut += NES_OUT_WIDTH;
	}
}

#endif

// Defined with the rest of the battery SRAM handling, below.
static void FlushSRAMPeriodically (void);

int InfoNES_LoadFrame (void)
{
#if !SKIP_DISPLAY
	// Drop this frame if the previous one is still going out, rather than wait
	// for the bus. Waiting would stall the emulator and cost the game its
	// speed and its audio pitch; dropping only costs smoothness.
	//
	// This adapts on its own to whatever the bus is actually running at, which
	// matters because the core clock moves about and Circle fixes the SPI
	// divisor once at init. A full width frame is 12.3 ms at 100 MHz and goes
	// out every time; the same frame is 19.7 ms at 62.5 and lands every other
	// frame. Either way the game runs at the right speed.
	//
	// DISPLAY_FRAME_SKIP still applies on top, for forcing a lower rate.
	static unsigned nFrame = 0;
	if (++nFrame >= DISPLAY_FRAME_SKIP && !GamePi20_DisplayBusy ())
	{
		nFrame = 0;

#if NES_FILL_WIDTH
		ScaleFrame ();
		GamePi20_PresentFrame (s_Scaled);
#else
		GamePi20_PresentFrame (s_Frame);
#endif
	}
#endif

	// Before the wait, for the same reason: an SD write that fits in the slack
	// left in this frame costs nothing. It is a different peripheral from the
	// panel, so it does not disturb the transfer started above.
	FlushSRAMPeriodically ();

	// After presenting, not before: the frame is going out over DMA while this
	// waits, so the transfer costs nothing extra as long as it fits inside the
	// frame period.
	GamePi20_WaitForNextFrame ();

	return 0;
}

//
// Input
//

// SELECT and START together quit back to the ROM menu. The core checks
// PAD_SYS_QUIT once a scanline and unwinds out of InfoNES_Cycle(), which drops
// InfoNES_Main() back to InfoNES_Menu() - the menu below.
#define PAD_SELECT	0x04
#define PAD_START	0x08

void InfoNES_PadState (DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem)
{
	unsigned nPad = GamePi20_ReadPad ();

	*pdwPad1 = nPad;
	*pdwPad2 = 0;
	*pdwSystem =   (nPad & (PAD_SELECT | PAD_START)) == (PAD_SELECT | PAD_START)
		     ? PAD_SYS_QUIT : 0;
}

//
// Region
//
// InfoNES has no PAL support: it always runs 262 scanlines at the NTSC rate.
// A PAL game on that timing plays about 20% fast, with the music pitched up to
// match, because it expects 50 frames a second and gets 60.1.
//
// That much can be corrected from out here, without touching the shared core.
// Pace the frames at 50.007 Hz and open the sound device at the same fraction
// of its nominal rate, and speed and pitch both come right - see
// InfoNES_SoundOpen() for why the second one is needed.
//
// What is NOT corrected is the scanline count. A real PAL machine has 312
// lines and correspondingly more vblank; this still has 262. Games that do
// heavy work in vblank, or that time raster effects to the longer frame, can
// still misbehave. Proper PAL support means changing the core, which would
// cost the untouched-upstream property this port is built around.
//
// A happy accident: the APU frame counter ticks per scanline, so slowing the
// frames to 50 Hz puts it at 240 * 50/60 = 200 Hz, which is the real PAL rate.
//
#define FRAME_PERIOD_NTSC_US	16639		// 60.0988 Hz
#define FRAME_PERIOD_PAL_US	19997		// 50.0070 Hz

static boolean s_bPAL = FALSE;

// NesHeader is the 16 byte header as read, so it can go straight to the shared
// reader in NesRegion.h - the same one the ROM menu labels the list with, so
// the letter shown and the timing used can never disagree.
static boolean HeaderSaysPAL (void)
{
	static_assert (sizeof NesHeader == 16, "iNES header must be 16 bytes");

	return NesRegionFromHeader ((const unsigned char *) &NesHeader)
	       == NesRegionPAL;
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

	// Before InfoNES_Reset(), which is what opens the sound device, so the
	// rate it is opened at can follow the region.
	s_bPAL = HeaderSaysPAL ();
	GamePi20_SetFramePeriod (s_bPAL ? FRAME_PERIOD_PAL_US
					: FRAME_PERIOD_NTSC_US);

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
// Battery backed SRAM
//
// The 8 KB at 0x6000-0x7fff on carts that declare it (ROM_SRAM, set from the
// iNES header by InfoNES_Reset). Kept alongside the ROM as <game>.sav.
//
// This is not a save state: it is the cart's own battery, so it only helps
// games that had one. Zelda and Final Fantasy keep their save slots; a game
// that used passwords still uses passwords.
//

#define SAVE_PATH_LENGTH	256

// The .sav path for the game now running, empty when none is loaded, and the
// .tmp the new contents are built in first - see SaveSRAM().
static char s_SavePath[SAVE_PATH_LENGTH];
static char s_TempPath[SAVE_PATH_LENGTH];

// What is currently on the card, so a flush can tell whether anything actually
// changed. SRAMwritten is no use for this on its own: it is set by any write to
// the region, and games that treat it as scratch work RAM set it every frame,
// which would mean rewriting an identical file forever.
static BYTE s_SavedSRAM[SRAM_SIZE];

// "/nes/Game.nes" -> "/nes/Game.sav". Anything without room for the suffix, or
// with no extension to replace, gets no save file rather than a mangled one.
static void MakeSavePath (const char *pszRom)
{
	s_SavePath[0] = '\0';
	s_TempPath[0] = '\0';

	size_t nLength = strlen (pszRom);
	if (nLength < 5 || nLength >= SAVE_PATH_LENGTH)
	{
		return;
	}

	const char *pExt = pszRom + nLength - 4;
	if (*pExt != '.')
	{
		return;
	}

	memcpy (s_SavePath, pszRom, nLength - 4);
	strcpy (s_SavePath + nLength - 4, ".sav");

	memcpy (s_TempPath, pszRom, nLength - 4);
	strcpy (s_TempPath + nLength - 4, ".tmp");
}

// Read SRAM_SIZE bytes from pszPath into SRAM. Anything shorter is a write cut
// short by a power off, and is rejected rather than half loaded.
static boolean ReadSaveFile (const char *pszPath)
{
	FIL File;
	if (f_open (&File, pszPath, FA_READ) != FR_OK)
	{
		return FALSE;
	}

	UINT nRead;
	boolean bOK =    f_read (&File, SRAM, SRAM_SIZE, &nRead) == FR_OK
		      && nRead == SRAM_SIZE;

	f_close (&File);

	return bOK;
}

// Called once the machine is reset, so ROM_SRAM is valid and InfoNES_ReadRom
// has already zeroed SRAM (and laid down a trainer, if the cart has one - a
// cart with both is not a combination that exists, but loading after leaves
// the battery contents winning either way).
static void LoadSRAM (void)
{
	memset (s_SavedSRAM, 0, SRAM_SIZE);

	if (!ROM_SRAM || s_SavePath[0] == '\0')
	{
		return;
	}

	// The .tmp is the fallback, not the preference: it is only the newer of the
	// two in the instant between the unlink and the rename in SaveSRAM(), and
	// in that instant there is no .sav to prefer anyway.
	if (   !ReadSaveFile (s_SavePath)
	    && !ReadSaveFile (s_TempPath))
	{
		// Either no save yet, which is not an error and the first flush will
		// create one, or both copies are unreadable. Start the cart blank.
		memset (SRAM, 0, SRAM_SIZE);

		SRAMwritten = false;

		return;
	}

	memcpy (s_SavedSRAM, SRAM, SRAM_SIZE);

	SRAMwritten = false;
}

// Write the battery out if it differs from what is on the card. Returns
// quickly and does nothing at all in the common case.
//
// Built in a .tmp and swapped in, never written over the .sav in place. The
// flush is periodic and unannounced, so the machine is most likely to be
// switched off during exactly this - and writing in place means truncating a
// good save and then taking milliseconds to replace it, which loses everything
// if the power goes in between. Building beside it means the worst case is a
// throwaway .tmp and a .sav that is merely a few seconds stale.
static void SaveSRAM (void)
{
	if (   !ROM_SRAM
	    || s_SavePath[0] == '\0'
	    || memcmp (SRAM, s_SavedSRAM, SRAM_SIZE) == 0)
	{
		return;
	}

	FIL File;
	if (f_open (&File, s_TempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return;
	}

	UINT nWritten;
	boolean bOK =    f_write (&File, SRAM, SRAM_SIZE, &nWritten) == FR_OK
		      && nWritten == SRAM_SIZE;

	// The close is what commits the data and the length; a failure here means
	// the .tmp is not sound, so the .sav it would replace is left alone.
	bOK = f_close (&File) == FR_OK && bOK;

	if (!bOK)
	{
		return;
	}

	// f_rename will not overwrite, so the old one goes first. This is the only
	// window where the .sav is missing, and it is metadata rather than 8 KB of
	// data - microseconds against milliseconds. LoadSRAM() falls back to the
	// .tmp for it.
	f_unlink (s_SavePath);

	if (f_rename (s_TempPath, s_SavePath) != FR_OK)
	{
		// The .tmp still holds the new contents and LoadSRAM() will find it,
		// but the shadow must not claim the save is on the card under its
		// proper name, or the next flush would skip the retry.
		return;
	}

	memcpy (s_SavedSRAM, SRAM, SRAM_SIZE);

	SRAMwritten = false;
}

// Quitting to the menu is not how this device is usually put down - it is
// switched off mid-game - so waiting for the quit chord to flush would lose
// most saves. Checked once every few seconds instead, which bounds the loss to
// that and costs a memcmp the rest of the time.
#define SRAM_FLUSH_FRAMES	300		// about five seconds

static void FlushSRAMPeriodically (void)
{
	static unsigned nFrame = 0;

	if (++nFrame >= SRAM_FLUSH_FRAMES)
	{
		nFrame = 0;

		SaveSRAM ();
	}
}

//
// Menu
//

// Called by InfoNES_Main() before each game, and again every time one is
// quit. Returning -1 would end InfoNES_Main() altogether, which only happens
// here when there is nothing to play.
int InfoNES_Menu (void)
{
	// The game that just quit, before its ROM and SRAM are replaced below.
	// On the first call there is none and s_SavePath is empty.
	SaveSRAM ();

	const char *pRom = GamePi20_ChooseRom ();
	if (pRom == nullptr)
	{
		return -1;
	}

	// Nothing loaded from here until the new game is up, so a failure below
	// cannot write the old game's battery into the new game's file.
	s_SavePath[0] = '\0';

	// Releases the previous ROM, reads the new one and resets the machine.
	if (InfoNES_Load (pRom) != 0)
	{
		return -1;
	}

	MakeSavePath (pRom);
	LoadSRAM ();

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
	// On a PAL ROM the device is opened *slower* than the APU thinks it is.
	//
	// The APU's output per second is tied to the frame rate: it generates a
	// fixed number of samples per scanline (ApuQual[] in InfoNES_pAPU.cpp,
	// 22050/60/262 of them), so pacing at 50 Hz produces five sixths as many
	// samples a second. Left at 22050 that is a permanent underrun, breaking
	// up fifty times a second.
	//
	// Opening at five sixths of the rate makes the arithmetic balance again,
	// and fixes the pitch in the same stroke: samples the APU computed for
	// 22050 Hz, played at 18347, come out a factor 0.8321 lower, which is
	// exactly the 50.007/60.0988 a PAL game wants. One change, both problems.
	if (s_bPAL)
	{
		sample_rate = (int) ((u64) sample_rate * FRAME_PERIOD_NTSC_US
				     / FRAME_PERIOD_PAL_US);
	}

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
	// The five channels are summed, scaled by the volume, and offset so that
	// silence sits at mid scale rather than at zero.
	//
	// The offset is the important part. The APU's idea of silence is 0, which
	// is 0% PWM duty - the bottom rail. Circle pads an underrun with its null
	// frame, which for PWM is half scale (CSoundBaseDevice fills it with
	// m_nRangeMax / 2). With silence at 0 every underrun steps the output by
	// half of full scale and back, which is a loud click; sixty times a second
	// that is a steady chug under the music. It is also immune to muting -
	// muting drives the samples to 0, the value furthest from the null frame,
	// so it makes each step larger rather than smaller.
	//
	// Centring on 128 costs half the dynamic range, which the volume control
	// can make back, and makes an underrun inaudible instead of a click.
	// A divisor of 500 keeps volume 50 at the range the reference ports use.
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
			int nValue = 128 + (int) ((nSum * nVolume) / 500);

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
