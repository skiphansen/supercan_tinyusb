/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2022 Jean Gressmann <jean@0x42.de>
 *
 */

#pragma once


#include <sam.h>



#define SLLIN_BOARD_USB_BCD_DEVICE (1 << 8)
#define SLLIN_BOARD_USB_MANUFACTURER_STRING "Adafruit"
#define SLLIN_BOARD_LIN_COUNT 1
#define SLLIN_BOARD_NAME "Trinket M0"

enum {
	SLLIN_BOARD_DEBUG_DEFAULT,
	SLLIN_BOARD_DOTSTAR_RED,
	SLLIN_BOARD_DOTSTAR_GREEN,
	SLLIN_BOARD_DOTSTAR_BLUE,
	SLLIN_BOARD_LED_COUNT
};

