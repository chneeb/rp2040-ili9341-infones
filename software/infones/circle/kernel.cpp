//
//  kernel.cpp
//
#include "kernel.h"
#include "GamePi20.h"
#include "RomMenu.h"
#include <circle/2dgraphics.h>
#include <circle/util.h>
#include <circle/machineinfo.h>
#include <assert.h>

// The emulator's own header. Kept to this one place, and to
// InfoNES_System_Circle.cpp, so Circle and InfoNES never meet in a header.
// Not extern "C": the core is C++, and its symbols are mangled accordingly.
#include "InfoNES.h"

#define DRIVE		"SD:"

// Where the menu looks for .nes files.
#define ROM_DIRECTORY	"SD:/nes"

// The panel's colour model is big endian RGB565 (ST7789_SWAP_COLOR_BYTES is
// TRUE, because the NES palette is stored that way), so colours written by
// hand have to be swapped to match.
#define RGB565(r, g, b)	((u16) __builtin_bswap16 ((u16) ((((r) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((b) & 0x1F))))

// InfoNES pad bits.
#define NES_PAD_A	0x01
#define NES_PAD_B	0x02
#define NES_PAD_SELECT	0x04
#define NES_PAD_START	0x08
#define NES_PAD_UP	0x10
#define NES_PAD_DOWN	0x20
#define NES_PAD_LEFT	0x40
#define NES_PAD_RIGHT	0x80

CKernel *CKernel::s_pThis = 0;

// Ask for core/DIVISOR rather than a fixed rate, so Circle's truncating divide
// lands on the divisor we actually want whatever the core clock reads at boot.


// The board's buttons, and what the NES sees them as.
static const struct
{
	unsigned nPin;
	unsigned nPadBit;
}
s_ButtonMap[] =
{
	{ GPIO_BUTTON_UP,	NES_PAD_UP	},
	{ GPIO_BUTTON_DOWN,	NES_PAD_DOWN	},
	{ GPIO_BUTTON_LEFT,	NES_PAD_LEFT	},
	{ GPIO_BUTTON_RIGHT,	NES_PAD_RIGHT	},
	// Crossed over on purpose: the board's silkscreen does not follow the
	// convention NES games expect, where A is the primary action and sits
	// under the thumb on the right. Swap these two lines back for a literal
	// A-to-A mapping.
	{ GPIO_BUTTON_A,	NES_PAD_B	},
	{ GPIO_BUTTON_B,	NES_PAD_A	},
	{ GPIO_BUTTON_SELECT,	NES_PAD_SELECT	},
	{ GPIO_BUTTON_START,	NES_PAD_START	},
	// Spare buttons, doubling for the two above so either pair can be used.
	// TL and TR are not here: they work the volume instead.
	{ GPIO_BUTTON_X,	NES_PAD_B	},
	{ GPIO_BUTTON_Y,	NES_PAD_A	}
};

CKernel::CKernel (void)
:	m_Timer (&m_Interrupt),
	m_Display (&m_Interrupt, ST7789_DC_PIN, ST7789_RESET_PIN, ST7789_BACKLIGHT_PIN,
		   ST7789_CS_PIN, ST7789_WIDTH, ST7789_HEIGHT,
		   CPanelDisplay::TargetClock (), ST7789_CPOL, ST7789_CPHA,
		   ST7789_MADCTL, ST7789_SWAP_COLOR_BYTES),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED)
#ifdef PANEL_MHS35
	, m_USBHCI (&m_Interrupt, &m_Timer, TRUE)	// TRUE: plug-and-play
#endif
{
	s_pThis = this;
	m_ActLED.Blink (5);

#ifdef PANEL_MHS35
	memset (&m_GamePadState, 0, sizeof m_GamePadState);
#endif
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	// Interrupts first: the display signals the end of a transfer from an
	// interrupt, and its own init already uses DMA.
	m_nCoreClockAtInit = CMachineInfo::Get ()->GetClockRate (CLOCK_ID_CORE);

	if (!m_Interrupt.Initialize ())	return FALSE;
	if (!m_Timer.Initialize ())	return FALSE;
	if (!m_Display.Initialize ())	return FALSE;
	if (!m_EMMC.Initialize ())	return FALSE;

#ifdef PANEL_MHS35
	if (!m_USBHCI.Initialize ())	return FALSE;
#else
	InitializeButtons ();
#endif

#if HDMI_OUTPUT
	// Optional: a second, independent output. Failure here (no monitor) is not
	// fatal - the SPI panel is the primary display.
	m_pHDMI = new CBcmFrameBuffer (HDMI_WIDTH, HDMI_HEIGHT, 16);
	if (m_pHDMI == 0 || !m_pHDMI->Initialize ())
	{
		delete m_pHDMI;
		m_pHDMI = 0;
	}
#endif

	return TRUE;
}

