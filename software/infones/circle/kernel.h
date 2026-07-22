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
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/usb/gadget/usbmsdgadget.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

class CRomMenu;

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
	boolean DisplayBusy (void) const	{ return m_Display.IsBusy (); }
	unsigned ReadPad (void);
	void WaitForNextFrame (void);

	/// \brief How long one frame should last, in microseconds. Set per ROM from
	///        its region - 16639 NTSC, 19997 PAL. Zero is ignored.
	void SetFramePeriod (unsigned nMicros);
	const char *ChooseRom (void);
	// The core clock as it was when the SPI divisor was fixed. Circle sets
	// that divisor once and never revisits it, so this is what the current
	// bus rate has to be worked out against.
	unsigned GetCoreClockAtInit (void) const	{ return m_nCoreClockAtInit; }
	// Frames per second actually achieved, measured over the last second of
	// play. 0 until a second has been measured.
	// Snapshotted when the menu opens, so it reports the game just played
	// rather than the menu's own loop, which is paced by the same code.
	unsigned GetMeasuredFPS (void) const	{ return m_nLastGameFPS; }
	int SoundOpen (int nSampleRate);
	void SoundClose (void);
	int SoundWrite (const unsigned char *pSamples, int nCount);
	int SoundBufferAvail (void);
	// 0 while muted, so nothing downstream has to know about the mute.
	unsigned GetVolume (void) const	{ return m_bMuted ? 0 : m_nVolume; }
	// The level as set, whether or not it is currently muted.
	unsigned GetVolumeLevel (void) const	{ return m_nVolume; }
	boolean IsMuted (void) const		{ return m_bMuted; }

	static CKernel *Get (void)	{ return s_pThis; }

private:
#if ST7789_TEST_PATTERN
	void DrawTestPattern (void);
#endif
	void InitializeButtons (void);
	// Held at power-on, Up puts the device into USB mass storage mode instead
	// of running the emulator, so the card can be read from a PC.
	boolean UpHeldAtBoot (void);
	TShutdownMode RunUSBGadget (void);
	void UpdateVolume (void);

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

	static const unsigned GPIOButtonCount = 10;
	CGPIOPin		m_ButtonPins[GPIOButtonCount];

	// The shoulder buttons work the volume rather than the pad.
	CGPIOPin		m_VolumeDownPin;
	CGPIOPin		m_VolumeUpPin;
	boolean			m_bVolumeDownWasDown = FALSE;
	boolean			m_bVolumeUpWasDown = FALSE;
	boolean			m_bVolumeChord = FALSE;
	boolean			m_bMuted = FALSE;
	unsigned		m_nVolume = VOLUME_DEFAULT;

	// NTSC until a ROM says otherwise. FRAME_PERIOD_US in kernel.cpp.
	unsigned		m_nFramePeriodUs = 16639;

	// What the sound device was opened at, so a game with a different region
	// can be spotted and the device rebuilt.
	unsigned		m_nSoundRate = 0;

	// Frame pacing. The deadline is carried forward rather than taken from the
	// time the last wait ended, so a frame that runs long does not push every
	// later frame back with it.
	u64 m_nNextFrameTime = 0;
	unsigned m_nCoreClockAtInit = 0;
	u64 m_nSecondStarted = 0;
	unsigned m_nFramesThisSecond = 0;
	unsigned m_nMeasuredFPS = 0;
	unsigned m_nLastGameFPS = 0;

	// Created once the emulator says what rate it wants.
	CPWMSoundBaseDevice *m_pSound = 0;

	// Lives for the whole session: InfoNES_Main() comes back to the menu
	// every time a game is quit.
	CRomMenu *m_pMenu = 0;

	static CKernel *s_pThis;
};

#endif
