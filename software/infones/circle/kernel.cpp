//
//  kernel.cpp
//
#include "kernel.h"
#include <circle/util.h>
#include <assert.h>

// RGB565, matching the panel's colour model (ST7789_SWAP_COLOR_BYTES FALSE).
#define RGB565(r, g, b)	((u16) ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F)))

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Display (&m_Interrupt, ST7789_DC_PIN, ST7789_RESET_PIN, ST7789_BACKLIGHT_PIN,
		   ST7789_CS_PIN, ST7789_WIDTH, ST7789_HEIGHT,
		   ST7789_CLOCK_SPEED, ST7789_CPOL, ST7789_CPHA,
		   ST7789_MADCTL, ST7789_SWAP_COLOR_BYTES)
{
	m_ActLED.Blink (5);
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	// Interrupts first: the display signals the end of a transfer from an
	// interrupt, and its own init already uses DMA.
	if (!m_Interrupt.Initialize ())
	{
		return FALSE;
	}

	if (!m_Timer.Initialize ())
	{
		return FALSE;
	}

	if (!m_Display.Initialize ())
	{
		return FALSE;
	}

	memset (m_Frame, 0, sizeof m_Frame);

	return TRUE;
}

// Hand the 256x240 picture to the panel, centred in its 320x240. SetArea starts
// the transfer and returns, so the next frame can be worked out while this one
// is still going out.
void CKernel::PresentFrame (void)
{
	CDisplay::TArea Area;
	Area.x1 = NES_OFFSET_X;
	Area.x2 = NES_OFFSET_X + NES_WIDTH - 1;
	Area.y1 = NES_OFFSET_Y;
	Area.y2 = NES_OFFSET_Y + NES_HEIGHT - 1;

	m_Display.SetArea (Area, m_Frame);
}

TShutdownMode CKernel::Run (void)
{
#if ST7789_TEST_PATTERN
	DrawTestPattern ();
#endif

	for (;;)
	{
		m_ActLED.Blink (1);
		m_Timer.MsDelay (1000);
	}

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
	// Four vertical bars, to check the colour order.
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
			m_Frame[y * NES_WIDTH + x] = Bars[x / (NES_WIDTH / 4)];
		}
	}

	// A three pixel border, to check that the whole 256x240 area lands inside
	// the panel and that the pillarbox offsets are right. Anything outside it
	// should stay black, giving 32 px bars left and right.
	const u16 Yellow = RGB565 (31, 63, 0);
	for (unsigned i = 0; i < 3; i++)
	{
		for (unsigned x = 0; x < NES_WIDTH; x++)
		{
			m_Frame[i * NES_WIDTH + x] = Yellow;
			m_Frame[(NES_HEIGHT - 1 - i) * NES_WIDTH + x] = Yellow;
		}
		for (unsigned y = 0; y < NES_HEIGHT; y++)
		{
			m_Frame[y * NES_WIDTH + i] = Yellow;
			m_Frame[y * NES_WIDTH + (NES_WIDTH - 1 - i)] = Yellow;
		}
	}

	// Corner markers, to check the orientation: yellow top left, magenta top
	// right.
	const u16 Magenta = RGB565 (31, 0, 31);
	for (unsigned y = 0; y < 20; y++)
	{
		for (unsigned x = 0; x < 20; x++)
		{
			m_Frame[y * NES_WIDTH + x] = Yellow;
		}
		for (unsigned x = NES_WIDTH - 10; x < NES_WIDTH; x++)
		{
			m_Frame[y * NES_WIDTH + x] = Magenta;
		}
	}

	PresentFrame ();
	m_Display.WaitForTransfer ();
}

#endif
