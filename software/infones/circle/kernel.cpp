//
//  kernel.cpp
//
#include "kernel.h"
#include "GamePi20.h"
#include "RomMenu.h"
#include <circle/util.h>
#include <assert.h>

// The emulator's own header. Kept to this one place, and to
// InfoNES_System_Circle.cpp, so Circle and InfoNES never meet in a header.
// Not extern "C": the core is C++, and its symbols are mangled accordingly.
#include "InfoNES.h"

#define DRIVE		"SD:"

// Where the menu looks for .nes files.
#define ROM_DIRECTORY	"SD:/nes"

// The panel's colour model is big endian RGB565 (ST7789_SWAP_COLOR_BYTES is
// TRUE, because the NES palette is stored that way), so colours written by
// hand have to be swapped to match.
#define RGB565(r, g, b)	((u16) __builtin_bswap16 ((u16) ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F))))

// InfoNES pad bits.
#define NES_PAD_A	0x01
#define NES_PAD_B	0x02
#define NES_PAD_SELECT	0x04
#define NES_PAD_START	0x08
#define NES_PAD_UP	0x10
#define NES_PAD_DOWN	0x20
#define NES_PAD_LEFT	0x40
#define NES_PAD_RIGHT	0x80

CKernel *CKernel::s_pThis = 0;

// The board's buttons, and what the NES sees them as.
static const struct
{
	unsigned nPin;
	unsigned nPadBit;
}
s_ButtonMap[] =
{
	{ GPIO_BUTTON_UP,	NES_PAD_UP	},
	{ GPIO_BUTTON_DOWN,	NES_PAD_DOWN	},
	{ GPIO_BUTTON_LEFT,	NES_PAD_LEFT	},
	{ GPIO_BUTTON_RIGHT,	NES_PAD_RIGHT	},
	// Crossed over on purpose: the board's silkscreen does not follow the
	// convention NES games expect, where A is the primary action and sits
	// under the thumb on the right. Swap these two lines back for a literal
	// A-to-A mapping.
	{ GPIO_BUTTON_A,	NES_PAD_B	},
	{ GPIO_BUTTON_B,	NES_PAD_A	},
	{ GPIO_BUTTON_SELECT,	NES_PAD_SELECT	},
	{ GPIO_BUTTON_START,	NES_PAD_START	},
	// Spare buttons, doubling for the two above so either pair can be used.
	// TL and TR are not here: they work the volume instead.
	{ GPIO_BUTTON_X,	NES_PAD_B	},
	{ GPIO_BUTTON_Y,	NES_PAD_A	}
};

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Display (&m_Interrupt, ST7789_DC_PIN, ST7789_RESET_PIN, ST7789_BACKLIGHT_PIN,
		   ST7789_CS_PIN, ST7789_WIDTH, ST7789_HEIGHT,
		   ST7789_CLOCK_SPEED, ST7789_CPOL, ST7789_CPHA,
		   ST7789_MADCTL, ST7789_SWAP_COLOR_BYTES),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED)
{
	s_pThis = this;
	m_ActLED.Blink (5);
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	// Interrupts first: the display signals the end of a transfer from an
	// interrupt, and its own init already uses DMA.
	if (!m_Interrupt.Initialize ())	return FALSE;
	if (!m_Timer.Initialize ())	return FALSE;
	if (!m_Display.Initialize ())	return FALSE;
	if (!m_EMMC.Initialize ())	return FALSE;

	InitializeButtons ();

	return TRUE;
}

void CKernel::InitializeButtons (void)
{
	static_assert (sizeof s_ButtonMap / sizeof s_ButtonMap[0] == GPIOButtonCount,
		       "s_ButtonMap and GPIOButtonCount disagree");

	for (unsigned i = 0; i < GPIOButtonCount; i++)
	{
		m_ButtonPins[i].AssignPin (s_ButtonMap[i].nPin);
		m_ButtonPins[i].SetMode (GPIOModeInputPullUp);
	}

	m_VolumeDownPin.AssignPin (GPIO_BUTTON_TL);
	m_VolumeDownPin.SetMode (GPIOModeInputPullUp);
	m_VolumeUpPin.AssignPin (GPIO_BUTTON_TR);
	m_VolumeUpPin.SetMode (GPIOModeInputPullUp);
}

// The buttons pull their pin to ground, so LOW means pressed.
//
// The shoulder buttons are handled here too, on the press rather than while
// held: this runs once a frame, so a held button would otherwise run the
// volume from one end to the other in well under a second.
unsigned CKernel::ReadPad (void)
{
	unsigned nPad = 0;

	for (unsigned i = 0; i < GPIOButtonCount; i++)
	{
		if (m_ButtonPins[i].Read () == LOW)
		{
			nPad |= s_ButtonMap[i].nPadBit;
		}
	}

	UpdateVolume ();

	return nPad;
}

