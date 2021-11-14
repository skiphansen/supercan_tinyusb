/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Jean Gressmann <jean@0x42.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#ifdef SAME54XPLAINEDPRO

#include <sllin_debug.h>
#include <sllin_board.h>


#include <hal/include/hal_gpio.h>
#include <crc32.h>
#include <mcu.h>
#include <bsp/board.h>


enum {
	SLAVE_PROTO_STEP_RX_BREAK,
	// SLAVE_PROTO_STEP_RX_SYNC,
	SLAVE_PROTO_STEP_RX_PID,
	SLAVE_PROTO_STEP_TX_DATA,
	// SLAVE_PROTO_STEP_TX_CRC,
	// SLAVE_PROTO_STEP_TX_FIN,
	MASTER_PROTO_STEP_RX_PID,
	MASTER_PROTO_STEP_RX_DATA,
};

typedef void (*sllin_lin_int_callback)(uint8_t index);
SLLIN_RAMFUNC static void lin_int_master(uint8_t index);
SLLIN_RAMFUNC static void lin_int_slave(uint8_t index);

struct lin {
	Sercom* const sercom;
	sllin_lin_int_callback irq_handler;
	sllin_queue_element rx_frame;
	unsigned crc;
	uint8_t master_tx_data[9];
	uint8_t master_proto_step;
	// uint8_t master_tx_busy;
	// uint8_t rx_full;
	uint8_t tx_gi; // not an index, uses full range of type
	uint8_t tx_pi; // not an index, uses full range of type
	// uint8_t rx_gi; // not an index, uses full range of type
	uint8_t rx_pi; // not an index, uses full range of type
	// uint8_t rx_state;

	uint8_t slave_frame_len[64];
	uint8_t slave_frame_data[64][9];
	uint8_t slave_proto_step;
	uint8_t slave_frame_offset;
	uint8_t slave_tx_value;
};

static struct lin lins[SLLIN_BOARD_LIN_COUNT] = {
	{
		.sercom = SERCOM6,
	},
	{
		.sercom = SERCOM7,
	},
};

static inline void same5x_enable_cache(void)
{
	// DS60001507E-page 83
	if (!CMCC->SR.bit.CSTS) {
		CMCC->CTRL.bit.CEN = 1;
	}
}

static uint32_t device_identifier;

extern uint32_t sllin_board_identifier(void)
{
	return device_identifier;
}


void same5x_init_device_identifier(void)
{
	uint32_t serial_number[4];
	int error = CRC32E_NONE;

	same51_get_serial_number(serial_number);

#if SLLIN_DEBUG
	char serial_buffer[64];
	memset(serial_buffer, '0', 32);
	char hex_buffer[16];
	int chars = usnprintf(hex_buffer, sizeof(hex_buffer), "%x", serial_number[0]);
	memcpy(&serial_buffer[8-chars], hex_buffer, chars);
	chars = usnprintf(hex_buffer, sizeof(hex_buffer), "%x", serial_number[1]);
	memcpy(&serial_buffer[16-chars], hex_buffer, chars);
	chars = usnprintf(hex_buffer, sizeof(hex_buffer), "%x", serial_number[2]);
	memcpy(&serial_buffer[24-chars], hex_buffer, chars);
	chars = usnprintf(hex_buffer, sizeof(hex_buffer), "%x", serial_number[3]);
	memcpy(&serial_buffer[32-chars], hex_buffer, chars);
	serial_buffer[32] = 0;
	LOG("SAM serial number %s\n", serial_buffer);
#endif

#if TU_LITTLE_ENDIAN == TU_BYTE_ORDER
	// swap integers so they have printf layout
	serial_number[0] = __builtin_bswap32(serial_number[0]);
	serial_number[1] = __builtin_bswap32(serial_number[1]);
	serial_number[2] = __builtin_bswap32(serial_number[2]);
	serial_number[3] = __builtin_bswap32(serial_number[3]);
#endif

	error = crc32f((uint32_t)serial_number, 16, CRC32E_FLAG_UNLOCK, &device_identifier);
	if (unlikely(error)) {
		device_identifier = serial_number[0];
		LOG("ERROR: failed to compute CRC32: %d. Using fallback device identifier\n", error);
	}

#if SLLIN_DEBUG
	memset(serial_buffer, '0', 8);
	chars = usnprintf(hex_buffer, sizeof(hex_buffer), "%x", device_identifier);
	memcpy(&serial_buffer[8-chars], hex_buffer, chars);
	serial_buffer[8] = 0;
	LOG("device identifier %s\n", serial_buffer);
#endif
}


