//
//  ST7789DMADisplay.h
//
//  An ST7789 panel driven over SPI with DMA.
//
//  Circle ships CST7789Display in addon/display, and it works, but its
//  SendData() goes through CSPIMaster::Write(), which is polled: the CPU spins
//  for the whole transfer. At 320x240x16bpp and 62.5 MHz that is 19.7 ms of
//  every frame spent doing nothing, which is most of the frame budget.
//
//  This class does the same job with CSPIMasterDMA, and hands the frame over
//  asynchronously: SetArea() copies the picture, starts the transfer and
//  returns, so the next frame can be worked out while the current one is still
//  going out over the wire.
//
//  It is a separate class rather than a subclass because CST7789Display keeps
//  SendData(), SetWindow(), Command() and Data() private, so there is nothing
//  for a subclass to reuse - and patching the pinned submodule is worse.
//
#pragma once

#include <circle/display.h>
#include <circle/spimasterdma.h>
#include <circle/gpiopin.h>
#include <circle/interrupt.h>
#include <circle/types.h>

class CST7789DMADisplay : public CDisplay
{
public:
	/// \param pInterrupt   Interrupt system, needed by the DMA channels
	/// \param nDCPin       Data/command pin (SoC number)
	/// \param nResetPin    Reset pin, or None
	/// \param nBackLightPin Backlight pin, or None
	/// \param nCSPin       Chip select pin, driven by hand (see below)
	/// \param nWidth       Panel width in pixels
	/// \param nHeight      Panel height in pixels
	/// \param nClockSpeed  SPI clock in Hz
	/// \param CPOL         SPI clock polarity
	/// \param CPHA         SPI clock phase
	/// \param uchMADCTL    Memory access control, sets the orientation
	/// \param bSwapColorBytes Big endian colors instead of normal RGB565
	static const unsigned None = GPIO_PINS;

	CST7789DMADisplay (CInterruptSystem *pInterrupt,
			   unsigned nDCPin, unsigned nResetPin, unsigned nBackLightPin,
			   unsigned nCSPin,
			   unsigned nWidth, unsigned nHeight,
			   unsigned nClockSpeed, unsigned CPOL, unsigned CPHA,
			   u8 uchMADCTL, boolean bSwapColorBytes);

	~CST7789DMADisplay (void);

	boolean Initialize (void);

	unsigned GetWidth (void) const override	{ return m_nWidth; }
	unsigned GetHeight (void) const override	{ return m_nHeight; }
	unsigned GetDepth (void) const override	{ return 16; }

	void SetPixel (unsigned nPosX, unsigned nPosY, TRawColor nColor) override;

	void SetArea (const TArea &rArea, const void *pPixels,
		      TAreaCompletionRoutine *pRoutine = nullptr,
		      void *pParam = nullptr) override;

	/// \brief Block until the frame being sent has gone out.
	void WaitForTransfer (void);

private:
	void Command (u8 uchByte);
	void Data (u8 uchByte);
	void WriteBytes (const void *pData, size_t nLength, boolean bIsData);
	void SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1);
	void ClearPanel (void);

	void StartNextChunk (void);
	static void DMACompletionStub (boolean bStatus, void *pParam);

	// The SPI peripheral takes at most 0xFFFF bytes per transfer, so a frame
	// goes out in several chunks. Keep the chunk a whole number of rows, so a
	// partly sent frame never leaves half a pixel behind.
	static const unsigned MaxChunkBytes = 0xF000;	// 61440, and 96 rows of 320

	unsigned m_nDCPin, m_nResetPin, m_nBackLightPin, m_nCSPin;
	unsigned m_nWidth, m_nHeight;
	unsigned m_nClockSpeed, m_CPOL, m_CPHA;
	u8	 m_uchMADCTL;

	CSPIMasterDMA m_SPIMaster;
	CGPIOPin      m_DCPin;
	CGPIOPin      m_ResetPin;
	CGPIOPin      m_BackLightPin;
	CGPIOPin      m_CSPin;

	u8 *m_pFrameBuffer;	// the picture being sent, owned here
	u8 *m_pDummyRXBuffer;	// StartWriteRead() insists on a read buffer

	volatile boolean m_bTransferActive;
	unsigned m_nBytesTotal;
	unsigned m_nBytesSent;

	TAreaCompletionRoutine *m_pCompletionRoutine;
	void *m_pCompletionParam;
};
