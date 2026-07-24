//
//  InputConfig.h
//
//  The Waveshare GamePi20's buttons, read straight off the GPIO pins and
//  handed to InfoNES through InfoNES_PadState().
//
//  Pin numbers are SoC (BCM) numbers, not header positions. All buttons are
//  active low and are read with the internal pull-ups enabled. No debounce is
//  needed: sampling once per frame is far slower than contact bounce.
//
#pragma once

#define GPIO_BUTTON_UP		12
#define GPIO_BUTTON_DOWN	20
#define GPIO_BUTTON_LEFT	21
#define GPIO_BUTTON_RIGHT	13

#define GPIO_BUTTON_A		23
#define GPIO_BUTTON_B		4
#define GPIO_BUTTON_X		22
#define GPIO_BUTTON_Y		17

// The shoulder buttons work the volume rather than the pad: TL down, TR up,
// both together to mute and unmute.
#define GPIO_BUTTON_TL		5
#define GPIO_BUTTON_TR		6

// Set to 0 to leave the sound device unopened altogether. InfoNES_SoundOpen()
// then fails, APU_Mute stays set, and the APU does no work at all - so this
// also frees the CPU time the mixing took, and leaves GPIO 18 undriven.
//
// Off for the MHS35/Pi 3B bring-up: that board's PWM audio routing differs from
// the Zero's (and this rig has no speaker), so sound is a later step.
#ifdef PANEL_MHS35
#define SOUND_ENABLED		0
#else
#define SOUND_ENABLED		1
#endif

// USB vendor and product ID for the mass storage gadget.
//
// Circle ships USB_GADGET_VENDOR_ID as 0x0000 on purpose - it is not a valid
// ID and CDWUSBGadget::Initialize() rejects it outright, so a gadget with the
// default simply never starts. One has to be supplied.
//
// 0x1209 is pid.codes, a registered VID that hands out PIDs for open source
// and hobby projects precisely so they do not have to squat on someone else's.
// 0x0001 is its reserved test PID.
#define USB_GADGET_VID		0x1209
#define USB_GADGET_PID		0x0001

// Volume as a percentage. 50 is unity - the plain average of the five APU
// channels that the reference ports use. 100 is twice that and will clip the
// loudest passages. A single NES channel only ever reaches a fifth of full
// scale, which is why unity is quiet to begin with.
//
// The default is well below unity because this board drives a small speaker
// straight off the PWM pin, where unity is louder than it needs to be. TL and
// TR step from here, so starting low costs a couple of presses at most - and
// starting too loud is the mistake that cannot be taken back.
#define VOLUME_DEFAULT		15
#define VOLUME_STEP		5
#define VOLUME_MAX		100

#define GPIO_BUTTON_SELECT	16
#define GPIO_BUTTON_START	26
