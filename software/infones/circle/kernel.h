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
#ifdef PANEL_MHS35
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbgamepad.h>
#endif
#include <SDCard/emmc.h>
#include <fatfs/ff.h>

class CRomMenu;

#include "DisplayConfig.h"
#if HDMI_OUTPUT
#include <circle/bcmframebuffer.h>
#include <circle/sound/hdmisoundbasedevice.h>
#endif
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
#if HDMI_OUTPUT
	// Scale an RGB565 (big-endian) source of srcW x srcH into the HDMI
	// framebuffer, if present. Used for both the emulator frame (256x240) and
	// the menu's own buffer (panel size), so the whole UI reaches HDMI.
	void PresentHDMI (const u16 *pSrc, unsigned srcW, unsigned srcH);
#endif
	boolean DisplayBusy (void) const	{ return m_Display.IsBusy (); }
	// Should the SPI panel be fed at all? False only when HDMI is connected at
	// boot and the board opted to go dark then (TFT_OFF_WHEN_HDMI). The game and
	// menu skip both the scale and the transfer when this is false.
	boolean PanelActive (void) const	{ return m_bPanelActive; }
#if HDMI_OUTPUT
	// True when a monitor was found at boot, i.e. the device is docked. Drives
	// the audio routing (HDMI vs the PWM jack).
	boolean HdmiActive (void) const		{ return m_pHDMI != 0; }
#endif
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
#if HDMI_OUTPUT
	// Open the HDMI sound device instead of the PWM one, for docked play. Used
	// only for NTSC, whose 22050 doubles exactly to HDMI's 44100.
	int SoundOpenHDMI (int nSampleRate);
#endif
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
	void UpdateVolume (boolean bDown, boolean bUp);

#ifdef PANEL_MHS35
	// USB gamepad input, used instead of GPIO buttons on the MHS35/Pi 3B. The
	// discovery and normalisation are lifted from the circle-arcade project,
	// which uses the same pad.
	void PollGamePad (void);
	static void GamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pState);
	static void GamePadRemovedHandler (CDevice *pDevice, void *pContext);
	static void NormalizeGamePadState (TGamePadState *pState);
	static unsigned AxisToButtons (const TGamePadState *pState, unsigned nAxis,
				       unsigned nLowButton, unsigned nHighButton);
#endif

	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CPanelDisplay		m_Display;
#if HDMI_OUTPUT
	CBcmFrameBuffer *	m_pHDMI = 0;	// 0 if no HDMI display was found
#endif
	// Cleared once at boot when HDMI is present and the board goes HDMI-only.
	boolean			m_bPanelActive = TRUE;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;

#ifdef PANEL_MHS35
	CUSBHCIDevice		m_USBHCI;
	CUSBGamePadDevice * volatile m_pGamePad = 0;
	TGamePadState		m_GamePadState;
#endif

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

	// Created once the emulator says what rate it wants. A base-class pointer so
	// it can be either the PWM device (handheld) or the HDMI device (docked).
	CSoundBaseDevice *m_pSound = 0;

	// True when m_pSound is the HDMI device: it runs at 44100 and takes each
	// 22050 NES sample twice (2:1) as a stereo pair. Changes the SoundWrite
	// duplication and the buffer-room accounting.
	boolean m_bHDMIAudio = FALSE;

	// Lives for the whole session: InfoNES_Main() comes back to the menu
	// every time a game is quit.
	CRomMenu *m_pMenu = 0;

	static CKernel *s_pThis;
};

#endif