extern void sllin_board_led_set(uint8_t index, bool on)
{
	SLLIN_DEBUG_ASSERT(0 == index);
	(void)index;

	board_led_write(on);
}


extern void sllin_board_leds_on_unsafe(void)
{
	board_led_write(true);
}



/** Initializes the clocks from the external 12 MHz crystal
 *
 * The goal of this setup is to preserve the second PLL
 * for the application code while still having a reasonable
 * 48 MHz clock for USB / UART.
 *
 * GCLK0:   CONF_CPU_FREQUENCY (default 120 MHz) from PLL0
 * GCLK1:   unused
 * GCLK2:   12 MHz from XOSC1
 * DFLL48M: closed loop from GLCK2
 * GCLK3:   48 MHz
 */
static inline void clock_init(void)
{
	/* configure for a 12MHz crystal connected to XIN1/XOUT1 */
	OSCCTRL->XOSCCTRL[1].reg =
		OSCCTRL_XOSCCTRL_STARTUP(6) | // 1.953 ms
		OSCCTRL_XOSCCTRL_RUNSTDBY |
		OSCCTRL_XOSCCTRL_ENALC |
		OSCCTRL_XOSCCTRL_IMULT(4) | OSCCTRL_XOSCCTRL_IPTAT(3) | // 8MHz to 16MHz
		OSCCTRL_XOSCCTRL_XTALEN |
		OSCCTRL_XOSCCTRL_ENABLE;
	while(0 == OSCCTRL->STATUS.bit.XOSCRDY1);

	OSCCTRL->Dpll[0].DPLLCTRLB.reg = OSCCTRL_DPLLCTRLB_DIV(2) | OSCCTRL_DPLLCTRLB_REFCLK_XOSC1; /* 12MHz / 6 = 2Mhz, input = XOSC1 */
	OSCCTRL->Dpll[0].DPLLRATIO.reg = OSCCTRL_DPLLRATIO_LDRFRAC(0x0) | OSCCTRL_DPLLRATIO_LDR((CONF_CPU_FREQUENCY / 1000000 / 2) - 1); /* multiply to get CONF_CPU_FREQUENCY (default = 120MHz) */
	OSCCTRL->Dpll[0].DPLLCTRLA.reg = OSCCTRL_DPLLCTRLA_RUNSTDBY | OSCCTRL_DPLLCTRLA_ENABLE;
	while(0 == OSCCTRL->Dpll[0].DPLLSTATUS.bit.CLKRDY); /* wait for the PLL0 to be ready */

	/* configure clock-generator 0 to use DPLL0 as source -> GCLK0 is used for the core */
	GCLK->GENCTRL[0].reg =
		GCLK_GENCTRL_DIV(0) |
		GCLK_GENCTRL_RUNSTDBY |
		GCLK_GENCTRL_GENEN |
		GCLK_GENCTRL_SRC_DPLL0 |
		GCLK_GENCTRL_IDC;
	while(1 == GCLK->SYNCBUSY.bit.GENCTRL0); /* wait for the synchronization between clock domains to be complete */

	// configure GCLK2 for 12MHz from XOSC1
	GCLK->GENCTRL[2].reg =
		GCLK_GENCTRL_DIV(0) |
		GCLK_GENCTRL_RUNSTDBY |
		GCLK_GENCTRL_GENEN |
		GCLK_GENCTRL_SRC_XOSC1 |
		GCLK_GENCTRL_IDC;
	while(1 == GCLK->SYNCBUSY.bit.GENCTRL2); /* wait for the synchronization between clock domains to be complete */

	 /* setup DFLL48M to use GLCK2 */
	GCLK->PCHCTRL[OSCCTRL_GCLK_ID_DFLL48].reg = GCLK_PCHCTRL_GEN_GCLK2 | GCLK_PCHCTRL_CHEN;

	OSCCTRL->DFLLCTRLA.reg = 0;
	while(1 == OSCCTRL->DFLLSYNC.bit.ENABLE);

	OSCCTRL->DFLLCTRLB.reg = OSCCTRL_DFLLCTRLB_MODE | OSCCTRL_DFLLCTRLB_WAITLOCK;
	OSCCTRL->DFLLMUL.bit.MUL = 4; // 4 * 12MHz -> 48MHz

	OSCCTRL->DFLLCTRLA.reg =
		OSCCTRL_DFLLCTRLA_ENABLE |
		OSCCTRL_DFLLCTRLA_RUNSTDBY;
	while(1 == OSCCTRL->DFLLSYNC.bit.ENABLE);

	// setup 48 MHz GCLK3 from DFLL48M
	GCLK->GENCTRL[3].reg =
		GCLK_GENCTRL_DIV(0) |
		GCLK_GENCTRL_RUNSTDBY |
		GCLK_GENCTRL_GENEN |
		GCLK_GENCTRL_SRC_DFLL |
		GCLK_GENCTRL_IDC;
	while(1 == GCLK->SYNCBUSY.bit.GENCTRL3);
}


