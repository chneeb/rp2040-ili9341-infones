//
//  kernel.h
//
//  InfoNES on the Waveshare GamePi20, bare metal via Circle.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/types.h>

#include "DisplayConfig.h"
#include "InputConfig.h"

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
#if ST7789_TEST_PATTERN
	void DrawTestPattern (void);
#endif

	// The picture handed to the panel. The NES renders 256x240 into this and
	// it goes out pillarboxed, so the panel's own 320 width never has to be
	// scaled to.
	void PresentFrame (void);

	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CST7789DMADisplay	m_Display;

	u16 m_Frame[NES_WIDTH * NES_HEIGHT];
};

#endif