void CKernel::InitializeButtons (void)
{
	static_assert (sizeof s_ButtonMap / sizeof s_ButtonMap[0] == GPIOButtonCount,
		       "s_ButtonMap and GPIOButtonCount disagree");

	for (unsigned i = 0; i < GPIOButtonCount; i++)
	{
		m_ButtonPins[i].AssignPin (s_ButtonMap[i].nPin);
		m_ButtonPins[i].SetMode (GPIOModeInputPullUp);
	}

	m_VolumeDownPin.AssignPin (GPIO_BUTTON_TL);
	m_VolumeDownPin.SetMode (GPIOModeInputPullUp);
	m_VolumeUpPin.AssignPin (GPIO_BUTTON_TR);
	m_VolumeUpPin.SetMode (GPIOModeInputPullUp);
}

#ifdef PANEL_MHS35

// USB gamepad -> NES pad bits. The face buttons map straight across; X and Y
// double for B and A as they do on the GamePi20. The d-pad comes through
// whichever way the pad reports it (buttons, hat or axes), because
// NormalizeGamePadState has already folded hat and axes into the button bits.
unsigned CKernel::ReadPad (void)
{
	PollGamePad ();

	if (m_pGamePad == 0)
	{
		return 0;
	}

	u32 b = m_GamePadState.buttons;
	unsigned nPad = 0;

	// This pad is a "SimpleGamePad" (confirmed against circle-arcade, same
	// pad). Its buttons do NOT land on Circle's semantic GamePadButtonA/B/
	// Start/Select bits - Circle's GamePadButtonA is 0x200, which is actually
	// this pad's Start. The bit layout below was read straight off the
	// hardware with a menu readout:
	//
	//   X 0x01   A 0x02   B 0x04   Y 0x08   L 0x10   R 0x20
	//   Select 0x100   Start 0x200
	//
	// A -> jump, B -> run, as NES games expect. X/Y/L/R are left free for a
	// later use (turbo, save states). The d-pad is read by name because
	// NormalizeGamePadState folds the hat and analog axes into
	// GamePadButtonUp/Down/Left/Right.
	#define SIMPLE_PAD_A		0x0002
	#define SIMPLE_PAD_B		0x0004
	#define SIMPLE_PAD_L		0x0010
	#define SIMPLE_PAD_R		0x0020
	#define SIMPLE_PAD_SELECT	0x0100
	#define SIMPLE_PAD_START	0x0200

	if (b & SIMPLE_PAD_A)      nPad |= NES_PAD_A;
	if (b & SIMPLE_PAD_B)      nPad |= NES_PAD_B;
	if (b & SIMPLE_PAD_SELECT) nPad |= NES_PAD_SELECT;
	if (b & SIMPLE_PAD_START)  nPad |= NES_PAD_START;
	if (b & GamePadButtonUp)    nPad |= NES_PAD_UP;
	if (b & GamePadButtonDown)  nPad |= NES_PAD_DOWN;
	if (b & GamePadButtonLeft)  nPad |= NES_PAD_LEFT;
	if (b & GamePadButtonRight) nPad |= NES_PAD_RIGHT;

	// L/R shoulders work the volume, like the GamePi20's TL/TR: L down, R up,
	// both together mute/unmute. Not part of the NES pad.
	UpdateVolume ((b & SIMPLE_PAD_L) != 0, (b & SIMPLE_PAD_R) != 0);

	return nPad;
}

