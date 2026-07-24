//
//  ILI9486DMADisplay.h
//
//  An ILI9486 panel (e.g. the goodtft MHS35, 480x320) driven over SPI with DMA.
//
//  Structured like ST7789DMADisplay - same asynchronous chunked-DMA frame path,
//  same hand-driven chip select held low across a whole RAMWR - but with the one
//  thing the ILI9486 needs that the ST7789 does not: a 16-bit register
//  interface. Every command and parameter goes out as a 16-bit word (0x00 high
//  byte, value low byte), matching the goodtft mhs35 overlay's regwidth=16 and
//  Linux fbtft's write_reg16_bus8. Pixels are unaffected - RGB565 is genuinely
//  16-bit already, two raw bytes each.
//
//  Sending 8-bit commands leaves the controller latching half a word per
//  command and it desyncs forever, staying dead white - which is exactly what a
//  long probe session chased before the Linux driver revealed regwidth=16.
//
#pragma once

#include <circle/display.h>
#include <circle/spimasterdma.h>
#include <circle/gpiopin.h>
#include <circle/interrupt.h>
#include <circle/types.h>

class CILI9486DMADisplay : public CDisplay
{
public:
	static const unsigned None = GPIO_PINS;

	CILI9486DMADisplay (CInterruptSystem *pInterrupt,
			    unsigned nDCPin, unsigned nResetPin, unsigned nBackLightPin,
			    unsigned nCSPin,
			    unsigned nWidth, unsigned nHeight,
			    unsigned nClockSpeed, unsigned CPOL, unsigned CPHA,
			    u8 uchMADCTL, boolean bSwapColorBytes);

	~CILI9486DMADisplay (void);

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

	/// \return Is a frame still going out?
	boolean IsBusy (void) const	{ return m_bTransferActive; }

	// For interface parity with CST7789DMADisplay, so the kernel can treat the
	// two panels the same. Unlike the ST7789 path this runs at a fixed rate -
	// the ILI9486 has plenty of headroom (Linux drives it at 133 MHz) and no
	// picture-rate-from-config.txt story - so there is no dynamic clock to hold:
	// TargetClock() is that fixed rate and SetTargetClock() does nothing.
	static unsigned TargetClock (void)	{ return 48000000; }
	void SetTargetClock (unsigned nTargetHz) { (void) nTargetHz; }

private:
	// 16-bit register interface: each command/parameter is one 16-bit word.
	void Command (u8 uchByte);
	void Data (u8 uchByte);
	// Raw byte stream (pixel data, DC high), hand CS around it.
	void WriteBytes (const void *pData, size_t nLength, boolean bIsData);
	void SetWindow (unsigned x0, unsigned y0, unsigned x1, unsigned y1);
	void ClearPanel (void);

	void StartNextChunk (void);
	static void DMACompletionStub (boolean bStatus, void *pParam);

	static const unsigned MaxChunkBytes = 0xF000;	// 61440

	unsigned m_nDCPin, m_nResetPin, m_nBackLightPin, m_nCSPin;
	unsigned m_nWidth, m_nHeight;
	unsigned m_nClockSpeed, m_CPOL, m_CPHA;
	u8	 m_uchMADCTL;

	CSPIMasterDMA m_SPIMaster;
	CGPIOPin      m_DCPin;
	CGPIOPin      m_ResetPin;
	CGPIOPin      m_BackLightPin;
	CGPIOPin      m_CSPin;

	u8 *m_pFrameBuffer;
	u8 *m_pDummyRXBuffer;

	volatile boolean m_bTransferActive;
	unsigned m_nBytesTotal;
	unsigned m_nBytesSent;

	TAreaCompletionRoutine *m_pCompletionRoutine;
	void *m_pCompletionParam;
};
