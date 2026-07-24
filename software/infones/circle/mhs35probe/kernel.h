//
//  kernel.h
//
//  MHS35 probe - stage 2. The panel is proven (16-bit register writes, RGB565).
//  This stage drives it through the REAL port driver CILI9486DMADisplay over
//  DMA, to verify the DMA + hand-CS + 16-bit combination the emulator will use,
//  before folding it into the kernel proper. See README.md.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/types.h>

#include "../ILI9486DMADisplay.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	TShutdownMode Run (void);

private:
	void Blink (unsigned nTimes);

	CActLED		m_ActLED;
	CInterruptSystem m_Interrupt;
	CTimer		m_Timer;
	CILI9486DMADisplay m_Display;
};

#endif