// Find and attach the pad, and keep it hot-pluggable. Called once a frame
// (InfoNES_PadState runs in vblank), so UpdatePlugAndPlay is cheap here.
//
// It must run every frame, connected or not: USB removal is only processed
// during UpdatePlugAndPlay, and that is what fires GamePadRemovedHandler (which
// clears m_pGamePad) on an unplug. While disconnected the poll scans for a pad
// each frame; on re-plug it re-registers the handlers.
void CKernel::PollGamePad (void)
{
	m_USBHCI.UpdatePlugAndPlay ();

	if (m_pGamePad != 0)
	{
		// Still attached; the status handler keeps m_GamePadState current, and
		// a removal above will have cleared m_pGamePad before we get here.
		return;
	}

	// Look for a pad. Scan a few slots rather than assume index 1: a pad
	// re-plugged during play can come back as upad2, upad3, ... if the old
	// name has not been freed yet.
	for (unsigned nIndex = 1; nIndex <= 4 && m_pGamePad == 0; nIndex++)
	{
		m_pGamePad = (CUSBGamePadDevice *)
			m_DeviceNameService.GetDevice ("upad", nIndex, FALSE);
	}

	if (m_pGamePad == 0)
	{
		return;
	}

	m_pGamePad->RegisterRemovedHandler (GamePadRemovedHandler);

	const TGamePadState *pState = m_pGamePad->GetInitialState ();
	if (pState != 0)
	{
		memcpy (&m_GamePadState, pState, sizeof m_GamePadState);
		NormalizeGamePadState (&m_GamePadState);
	}

	m_pGamePad->RegisterStatusHandler (GamePadStatusHandler);
}

void CKernel::GamePadStatusHandler (unsigned nDeviceIndex, const TGamePadState *pState)
{
	if (nDeviceIndex != 0 || s_pThis == 0 || pState == 0)
	{
		return;
	}

	memcpy (&s_pThis->m_GamePadState, pState, sizeof s_pThis->m_GamePadState);
	NormalizeGamePadState (&s_pThis->m_GamePadState);
}

void CKernel::GamePadRemovedHandler (CDevice *pDevice, void *pContext)
{
	(void) pDevice;
	(void) pContext;

	if (s_pThis != 0)
	{
		s_pThis->m_pGamePad = 0;
	}
}

// Cheap pads report the d-pad as a hat switch or an analog axis pair rather
// than digital buttons; fold either into the button bits so ReadPad can just
// read buttons. Lifted from circle-arcade, same pad.
void CKernel::NormalizeGamePadState (TGamePadState *pState)
{
	if (pState->nhats >= 1)
	{
		static const unsigned HatToButtons[8] =
		{
			GamePadButtonUp,
			GamePadButtonUp   | GamePadButtonRight,
			GamePadButtonRight,
			GamePadButtonDown | GamePadButtonRight,
			GamePadButtonDown,
			GamePadButtonDown | GamePadButtonLeft,
			GamePadButtonLeft,
			GamePadButtonUp   | GamePadButtonLeft
		};

		int nHat = pState->hats[0];
		if (0 <= nHat && nHat < 8)
		{
			pState->buttons |= HatToButtons[nHat];
		}
	}

	// The pad's d-pad-as-axes sit on axes 3 and 4 (as on circle-arcade's pad).
	if (pState->naxes > 3)
	{
		pState->buttons |= AxisToButtons (pState, 3,
						  GamePadButtonLeft, GamePadButtonRight);
	}
	if (pState->naxes > 4)
	{
		pState->buttons |= AxisToButtons (pState, 4,
						  GamePadButtonUp, GamePadButtonDown);
	}
}

unsigned CKernel::AxisToButtons (const TGamePadState *pState, unsigned nAxis,
				 unsigned nLowButton, unsigned nHighButton)
{
	int nMinimum = pState->axes[nAxis].minimum;
	int nMaximum = pState->axes[nAxis].maximum;
	int nValue   = pState->axes[nAxis].value;

	int nRange = nMaximum - nMinimum;
	if (nRange <= 0)
	{
		return 0;
	}

	if (nValue <= nMinimum + nRange / 4)
	{
		return nLowButton;
	}
	if (nValue >= nMaximum - nRange / 4)
	{
		return nHighButton;
	}

	return 0;
}

#else	// GamePi20 GPIO buttons

// The buttons pull their pin to ground, so LOW means pressed.
//
// The shoulder buttons are handled here too, on the press rather than while
// held: this runs once a frame, so a held button would otherwise run the
// volume from one end to the other in well under a second.
unsigned CKernel::ReadPad (void)
{
	unsigned nPad = 0;

	for (unsigned i = 0; i < GPIOButtonCount; i++)
	{
		if (m_ButtonPins[i].Read () == LOW)
		{
			nPad |= s_ButtonMap[i].nPadBit;
		}
	}

	UpdateVolume (m_VolumeDownPin.Read () == LOW, m_VolumeUpPin.Read () == LOW);

	return nPad;
}

