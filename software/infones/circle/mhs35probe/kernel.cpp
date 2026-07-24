//
//  kernel.cpp
//
//  MHS35 probe stage 3: landscape 480x320, and nearest-neighbour scaling of a
//  synthetic 256x240 "NES" frame up to fill the panel - the two things the
//  emulator integration needs. The test image is asymmetric and labelled by
//  colour so its orientation on the panel is unambiguous:
//
//     +---------------------------+
//     |  RED (TL)   |  GREEN (TR) |
//     |-------------+-------------|
//     |  BLUE (BL)  |  WHITE (BR) |
//     +---------------------------+
//
//  with a thin YELLOW border. Whatever lands where tells us the MADCTL value.
//
#include "kernel.h"

#define PIN_DC		24
#define PIN_RST		25
#define PIN_CS		8

// Landscape now.
#define PANEL_WIDTH	480
#define PANEL_HEIGHT	320

// MADCTL for landscape. 0xE8 = MY | MV | BGR, intended to match the Linux
// rotate=270. If the quadrants come out rotated or mirrored, this is the one
// value to change.
#define PANEL_MADCTL	0xE8

#define SPI_CLOCK_HZ	32000000

// The NES native frame, and the scaled output sent to the panel.
#define NES_WIDTH	256
#define NES_HEIGHT	240

static u16 s_Nes[NES_WIDTH * NES_HEIGHT];
static u16 s_Out[PANEL_WIDTH * PANEL_HEIGHT];

// Precomputed nearest-neighbour source coordinates.
static unsigned s_SrcCol[PANEL_WIDTH];
static unsigned s_SrcRow[PANEL_HEIGHT];

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Display (&m_Interrupt, PIN_DC, PIN_RST, CILI9486DMADisplay::None, PIN_CS,
		   PANEL_WIDTH, PANEL_HEIGHT, SPI_CLOCK_HZ, 0, 0, PANEL_MADCTL, TRUE)
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	if (!m_Interrupt.Initialize ())	return FALSE;
	if (!m_Timer.Initialize ())	return FALSE;
	if (!m_Display.Initialize ())	return FALSE;

	return TRUE;
}

void CKernel::Blink (unsigned nTimes)
{
	for (unsigned i = 0; i < nTimes; i++)
	{
		m_ActLED.On ();
		CTimer::SimpleMsDelay (150);
		m_ActLED.Off ();
		CTimer::SimpleMsDelay (150);
	}
}

// Store a pixel big endian, as the panel wants it on the wire.
static inline u16 Wire (u16 usColour)
{
	return __builtin_bswap16 (usColour);
}

TShutdownMode CKernel::Run (void)
{
	static const u16 RED = 0xF800, GREEN = 0x07E0, BLUE = 0x001F,
			 WHITE = 0xFFFF, YELLOW = 0xFFE0;

	// Build the 256x240 test frame: four colour quadrants and a yellow border.
	for (unsigned y = 0; y < NES_HEIGHT; y++)
	{
		for (unsigned x = 0; x < NES_WIDTH; x++)
		{
			u16 c;
			if (x < 4 || x >= NES_WIDTH - 4 || y < 4 || y >= NES_HEIGHT - 4)
			{
				c = YELLOW;
			}
			else if (y < NES_HEIGHT / 2)
			{
				c = (x < NES_WIDTH / 2) ? RED : GREEN;
			}
			else
			{
				c = (x < NES_WIDTH / 2) ? BLUE : WHITE;
			}

			s_Nes[y * NES_WIDTH + x] = Wire (c);
		}
	}

	// Nearest-neighbour scale tables: each output pixel maps back to a source
	// pixel. 256->480 and 240->320, both stretched to fill.
	for (unsigned x = 0; x < PANEL_WIDTH; x++)
	{
		s_SrcCol[x] = x * NES_WIDTH / PANEL_WIDTH;
	}
	for (unsigned y = 0; y < PANEL_HEIGHT; y++)
	{
		s_SrcRow[y] = y * NES_HEIGHT / PANEL_HEIGHT;
	}

	// Scale into the output frame.
	for (unsigned y = 0; y < PANEL_HEIGHT; y++)
	{
		const u16 *pSrcRow = &s_Nes[s_SrcRow[y] * NES_WIDTH];
		u16 *pOut = &s_Out[y * PANEL_WIDTH];

		for (unsigned x = 0; x < PANEL_WIDTH; x++)
		{
			pOut[x] = pSrcRow[s_SrcCol[x]];
		}
	}

	CDisplay::TArea Area;
	Area.x1 = 0;
	Area.y1 = 0;
	Area.x2 = PANEL_WIDTH - 1;
	Area.y2 = PANEL_HEIGHT - 1;

	m_Display.SetArea (Area, s_Out);
	m_Display.WaitForTransfer ();

	for (;;)
	{
		Blink (3);
		CTimer::SimpleMsDelay (1500);
	}

	return ShutdownHalt;
}
