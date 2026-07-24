//
//  ILI9486DMADisplay.cpp
//
//  See ILI9486DMADisplay.h. The DMA frame path is the same as ST7789DMADisplay;
//  the differences are the 16-bit register framing in Command()/Data() and the
//  fbtft ILI9486 init sequence.
//
#include "ILI9486DMADisplay.h"

#include <circle/timer.h>
#include <circle/machineinfo.h>
#include <circle/util.h>
#include <assert.h>

// Command opcodes (MIPI DCS, 8-bit values carried in 16-bit words).
#define ILI9486_CMD_SLPOUT	0x11
#define ILI9486_CMD_DISPON	0x29
#define ILI9486_CMD_CASET	0x2A
#define ILI9486_CMD_RASET	0x2B
#define ILI9486_CMD_RAMWR	0x2C
#define ILI9486_CMD_MADCTL	0x36
#define ILI9486_CMD_COLMOD	0x3A

CILI9486DMADisplay::CILI9486DMADisplay (CInterruptSystem *pInterrupt,
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
	m_SPIMaster (pInterrupt, nClockSpeed, CPOL, CPHA, FALSE),
	m_nCoreAtInit (CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE)),
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

CILI9486DMADisplay::~CILI9486DMADisplay (void)
{
	WaitForTransfer ();

	delete [] m_pFrameBuffer;
	delete [] m_pDummyRXBuffer;
}

//
// Low level transfers.
//

// Raw bytes with hand CS. Used for pixel data (DC high) and, via Command/Data
// below, for the 16-bit register words.
void CILI9486DMADisplay::WriteBytes (const void *pData, size_t nLength, boolean bIsData)
{
	assert (nLength <= MaxChunkBytes);

	m_DCPin.Write (bIsData ? HIGH : LOW);

	m_CSPin.Write (LOW);
	m_SPIMaster.WriteReadSync (m_SPIMaster.ChipSelectNone, pData,
				   m_pDummyRXBuffer, nLength);
	m_CSPin.Write (HIGH);
}

// A command is a 16-bit word: 0x00 high byte, opcode low byte, big endian.
void CILI9486DMADisplay::Command (u8 uchByte)
{
	u8 Word[2] = { 0x00, uchByte };
	WriteBytes (Word, sizeof Word, FALSE);
}

// A parameter is likewise a 16-bit word.
void CILI9486DMADisplay::Data (u8 uchByte)
{
	u8 Word[2] = { 0x00, uchByte };
	WriteBytes (Word, sizeof Word, TRUE);
}

void CILI9486DMADisplay::SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	assert (x0 <= x1 && x1 < m_nWidth);
	assert (y0 <= y1 && y1 < m_nHeight);

	Command (ILI9486_CMD_CASET);
	Data (x0 >> 8); Data (x0 & 0xFF);
	Data (x1 >> 8); Data (x1 & 0xFF);

	Command (ILI9486_CMD_RASET);
	Data (y0 >> 8); Data (y0 & 0xFF);
	Data (y1 >> 8); Data (y1 & 0xFF);

	Command (ILI9486_CMD_RAMWR);
}

boolean CILI9486DMADisplay::Initialize (void)
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
		CTimer::SimpleMsDelay (150);
	}

	// The fbtft fb_ili9486 init sequence, proven on this panel. Gamma is
	// included as fbtft sends it (tone only, but it removes a variable).
	Command (0xB0); Data (0x00);			// Interface Mode Control

	Command (ILI9486_CMD_SLPOUT);
	CTimer::SimpleMsDelay (250);

	Command (ILI9486_CMD_COLMOD); Data (0x55);	// 16-bit / pixel (RGB565)

	Command (0xC2); Data (0x44);			// Power Control 3
	Command (0xC5); Data (0x00); Data (0x00); Data (0x00); Data (0x00);  // VCOM

	static const u8 GammaP[] = { 0x0F, 0x1F, 0x1C, 0x0C, 0x0F, 0x08, 0x48, 0x98,
				     0x37, 0x0A, 0x13, 0x04, 0x11, 0x0D, 0x00 };
	Command (0xE0);
	for (unsigned i = 0; i < sizeof GammaP; i++) Data (GammaP[i]);

	static const u8 GammaN[] = { 0x0F, 0x32, 0x2E, 0x0B, 0x0D, 0x05, 0x47, 0x75,
				     0x37, 0x06, 0x10, 0x03, 0x24, 0x20, 0x00 };
	Command (0xE1);
	for (unsigned i = 0; i < sizeof GammaN; i++) Data (GammaN[i]);

	Command (ILI9486_CMD_MADCTL); Data (m_uchMADCTL);

	// Black the panel before it is lit, then display on, then backlight.
	memset (m_pFrameBuffer, 0, m_nWidth * m_nHeight * 2);
	ClearPanel ();

	Command (ILI9486_CMD_DISPON);
	CTimer::SimpleMsDelay (150);

	if (m_nBackLightPin != None)
	{
		m_BackLightPin.Write (HIGH);
	}

	return TRUE;
}