#endif	// PANEL_MHS35

// TL turns the volume down, TR up, both together mute and unmute.
//
// A step is taken when a button is *released*, not when it is pressed. Pressing
// two buttons together never quite happens at the same moment, so acting on the
// press would step the volume for whichever one arrived first, every time the
// mute chord was used. Waiting for the release means the chord can be spotted
// first and the step suppressed.
// The volume state machine, shared by both input sources: the GamePi20 passes
// its TL/TR GPIO pins, the MHS35 passes its gamepad L/R buttons. Down lowers,
// up raises, both together toggle mute. Every action is taken on release, not
// press, so the two never-quite-simultaneous presses of the mute chord are
// recognised as a chord before either is mistaken for a step.
void CKernel::UpdateVolume (boolean bDown, boolean bUp)
{
	if (bDown && bUp)
	{
		if (!m_bVolumeChord)
		{
			m_bVolumeChord = TRUE;
			m_bMuted = !m_bMuted;
		}
	}
	else if (!bDown && !bUp)
	{
		if (m_bVolumeChord)
		{
			// Both let go after a mute: the presses have been used up.
			m_bVolumeChord = FALSE;
		}
		else if (m_bVolumeDownWasDown)
		{
			m_nVolume = m_nVolume > VOLUME_STEP ? m_nVolume - VOLUME_STEP : 0;
		}
		else if (m_bVolumeUpWasDown)
		{
			m_nVolume += VOLUME_STEP;
			if (m_nVolume > VOLUME_MAX)
			{
				m_nVolume = VOLUME_MAX;
			}
		}
	}

	m_bVolumeDownWasDown = bDown;
	m_bVolumeUpWasDown = bUp;
}

// Hand the picture to the panel. NES_OUT_WIDTH is 320 when the width is being
// filled and 256 when it is pillarboxed; either way it is centred. SetArea
// starts the transfer and returns, so the next frame is emulated while this one
// is still going out.
void CKernel::PresentFrame (const u16 *pFrame)
{
	CDisplay::TArea Area;
	Area.x1 = NES_OFFSET_X;
	Area.x2 = NES_OFFSET_X + NES_OUT_WIDTH - 1;
	Area.y1 = NES_OFFSET_Y;
	Area.y2 = NES_OFFSET_Y + NES_OUT_HEIGHT - 1;

	m_Display.SetArea (Area, pFrame);
}

#if HDMI_OUTPUT

// Scale the raw 256x240 NES frame to fill the HDMI framebuffer (nearest
// neighbour, tables worked out once) and write it. The GPU scans the buffer to
// HDMI on its own, so this is just a memory write - no bus contention with the
// SPI panel, and it runs every frame (HDMI has none of the SPI bandwidth wall).
//
// The NES palette is big-endian RGB565 (prepared for the RGB565_BE SPI panel);
// the framebuffer is native RGB565, so each pixel is byte-swapped. If HDMI
// colours come out wrong (e.g. red/blue swapped), it is this swap or the
// framebuffer's RGB-vs-BGR order - the first thing to try is dropping the swap.
void CKernel::PresentHDMI (const u16 *pSrc, unsigned srcW, unsigned srcH)
{
	if (m_pHDMI == 0 || srcW == 0 || srcH == 0)
	{
		return;
	}

	// The tables depend on the source size, which alternates between the
	// emulator frame (256x240) and the menu (panel size). Recompute only when
	// it changes, so the common case (same size frame after frame) is free.
	static unsigned s_Col[HDMI_WIDTH];
	static unsigned s_Row[HDMI_HEIGHT];
	static unsigned s_LastW = 0, s_LastH = 0;
	if (srcW != s_LastW || srcH != s_LastH)
	{
		for (unsigned x = 0; x < HDMI_WIDTH; x++)
		{
			s_Col[x] = x * srcW / HDMI_WIDTH;
		}
		for (unsigned y = 0; y < HDMI_HEIGHT; y++)
		{
			s_Row[y] = y * srcH / HDMI_HEIGHT;
		}
		s_LastW = srcW;
		s_LastH = srcH;
	}

	u8 *pBase = (u8 *) (uintptr) m_pHDMI->GetBuffer ();
	unsigned nPitch = m_pHDMI->GetPitch ();		// bytes per row (may be padded)

	for (unsigned y = 0; y < HDMI_HEIGHT; y++)
	{
		const u16 *pRow = &pSrc[s_Row[y] * srcW];
		u16 *pDst = (u16 *) (pBase + y * nPitch);

		for (unsigned x = 0; x < HDMI_WIDTH; x++)
		{
			pDst[x] = __builtin_bswap16 (pRow[s_Col[x]]);
		}
	}
}