/* Initialize SERCOM2 for 115200 bps 8N1 using a 48 MHz clock */
static inline void uart_init(void)
{
	gpio_set_pin_function(PIN_PB24, PINMUX_PB24D_SERCOM2_PAD1);
	gpio_set_pin_function(PIN_PB25, PINMUX_PB25D_SERCOM2_PAD0);

	MCLK->APBBMASK.bit.SERCOM2_ = 1;
	GCLK->PCHCTRL[SERCOM2_GCLK_ID_CORE].reg = GCLK_PCHCTRL_GEN_GCLK3 | GCLK_PCHCTRL_CHEN;

	SERCOM2->USART.CTRLA.bit.SWRST = 1; /* reset and disable SERCOM -> enable configuration */
	while (SERCOM2->USART.SYNCBUSY.bit.SWRST);

	SERCOM2->USART.CTRLA.reg  =
		SERCOM_USART_CTRLA_SAMPR(0) | /* 0 = 16x / arithmetic baud rate, 1 = 16x / fractional baud rate */
		SERCOM_USART_CTRLA_SAMPA(0) | /* 16x over sampling */
		SERCOM_USART_CTRLA_FORM(0) | /* 0x0 USART frame, 0x1 USART frame with parity, ... */
		SERCOM_USART_CTRLA_DORD | /* LSB first */
		SERCOM_USART_CTRLA_MODE(1) | /* 0x0 USART with external clock, 0x1 USART with internal clock */
		SERCOM_USART_CTRLA_RXPO(1) | /* SERCOM PAD[1] is used for data reception */
		SERCOM_USART_CTRLA_TXPO(0); /* SERCOM PAD[0] is used for data transmission */

	SERCOM2->USART.CTRLB.reg = /* RXEM = 0 -> receiver disabled, LINCMD = 0 -> normal USART transmission, SFDE = 0 -> start-of-frame detection disabled, SBMODE = 0 -> one stop bit, CHSIZE = 0 -> 8 bits */
		SERCOM_USART_CTRLB_TXEN | /* transmitter enabled */
		SERCOM_USART_CTRLB_RXEN; /* receiver enabled */
	// SERCOM2->USART.BAUD.reg = SERCOM_USART_BAUD_FRAC_FP(0) | SERCOM_USART_BAUD_FRAC_BAUD(26); /* 48000000/(16*115200) = 26.041666667 */
	SERCOM2->USART.BAUD.reg = SERCOM_USART_BAUD_BAUD(63019); /* 65536*(1−16*115200/48000000) */

	SERCOM2->USART.CTRLA.bit.ENABLE = 1; /* activate SERCOM */
	while (SERCOM2->USART.SYNCBUSY.bit.ENABLE); /* wait for SERCOM to be ready */
}

