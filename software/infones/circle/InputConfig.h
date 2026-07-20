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

#define GPIO_BUTTON_TL		5
#define GPIO_BUTTON_TR		6

#define GPIO_BUTTON_SELECT	16
#define GPIO_BUTTON_START	26