void CILI9486DMADisplay::ClearPanel (void)
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
// Frame transfers - identical in shape to ST7789DMADisplay: raw RGB565 pixels,
// chunked DMA, chip select held low across the whole frame.
//

void CILI9486DMADisplay::WaitForTransfer (void)
{
	while (m_bTransferActive)
	{
		// The completion routine runs from the DMA interrupt.
	}
}

void CILI9486DMADisplay::SetTargetClock (unsigned nTargetHz)
{
	unsigned nCoreNow = CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE);
	if (nCoreNow == 0 || m_nCoreAtInit == 0 || m_bTransferActive)
	{
		return;
	}

	// Scale the request by how far the core has moved since construction:
	// CSPIMasterDMA divides by the core rate it captured then, which is stale,
	// so this keeps the actual rate at nTargetHz whatever the core is doing.
	u64 nRequest = (u64) nTargetHz * m_nCoreAtInit / nCoreNow;

	// Hard safety cap: a misread core (or an unpinned one caught boosting
	// between the once-a-second re-aims) must never drive the panel arbitrarily
	// fast.
	if (nRequest > SPI_CLOCK_CEILING)
	{
		nRequest = SPI_CLOCK_CEILING;
	}
	if (nRequest == 0)
	{
		return;
	}

	m_nClockSpeed = (unsigned) nRequest;
	m_SPIMaster.SetClock (m_nClockSpeed);
}

void CILI9486DMADisplay::StartNextChunk (void)
{
	unsigned nRemaining = m_nBytesTotal - m_nBytesSent;
	unsigned nChunk = nRemaining < MaxChunkBytes ? nRemaining : MaxChunkBytes;

	m_SPIMaster.SetCompletionRoutine (DMACompletionStub, this);
	m_SPIMaster.StartWriteRead (m_SPIMaster.ChipSelectNone,
				    m_pFrameBuffer + m_nBytesSent,
				    m_pDummyRXBuffer, nChunk);

	m_nBytesSent += nChunk;
}

void CILI9486DMADisplay::DMACompletionStub (boolean bStatus, void *pParam)
{
	CILI9486DMADisplay *pThis = (CILI9486DMADisplay *) pParam;
	assert (pThis != 0);

	if (bStatus && pThis->m_nBytesSent < pThis->m_nBytesTotal)
	{
		pThis->StartNextChunk ();

		return;
	}

	pThis->m_CSPin.Write (HIGH);
	pThis->m_bTransferActive = FALSE;

	if (pThis->m_pCompletionRoutine != 0)
	{
		TAreaCompletionRoutine *pRoutine = pThis->m_pCompletionRoutine;
		pThis->m_pCompletionRoutine = 0;

		(*pRoutine) (pThis->m_pCompletionParam);
	}
}

void CILI9486DMADisplay::SetArea (const TArea &rArea, const void *pPixels,
				  TAreaCompletionRoutine *pRoutine, void *pParam)
{
	assert (pPixels != 0);

	WaitForTransfer ();

	unsigned nWidth  = rArea.x2 - rArea.x1 + 1;
	unsigned nHeight = rArea.y2 - rArea.y1 + 1;
	unsigned nBytes  = nWidth * nHeight * 2;

	assert (nBytes <= m_nWidth * m_nHeight * 2);

	memcpy (m_pFrameBuffer, pPixels, nBytes);

	SetWindow (rArea.x1, rArea.y1, rArea.x2, rArea.y2);

	m_nBytesTotal = nBytes;
	m_nBytesSent = 0;
	m_pCompletionRoutine = pRoutine;
	m_pCompletionParam = pParam;
	m_bTransferActive = TRUE;

	m_DCPin.Write (HIGH);
	m_CSPin.Write (LOW);

	StartNextChunk ();
}

void CILI9486DMADisplay::SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor)
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