#endif	// HDMI_OUTPUT

// NTSC NES runs at 60.0988 Hz, which is 16639 us a frame. The pico build uses
// 16666 (a flat 60 Hz); the difference is small but it is free to get right,
// and it is what decides whether music plays at the pitch it should.
//
// Only the starting value: the platform layer replaces it per ROM, since a PAL
// game wants 19997. See the region notes in InfoNES_System_Circle.cpp.
#define FRAME_PERIOD_US		16639

void CKernel::SetFramePeriod (unsigned nMicros)
{
	if (nMicros != 0)
	{
		m_nFramePeriodUs = nMicros;
	}
}

// Wait until the next frame is due.
//
// The deadline is carried forward, rather than set from the time this wait
// happened to end, so that the odd long frame does not push every later frame
// back with it. If a frame runs so long that the deadline has already gone by,
// the lost time is written off instead of being made up - catching up would
// mean sprinting through the following frames, which looks far worse than a
// single late one.
void CKernel::WaitForNextFrame (void)
{
	u64 nNow = CTimer::GetClockTicks64 ();

	if (m_nNextFrameTime == 0)
	{
		m_nNextFrameTime = nNow;
	}

	m_nNextFrameTime += m_nFramePeriodUs;

	if (nNow < m_nNextFrameTime)
	{
		while (CTimer::GetClockTicks64 () < m_nNextFrameTime)
		{
			// The frame is already on its way out over DMA while this spins.
		}
	}
	else
	{
		m_nNextFrameTime = nNow;
	}

	// Hold the bus at its target rate. The core clock moves on its own and
	// Circle's divisor does not follow it, so without this the panel ends up
	// overdriven whenever the core boosts. Once a second is often enough and
	// keeps the mailbox call out of the frame budget.
	if (m_nFramesThisSecond == 0)
	{
		m_Display.SetTargetClock (CPanelDisplay::TargetClock ());
	}

	// Count what is actually achieved. The pacing above should give 60, and if
	// it does not this is the number that says so.
	m_nFramesThisSecond++;

	nNow = CTimer::GetClockTicks64 ();
	if (m_nSecondStarted == 0)
	{
		m_nSecondStarted = nNow;
	}
	else if (nNow - m_nSecondStarted >= 1000000)
	{
		// Rate over the window that actually elapsed, not a count of frames in
		// it. The window ends on the first frame past a second, so it runs a
		// frame long - counting would read one high and make a correct 60.1 Hz
		// look like 61.
		u64 nElapsed = nNow - m_nSecondStarted;

		m_nMeasuredFPS = (unsigned) ((u64) m_nFramesThisSecond * 1000000 / nElapsed);
		m_nFramesThisSecond = 0;
		m_nSecondStarted = nNow;
	}
}

//
// Sound. PWM on GPIO 18, which on this board feeds both the speaker and the
// earphone jack - see configure-gamepi20.sh for the three options that put it
// there rather than on GPIO 12 and 13, which are the Up and Right buttons.
//
// The APU produces 8 bit unsigned mono, which Circle takes directly as
// SoundFormatUnsigned8, so nothing has to be converted.
//

// Enough queued audio to ride out a frame that runs long, without adding so
// much delay that the sound drifts noticeably behind the picture.
#define SOUND_QUEUE_MSECS	100

