//
//  ST7789DMADisplay.cpp
//
//  See ST7789DMADisplay.h. The register sequence is the one from Circle's
//  addon/display/st7789display.cpp, which is GPL like this project; what is
//  different here is that the pixels go out over DMA instead of a polled write,
//  and that MADCTL is a parameter rather than a hardcoded 0x70.
//
#include "ST7789DMADisplay.h"

#include <circle/timer.h>
#include <circle/util.h>
#include <assert.h>

#define ST7789_SWRESET	0x01
#define ST7789_SLPOUT	0x11
#define ST7789_INVON	0x21
#define ST7789_DISPON	0x29
#define ST7789_CASET	0x2A
#define ST7789_RASET	0x2B
#define ST7789_RAMWR	0x2C
#define ST7789_MADCTL	0x36
#define ST7789_COLMOD	0x3A
#define ST7789_RAMCTRL	0xB0
#define ST7789_FRMCTR2	0xB2
#define ST7789_GCTRL	0xB7
#define ST7789_VCOMS	0xBB
#define ST7789_LCMCTRL	0xC0
#define ST7789_VDVVRHEN	0xC2
#define ST7789_VRHS	0xC3
#define ST7789_VDVS	0xC4
#define ST7789_FRCTRL2	0xC6
#define ST7789_PWCTRL1	0xD0
#define ST7789_GMCTRP1	0xE0
#define ST7789_GMCTRN1	0xE1

CST7789DMADisplay::CST7789DMADisplay (CInterruptSystem *pInterrupt,
				      unsigned nDCPin, unsigned nResetPin,
				      unsigned nBackLightPin, unsigned nCSPin,
				      unsigned nWidth, unsigned nHeight,
				      unsigned nClockSpeed, unsigned CPOL, unsigned CPHA,
				      u8 uchMADCTL, boolean bSwapColorBytes)
:	CDisplay (bSwapColorBytes ? RGB565_BE : RGB565),
	m_nDCPin (nDCPin),
	m_nResetPin (nResetPin),
	m_nBackLightPin (nBackLightPin),
	m_nCSPin (nCSPin),
	m_nWidth (nWidth),
	m_nHeight (nHeight),
	m_nClockSpeed (nClockSpeed),
	m_CPOL (CPOL),
	m_CPHA (CPHA),
	m_uchMADCTL (uchMADCTL),
	// ChipSelectNone: chip select is driven by hand below, so that it can be
	// held low across the several DMA transfers one frame takes. Letting the
	// SPI peripheral toggle it per transfer would break the RAMWR stream at
	// every chunk boundary.
	// bDMAChannelLite FALSE: the header asks for it at very high speeds, and
	// 62.5 MHz is the fastest divisor this core allows.
	m_SPIMaster (pInterrupt, nClockSpeed, CPOL, CPHA, FALSE),
	m_pFrameBuffer (0),
	m_pDummyRXBuffer (0),
	m_bTransferActive (FALSE),
	m_nBytesTotal (0),
	m_nBytesSent (0),
	m_pCompletionRoutine (0),
	m_pCompletionParam (0)
{
	m_DCPin.AssignPin (m_nDCPin);
	m_DCPin.SetMode (GPIOModeOutput);

	m_CSPin.AssignPin (m_nCSPin);
	m_CSPin.SetMode (GPIOModeOutput);
	m_CSPin.Write (HIGH);

	if (m_nResetPin != None)
	{
		m_ResetPin.AssignPin (m_nResetPin);
		m_ResetPin.SetMode (GPIOModeOutput);
	}

	if (m_nBackLightPin != None)
	{
		m_BackLightPin.AssignPin (m_nBackLightPin);
		m_BackLightPin.SetMode (GPIOModeOutput);
	}
}

CST7789DMADisplay::~CST7789DMADisplay (void)
{
	WaitForTransfer ();

	delete [] m_pFrameBuffer;
	delete [] m_pDummyRXBuffer;
}

//
// Low level transfers. These are the small ones - commands, register data and
// the window setup - and they are synchronous, because they are a handful of
// bytes and happen once per frame at most.
//

void CST7789DMADisplay::WriteBytes (const void *pData, size_t nLength, boolean bIsData)
{
	assert (nLength <= MaxChunkBytes);

	m_DCPin.Write (bIsData ? HIGH : LOW);

	m_CSPin.Write (LOW);
	m_SPIMaster.WriteReadSync (m_SPIMaster.ChipSelectNone, pData,
				   m_pDummyRXBuffer, nLength);
	m_CSPin.Write (HIGH);
}

