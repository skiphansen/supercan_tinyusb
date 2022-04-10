/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2022 Jean Gressmann <jean@0x42.de>
 *
 */

#pragma once


#include <sam.h>

#if SUPERDFU_APP
	#include <dfu_ram.h>
	#include <dfu_app.h>
	#include <dfu_usb_descriptors.h>
	#include <mcu.h>
	#define DFU_USB_RESET_TIMEOUT_MS 1000
#endif



#define SLLIN_BOARD_USB_BCD_DEVICE (1 << 8)
#define SLLIN_BOARD_USB_MANUFACTURER_STRING "2guys"
#define SLLIN_BOARD_LIN_COUNT 2
#define SLLIN_BOARD_NAME "D5035-02"

enum {
	SLLIN_BOARD_DEBUG_DEFAULT,
	LED_DEBUG_0,
	LED_DEBUG_1,
	LED_DEBUG_2,
	LED_DEBUG_3,
	LED_LIN0_STATUS_GREEN,
	LED_LIN0_STATUS_RED,
	LED_LIN1_STATUS_GREEN,
	LED_LIN1_STATUS_RED,
	SLLIN_BOARD_LED_COUNT
};



