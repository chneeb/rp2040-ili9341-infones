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
	unsigned ReadPad (void);
	void WaitForNextFrame (void);
	const char *ChooseRom (void);
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

	// Frame pacing. The deadline is carried forward rather than taken from the
	// time the last wait ended, so a frame that runs long does not push every
	// later frame back with it.
	u64 m_nNextFrameTime = 0;

	// Created once the emulator says what rate it wants.
	CPWMSoundBaseDevice *m_pSound = 0;

	// Lives for the whole session: InfoNES_Main() comes back to the menu
	// every time a game is quit.
	CRomMenu *m_pMenu = 0;

	static CKernel *s_pThis;
};

#endif