// TL turns the volume down, TR up, both together mute and unmute.
//
// A step is taken when a button is *released*, not when it is pressed. Pressing
// two buttons together never quite happens at the same moment, so acting on the
// press would step the volume for whichever one arrived first, every time the
// mute chord was used. Waiting for the release means the chord can be spotted
// first and the step suppressed.
void CKernel::UpdateVolume (void)
{
	boolean bDown = m_VolumeDownPin.Read () == LOW;
	boolean bUp = m_VolumeUpPin.Read () == LOW;

	if (bDown && bUp)
	{
		if (!m_bVolumeChord)
		{
			m_bVolumeChord = TRUE;
			m_bMuted = !m_bMuted;
		}
	}
	else if (!bDown && !bUp)
	{
		if (m_bVolumeChord)
		{
			// Both let go after a mute: the presses have been used up.
			m_bVolumeChord = FALSE;
		}
		else if (m_bVolumeDownWasDown)
		{
			m_nVolume = m_nVolume > VOLUME_STEP ? m_nVolume - VOLUME_STEP : 0;
		}
		else if (m_bVolumeUpWasDown)
		{
			m_nVolume += VOLUME_STEP;
			if (m_nVolume > VOLUME_MAX)
			{
				m_nVolume = VOLUME_MAX;
			}
		}
	}

	m_bVolumeDownWasDown = bDown;
	m_bVolumeUpWasDown = bUp;
}

// Hand the 256x240 picture to the panel, centred in its 320x240. SetArea starts
// the transfer and returns, so the next frame is emulated while this one is
// still going out.
void CKernel::PresentFrame (const u16 *pFrame)
{
	CDisplay::TArea Area;
	Area.x1 = NES_OFFSET_X;
	Area.x2 = NES_OFFSET_X + NES_WIDTH - 1;
	Area.y1 = NES_OFFSET_Y;
	Area.y2 = NES_OFFSET_Y + NES_HEIGHT - 1;

	m_Display.SetArea (Area, pFrame);
}

// NTSC NES runs at 60.0988 Hz, which is 16639 us a frame. The pico build uses
// 16666 (a flat 60 Hz); the difference is small but it is free to get right,
// and it is what decides whether music plays at the pitch it should.
#define FRAME_PERIOD_US		16639

// Wait until the next frame is due.
//
// The deadline is carried forward, rather than set from the time this wait
// happened to end, so that the odd long frame does not push every later frame
// back with it. If a frame runs so long that the deadline has already gone by,
// the lost time is written off instead of being made up - catching up would
// mean sprinting through the following frames, which looks far worse than a
// single late one.
void CKernel::WaitForNextFrame (void)
{
	u64 nNow = CTimer::GetClockTicks64 ();

	if (m_nNextFrameTime == 0)
	{
		m_nNextFrameTime = nNow;
	}

	m_nNextFrameTime += FRAME_PERIOD_US;

	if (nNow < m_nNextFrameTime)
	{
		while (CTimer::GetClockTicks64 () < m_nNextFrameTime)
		{
			// The frame is already on its way out over DMA while this spins.
		}
	}
	else
	{
		m_nNextFrameTime = nNow;
	}

}

//
// Sound. PWM on GPIO 18, which on this board feeds both the speaker and the
// earphone jack - see configure-gamepi20.sh for the three options that put it
// there rather than on GPIO 12 and 13, which are the Up and Right buttons.
//
// The APU produces 8 bit unsigned mono, which Circle takes directly as
// SoundFormatUnsigned8, so nothing has to be converted.
//

// Enough queued audio to ride out a frame that runs long, without adding so
// much delay that the sound drifts noticeably behind the picture.
#define SOUND_QUEUE_MSECS	100

int CKernel::SoundOpen (int nSampleRate)
{
	if (m_pSound != 0)
	{
		return 0;
	}

	m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, nSampleRate);
	if (m_pSound == 0)
	{
		return -1;
	}

	if (!m_pSound->AllocateQueue (SOUND_QUEUE_MSECS))
	{
		delete m_pSound;
		m_pSound = 0;

		return -1;
	}

	m_pSound->SetWriteFormat (SoundFormatUnsigned8, 1);

	if (!m_pSound->Start ())
	{
		delete m_pSound;
		m_pSound = 0;

		return -1;
	}

	return 0;
}

void CKernel::SoundClose (void)
{
	if (m_pSound != 0)
	{
		m_pSound->Cancel ();

		delete m_pSound;
		m_pSound = 0;
	}
}

// Whatever does not fit is dropped. Waiting for room would stall the frame the
// emulator is in the middle of, and a dropped sample is far less noticeable
// than a late frame.
int CKernel::SoundWrite (const unsigned char *pSamples, int nCount)
{
	if (m_pSound == 0)
	{
		return nCount;
	}

	return m_pSound->Write (pSamples, nCount);
}