int CKernel::SoundOpen (int nSampleRate)
{
#if !SOUND_ENABLED
	return -1;
#endif

	// Already open at this rate - nothing to do. At a different rate it has to
	// be rebuilt: CPWMSoundBaseDevice takes its rate at construction, and going
	// from an NTSC game to a PAL one changes it.
	if (m_pSound != 0)
	{
		if ((int) m_nSoundRate == nSampleRate)
		{
			return 0;
		}

		SoundClose ();
	}

	m_pSound = new CPWMSoundBaseDevice (&m_Interrupt, nSampleRate);
	if (m_pSound == 0)
	{
		return -1;
	}

	if (!m_pSound->AllocateQueue (SOUND_QUEUE_MSECS))
	{
		delete m_pSound;
		m_pSound = 0;

		return -1;
	}

#ifdef PANEL_MHS35
	// The Pi 3B jack is stereo; the NES audio is mono. Two channels here, with
	// SoundWrite duplicating each sample to both, so both sides play. The
	// GamePi20 wires one PWM pin only, so it stays mono.
	m_pSound->SetWriteFormat (SoundFormatUnsigned8, 2);
#else
	m_pSound->SetWriteFormat (SoundFormatUnsigned8, 1);
#endif

	if (!m_pSound->Start ())
	{
		delete m_pSound;
		m_pSound = 0;

		return -1;
	}

	// Only once it is actually running, so a failed open cannot leave a rate
	// recorded that no device is using.
	m_nSoundRate = nSampleRate;

	return 0;
}

// Cancel() only *requests* the stop - the header says it "takes effect after a
// short delay" - and ~CPWMSoundBaseDevice does not wait for it. Deleting
// straight after the Cancel tears the DMA buffers down underneath a transfer
// that is still running, which hangs the machine.
//
// This went unnoticed for a long time because nothing called SoundClose()
// during play; it was only reached when switching between an NTSC and a PAL
// ROM, which is the one path that has to reopen the device.
void CKernel::SoundClose (void)
{
	if (m_pSound == 0)
	{
		return;
	}

	m_pSound->Cancel ();

	// A frame's worth is ample - the DMA gives up at the end of the block it
	// is in. Bounded so that a device which never goes idle costs a moment's
	// silence rather than the whole machine.
	unsigned nWaited = 0;
	while (m_pSound->IsActive () && nWaited < 100000)
	{
		CTimer::SimpleusDelay (100);
		nWaited += 100;
	}

	delete m_pSound;
	m_pSound = 0;
	m_nSoundRate = 0;
}

// Whatever does not fit is dropped. Waiting for room would stall the frame the
// emulator is in the middle of, and a dropped sample is far less noticeable
// than a late frame.
int CKernel::SoundWrite (const unsigned char *pSamples, int nCount)
{
	if (m_pSound == 0)
	{
		return nCount;
	}

#ifdef PANEL_MHS35
	// Mono -> stereo: duplicate each 8-bit sample into an interleaved L/R pair
	// so both sides of the jack play. One queue frame is one stereo pair, i.e.
	// one mono sample, so the buffer-room accounting is unchanged.
	static unsigned char Stereo[2 * 1024];
	int nDone = 0;
	while (nDone < nCount)
	{
		int nChunk = nCount - nDone;
		if (nChunk > 1024)
		{
			nChunk = 1024;
		}
		for (int i = 0; i < nChunk; i++)
		{
			Stereo[2 * i]     = pSamples[nDone + i];
			Stereo[2 * i + 1] = pSamples[nDone + i];
		}
		m_pSound->Write (Stereo, nChunk * 2);
		nDone += nChunk;
	}
	return nCount;
#else
	return m_pSound->Write (pSamples, nCount);
#endif
}

// Room left to write into, which is what the APU means by "buffer size": it
// clamps the number of samples it generates to this
// (InfoNES_pAPUHsync -> std::min(bufferLeft, n)).
//
// Not GetQueueFramesAvail(): despite the name that is the number of frames
// already queued and waiting to be sent. Returning it deadlocks the emulator
// into silence - an empty queue reads as no room, so the APU generates
// nothing, so the queue stays empty.
int CKernel::SoundBufferAvail (void)
{
	if (m_pSound == 0)
	{
		return 0;
	}

	return m_pSound->GetQueueSizeFrames () - m_pSound->GetQueueFramesAvail ();
}

// Up is read once, straight after the pull-ups are enabled. Nothing is written
// anywhere and no flag survives a boot, so this cannot leave the device stuck
// in a mode it will not come out of: power cycle without holding Up and it is
// an emulator again.
boolean CKernel::UpHeldAtBoot (void)
{
	// Let the pull-up settle before believing the pin.
	m_Timer.MsDelay (10);

	for (unsigned i = 0; i < GPIOButtonCount; i++)
	{
		if (   s_ButtonMap[i].nPin == GPIO_BUTTON_UP
		    && m_ButtonPins[i].Read () == LOW)
		{
			return TRUE;
		}
	}

	return FALSE;
}

