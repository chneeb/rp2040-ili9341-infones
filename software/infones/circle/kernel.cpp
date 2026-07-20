//
//  kernel.cpp
//
#include "kernel.h"
#include "GamePi20.h"
#include <circle/util.h>
#include <assert.h>

// The emulator's own header. Kept to this one place, and to
// InfoNES_System_Circle.cpp, so Circle and InfoNES never meet in a header.
// Not extern "C": the core is C++, and its symbols are mangled accordingly.
#include "InfoNES.h"

#define DRIVE		"SD:"
#define ROM_FILE	"SD:/rom.nes"

// RGB565, matching the panel's colour model (ST7789_SWAP_COLOR_BYTES FALSE).
#define RGB565(r, g, b)	((u16) ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F)))

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

// The board's buttons, and what the NES sees them as. The shoulder buttons and
// X/Y have no NES equivalent and are left out.
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
	{ GPIO_BUTTON_A,	NES_PAD_A	},
	{ GPIO_BUTTON_B,	NES_PAD_B	},
	{ GPIO_BUTTON_SELECT,	NES_PAD_SELECT	},
	{ GPIO_BUTTON_START,	NES_PAD_START	},
	// Spare buttons, mapped to the same actions for comfort.
	{ GPIO_BUTTON_X,	NES_PAD_A	},
	{ GPIO_BUTTON_Y,	NES_PAD_B	},
	{ GPIO_BUTTON_TL,	NES_PAD_SELECT	},
	{ GPIO_BUTTON_TR,	NES_PAD_START	}
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
}

// The buttons pull their pin to ground, so LOW means pressed.
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

	return nPad;
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

	InfoNES_Init ();

	if (InfoNES_Load (ROM_FILE) != 0)
	{
		// No ROM: leave the panel showing whatever is there and stop, rather
		// than running the emulator on nothing.
		return ShutdownHalt;
	}

	// Never returns: InfoNES_Main() is the emulator's own loop, and it calls
	// back into this port through InfoNES_System_Circle.cpp.
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