// Room left to write into, which is what the APU means by "buffer size": it
// clamps the number of samples it generates to this
// (InfoNES_pAPUHsync -> std::min(bufferLeft, n)).
//
// Not GetQueueFramesAvail(): despite the name that is the number of frames
// already queued and waiting to be sent. Returning it deadlocks the emulator
// into silence - an empty queue reads as no room, so the APU generates
// nothing, so the queue stays empty.
int CKernel::SoundBufferAvail (void)
{
	if (m_pSound == 0)
	{
		return 0;
	}

	return m_pSound->GetQueueSizeFrames () - m_pSound->GetQueueFramesAvail ();
}

TShutdownMode CKernel::Run (void)
{
#if ST7789_TEST_PATTERN
	DrawTestPattern ();

	for (;;)
	{
		m_ActLED.Blink (1);
		m_Timer.MsDelay (1000);
	}
#else
	if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
	{
		return ShutdownHalt;
	}

	m_pMenu = new CRomMenu (&m_Display);
	if (m_pMenu == 0 || !m_pMenu->Initialize ())
	{
		return ShutdownHalt;
	}

	m_pMenu->Scan (ROM_DIRECTORY);

	// InfoNES_Main() owns the loop from here: it calls InfoNES_Menu(), which
	// comes back through ChooseRom() below, then runs the game until the quit
	// chord is pressed, then asks for a ROM again.
	InfoNES_Main ();

	f_mount (0, DRIVE, 0);
#endif

	return ShutdownHalt;
}

#if ST7789_TEST_PATTERN

// A static pattern, drawn into the 256x240 area the emulator will use. What it
// tells you:
//
//   nothing at all       -> wiring, chip select or the backlight pin
//   wrong bar colours    -> ST7789_SWAP_COLOR_BYTES
//   markers wrong corner -> orientation, adjust ST7789_MADCTL
//   bars not centred     -> the pillarbox offsets
//
void CKernel::DrawTestPattern (void)
{
	static u16 Frame[NES_WIDTH * NES_HEIGHT];

	static const u16 Bars[4] =
	{
		RGB565 (31,  0,  0),		// red
		RGB565 ( 0, 63,  0),		// green
		RGB565 ( 0,  0, 31),		// blue
		RGB565 (31, 63, 31)		// white
	};

	for (unsigned y = 0; y < NES_HEIGHT; y++)
	{
		for (unsigned x = 0; x < NES_WIDTH; x++)
		{
			Frame[y * NES_WIDTH + x] = Bars[x / (NES_WIDTH / 4)];
		}
	}

	// A three pixel border, to check that the whole 256x240 area lands inside
	// the panel and that the pillarbox offsets are right: 32 px of black should
	// remain either side.
	const u16 Yellow = RGB565 (31, 63, 0);
	for (unsigned i = 0; i < 3; i++)
	{
		for (unsigned x = 0; x < NES_WIDTH; x++)
		{
			Frame[i * NES_WIDTH + x] = Yellow;
			Frame[(NES_HEIGHT - 1 - i) * NES_WIDTH + x] = Yellow;
		}
		for (unsigned y = 0; y < NES_HEIGHT; y++)
		{
			Frame[y * NES_WIDTH + i] = Yellow;
			Frame[y * NES_WIDTH + (NES_WIDTH - 1 - i)] = Yellow;
		}
	}

	// Corner markers: yellow top left, magenta top right.
	const u16 Magenta = RGB565 (31, 0, 31);
	for (unsigned y = 0; y < 20; y++)
	{
		for (unsigned x = 0; x < 20; x++)
		{
			Frame[y * NES_WIDTH + x] = Yellow;
		}
		for (unsigned x = NES_WIDTH - 10; x < NES_WIDTH; x++)
		{
			Frame[y * NES_WIDTH + x] = Magenta;
		}
	}

	PresentFrame (Frame);
	m_Display.WaitForTransfer ();
}

#endif

//
// The bridge declared in GamePi20.h.
//

void GamePi20_PresentFrame (const unsigned short *pFrame)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->PresentFrame ((const u16 *) pFrame);
}

unsigned GamePi20_ReadPad (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->ReadPad ();
}

void GamePi20_WaitForNextFrame (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->WaitForNextFrame ();
}

int GamePi20_SoundOpen (int nSampleRate)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->SoundOpen (nSampleRate);
}

void GamePi20_SoundClose (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->SoundClose ();
}

int GamePi20_SoundWrite (const unsigned char *pSamples, int nCount)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->SoundWrite (pSamples, nCount);
}

const char *CKernel::ChooseRom (void)
{
	if (m_pMenu == 0)
	{
		return nullptr;
	}

	return m_pMenu->Run (GamePi20_ReadPad, GamePi20_WaitForNextFrame);
}

const char *GamePi20_ChooseRom (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->ChooseRom ();
}

unsigned GamePi20_GetVolume (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->GetVolume ();
}

int GamePi20_SoundBufferAvail (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->SoundBufferAvail ();
}