// Present the SD card to a PC as a mass storage device.
//
// The file system is never mounted in this mode, so there is no question of
// both sides holding the same FAT. There is no frame deadline either, which
// matters because CUSBMSDGadget::Update() does the actual block I/O rather
// than just pumping state - it cannot be called from a 16 ms game loop.
TShutdownMode CKernel::RunUSBGadget (void)
{
	C2DGraphics Graphics (&m_Display);
	if (Graphics.Initialize ())
	{
		Graphics.ClearScreen (COLOR2D (0, 0, 40));
		Graphics.DrawText (m_Display.GetWidth () / 2, 90, COLOR2D (140, 190, 255),
				   "USB transfer mode", C2DGraphics::AlignCenter);
		Graphics.DrawText (m_Display.GetWidth () / 2, 120, COLOR2D (200, 200, 200),
				   "SD card is available to the PC", C2DGraphics::AlignCenter);
		Graphics.DrawText (m_Display.GetWidth () / 2, 150, COLOR2D (200, 200, 200),
				   "Eject there, then press START", C2DGraphics::AlignCenter);
		Graphics.UpdateDisplay ();

		// The frame goes out over DMA asynchronously. Let it finish before
		// touching USB: halting or reconfiguring mid-transfer leaves half a
		// screen, which is what a failed gadget start looked like.
		m_Display.WaitForTransfer ();
	}

	// Heap allocated and never freed on purpose: ~CUSBMSDGadget() is an
	// assert(0), so the gadget must outlive every path out of here. Leaking it
	// costs nothing, as the only way out is a reboot.
	CUSBMSDGadget *pGadget = new CUSBMSDGadget (&m_Interrupt, &m_EMMC,
						    USB_GADGET_VID, USB_GADGET_PID);
	if (pGadget == 0 || !pGadget->Initialize ())
	{
		if (Graphics.Initialize ())
		{
			Graphics.DrawText (m_Display.GetWidth () / 2, 190, COLOR2D (255, 120, 120),
					   "USB gadget failed to start",
					   C2DGraphics::AlignCenter);
			Graphics.UpdateDisplay ();
			m_Display.WaitForTransfer ();
		}

		for (;;)
		{
			m_ActLED.Blink (1);
			m_Timer.MsDelay (1000);
		}
	}

	for (;;)
	{
		pGadget->UpdatePlugAndPlay ();
		pGadget->Update ();

		if (ReadPad () & NES_PAD_START)
		{
			// Reboot rather than carry on: the emulator would have to mount
			// a card the PC may have just rewritten underneath it.
			return ShutdownReboot;
		}
	}

	return ShutdownHalt;
}

TShutdownMode CKernel::Run (void)
{
#ifndef PANEL_MHS35
	// USB mass-storage transfer mode. Not on the MHS35/Pi 3B: its USB is
	// host-only (behind the LAN9514 hub), so it cannot be a gadget at all, and
	// the Up pin is not a button there - it is the gamepad's now.
	if (UpHeldAtBoot ())
	{
		return RunUSBGadget ();
	}
#endif

#if ST7789_TEST_PATTERN
	DrawTestPattern ();

	for (;;)
	{
		m_ActLED.Blink (1);
		m_Timer.MsDelay (1000);
	}
#else
	if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
	{
		return ShutdownHalt;
	}

	m_pMenu = new CRomMenu (&m_Display);
	if (m_pMenu == 0 || !m_pMenu->Initialize ())
	{
		return ShutdownHalt;
	}

	m_pMenu->Scan (ROM_DIRECTORY);

	// InfoNES_Main() owns the loop from here: it calls InfoNES_Menu(), which
	// comes back through ChooseRom() below, then runs the game until the quit
	// chord is pressed, then asks for a ROM again.
	InfoNES_Main ();

	f_mount (0, DRIVE, 0);
#endif

	return ShutdownHalt;
}

#if ST7789_TEST_PATTERN