void CST7789DMADisplay::Command (u8 uchByte)
{
	WriteBytes (&uchByte, 1, FALSE);
}

void CST7789DMADisplay::Data (u8 uchByte)
{
	WriteBytes (&uchByte, 1, TRUE);
}

void CST7789DMADisplay::SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	assert (x0 <= x1 && x1 < m_nWidth);
	assert (y0 <= y1 && y1 < m_nHeight);

	Command (ST7789_CASET);
	Data (x0 >> 8); Data (x0 & 0xFF);
	Data (x1 >> 8); Data (x1 & 0xFF);

	Command (ST7789_RASET);
	Data (y0 >> 8); Data (y0 & 0xFF);
	Data (y1 >> 8); Data (y1 & 0xFF);

	Command (ST7789_RAMWR);
}

boolean CST7789DMADisplay::Initialize (void)
{
	if (!m_SPIMaster.Initialize ())
	{
		return FALSE;
	}

	m_SPIMaster.SetClock (m_nClockSpeed);
	m_SPIMaster.SetMode (m_CPOL, m_CPHA);

	m_pFrameBuffer = new u8[m_nWidth * m_nHeight * 2];
	m_pDummyRXBuffer = new u8[MaxChunkBytes];
	if (m_pFrameBuffer == 0 || m_pDummyRXBuffer == 0)
	{
		return FALSE;
	}

	// The backlight stays off until there is something worth looking at. The
	// panel's memory holds whatever was in it at power-up, and lighting it
	// before that has been cleared shows a screenful of garbage.
	if (m_nBackLightPin != None)
	{
		m_BackLightPin.Write (LOW);
	}

	if (m_nResetPin != None)
	{
		m_ResetPin.Write (HIGH);
		CTimer::SimpleMsDelay (50);
		m_ResetPin.Write (LOW);
		CTimer::SimpleMsDelay (50);
		m_ResetPin.Write (HIGH);
		CTimer::SimpleMsDelay (50);
	}

	Command (ST7789_SWRESET);
	CTimer::SimpleMsDelay (150);

	// The orientation, unlike in Circle's driver, comes from the caller. The
	// GamePi20 has the panel mounted upside down and wants 0xB0 rather than
	// the usual 0x70.
	Command (ST7789_MADCTL);
	Data (m_uchMADCTL);

	Command (ST7789_FRMCTR2);
	Data (0x0C); Data (0x0C); Data (0x00); Data (0x33); Data (0x33);

	Command (ST7789_COLMOD);
	Data (0x05);

	Command (ST7789_RAMCTRL);
	Data (0x00);
	Data (GetColorModel () == RGB565_BE ? 0xF0 : 0xF8);

	Command (ST7789_GCTRL);		Data (0x14);
	Command (ST7789_VCOMS);		Data (0x37);
	Command (ST7789_LCMCTRL);	Data (0x2C);
	Command (ST7789_VDVVRHEN);	Data (0x01);
	Command (ST7789_VRHS);		Data (0x12);
	Command (ST7789_VDVS);		Data (0x20);
	Command (ST7789_PWCTRL1);	Data (0xA4); Data (0xA1);
	Command (ST7789_FRCTRL2);	Data (0x0F);

	static const u8 GammaP[] = { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
				     0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 };
	Command (ST7789_GMCTRP1);
	for (unsigned i = 0; i < sizeof GammaP; i++) Data (GammaP[i]);

	static const u8 GammaN[] = { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
				     0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 };
	Command (ST7789_GMCTRN1);
	for (unsigned i = 0; i < sizeof GammaN; i++) Data (GammaN[i]);

	Command (ST7789_INVON);
	Command (ST7789_SLPOUT);
	CTimer::SimpleMsDelay (100);

	// Black out the panel's memory before anything is shown, then turn the
	// display on and only then the backlight. In that order there is never a
	// moment where power-up garbage is lit.
	memset (m_pFrameBuffer, 0, m_nWidth * m_nHeight * 2);
	ClearPanel ();

	Command (ST7789_DISPON);
	CTimer::SimpleMsDelay (100);

	if (m_nBackLightPin != None)
	{
		m_BackLightPin.Write (HIGH);
	}

	return TRUE;
}