static inline void lin_init_once(void)
{
	// lin0
	gpio_set_pin_function(PIN_PC04, PINMUX_PC04C_SERCOM6_PAD0);
	gpio_set_pin_function(PIN_PC05, PINMUX_PC05C_SERCOM6_PAD1);

	MCLK->APBDMASK.bit.SERCOM6_ = 1;
	GCLK->PCHCTRL[SERCOM6_GCLK_ID_CORE].reg = GCLK_PCHCTRL_GEN_GCLK3 | GCLK_PCHCTRL_CHEN;

	NVIC_SetPriority(SERCOM6_0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SERCOM6_1_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SERCOM6_2_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SERCOM6_3_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);

	NVIC_EnableIRQ(SERCOM6_0_IRQn);
	NVIC_EnableIRQ(SERCOM6_1_IRQn);
	NVIC_EnableIRQ(SERCOM6_2_IRQn);
	NVIC_EnableIRQ(SERCOM6_3_IRQn);

	// lin1
	// gpio_set_pin_function(PIN_PA04, PINMUX_PA04D_SERCOM0_PAD0);
	// gpio_set_pin_function(PIN_PA05, PINMUX_PA05D_SERCOM0_PAD1);

	// MCLK->APBAMASK.bit.SERCOM0_ = 1;
	// GCLK->PCHCTRL[SERCOM0_GCLK_ID_CORE].reg = GCLK_PCHCTRL_GEN_GCLK3 | GCLK_PCHCTRL_CHEN;

	// NVIC_SetPriority(SERCOM0_0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	// NVIC_SetPriority(SERCOM0_1_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	// NVIC_SetPriority(SERCOM0_2_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	// NVIC_SetPriority(SERCOM0_3_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);

	// NVIC_EnableIRQ(SERCOM0_0_IRQn);
	// NVIC_EnableIRQ(SERCOM0_1_IRQn);
	// NVIC_EnableIRQ(SERCOM0_2_IRQn);
	// NVIC_EnableIRQ(SERCOM0_3_IRQn);

	gpio_set_pin_function(PIN_PD08, PINMUX_PD08C_SERCOM7_PAD0);
	gpio_set_pin_function(PIN_PD09, PINMUX_PD09C_SERCOM7_PAD1);

	MCLK->APBDMASK.bit.SERCOM7_ = 1;
	GCLK->PCHCTRL[SERCOM7_GCLK_ID_CORE].reg = GCLK_PCHCTRL_GEN_GCLK3 | GCLK_PCHCTRL_CHEN;

	NVIC_SetPriority(SERCOM7_0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SERCOM7_1_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SERCOM7_2_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(SERCOM7_3_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);

	NVIC_EnableIRQ(SERCOM7_0_IRQn);
	NVIC_EnableIRQ(SERCOM7_1_IRQn);
	NVIC_EnableIRQ(SERCOM7_2_IRQn);
	NVIC_EnableIRQ(SERCOM7_3_IRQn);

}

static inline void lin_slave_cleanup(struct lin *lin)
{
	lin->slave_proto_step = SLAVE_PROTO_STEP_RX_BREAK;
	lin->slave_frame_offset = 0;
	lin->rx_pi = 0;
	// s->USART.INTENCLR.reg = SERCOM_USART_INTENCLR_TXC;
	// | SERCOM_USART_INTENCLR_DRE;
}

static inline void lin_init(uint8_t index, uint16_t bitrate, bool master)
{
	struct lin *lin = &lins[index];
	Sercom *sercom = lin->sercom;

	lin->irq_handler = master ? &lin_int_master : &lin_int_slave;

	sercom->USART.CTRLA.bit.SWRST = 1; /* reset and disable SERCOM -> enable configuration */
	while (lin->sercom->USART.SYNCBUSY.bit.SWRST);

	sercom->USART.CTRLA.reg  =
		SERCOM_USART_CTRLA_SAMPR(1) | /* 0 = 16x / arithmetic baud rate, 1 = 16x / fractional baud rate */
		SERCOM_USART_CTRLA_SAMPA(0) |
		SERCOM_USART_CTRLA_FORM(master ? 0x2 : 0x4) | /* 0x3 LIN master, 0x4 LIN slave, ... */
		SERCOM_USART_CTRLA_DORD | /* LSB first */
		SERCOM_USART_CTRLA_MODE(1) | /* 0x0 USART with external clock, 0x1 USART with internal clock */
		SERCOM_USART_CTRLA_RXPO(1) |
		SERCOM_USART_CTRLA_TXPO(0);

	sercom->USART.CTRLB.reg = /* RXEM = 0 -> receiver disabled, LINCMD = 0 -> normal USART transmission, SFDE = 0 -> start-of-frame detection disabled, SBMODE = 0 -> one stop bit, CHSIZE = 0 -> 8 bits */
		SERCOM_USART_CTRLB_TXEN | /* transmitter enabled */
		SERCOM_USART_CTRLB_RXEN | /* receiver enabled */
		SERCOM_USART_CTRLB_COLDEN; /* collision detection enabled */

	uint16_t baud = 48000000 / (16 * bitrate);
	uint16_t frac = 48000000 / (2 * bitrate) - 8 * baud;
	sercom->USART.BAUD.reg = SERCOM_USART_BAUD_FRAC_FP(frac) | SERCOM_USART_BAUD_FRAC_BAUD(baud);

	sercom->USART.CTRLA.bit.ENABLE = 1; /* activate SERCOM */
	while (sercom->USART.SYNCBUSY.bit.ENABLE); /* wait for SERCOM to be ready */

	sercom->USART.INTENCLR.reg = ~0;

	if (master) {
		sercom->USART.INTENSET.reg = SERCOM_USART_INTENSET_ERROR | SERCOM_USART_INTENSET_RXC;
	} else {
		//sercom->USART.INTENSET.reg = SERCOM_USART_INTENSET_RXBRK | SERCOM_USART_INTENSET_ERROR | SERCOM_USART_INTENSET_RXC;
		sercom->USART.INTENSET.reg =
			SERCOM_USART_INTENSET_RXBRK |
			SERCOM_USART_INTENSET_ERROR |
			SERCOM_USART_INTENSET_RXC |
			SERCOM_USART_INTENSET_TXC;

		lin_slave_cleanup(lin);
	}
}

static inline void usb_init(void)
{
	NVIC_SetPriority(USB_0_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(USB_1_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(USB_2_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	NVIC_SetPriority(USB_3_IRQn, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);


	/* USB clock init
	 * The USB module requires a GCLK_USB of 48 MHz ~ 0.25% clock
	 * for low speed and full speed operation.
	 */
	hri_gclk_write_PCHCTRL_reg(GCLK, USB_GCLK_ID, GCLK_PCHCTRL_GEN_GCLK3_Val | GCLK_PCHCTRL_CHEN);
	hri_mclk_set_AHBMASK_USB_bit(MCLK);
	hri_mclk_set_APBBMASK_USB_bit(MCLK);

	// USB pin init
	gpio_set_pin_direction(PIN_PA24, GPIO_DIRECTION_OUT);
	gpio_set_pin_level(PIN_PA24, false);
	gpio_set_pin_pull_mode(PIN_PA24, GPIO_PULL_OFF);
	gpio_set_pin_direction(PIN_PA25, GPIO_DIRECTION_OUT);
	gpio_set_pin_level(PIN_PA25, false);
	gpio_set_pin_pull_mode(PIN_PA25, GPIO_PULL_OFF);

	gpio_set_pin_function(PIN_PA24, PINMUX_PA24H_USB_DM);
	gpio_set_pin_function(PIN_PA25, PINMUX_PA25H_USB_DP);
}

extern void sllin_board_init_begin(void)
{
	clock_init();

	SystemCoreClock = CONF_CPU_FREQUENCY;

	uart_init();
	LOG("CONF_CPU_FREQUENCY=%lu\n", (unsigned long)CONF_CPU_FREQUENCY);

	// LED0 init
	gpio_set_pin_function(PIN_PC18, GPIO_PIN_FUNCTION_OFF);
	gpio_set_pin_direction(PIN_PC18, GPIO_DIRECTION_OUT);
	board_led_write(0);

	usb_init();

	lin_init_once();

	LOG("Enabling cache\n");
	same5x_enable_cache();

	same5x_init_device_identifier();

	// PC07 is hooked up to relay
	// configure as out and set to low
	PORT->Group[2].DIRSET.reg |= 1ul << 7;
	PORT->Group[2].OUTCLR.reg |= 1ul << 7;

	//PORT->Group[2].DIRSET.reg |= 1ul << 7;
}

extern void sllin_board_init_end(void)
{

}

__attribute__((noreturn)) extern void sllin_board_reset(void)
{
	NVIC_SystemReset();
	__unreachable();
}

extern void sllin_board_lin_init(uint8_t index, uint16_t bitrate, bool master)
{
	lin_init(index, bitrate, master);
}


SLLIN_RAMFUNC extern bool sllin_board_lin_master_tx(
	uint8_t index,
	uint8_t id,
	uint8_t len,
	uint8_t const *data,
	uint8_t crc,
	uint8_t flags)
{
	struct lin *lin = &lins[index];
	Sercom *s = NULL;
	uint8_t pid = sllin_id_to_pid(id);

	SLLIN_DEBUG_ASSERT(index < TU_ARRAY_SIZE(lins));
	SLLIN_DEBUG_ASSERT(len <= 8);

	// if (unlikely(__atomic_load_n(&lin->master_tx_busy, __ATOMIC_ACQUIRE))) {
	// 	return false;
	// }

	s = lin->sercom;

	// reboot
	s->USART.CTRLA.bit.ENABLE = 0;
	while (s->USART.SYNCBUSY.bit.ENABLE);
	s->USART.CTRLA.bit.ENABLE = 1;

	lin->rx_pi = 0;
	lin->tx_gi = 0;
	lin->crc = sllin_crc_start();





	if (data) { // frame
		for (unsigned i = 0; i < len; ++i) {
			lin->master_tx_data[i] = data[i];
		}

		lin->master_tx_data[len] = crc;


		s->USART.INTENSET.reg = SERCOM_USART_INTENSET_TXC | SERCOM_USART_INTENSET_DRE;
		// s->USART.INTENSET.reg = SERCOM_USART_INTENSET_TXC;

		lin->tx_pi = len + 1;

	} else { // header only
		if (flags & SLLIN_FRAME_FLAG_ENHANCED_CHECKSUM) {
			lin->crc = sllin_crc_update1(lin->crc, pid);
		}

		s->USART.INTENCLR.reg = SERCOM_USART_INTENSET_TXC | SERCOM_USART_INTENSET_DRE;
		// s->USART.INTENCLR.reg = SERCOM_USART_INTENSET_TXC;


		lin->tx_pi = 0;
	}

	lin->rx_frame.type = SLLIN_QUEUE_ELEMENT_TYPE_RX_FRAME;
	lin->rx_frame.lin_frame.id = id;
	lin->rx_frame.lin_frame.len = len;
	lin->rx_frame.lin_frame.flags = flags;
	lin->master_proto_step = MASTER_PROTO_STEP_RX_PID;

	__atomic_thread_fence(__ATOMIC_RELEASE);
	// __atomic_test_and_set(&lin->master_tx_busy, __ATOMIC_RELAXED);
	while (s->USART.SYNCBUSY.bit.ENABLE);

	s->USART.CTRLB.bit.LINCMD = 0x2;
	s->USART.DATA.reg = pid;

	return true;
}


SLLIN_RAMFUNC extern void sllin_board_lin_slave_tx(
	uint8_t index,
	uint8_t id,
	uint8_t len,
	uint8_t const *data,
	uint8_t crc)
{
	struct lin *lin = &lins[index];

	SLLIN_DEBUG_ASSERT(index < TU_ARRAY_SIZE(lins));
	SLLIN_DEBUG_ASSERT(len <= 8);

	if (len) {
		lin->slave_frame_len[id] = len + 1;
		memcpy(lin->slave_frame_data[id], data, len);
		lin->slave_frame_data[id][len] = crc;
	} else {
		// clear tx frame
		lin->slave_frame_len[id] = 0;
	}

	__atomic_thread_fence(__ATOMIC_RELEASE);
}


SLLIN_RAMFUNC static void lin_int_master(uint8_t index)
{
	struct lin *lin = &lins[index];
	Sercom *s = lin->sercom;

	uint32_t intflag = s->USART.INTFLAG.reg;
	// uint32_t status = s->USART.STATUS.reg;

	// LOG("ch%u INTFLAG=%x\n", index, intflag);
	// LOG("ch%u STATUS=%x\n", index, status);
	LOG(".");
	s->USART.INTFLAG.reg = ~0;
	s->USART.STATUS.reg = ~0;

	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	if (intflag & SERCOM_USART_INTFLAG_RXC) {
		uint8_t byte = s->USART.DATA.reg;

		if (lin->master_proto_step == MASTER_PROTO_STEP_RX_PID) {
			lin->master_proto_step = MASTER_PROTO_STEP_RX_DATA;
		} else {
			if (lin->rx_frame.lin_frame.len == lin->rx_pi) {
				lin->rx_frame.lin_frame.crc = sllin_crc_finalize(lin->crc);
				if (likely(lin->rx_frame.lin_frame.crc == byte)) {
					sllin_lin_task_queue(index, &lin->rx_frame);
					sllin_lin_task_notify_isr(index, 1);
					// __atomic_clear(&lin->master_tx_busy, __ATOMIC_RELEASE);
				} else {
					LOG("ch%u crc mismatch want=%x have=%x data=%x %x %x %x %x %x %x %x\n",
						index, lin->rx_frame.lin_frame.crc, byte,
						lin->rx_frame.lin_frame.data[0], lin->rx_frame.lin_frame.data[1],
						lin->rx_frame.lin_frame.data[2], lin->rx_frame.lin_frame.data[3],
						lin->rx_frame.lin_frame.data[4], lin->rx_frame.lin_frame.data[5],
						lin->rx_frame.lin_frame.data[6], lin->rx_frame.lin_frame.data[7]);
				}
			} else {
				lin->rx_frame.lin_frame.data[lin->rx_pi++] = byte;
				lin->crc = sllin_crc_update1(lin->crc, byte);
			}
		}

		// LOG("ch%u RX=%x\n", index, byte);
	}

	if (intflag & SERCOM_USART_INTFLAG_DRE) {
		uint8_t tx_gi = lin->tx_gi;
		uint8_t tx_pi = lin->tx_pi;

		if (tx_pi != tx_gi) {
			uint8_t tx_index = tx_gi;
			uint8_t byte = lin->master_tx_data[tx_index];

			SLLIN_ISR_ASSERT(tx_gi < sizeof(lin->master_tx_data));
			SLLIN_ISR_ASSERT(tx_pi < sizeof(lin->master_tx_data));
			SLLIN_ISR_ASSERT(tx_gi < tx_pi);

			++lin->tx_gi;

			s->USART.CTRLB.bit.LINCMD = 0x0;
			s->USART.DATA.reg = byte;
			// LOG("ch%u TX=%x\n", index, byte);
		} else {
			s->USART.INTENCLR.reg = SERCOM_USART_INTENSET_DRE;
		}
	}
}

SLLIN_RAMFUNC static inline void lin_int_slave_tx_more_data(uint8_t index, uint8_t id)
{
	struct lin *lin = &lins[index];
	Sercom *s = lin->sercom;
	uint8_t len = lin->slave_frame_len[id];
	uint8_t byte = lin->slave_frame_data[id][lin->slave_frame_offset++];

	SLLIN_ISR_ASSERT(lin->slave_frame_offset < len);
	(void)len;

	lin->slave_tx_value = byte;
	s->USART.DATA.reg = byte;
	LOG("ch%u TX=%x\n", index, byte);

	// if (lin->slave_frame_offset == len) {
	// 	lin->slave_proto_step = SLAVE_PROTO_STEP_TX_FIN;
	// }
}

SLLIN_RAMFUNC static void lin_int_slave(uint8_t index)
{
	struct lin *lin = &lins[index];
	Sercom *s = lin->sercom;

	uint32_t intflag = s->USART.INTFLAG.reg;
	// uint32_t status = s->USART.STATUS.reg;

	LOG("ch%u INTFLAG=%x\n", index, intflag);
	// LOG("ch%u STATUS=%x\n", index, status);
	s->USART.INTFLAG.reg = ~0;
	s->USART.STATUS.reg = ~0;

	LOG("/");

	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	if (intflag & SERCOM_USART_INTFLAG_RXBRK) {
		lin->slave_proto_step = SLAVE_PROTO_STEP_RX_PID;
		LOG("ch%u BREAK\n", index);
		intflag &= ~SERCOM_USART_INTFLAG_DRE;
	} else if (intflag & SERCOM_USART_INTFLAG_RXC) {
		uint8_t byte = s->USART.DATA.reg;

		LOG("ch%u RX %x\n", index, byte);
		intflag &= ~SERCOM_USART_INTFLAG_DRE;

		if (lin->slave_proto_step == SLAVE_PROTO_STEP_RX_PID) {
			uint8_t id = sllin_pid_to_id(byte);
			uint8_t len = lin->slave_frame_len[id];

			if (len) {
				lin->rx_frame.lin_frame.id = id;
				lin->rx_frame.lin_frame.len = len;
				// s->USART.INTENSET.reg = SERCOM_USART_INTENSET_TXC;
				// | SERCOM_USART_INTENSET_DRE;
				lin->slave_proto_step = SLAVE_PROTO_STEP_TX_DATA;
				lin_int_slave_tx_more_data(index, id);
			} else {
				// lin->slave_proto_step = SLAVE_PROTO_STEP_RX_BREAK;
				LOG("ch%u no data for id=%x\n", index, id);
				goto cleanup;
			}
		} else if (lin->slave_proto_step == SLAVE_PROTO_STEP_TX_DATA) {
			if (likely(byte == lin->slave_tx_value)) {
				if (lin->rx_pi + 1 == lin->rx_frame.lin_frame.len) {
					lin->rx_frame.lin_frame.crc = byte;

					sllin_lin_task_queue(index, &lin->rx_frame);
					sllin_lin_task_notify_isr(index, 1);

					goto cleanup;
				} else {
					lin->rx_frame.lin_frame.data[lin->rx_pi++] = byte;
				}
			} else {
				LOG("ch%u tx/rx data mismatch want=%x have=%x\n", index, lin->slave_tx_value, byte);
				// TODO send error frame
				goto cleanup;
			}
		// } else if (lin->slave_proto_step == SLAVE_PROTO_STEP_TX_CRC) {
		// 	lin->rx_frame.lin_frame.crc = byte;
		} else {
			LOG("ch%u step=%u\n", index, lin->slave_proto_step);
			goto cleanup;
		}
	}

	// if (intflag & SERCOM_USART_INTFLAG_DRE) {
	if (intflag & SERCOM_USART_INTFLAG_TXC) {
		if (lin->slave_proto_step == SLAVE_PROTO_STEP_TX_DATA) {
			lin_int_slave_tx_more_data(index, lin->rx_frame.lin_frame.id);
		// } else if (lin->slave_proto_step == SLAVE_PROTO_STEP_TX_CRC) {
		// 	uint8_t crc = 0x42;
		// 	s->USART.DATA.reg = crc;
		// 	LOG("ch%u TX=%x\n", index, crc);
		// 	lin->slave_proto_step = SLAVE_PROTO_STEP_TX_FIN;
		// } else if (lin->slave_proto_step == SLAVE_PROTO_STEP_TX_FIN) {
		// 	sllin_lin_task_queue(index, &lin->rx_frame);
		// 	sllin_lin_task_notify_isr(index, 1);
		// 	goto cleanup;
		} else {
			goto cleanup;
		}
	}

	if (intflag & SERCOM_USART_INTFLAG_ERROR) {
cleanup:
		LOG("ch%u cleanup\n", index);
		lin_slave_cleanup(lin);
	}
}


SLLIN_RAMFUNC void SERCOM6_0_Handler(void)
{
	lins[0].irq_handler(0);
}

SLLIN_RAMFUNC void SERCOM6_1_Handler(void)
{
	lins[0].irq_handler(0);
}

SLLIN_RAMFUNC void SERCOM6_2_Handler(void)
{
	lins[0].irq_handler(0);
}

SLLIN_RAMFUNC void SERCOM6_3_Handler(void)
{
	lins[0].irq_handler(0);
}

// SLLIN_RAMFUNC void SERCOM0_0_Handler(void)
// {
// 	lins[1].irq_handler(1);
// }

// SLLIN_RAMFUNC void SERCOM0_1_Handler(void)
// {
// 	lins[1].irq_handler(1);
// }

// SLLIN_RAMFUNC void SERCOM0_2_Handler(void)
// {
// 	lins[1].irq_handler(1);
// }

// SLLIN_RAMFUNC void SERCOM0_3_Handler(void)
// {
// 	lins[1].irq_handler(1);
// }

SLLIN_RAMFUNC void SERCOM7_0_Handler(void)
{
	lins[1].irq_handler(1);
}

SLLIN_RAMFUNC void SERCOM7_1_Handler(void)
{
	lins[1].irq_handler(1);
}

SLLIN_RAMFUNC void SERCOM7_2_Handler(void)
{
	lins[1].irq_handler(1);
}

SLLIN_RAMFUNC void SERCOM7_3_Handler(void)
{
	lins[1].irq_handler(1);
}


#endif // #ifdef SAME54XPLAINEDPRO