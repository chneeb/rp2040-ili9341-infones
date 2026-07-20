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
#include <circle/gpiopin.h>
#include <circle/types.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

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

	// Reached from the InfoNES platform layer through GamePi20.h.
	void PresentFrame (const u16 *pFrame);
	unsigned ReadPad (void);

	static CKernel *Get (void)	{ return s_pThis; }

private:
#if ST7789_TEST_PATTERN
	void DrawTestPattern (void);
#endif
	void InitializeButtons (void);

	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CST7789DMADisplay	m_Display;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;

	static const unsigned GPIOButtonCount = 12;
	CGPIOPin		m_ButtonPins[GPIOButtonCount];

	static CKernel *s_pThis;
};

#endif