// Push the (zeroed) frame buffer out synchronously. Used once, during init,
// before the asynchronous path is in play.
void CST7789DMADisplay::ClearPanel (void)
{
	SetWindow (0, 0, m_nWidth - 1, m_nHeight - 1);

	unsigned nTotal = m_nWidth * m_nHeight * 2;
	unsigned nSent = 0;

	m_DCPin.Write (HIGH);
	m_CSPin.Write (LOW);

	while (nSent < nTotal)
	{
		unsigned nRemaining = nTotal - nSent;
		unsigned nChunk = nRemaining < MaxChunkBytes ? nRemaining : MaxChunkBytes;

		m_SPIMaster.WriteReadSync (m_SPIMaster.ChipSelectNone,
					   m_pFrameBuffer + nSent,
					   m_pDummyRXBuffer, nChunk);

		nSent += nChunk;
	}

	m_CSPin.Write (HIGH);
}

//
// Frame transfers.
//

void CST7789DMADisplay::WaitForTransfer (void)
{
	while (m_bTransferActive)
	{
		// The completion routine runs from the DMA interrupt.
	}
}

void CST7789DMADisplay::StartNextChunk (void)
{
	unsigned nRemaining = m_nBytesTotal - m_nBytesSent;
	unsigned nChunk = nRemaining < MaxChunkBytes ? nRemaining : MaxChunkBytes;

	m_SPIMaster.SetCompletionRoutine (DMACompletionStub, this);
	m_SPIMaster.StartWriteRead (m_SPIMaster.ChipSelectNone,
				    m_pFrameBuffer + m_nBytesSent,
				    m_pDummyRXBuffer, nChunk);

	m_nBytesSent += nChunk;
}

void CST7789DMADisplay::DMACompletionStub (boolean bStatus, void *pParam)
{
	CST7789DMADisplay *pThis = (CST7789DMADisplay *) pParam;
	assert (pThis != 0);

	if (bStatus && pThis->m_nBytesSent < pThis->m_nBytesTotal)
	{
		pThis->StartNextChunk ();

		return;
	}

	// Frame done: release the bus and let anyone waiting know.
	pThis->m_CSPin.Write (HIGH);
	pThis->m_bTransferActive = FALSE;

	if (pThis->m_pCompletionRoutine != 0)
	{
		TAreaCompletionRoutine *pRoutine = pThis->m_pCompletionRoutine;
		pThis->m_pCompletionRoutine = 0;

		(*pRoutine) (pThis->m_pCompletionParam);
	}
}

void CST7789DMADisplay::SetArea (const TArea &rArea, const void *pPixels,
				 TAreaCompletionRoutine *pRoutine, void *pParam)
{
	assert (pPixels != 0);

	// One frame at a time. Waiting here rather than at the end of the previous
	// call is what buys the overlap: the caller got its time back as soon as
	// the last frame started going out.
	WaitForTransfer ();

	unsigned nWidth  = rArea.x2 - rArea.x1 + 1;
	unsigned nHeight = rArea.y2 - rArea.y1 + 1;
	unsigned nBytes  = nWidth * nHeight * 2;

	assert (nBytes <= m_nWidth * m_nHeight * 2);

	// Copy out of the caller's buffer, so that it is free to be drawn into
	// again while this frame is still on the wire.
	memcpy (m_pFrameBuffer, pPixels, nBytes);

	SetWindow (rArea.x1, rArea.y1, rArea.x2, rArea.y2);

	m_nBytesTotal = nBytes;
	m_nBytesSent = 0;
	m_pCompletionRoutine = pRoutine;
	m_pCompletionParam = pParam;
	m_bTransferActive = TRUE;

	// Chip select stays low for the whole frame, across every chunk, so the
	// panel sees one uninterrupted RAMWR.
	m_DCPin.Write (HIGH);
	m_CSPin.Write (LOW);

	StartNextChunk ();
}

void CST7789DMADisplay::SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor)
{
	if (nPosX >= m_nWidth || nPosY >= m_nHeight)
	{
		return;
	}

	WaitForTransfer ();

	u16 usColor = (u16) nColor;

	SetWindow (nPosX, nPosY, nPosX, nPosY);
	WriteBytes (&usColor, sizeof usColor, TRUE);
}
