/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2022 Jean Gressmann <jean@0x42.de>
 *
 */

#pragma once


#include <sam.h>





enum {
	SLAVE_PROTO_STEP_RX_BREAK = 0,
	SLAVE_PROTO_STEP_RX_SYNC,
	SLAVE_PROTO_STEP_RX_PID,
	SLAVE_PROTO_STEP_TX_DATA,
	SLAVE_PROTO_STEP_RX_DATA,

	MASTER_PROTO_STEP_TX_BREAK = 0,
	MASTER_PROTO_STEP_TX_SYNC,
	MASTER_PROTO_STEP_FINISHED,

	MASTER_PROTO_TX_BREAK_ONLY_PID = 0xff, // any non-valid PID will do

	TIMER_TYPE_SLEEP = 0,
	TIMER_TYPE_BREAK,
	TIMER_TYPE_HIGH,
	TIMER_TYPE_SOF,
	TIMER_TYPE_DATA,
};


SLLIN_RAMFUNC void sam_lin_usart_int(uint8_t index);
SLLIN_RAMFUNC void sam_lin_timer_int(uint8_t index);



struct slave {
	sllin_queue_element elem;
	uint8_t slave_frame_enabled[64];
	uint32_t sleep_timeout_us;
	uint32_t sleep_elapsed_us;
	uint16_t data_timeout_us;
	uint8_t slave_proto_step;
	uint8_t slave_tx_offset;
	uint8_t slave_rx_offset;
#if SLLIN_DEBUG
	uint8_t rx_byte;
#endif
};

struct master {
	uint16_t break_timeout_us;
	uint16_t high_timeout_us;
	uint8_t busy;
	uint8_t proto_step;
	uint8_t pid;
};

struct sam_lin {
	Sercom* const sercom;
	Tc* const timer;
	struct slave slave;
	struct master master;
	IRQn_Type const timer_irq;
	uint16_t sof_timeout_us;
	uint16_t baud;
	uint8_t const rx_port_pin_mux;            // (GROUP << 5) | PIN
	uint8_t const master_slave_port_pin_mux;  // set for master, clear for slave
	uint8_t const led_status_green;
	uint8_t const led_status_red;
	uint8_t bus_state;
	uint8_t bus_error;
	uint8_t timer_type;
};

extern struct sam_lin sam_lins[SLLIN_BOARD_LIN_COUNT];

uint32_t sam_init_device_identifier(uint32_t const serial_number[4]);
void sam_lin_init_once(void);


#define sam_timer_cleanup_begin(lin) do { (lin)->timer->COUNT16.CTRLBSET.bit.CMD = TC_CTRLBSET_CMD_STOP_Val; } while (0)

SLLIN_RAMFUNC static inline void sam_timer_cleanup_end(struct sam_lin * const lin)
{
	// wait for sync
	sam_timer_sync_wait(lin->timer);
	// reset value
	lin->timer->COUNT16.COUNT.reg = 0;
	// clear interrupt flags
	lin->timer->COUNT16.INTFLAG.reg = ~0;
	// if there is an interrupt pending, clear it
	NVIC_ClearPendingIRQ(lin->timer_irq);
}



/* According to DS60001507E-page 1717 it should
 * suffice to write the re-trigger command. This
 * _does_ work if there is a pause after the write
 * during which the timer isn't manipulated.
 * It does _not_ work for data byte timeouts or
 * wake up timeouts (basically any case in which the command
 * is repeatedly given).
 *
 * Thus here is a solution that appears to work.
 */
#define sam_timer_start_or_restart_begin(lin) sam_timer_cleanup_begin(lin)
#define sam_timer_start_or_restart_end(lin) \
	do { \
		sam_timer_cleanup_end(lin); \
		(lin)->timer->COUNT16.CTRLBSET.bit.CMD = TC_CTRLBSET_CMD_RETRIGGER_Val; \
	} while (0)

#define sam_timer_start_or_restart(lin) \
	do { \
		timer_start_or_restart_begin(lin); \
		timer_start_or_restart_end(lin); \
	} while (0)


#define sof_start_or_restart_begin(lin) sam_timer_start_or_restart_begin(lin)

#define sof_start_or_restart_end(lin) \
	do { \
		sam_timer_cleanup_end(lin); \
		lin->timer_type = TIMER_TYPE_SOF; \
		lin->timer->COUNT16.CC[0].reg = lin->sof_timeout_us; \
		(lin)->timer->COUNT16.CTRLBSET.bit.CMD = TC_CTRLBSET_CMD_RETRIGGER_Val; \
	} while (0)


#define break_start_or_restart_begin(lin) sam_timer_cleanup_begin(lin)

#define break_start_or_restart_end(lin) \
	do { \
		sam_timer_cleanup_end(lin); \
		lin->timer_type = TIMER_TYPE_BREAK; \
		lin->timer->COUNT16.CC[0].reg = lin->master.break_timeout_us; \
		(lin)->timer->COUNT16.CTRLBSET.bit.CMD = TC_CTRLBSET_CMD_RETRIGGER_Val; \
	} while (0)



#define sleep_start(lin) \
	do { \
		(lin)->slave.sleep_elapsed_us = 0; \
		(lin)->timer_type = TIMER_TYPE_SLEEP; \
		(lin)->timer->COUNT16.CC[0].reg = 0xffff; \
		(lin)->timer->COUNT16.CTRLBSET.bit.CMD = TC_CTRLBSET_CMD_RETRIGGER_Val; \
	} while (0)

SLLIN_RAMFUNC static inline void lin_cleanup_master_tx(struct sam_lin *lin, uint8_t slave_proto_step)
{
	struct slave *sl = &lin->slave;

	sl->slave_proto_step = slave_proto_step;
	sl->slave_tx_offset = 0;
	sl->slave_rx_offset = 0;
	sl->elem.frame.id = 0;
	sl->elem.frame.len = 0;
}

#define lin_cleanup_full(lin, slave_proto_step) \
	do { \
		lin_cleanup_master_tx(lin, slave_proto_step); \
		(lin)->sercom->USART.INTENCLR.reg = SERCOM_USART_INTENCLR_DRE; \
	} while (0)