// A static pattern, drawn into the 256x240 area the emulator will use. What it
// tells you:
//
//   nothing at all       -> wiring, chip select or the backlight pin
//   wrong bar colours    -> ST7789_SWAP_COLOR_BYTES
//   markers wrong corner -> orientation, adjust ST7789_MADCTL
//   bars not centred     -> the pillarbox offsets
//
void CKernel::DrawTestPattern (void)
{
	static u16 Frame[NES_OUT_WIDTH * NES_HEIGHT];

	static const u16 Bars[4] =
	{
		RGB565 (31,  0,  0),		// red
		RGB565 ( 0, 63,  0),		// green
		RGB565 ( 0,  0, 31),		// blue
		RGB565 (31, 63, 31)		// white
	};

	for (unsigned y = 0; y < NES_HEIGHT; y++)
	{
		for (unsigned x = 0; x < NES_OUT_WIDTH; x++)
		{
			Frame[y * NES_OUT_WIDTH + x] = Bars[x / (NES_OUT_WIDTH / 4)];
		}
	}

	// A three pixel border, to check that the whole 256x240 area lands inside
	// the panel and that the pillarbox offsets are right: 32 px of black should
	// remain either side.
	const u16 Yellow = RGB565 (31, 63, 0);
	for (unsigned i = 0; i < 3; i++)
	{
		for (unsigned x = 0; x < NES_OUT_WIDTH; x++)
		{
			Frame[i * NES_OUT_WIDTH + x] = Yellow;
			Frame[(NES_HEIGHT - 1 - i) * NES_OUT_WIDTH + x] = Yellow;
		}
		for (unsigned y = 0; y < NES_HEIGHT; y++)
		{
			Frame[y * NES_OUT_WIDTH + i] = Yellow;
			Frame[y * NES_OUT_WIDTH + (NES_OUT_WIDTH - 1 - i)] = Yellow;
		}
	}

	// Corner markers: yellow top left, magenta top right.
	const u16 Magenta = RGB565 (31, 0, 31);
	for (unsigned y = 0; y < 20; y++)
	{
		for (unsigned x = 0; x < 20; x++)
		{
			Frame[y * NES_OUT_WIDTH + x] = Yellow;
		}
		for (unsigned x = NES_OUT_WIDTH - 10; x < NES_OUT_WIDTH; x++)
		{
			Frame[y * NES_OUT_WIDTH + x] = Magenta;
		}
	}

	PresentFrame (Frame);
	m_Display.WaitForTransfer ();
}

#endif

//
// The bridge declared in GamePi20.h.
//

void GamePi20_PresentFrame (const unsigned short *pFrame)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->PresentFrame ((const u16 *) pFrame);
}

void GamePi20_PresentHDMI (const unsigned short *pSrc, unsigned nWidth, unsigned nHeight)
{
#if HDMI_OUTPUT
	CKernel *pKernel = CKernel::Get ();
	if (pKernel != 0)
	{
		pKernel->PresentHDMI ((const u16 *) pSrc, nWidth, nHeight);
	}
#else
	(void) pSrc; (void) nWidth; (void) nHeight;
#endif
}

unsigned GamePi20_ReadPad (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->ReadPad ();
}

int GamePi20_DisplayBusy (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->DisplayBusy () ? 1 : 0;
}

void GamePi20_SetFramePeriod (unsigned nMicros)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->SetFramePeriod (nMicros);
}

void GamePi20_WaitForNextFrame (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->WaitForNextFrame ();
}

int GamePi20_SoundOpen (int nSampleRate)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->SoundOpen (nSampleRate);
}

void GamePi20_SoundClose (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	pKernel->SoundClose ();
}

int GamePi20_SoundWrite (const unsigned char *pSamples, int nCount)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->SoundWrite (pSamples, nCount);
}

const char *CKernel::ChooseRom (void)
{
	if (m_pMenu == 0)
	{
		return nullptr;
	}

	// Freeze what the game managed, before the menu's own paced loop overwrites
	// the running measurement.
	m_nLastGameFPS = m_nMeasuredFPS;

	return m_pMenu->Run (GamePi20_ReadPad, GamePi20_WaitForNextFrame);
}

const char *GamePi20_ChooseRom (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->ChooseRom ();
}

unsigned GamePi20_GetVolume (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->GetVolume ();
}

unsigned GamePi20_GetMeasuredFPS (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->GetMeasuredFPS ();
}

unsigned GamePi20_GetCoreClockAtInit (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->GetCoreClockAtInit ();
}

unsigned GamePi20_GetVolumeLevel (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->GetVolumeLevel ();
}

int GamePi20_IsMuted (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->IsMuted () ? 1 : 0;
}

int GamePi20_SoundBufferAvail (void)
{
	CKernel *pKernel = CKernel::Get ();
	assert (pKernel != 0);

	return pKernel->SoundBufferAvail ();
}

