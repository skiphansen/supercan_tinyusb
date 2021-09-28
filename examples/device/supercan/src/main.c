/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020-2021 Jean Gressmann <jean@0x42.de>
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

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <timers.h>


#include <bsp/board.h>
#include <tusb.h>
#include <usbd_pvt.h>
#include <class/dfu/dfu_rt_device.h>
#include <dcd.h>

#define SC_PACKED __packed

#include <supercan.h>
#include <supercan_m1.h>
#include <supercan_debug.h>
#include <supercan_version.h>
#include <supercan_board.h>
#include <usb_descriptors.h>
// #include <m_can.h>
#include <mcu.h>

#include <leds.h>
#include <crc32.h>
#include <device.h>
#include <sections.h>


#define CLOCK_MAX 0xffffffff
#define SPAM 0


#if TU_BIG_ENDIAN == TU_BYTE_ORDER
static inline uint16_t le16_to_cpu(uint16_t value) { return __builtin_bswap16(value); }
static inline uint32_t le32_to_cpu(uint32_t value) { return __builtin_bswap32(value); }
static inline uint16_t cpu_to_le16(uint16_t value) { return __builtin_bswap16(value); }
static inline uint32_t cpu_to_le32(uint32_t value) { return __builtin_bswap32(value); }
static inline uint16_t be16_to_cpu(uint16_t value) { return value; }
static inline uint32_t be32_to_cpu(uint32_t value) { return value; }
static inline uint16_t cpu_to_be16(uint16_t value) { return value; }
static inline uint32_t cpu_to_be32(uint32_t value) { return value; }
#else
static inline uint16_t le16_to_cpu(uint16_t value) { return value; }
static inline uint32_t le32_to_cpu(uint32_t value) { return value; }
static inline uint16_t cpu_to_le16(uint16_t value) { return value; }
static inline uint32_t cpu_to_le32(uint32_t value) { return value; }
static inline uint16_t be16_to_cpu(uint16_t value) { return __builtin_bswap16(value); }
static inline uint32_t be32_to_cpu(uint32_t value) { return __builtin_bswap32(value); }
static inline uint16_t cpu_to_be16(uint16_t value) { return __builtin_bswap16(value); }
static inline uint32_t cpu_to_be32(uint32_t value) { return __builtin_bswap32(value); }
#endif



static inline void can_log_bit_timing(sc_can_bit_timing const *c, char const* name)
{
	(void)c;
	(void)name;

	LOG("%s brp=%u sjw=%u tseg1=%u tseg2=%u bitrate=%lu sp=%u/1000\n",
		name, c->brp, c->sjw, c->tseg1, c->tseg2,
		SC_BOARD_CAN_CLK_HZ / ((uint32_t)c->brp * (1 + c->tseg1 + c->tseg2)),
		((1 + c->tseg1) * 1000) / (1 + c->tseg1 + c->tseg2)
	);
}


SC_RAMFUNC static inline uint8_t dlc_to_len(uint8_t dlc)
{
	static const uint8_t map[16] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64
	};
	return map[dlc & 0xf];
}


static StackType_t usb_device_stack[configMINIMAL_SECURE_STACK_SIZE];
static StaticTask_t usb_device_stack_mem;

static void tusb_device_task(void* param);


#define LED_BURST_DURATION_MS 8

#define USB_TRAFFIC_DO_LED led_burst(LED_DEBUG_3, LED_BURST_DURATION_MS)
#define POWER_LED LED_DEBUG_0
#define CAN0_TRAFFIC_LED LED_DEBUG_1
#define CAN1_TRAFFIC_LED LED_DEBUG_2



struct usb_can {
	CFG_TUSB_MEM_ALIGN uint8_t tx_buffers[2][MSG_BUFFER_SIZE];
	CFG_TUSB_MEM_ALIGN uint8_t rx_buffers[2][MSG_BUFFER_SIZE];
	StaticSemaphore_t mutex_mem;
	SemaphoreHandle_t mutex_handle;
	uint16_t tx_offsets[2];
	uint8_t tx_bank;
	uint8_t rx_bank;
	uint8_t pipe;
};

struct usb_cmd {
	CFG_TUSB_MEM_ALIGN uint8_t tx_buffers[2][CMD_BUFFER_SIZE];
	CFG_TUSB_MEM_ALIGN uint8_t rx_buffers[2][CMD_BUFFER_SIZE];
	uint16_t tx_offsets[2];
	uint8_t tx_bank;
	uint8_t rx_bank;
	uint8_t pipe;
};


static struct usb {
	struct usb_cmd cmd[SC_BOARD_CAN_COUNT];
	struct usb_can can[SC_BOARD_CAN_COUNT];
	uint8_t port;
	bool mounted;
} usb;

static inline bool sc_cmd_bulk_in_ep_ready(uint8_t index)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.cmd));
	struct usb_cmd *cmd = &usb.cmd[index];
	return 0 == cmd->tx_offsets[!cmd->tx_bank];
}

static inline void sc_cmd_bulk_in_submit(uint8_t index)
{
	SC_DEBUG_ASSERT(sc_cmd_bulk_in_ep_ready(index));
	struct usb_cmd *cmd = &usb.cmd[index];
	SC_DEBUG_ASSERT(cmd->tx_offsets[cmd->tx_bank] > 0);
	SC_DEBUG_ASSERT(cmd->tx_offsets[cmd->tx_bank] <= CMD_BUFFER_SIZE);
	(void)dcd_edpt_xfer(usb.port, 0x80 | cmd->pipe, cmd->tx_buffers[cmd->tx_bank], cmd->tx_offsets[cmd->tx_bank]);
	cmd->tx_bank = !cmd->tx_bank;
}

SC_RAMFUNC static inline bool sc_can_bulk_in_ep_ready(uint8_t index)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.can));
	struct usb_can *can = &usb.can[index];
	return 0 == can->tx_offsets[!can->tx_bank];
}

SC_RAMFUNC static inline void sc_can_bulk_in_submit(uint8_t index, char const *func)
{
	SC_DEBUG_ASSERT(sc_can_bulk_in_ep_ready(index));
	struct usb_can *can = &usb.can[index];
	SC_DEBUG_ASSERT(can->tx_bank < 2);
	SC_DEBUG_ASSERT(can->tx_offsets[can->tx_bank] > 0);
	// SC_DEBUG_ASSERT(can->tx_offsets[can->tx_bank] <= MSG_BUFFER_SIZE);

	(void)func;

	// LOG("ch%u %s: %u bytes\n", index, func, can->tx_offsets[can->tx_bank]);

#if SUPERCAN_DEBUG

	uint32_t rx_ts_last = 0;
	uint32_t tx_ts_last = 0;



	// LOG("ch%u %s: send %u bytes\n", index, func, can->tx_offsets[can->tx_bank]);
	if (can->tx_offsets[can->tx_bank] > MSG_BUFFER_SIZE) {
		LOG("ch%u %s: msg buffer size %u out of bounds\n", index, func, can->tx_offsets[can->tx_bank]);
		SC_DEBUG_ASSERT(false);
		can->tx_offsets[can->tx_bank] = 0;
		return;
	}

	uint8_t const *sptr = can->tx_buffers[can->tx_bank];
	uint8_t const *eptr = sptr + can->tx_offsets[can->tx_bank];
	uint8_t const *ptr = sptr;

	// LOG("ch%u %s: chunk %u\n", index, func, i);
	// sc_dump_mem(data_ptr, data_size);

	for (; ptr + SC_MSG_HEADER_LEN <= eptr; ) {
		struct sc_msg_header *hdr = (struct sc_msg_header *)ptr;
		if (!hdr->id || !hdr->len) {
			LOG("ch%u %s msg offset %u zero id/len msg\n", index, func, ptr - sptr);
			// sc_dump_mem(sptr, eptr - sptr);
			SC_DEBUG_ASSERT(false);
			can->tx_offsets[can->tx_bank] = 0;
			return;
		}

		if (hdr->len < SC_MSG_HEADER_LEN) {
			LOG("ch%u %s msg offset %u msg header len %u\n", index, func, ptr - sptr, hdr->len);
			SC_DEBUG_ASSERT(false);
			can->tx_offsets[can->tx_bank] = 0;
			return;
		}

		if (ptr + hdr->len > eptr) {
			LOG("ch%u %s msg offset=%u len=%u exceeds buffer len=%u\n", index, func, ptr - sptr, hdr->len, MSG_BUFFER_SIZE);
			SC_DEBUG_ASSERT(false);
			can->tx_offsets[can->tx_bank] = 0;
			return;
		}

		switch (hdr->id) {
		case SC_MSG_CAN_STATUS:
			break;
		case SC_MSG_CAN_RX: {
			struct sc_msg_can_rx const *msg = (struct sc_msg_can_rx const *)hdr;
			uint32_t ts = msg->timestamp_us;
			if (rx_ts_last) {
				uint32_t delta = (ts - rx_ts_last) & CLOCK_MAX;
				bool rx_ts_ok = delta <= CLOCK_MAX / 4;
				if (unlikely(!rx_ts_ok)) {
					LOG("ch%u rx ts=%lx prev=%lx\n", index, ts, rx_ts_last);
					SC_ASSERT(false);
					can->tx_offsets[can->tx_bank] = 0;
					return;
				}
			}
			rx_ts_last = ts;
		} break;
		case SC_MSG_CAN_TXR: {
			struct sc_msg_can_txr const *msg = (struct sc_msg_can_txr const *)hdr;
			uint32_t ts = msg->timestamp_us;
			if (tx_ts_last) {
				uint32_t delta = (ts - tx_ts_last) & CLOCK_MAX;
				bool tx_ts_ok = delta <= CLOCK_MAX / 4;
				if (unlikely(!tx_ts_ok)) {
					LOG("ch%u tx ts=%lx prev=%lx\n", index, ts, tx_ts_last);
					SC_ASSERT(false);
					can->tx_offsets[can->tx_bank] = 0;
					return;
				}
			}
			tx_ts_last = ts;
		} break;
		case SC_MSG_CAN_ERROR:
			break;
		default:
			LOG("ch%u %s msg offset %u non-device msg id %#02x\n", index, func, ptr - sptr, hdr->id);
			can->tx_offsets[can->tx_bank] = 0;
			return;
		}

		ptr += hdr->len;
	}


#endif

	// Required to immediately send URBs when buffer size > endpoint size
	// and the transfer size is a multiple of the endpoint size, and the
	// tranfer size is smaller than the buffer size, we can either send a zlp
	// or increase the payload size.
	if (MSG_BUFFER_SIZE > SC_M1_EP_SIZE) {
		uint16_t offset = can->tx_offsets[can->tx_bank];
		bool need_to_send_zlp = offset < MSG_BUFFER_SIZE && 0 == (offset % SC_M1_EP_SIZE);
		if (need_to_send_zlp) {
			// LOG("zlpfix\n");
			memset(&can->tx_buffers[can->tx_bank][offset], 0, 4);
			can->tx_offsets[can->tx_bank] += 4;
		}
	}

	// LOG("ch%u %s bytes=%u\n", index, func, can->tx_offsets[can->tx_bank]);
	(void)dcd_edpt_xfer(usb.port, 0x80 | can->pipe, can->tx_buffers[can->tx_bank], can->tx_offsets[can->tx_bank]);
	can->tx_bank = !can->tx_bank;
	SC_DEBUG_ASSERT(!can->tx_offsets[can->tx_bank]);
	// memset(can->tx_buffers[can->tx_bank], 0, MSG_BUFFER_SIZE);
	// LOG("ch%u %s sent\n", index, func);
}

static void sc_cmd_bulk_out(uint8_t index, uint32_t xferred_bytes);
static void sc_cmd_bulk_in(uint8_t index);
SC_RAMFUNC static void sc_can_bulk_out(uint8_t index, uint32_t xferred_bytes);
SC_RAMFUNC static void sc_can_bulk_in(uint8_t index);
static void sc_cmd_place_error_reply(uint8_t index, int8_t error);

static void sc_cmd_bulk_out(uint8_t index, uint32_t xferred_bytes)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.cmd));
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.can));

	struct usb_cmd *usb_cmd = &usb.cmd[index];
	struct usb_can *usb_can = &usb.can[index];


	uint8_t const *in_ptr = usb_cmd->rx_buffers[usb_cmd->rx_bank];
	uint8_t const * const in_end = in_ptr + xferred_bytes;

	// setup next transfer
	usb_cmd->rx_bank = !usb_cmd->rx_bank;
	(void)dcd_edpt_xfer(usb.port, usb_cmd->pipe, usb_cmd->rx_buffers[usb_cmd->rx_bank], CMD_BUFFER_SIZE);

	// process messages
	while (in_ptr + SC_MSG_HEADER_LEN <= in_end) {
		struct sc_msg_header const *msg = (struct sc_msg_header const *)in_ptr;
		if (in_ptr + msg->len > in_end) {
			LOG("ch%u malformed msg\n", index);
			break;
		}

		if (!msg->len) {
			break;
		}

		in_ptr += msg->len;

		switch (msg->id) {
		case SC_MSG_EOF: {
			LOG("ch%u SC_MSG_EOF\n", index);
			in_ptr = in_end;
		} break;

		case SC_MSG_HELLO_DEVICE: {
			LOG("ch%u SC_MSG_HELLO_DEVICE\n", index);

			// reset
			sc_board_can_reset(index);

			// transmit empty buffers (clear whatever was in there before)
			(void)dcd_edpt_xfer(usb.port, 0x80 | usb_can->pipe, usb_can->tx_buffers[usb_can->tx_bank], usb_can->tx_offsets[usb_can->tx_bank]);

			// reset tx buffer
			uint8_t len = sizeof(struct sc_msg_hello);
			usb_cmd->tx_offsets[usb_cmd->tx_bank] = len;
			struct sc_msg_hello *rep = (struct sc_msg_hello *)&usb_cmd->tx_buffers[usb_cmd->tx_bank][0];
			rep->id = SC_MSG_HELLO_HOST;
			rep->len = len;
			rep->proto_version = SC_VERSION;
#if TU_BIG_ENDIAN == TU_BYTE_ORDER
			rep->byte_order = SC_BYTE_ORDER_BE;
#else
			rep->byte_order = SC_BYTE_ORDER_LE;
#endif
			rep->cmd_buffer_size = cpu_to_be16(CMD_BUFFER_SIZE);

			// don't process any more messages
			in_ptr = in_end;

			// assume in token is available
		} break;
		case SC_MSG_DEVICE_INFO: {
			LOG("ch%u SC_MSG_DEVICE_INFO\n", index);
			uint8_t bytes = sizeof(struct sc_msg_dev_info);

			uint8_t *out_ptr;
			uint8_t *out_end;

send_dev_info:
			out_ptr = usb_cmd->tx_buffers[usb_cmd->tx_bank] + usb_cmd->tx_offsets[usb_cmd->tx_bank];
			out_end = usb_cmd->tx_buffers[usb_cmd->tx_bank] + CMD_BUFFER_SIZE;
			if (out_end - out_ptr >= bytes) {
				usb_cmd->tx_offsets[usb_cmd->tx_bank] += bytes;
				struct sc_msg_dev_info *rep = (struct sc_msg_dev_info *)out_ptr;
				rep->id = SC_MSG_DEVICE_INFO;
				rep->len = bytes;
				rep->feat_perm = sc_board_can_feat_perm(index);
				rep->feat_conf = sc_board_can_feat_conf(index);
				rep->fw_ver_major = SUPERCAN_VERSION_MAJOR;
				rep->fw_ver_minor = SUPERCAN_VERSION_MINOR;
				rep->fw_ver_patch = SUPERCAN_VERSION_PATCH;
				static const char dev_name[] = SC_BOARD_NAME " " SC_NAME " chX";
				rep->name_len = tu_min8(sizeof(dev_name)-1, sizeof(rep->name_bytes));
				memcpy(rep->name_bytes, dev_name, rep->name_len);
				if (rep->name_len <= TU_ARRAY_SIZE(rep->name_bytes)) {
					rep->name_bytes[rep->name_len-1] = '0' + index;
				}

				rep->sn_bytes[0] = (device_identifier >> 24) & 0xff;
				rep->sn_bytes[1] = (device_identifier >> 16) & 0xff;
				rep->sn_bytes[2] = (device_identifier >> 8) & 0xff;
				rep->sn_bytes[3] = (device_identifier >> 0) & 0xff;
				rep->sn_len = 4;
			} else {
				if (sc_cmd_bulk_in_ep_ready(index)) {
					sc_cmd_bulk_in_submit(index);
					goto send_dev_info;
				} else {
					LOG("no space for device info reply\n");
				}
			}
		} break;
		case SC_MSG_CAN_INFO: {
			LOG("ch%u SC_MSG_CAN_INFO\n", index);
			uint8_t bytes = sizeof(struct sc_msg_can_info);

			uint8_t *out_ptr;
			uint8_t *out_end;

send_can_info:
			out_ptr = usb_cmd->tx_buffers[usb_cmd->tx_bank] + usb_cmd->tx_offsets[usb_cmd->tx_bank];
			out_end = usb_cmd->tx_buffers[usb_cmd->tx_bank] + CMD_BUFFER_SIZE;
			if (out_end - out_ptr >= bytes) {
				struct sc_msg_can_info *rep = (struct sc_msg_can_info *)out_ptr;
				sc_can_bit_timing_range const *nm_bt = sc_board_can_nm_bit_timing_range(index);
				sc_can_bit_timing_range const *dt_bt = sc_board_can_dt_bit_timing_range(index);

				usb_cmd->tx_offsets[usb_cmd->tx_bank] += bytes;

				rep->id = SC_MSG_CAN_INFO;
				rep->len = bytes;
				rep->can_clk_hz = SC_BOARD_CAN_CLK_HZ;
				rep->nmbt_brp_min = nm_bt->min.brp;
				rep->nmbt_brp_max = nm_bt->max.brp;
				rep->nmbt_sjw_max = nm_bt->max.sjw;
				rep->nmbt_tseg1_min = nm_bt->min.tseg1;
				rep->nmbt_tseg1_max = nm_bt->max.tseg1;
				rep->nmbt_tseg2_min = nm_bt->min.tseg2;
				rep->nmbt_tseg2_max = nm_bt->max.tseg2;
				rep->dtbt_brp_min = dt_bt->min.brp;
				rep->dtbt_brp_max = dt_bt->max.brp;
				rep->dtbt_sjw_max = dt_bt->max.sjw;
				rep->dtbt_tseg1_min = dt_bt->min.tseg1;
				rep->dtbt_tseg1_max = dt_bt->max.tseg1;
				rep->dtbt_tseg2_min = dt_bt->min.tseg2;
				rep->dtbt_tseg2_max = dt_bt->max.tseg2;
				rep->tx_fifo_size = SC_BOARD_CAN_TX_FIFO_SIZE;
				rep->rx_fifo_size = SC_BOARD_CAN_RX_FIFO_SIZE;
				rep->msg_buffer_size = MSG_BUFFER_SIZE;
			} else {
				if (sc_cmd_bulk_in_ep_ready(index)) {
					sc_cmd_bulk_in_submit(index);
					goto send_can_info;
				} else {
					LOG("no space for can info reply\n");
				}
			}
		} break;
		case SC_MSG_NM_BITTIMING: {
			LOG("ch%u SC_MSG_NM_BITTIMING\n", index);
			int8_t error = SC_CAN_ERROR_NONE;
			struct sc_msg_bittiming const *tmsg = (struct sc_msg_bittiming const *)msg;
			if (unlikely(msg->len < sizeof(*tmsg))) {
				LOG("ch%u ERROR: msg too short\n", index);
				error = SC_ERROR_SHORT;
			} else {

				sc_can_bit_timing_range const *nm_bt = sc_board_can_nm_bit_timing_range(index);
				sc_can_bit_timing bt_target;

				// clamp
				bt_target.brp = tu_max16(nm_bt->min.brp, tu_min16(tmsg->brp, nm_bt->max.brp));
				bt_target.sjw = tu_max8(nm_bt->min.sjw, tu_min8(tmsg->sjw, nm_bt->max.sjw));
				bt_target.tseg1 = tu_max16(nm_bt->min.tseg1, tu_min16(tmsg->tseg1, nm_bt->max.tseg1));
				bt_target.tseg2 = tu_max8(nm_bt->min.tseg2, tu_min8(tmsg->tseg2, nm_bt->max.tseg2));

				can_log_bit_timing(&bt_target, "nominal");

				sc_board_can_nm_bit_timing_set(index, &bt_target);
			}

			sc_cmd_place_error_reply(index, error);
		} break;
		case SC_MSG_DT_BITTIMING: {
			LOG("ch%u SC_MSG_DT_BITTIMING\n", index);
			int8_t error = SC_CAN_ERROR_NONE;
			struct sc_msg_bittiming const *tmsg = (struct sc_msg_bittiming const *)msg;
			if (unlikely(msg->len < sizeof(*tmsg))) {
				LOG("ch%u ERROR: msg too short\n", index);
				error = SC_ERROR_SHORT;
			} else {

				sc_can_bit_timing_range const *dt_bt = sc_board_can_dt_bit_timing_range(index);
				sc_can_bit_timing bt_target;

				// clamp
				bt_target.brp = tu_max16(dt_bt->min.brp, tu_min16(tmsg->brp, dt_bt->max.brp));
				bt_target.sjw = tu_max8(dt_bt->min.sjw, tu_min8(tmsg->sjw, dt_bt->max.sjw));
				bt_target.tseg1 = tu_max16(dt_bt->min.tseg1, tu_min16(tmsg->tseg1, dt_bt->max.tseg1));
				bt_target.tseg2 = tu_max8(dt_bt->min.tseg2, tu_min8(tmsg->tseg2, dt_bt->max.tseg2));

				can_log_bit_timing(&bt_target, "data");

				sc_board_can_nm_bit_timing_set(index, &bt_target);
			}

			sc_cmd_place_error_reply(index, error);
		} break;
		case SC_MSG_FEATURES: {
			LOG("ch%u SC_MSG_FEATURES\n", index);
			struct sc_msg_features const *tmsg = (struct sc_msg_features const *)msg;
			int8_t error = SC_ERROR_NONE;
			if (unlikely(msg->len < sizeof(*tmsg))) {
				LOG("ch%u ERROR: msg too short\n", index);
				error = SC_ERROR_SHORT;
			} else {
				const uint16_t perm = sc_board_can_feat_perm(index);
				const uint16_t conf = sc_board_can_feat_perm(index);

				switch (tmsg->op) {
				case SC_FEAT_OP_CLEAR:
					can->features = perm;
					LOG("ch%u CLEAR features to %#x\n", index, can->features);
					break;
				case SC_FEAT_OP_OR: {
					uint32_t mode_bits = tmsg->arg & (SC_FEATURE_FLAG_MON_MODE | SC_FEATURE_FLAG_RES_MODE | SC_FEATURE_FLAG_EXT_LOOP_MODE);
					if (__builtin_popcount(mode_bits) > 1) {
						error = SC_ERROR_PARAM;
						LOG("ch%u ERROR: attempt to activate more than one mode %08lx\n", index, mode_bits);
					} else if (tmsg->arg & ~(perm | conf)) {
						error = SC_ERROR_UNSUPPORTED;
						LOG("ch%u ERROR: unsupported features %08lx\n", index, tmsg->arg);
					} else {
						can->features |= tmsg->arg;
						LOG("ch%u OR features to %#x\n", index, can->features);
					}
				} break;
				}
			}
			sc_cmd_place_error_reply(index, error);
		} break;
		case SC_MSG_BUS: {
			LOG("ch%u SC_MSG_BUS\n", index);
			struct sc_msg_config const *tmsg = (struct sc_msg_config const *)msg;
			int8_t error = SC_ERROR_NONE;
			if (unlikely(msg->len < sizeof(*tmsg))) {
				LOG("ERROR: msg too short\n");
				error = SC_ERROR_SHORT;
			} else {
				bool was_enabled = can->enabled;
				bool is_enabled = tmsg->arg != 0;
				if (was_enabled != is_enabled) {
					LOG("ch%u enabled=%u\n", index, is_enabled);
					if (is_enabled) {
						can_on(index);
					} else {
						can_off(index);
					}
				}
			}

			sc_cmd_place_error_reply(index, error);
		} break;
		default:
			TU_LOG2_MEM(msg, msg->len, 2);
			sc_cmd_place_error_reply(index, SC_ERROR_UNSUPPORTED);
			break;
		}
	}

	if (usb_cmd->tx_offsets[usb_cmd->tx_bank] > 0 && sc_cmd_bulk_in_ep_ready(index)) {
		sc_cmd_bulk_in_submit(index);
	}
}

SC_RAMFUNC static void sc_process_msg_can_tx(uint8_t index, struct sc_msg_header const *msg)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.can));
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(cans.can));
	SC_DEBUG_ASSERT(msg);
	SC_DEBUG_ASSERT(SC_MSG_CAN_TX == msg->id);

	struct can *can = &cans.can[index];
	struct usb_can *usb_can = &usb.can[index];

	// LOG("SC_MSG_CAN_TX %lx\n", __atomic_load_n(&can->sync_tscv, __ATOMIC_ACQUIRE));
	struct sc_msg_can_tx const *tmsg = (struct sc_msg_can_tx const *)msg;
	if (unlikely(msg->len < sizeof(*tmsg))) {
		LOG("ch%u ERROR: SC_MSG_CAN_TX msg too short\n", index);
		return;
	}

	const uint8_t can_frame_len = dlc_to_len(tmsg->dlc);
	if (!(tmsg->flags & SC_CAN_FRAME_FLAG_RTR)) {
		if (msg->len < sizeof(*tmsg) + can_frame_len) {
			LOG("ch%u ERROR: SC_MSG_CAN_TX msg too short\n", index);
			return;
		}
	}

	if (can->tx_available) {
		--can->tx_available;

		uint32_t id = tmsg->can_id;
		uint8_t put_index = can->m_can->TXFQS.bit.TFQPI;

		CAN_TXBE_0_Type t0;
		t0.reg = (((tmsg->flags & SC_CAN_FRAME_FLAG_ESI) == SC_CAN_FRAME_FLAG_ESI) << CAN_TXBE_0_ESI_Pos)
			| (((tmsg->flags & SC_CAN_FRAME_FLAG_RTR) == SC_CAN_FRAME_FLAG_RTR) << CAN_TXBE_0_RTR_Pos)
			| (((tmsg->flags & SC_CAN_FRAME_FLAG_EXT) == SC_CAN_FRAME_FLAG_EXT) << CAN_TXBE_0_XTD_Pos)
			;



		if (tmsg->flags & SC_CAN_FRAME_FLAG_EXT) {
			t0.reg |= CAN_TXBE_0_ID(id);
		} else {
			t0.reg |= CAN_TXBE_0_ID(id << 18);
		}

		can->tx_fifo[put_index].T0 = t0;
		can->tx_fifo[put_index].T1.bit.DLC = tmsg->dlc;
		can->tx_fifo[put_index].T1.bit.FDF = (tmsg->flags & SC_CAN_FRAME_FLAG_FDF) == SC_CAN_FRAME_FLAG_FDF;
		can->tx_fifo[put_index].T1.bit.BRS = (tmsg->flags & SC_CAN_FRAME_FLAG_BRS) == SC_CAN_FRAME_FLAG_BRS;
		can->tx_fifo[put_index].T1.bit.MM = tmsg->track_id;

		if (likely(!(tmsg->flags & SC_CAN_FRAME_FLAG_RTR))) {
			if (likely(can_frame_len)) {
				memcpy(can->tx_fifo[put_index].data, tmsg->data, can_frame_len);
			}
		}

		can->m_can->TXBAR.reg = UINT32_C(1) << put_index;
	} else {
		uint8_t * const tx_beg = usb_can->tx_buffers[usb_can->tx_bank];
		uint8_t * const tx_end = tx_beg + TU_ARRAY_SIZE(usb_can->tx_buffers[usb_can->tx_bank]);
		uint8_t *tx_ptr = NULL;
		struct sc_msg_can_txr* rep = NULL;


		++can->tx_dropped;
		counter_1MHz_request_current_value_lazy();

send_txr:
		tx_ptr = tx_beg + usb_can->tx_offsets[usb_can->tx_bank];
		if ((size_t)(tx_end - tx_ptr) >= sizeof(*rep)) {
			usb_can->tx_offsets[usb_can->tx_bank] += sizeof(*rep);

			rep = (struct sc_msg_can_txr*)tx_ptr;
			rep->id = SC_MSG_CAN_TXR;
			rep->len = sizeof(*rep);
			rep->track_id = tmsg->track_id;
			rep->flags = SC_CAN_FRAME_FLAG_DRP;
			uint32_t ts = counter_1MHz_wait_for_current_value();
			rep->timestamp_us = ts;
		} else {
			if (sc_can_bulk_in_ep_ready(index)) {
				sc_can_bulk_in_submit(index, __func__);
				goto send_txr;
			} else {
				LOG("ch%u: desync\n", index);
				can->desync = true;
			}
		}
	}
}

SC_RAMFUNC static void sc_can_bulk_out(uint8_t index, uint32_t xferred_bytes)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.can));
	// SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(cans.can));

	// struct can *can = &cans.can[index];
	struct usb_can *usb_can = &usb.can[index];
	// led_burst(can->led_traffic, LED_BURST_DURATION_MS);


	const uint8_t rx_bank = usb_can->rx_bank;
	(void)rx_bank;
	uint8_t *in_beg = usb_can->rx_buffers[usb_can->rx_bank];
	uint8_t *in_ptr = in_beg;
	uint8_t *in_end = in_ptr + xferred_bytes;


	// start new transfer right away
	usb_can->rx_bank = !usb_can->rx_bank;
	(void)dcd_edpt_xfer(usb.port, usb_can->pipe, usb_can->rx_buffers[usb_can->rx_bank], MSG_BUFFER_SIZE);

	if (unlikely(!xferred_bytes)) {
		return;
	}

	while (pdTRUE != xSemaphoreTake(usb_can->mutex_handle, portMAX_DELAY));

	// LOG("ch%u: bulk out %u bytes\n", index, (unsigned)(in_end - in_beg));


	// LOG("ch%u %s: chunk %u\n", index, func, i);
	// sc_dump_mem(data_ptr, data_size);

	// process messages
	while (in_ptr + SC_MSG_HEADER_LEN <= in_end) {
		struct sc_msg_header const *msg = (struct sc_msg_header const *)in_ptr;
		if (in_ptr + msg->len > in_end) {
			LOG("ch%u offset=%u len=%u exceeds buffer size=%u\n", index, (unsigned)(in_ptr - in_beg), msg->len, (unsigned)xferred_bytes);
			break;
		}

		if (!msg->id || !msg->len) {
			// Allow empty message to work around having to zend ZLP
			// LOG("ch%u offset=%u unexpected zero id/len msg\n", index, (unsigned)(in_ptr - in_beg));
			in_ptr = in_end;
			break;
		}

		in_ptr += msg->len;

		switch (msg->id) {
		case SC_MSG_CAN_TX:
			sc_process_msg_can_tx(index, msg);
			break;

		default:
#if SUPERCAN_DEBUG
			sc_dump_mem(msg, msg->len);
#endif
			break;
		}
	}

	if (sc_can_bulk_in_ep_ready(index) && usb_can->tx_offsets[usb_can->tx_bank]) {
		sc_can_bulk_in_submit(index, __func__);
	}

	xSemaphoreGive(usb_can->mutex_handle);

	// // notify CAN task on bus-off we don't get bittime ticks
	// vTaskNotifyGiveFromISR(can->task_handle, NULL);
}

static void sc_cmd_bulk_in(uint8_t index)
{
	// LOG("< cmd%u IN token\n", index);

	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.cmd));

	struct usb_cmd *usb_cmd = &usb.cmd[index];

	usb_cmd->tx_offsets[!usb_cmd->tx_bank] = 0;

	if (usb_cmd->tx_offsets[usb_cmd->tx_bank]) {
		sc_cmd_bulk_in_submit(index);
	}
}

SC_RAMFUNC static void sc_can_bulk_in(uint8_t index)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.can));

	struct usb_can *usb_can = &usb.can[index];

	while (pdTRUE != xSemaphoreTake(usb_can->mutex_handle, portMAX_DELAY));

	usb_can->tx_offsets[!usb_can->tx_bank] = 0;

	if (usb_can->tx_offsets[usb_can->tx_bank]) {
		sc_can_bulk_in_submit(index, __func__);
	}

	xSemaphoreGive(usb_can->mutex_handle);
}

static void sc_cmd_place_error_reply(uint8_t index, int8_t error)
{
	SC_DEBUG_ASSERT(index < TU_ARRAY_SIZE(usb.cmd));

	struct usb_cmd *usb_cmd = &usb.cmd[index];
	uint8_t bytes = sizeof(struct sc_msg_error);
	uint8_t *out_ptr;
	uint8_t *out_end;

send:
	out_ptr = usb_cmd->tx_buffers[usb_cmd->tx_bank] + usb_cmd->tx_offsets[usb_cmd->tx_bank];
	out_end = usb_cmd->tx_buffers[usb_cmd->tx_bank] + CMD_BUFFER_SIZE;
	if (out_end - out_ptr >= bytes) {
		usb_cmd->tx_offsets[usb_cmd->tx_bank] += bytes;
		struct sc_msg_error *rep = (struct sc_msg_error *)out_ptr;
		rep->id = SC_MSG_ERROR;
		rep->len = sizeof(*rep);
		rep->error = error;
	} else {
		if (sc_cmd_bulk_in_ep_ready(index)) {
			sc_cmd_bulk_in_submit(index);
			goto send;
		} else {
			LOG("ch%u: no space for error reply\n", index);
		}
	}
}



int main(void)
{
	sc_board_init();
	led_init();
	tusb_init();


	(void) xTaskCreateStatic(&tusb_device_task, "tusb", TU_ARRAY_SIZE(usb_device_stack), NULL, configMAX_PRIORITIES-1, usb_device_stack, &usb_device_stack_mem);
	(void) xTaskCreateStatic(&led_task, "led", TU_ARRAY_SIZE(led_task_stack), NULL, configMAX_PRIORITIES-1, led_task_stack, &led_task_mem);

	usb.can[0].mutex_handle = xSemaphoreCreateMutexStatic(&usb.can[0].mutex_mem);
	usb.can[1].mutex_handle = xSemaphoreCreateMutexStatic(&usb.can[1].mutex_mem);

	// cans.can[0].led_status_green = LED_CAN0_STATUS_GREEN;
	// cans.can[0].led_status_red = LED_CAN0_STATUS_RED;
	// cans.can[1].led_status_green = LED_CAN1_STATUS_GREEN;
	// cans.can[1].led_status_red = LED_CAN1_STATUS_RED;
	// cans.can[0].led_traffic = CAN0_TRAFFIC_LED;
	// cans.can[1].led_traffic = CAN1_TRAFFIC_LED;
	// cans.can[0].usb_task_handle = xTaskCreateStatic(&can_usb_task, "usb_can0", TU_ARRAY_SIZE(cans.can[0].usb_task_stack_mem), (void*)(uintptr_t)0, configMAX_PRIORITIES-1, cans.can[0].usb_task_stack_mem, &cans.can[0].usb_task_mem);
	// cans.can[1].usb_task_handle = xTaskCreateStatic(&can_usb_task, "usb_can1", TU_ARRAY_SIZE(cans.can[1].usb_task_stack_mem), (void*)(uintptr_t)1, configMAX_PRIORITIES-1, cans.can[1].usb_task_stack_mem, &cans.can[1].usb_task_mem);



	sc_board_init_end();

	// while (1) {
	// 	uint32_t c = counter_1MHz_read_sync();
	// 	counter_1MHz_request_current_value();
	// 	uint32_t x = 0;
	// 	while (!counter_1MHz_is_current_value_ready()) {
	// 		++x;
	// 	}

	// 	LOG("c=%lx, wait=%lx\n", c, x);
	// }

	vTaskStartScheduler();

	sc_board_reset();

	return 0;
}


//--------------------------------------------------------------------+
// USB DEVICE TASK
//--------------------------------------------------------------------+
SC_RAMFUNC static void tusb_device_task(void* param)
{
	(void) param;

	while (1) {
		LOG("tud_task\n");
		tud_task();
	}
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
	LOG("mounted\n");
	led_blink(0, 250);
	usb.mounted = true;

	cans_led_status_set(CANLED_STATUS_DISABLED);
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
	LOG("unmounted\n");
	led_blink(0, 1000);
	usb.mounted = false;

	cans_reset();
	cans_led_status_set(CANLED_STATUS_DISABLED);
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
	(void) remote_wakeup_en;
	LOG("suspend\n");
	usb.mounted = false;
	led_blink(0, 500);

	cans_reset();
	cans_led_status_set(CANLED_STATUS_DISABLED);
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
	LOG("resume\n");
	usb.mounted = true;
	led_blink(0, 250);
}


//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+
static inline const char* recipient_str(tusb_request_recipient_t r)
{
	switch (r) {
	case TUSB_REQ_RCPT_DEVICE:
		return "device (0)";
	case TUSB_REQ_RCPT_INTERFACE:
		return "interface (1)";
	case TUSB_REQ_RCPT_ENDPOINT:
		return "endpoint (2)";
	case TUSB_REQ_RCPT_OTHER:
		return "other (3)";
	default:
		return "???";
	}
}

static inline const char* type_str(tusb_request_type_t value)
{
	switch (value) {
	case TUSB_REQ_TYPE_STANDARD:
		return "standard (0)";
	case TUSB_REQ_TYPE_CLASS:
		return "class (1)";
	case TUSB_REQ_TYPE_VENDOR:
		return "vendor (2)";
	case TUSB_REQ_TYPE_INVALID:
		return "invalid (3)";
	default:
		return "???";
	}
}

static inline const char* dir_str(tusb_dir_t value)
{
	switch (value) {
	case TUSB_DIR_OUT:
		return "out (0)";
	case TUSB_DIR_IN:
		return "in (1)";
	default:
		return "???";
	}
}





static void sc_usb_init(void)
{
	LOG("SC init\n");
}

static void sc_usb_reset(uint8_t rhport)
{
	LOG("SC port %u reset\n", rhport);
	usb.mounted = false;
	usb.port = rhport;
	usb.cmd[0].pipe = SC_M1_EP_CMD0_BULK_OUT;
	usb.cmd[0].tx_offsets[0] = 0;
	usb.cmd[0].tx_offsets[1] = 0;
	usb.cmd[1].pipe = SC_M1_EP_CMD1_BULK_OUT;
	usb.cmd[1].tx_offsets[0] = 0;
	usb.cmd[1].tx_offsets[1] = 0;
	usb.can[0].pipe = SC_M1_EP_MSG0_BULK_OUT;
	usb.can[0].tx_offsets[0] = 0;
	usb.can[0].tx_offsets[1] = 0;
	usb.can[1].pipe = SC_M1_EP_MSG1_BULK_OUT;
	usb.can[1].tx_offsets[0] = 0;
	usb.can[1].tx_offsets[1] = 0;
}

static uint16_t sc_usb_open(uint8_t rhport, tusb_desc_interface_t const * desc_intf, uint16_t max_len)
{
	const uint8_t eps = 4;
	const uint16_t len_required = 9+eps*7;

	LOG("vendor port %u open\n", rhport);

	if (unlikely(rhport != usb.port)) {
		return 0;
	}

	if (unlikely(max_len < len_required)) {
		return 0;
	}

	TU_VERIFY(TUSB_CLASS_VENDOR_SPECIFIC == desc_intf->bInterfaceClass);

	if (unlikely(desc_intf->bInterfaceNumber >= TU_ARRAY_SIZE(usb.can))) {
		return 0;
	}


	struct usb_cmd *usb_cmd = &usb.cmd[desc_intf->bInterfaceNumber];
	struct usb_can *usb_can = &usb.can[desc_intf->bInterfaceNumber];

	uint8_t const *ptr = (void const *)desc_intf;

	ptr += 9;



	for (uint8_t i = 0; i < eps; ++i) {
		tusb_desc_endpoint_t const *ep_desc = (tusb_desc_endpoint_t const *)(ptr + i * 7);
		LOG("! ep %02x open\n", ep_desc->bEndpointAddress);
		bool success = dcd_edpt_open(rhport, ep_desc);
		SC_ASSERT(success);
	}

	bool success_cmd = dcd_edpt_xfer(rhport, usb_cmd->pipe, usb_cmd->rx_buffers[usb_cmd->rx_bank], CMD_BUFFER_SIZE);
	bool success_can = dcd_edpt_xfer(rhport, usb_can->pipe, usb_can->rx_buffers[usb_can->rx_bank], MSG_BUFFER_SIZE);
	SC_ASSERT(success_cmd);
	SC_ASSERT(success_can);

	// // Required to immediately send URBs when buffer size > endpoint size
	// // and transfers are multiple of enpoint size.
	// if (MSG_BUFFER_SIZE > SC_M1_EP_SIZE) {
	// 	dcd_auto_zlp(rhport, usb_can->pipe | 0x80, true);
	// }

	return len_required;
}

SC_RAMFUNC static bool sc_usb_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
{
	(void)result; // always success

	if (unlikely(rhport != usb.port)) {
		return false;
	}

	USB_TRAFFIC_DO_LED;



	switch (ep_addr) {
	case SC_M1_EP_CMD0_BULK_OUT:
		sc_cmd_bulk_out(0, xferred_bytes);
		break;
	case SC_M1_EP_CMD1_BULK_OUT:
		sc_cmd_bulk_out(1, xferred_bytes);
		break;
	case SC_M1_EP_CMD0_BULK_IN:
		sc_cmd_bulk_in(0);
		break;
	case SC_M1_EP_CMD1_BULK_IN:
		sc_cmd_bulk_in(1);
		break;
	case SC_M1_EP_MSG0_BULK_OUT:
		sc_can_bulk_out(0, xferred_bytes);
		break;
	case SC_M1_EP_MSG1_BULK_OUT:
		sc_can_bulk_out(1, xferred_bytes);
		break;
	case SC_M1_EP_MSG0_BULK_IN:
		sc_can_bulk_in(0);
		break;
	case SC_M1_EP_MSG1_BULK_IN:
		sc_can_bulk_in(1);
		break;
	default:
		LOG("port %u ep %02x result %d bytes %u\n", rhport, ep_addr, result, (unsigned)xferred_bytes);
		return false;
	}

	return true;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const * request)
{
	LOG("port=%u stage=%u\n", rhport, stage);

	USB_TRAFFIC_DO_LED;

	if (unlikely(rhport != usb.port)) {
		return false;
	}

	switch (stage) {
	case CONTROL_STAGE_SETUP: {
		switch (request->bRequest) {
		case VENDOR_REQUEST_MICROSOFT:
			if (request->wIndex == 7) {
				// Get Microsoft OS 2.0 compatible descriptor
				uint16_t total_len;
				memcpy(&total_len, desc_ms_os_20+8, 2);
				total_len = le16_to_cpu(total_len);
				return tud_control_xfer(rhport, request, (void*)desc_ms_os_20, total_len);
			}
			break;
		default:
			LOG("req type 0x%02x (reci %s type %s dir %s) req 0x%02x, value 0x%04x index 0x%04x reqlen %u\n",
				request->bmRequestType,
				recipient_str(request->bmRequestType_bit.recipient),
				type_str(request->bmRequestType_bit.type),
				dir_str(request->bmRequestType_bit.direction),
				request->bRequest, request->wValue, request->wIndex,
				request->wLength);
			break;
		}
	} break;
	case CONTROL_STAGE_DATA:
	case CONTROL_STAGE_ACK:
		switch (request->bRequest) {
		case VENDOR_REQUEST_MICROSOFT:
			return true;
		default:
			break;
		}
	default:
		break;
	}

	// stall unknown request
	return false;
}

#if CFG_TUD_DFU_RUNTIME
void tud_dfu_runtime_reboot_to_dfu_cb(uint16_t ms)
{
	LOG("tud_dfu_runtime_reboot_to_dfu_cb\n");
	/* The timer seems to be necessary, else dfu-util
	 * will fail spurriously with EX_IOERR (74).
	 */
	xTimerStart(dfu.timer_handle, pdMS_TO_TICKS(ms));

	// dfu_request_dfu(1);
	// NVIC_SystemReset();
}
#endif // #if CFG_TUD_DFU_RT

static const usbd_class_driver_t sc_usb_driver = {
#if CFG_TUSB_DEBUG >= 2
	.name = "SC",
#endif
	.init = &sc_usb_init,
	.reset = &sc_usb_reset,
	.open = &sc_usb_open,
	/* TinyUSB doesn't call this callback for vendor requests
	 * but tud_vendor_control_xfer_cb. Sigh :/
	 */
	.control_xfer_cb = NULL,
	.xfer_cb = &sc_usb_xfer_cb,
	.sof = NULL,
};

usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count)
{
	SC_ASSERT(driver_count);
	*driver_count = 1;
	return &sc_usb_driver;
}



//--------------------------------------------------------------------+
// CAN TASK
//--------------------------------------------------------------------+
#if SPAM
SC_RAMFUNC static void can_usb_task(void *param)
{
	const uint8_t index = (uint8_t)(uintptr_t)param;
	SC_ASSERT(index < TU_ARRAY_SIZE(cans.can));
	SC_ASSERT(index < TU_ARRAY_SIZE(usb.can));

	LOG("ch%u task start\n", index);

	struct can *can = &cans.can[index];
	struct usb_can *usb_can = &usb.can[index];

	unsigned next_dlc = 0;
	uint32_t counter = 0;
	uint32_t can_id = 0x42;


	while (42) {
		// LOG("CAN%u task wait\n", index);
		(void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

		// LOG("CAN%u task loop\n", index);
		if (unlikely(!usb.mounted)) {
			continue;
		}

		if (unlikely(!can->enabled)) {
			next_dlc = 0;
			counter = 0;
			LOG("ch%u usb state reset\n", index);
			continue;
		}

		led_burst(can->led_traffic, LED_BURST_DURATION_MS);

		while (pdTRUE != xSemaphoreTake(usb_can->mutex_handle, portMAX_DELAY));

		uint8_t * const tx_beg = usb_can->tx_buffers[usb_can->tx_bank];
		uint8_t * const tx_end = tx_beg + TU_ARRAY_SIZE(usb_can->tx_buffers[usb_can->tx_bank]);
		uint8_t *tx_ptr = tx_beg + usb_can->tx_offsets[usb_can->tx_bank];

		for (;;) {

			// consume all input
			__atomic_store_n(&can->rx_get_index, __atomic_load_n(&can->rx_put_index, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);

			uint8_t bytes = sizeof(struct sc_msg_can_rx);


			uint8_t dlc = next_dlc & 0xf;
			if (!dlc) {
				++dlc;
			}
			uint8_t can_frame_len = dlc_to_len(dlc);
			bytes += can_frame_len;
			if (bytes & (SC_MSG_CAN_LEN_MULTIPLE-1)) {
				bytes += SC_MSG_CAN_LEN_MULTIPLE - (bytes & (SC_MSG_CAN_LEN_MULTIPLE-1));
			}

			if ((size_t)(tx_end - tx_ptr) >= bytes) {
				counter_1MHz_request_current_value_lazy();
				struct sc_msg_can_rx *msg = (struct sc_msg_can_rx *)tx_ptr;
				usb_can->tx_offsets[usb_can->tx_bank] += bytes;
				tx_ptr += bytes;


				msg->id = SC_MSG_CAN_RX;
				msg->len = bytes;
				msg->dlc = dlc;
				msg->flags = SC_CAN_FRAME_FLAG_FDF | SC_CAN_FRAME_FLAG_BRS;
				msg->can_id = can_id;
				memset(msg->data, 0, can_frame_len);
				memcpy(&msg->data, &counter, sizeof(counter));
				msg->timestamp_us = counter_1MHz_wait_for_current_value();
				// LOG("ts=%lx\n", msg->timestamp_us);

				++next_dlc;
				++counter;

			} else {
				break;
			}
		}

		if (sc_can_bulk_in_ep_ready(index) && usb_can->tx_offsets[usb_can->tx_bank]) {
			sc_can_bulk_in_submit(index, __func__);
		}

		xSemaphoreGive(usb_can->mutex_handle);
	}
}
#else
SC_RAMFUNC static void can_usb_task(void *param)
{
	const unsigned BUS_ACTIVITY_TIMEOUT_MS = 256;

	const uint8_t index = (uint8_t)(uintptr_t)param;
	SC_ASSERT(index < TU_ARRAY_SIZE(cans.can));
	SC_ASSERT(index < TU_ARRAY_SIZE(usb.can));

	LOG("ch%u task start\n", index);

	struct can *can = &cans.can[index];
	struct usb_can *usb_can = &usb.can[index];
// #if SUPERCAN_DEBUG
// 	uint32_t rx_ts_last = 0;
// 	uint32_t tx_ts_last = 0;
// #endif
	uint8_t previous_bus_status = 0;
	uint8_t current_bus_status = 0;
	TickType_t bus_activity_tc = 0;
	bool had_bus_activity = false;
	bool has_bus_error = false;
	bool had_bus_error = false;
	bool send_can_status = 0;
	bool yield = false;


	while (42) {
		// LOG("CAN%u task wait\n", index);
		(void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

		// LOG("CAN%u task loop\n", index);
		if (unlikely(!usb.mounted)) {
			continue;
		}

		if (unlikely(!can->enabled)) {
			current_bus_status = 0;
			bus_activity_tc = xTaskGetTickCount() - pdMS_TO_TICKS(BUS_ACTIVITY_TIMEOUT_MS);
			had_bus_activity = false;
			has_bus_error = false;
			had_bus_error = false;
// #if SUPERCAN_DEBUG
// 			rx_ts_last = 0;
// 			tx_ts_last = 0;
// #endif
			LOG("ch%u usb state reset\n", index);
			continue;
		}

		led_burst(can->led_traffic, LED_BURST_DURATION_MS);
		send_can_status = 1;

		while (pdTRUE != xSemaphoreTake(usb_can->mutex_handle, portMAX_DELAY));

		for (bool done = false; !done; ) {
			done = true;

// #if SUPERCAN_DEBUG
// 			// loop
// 			{
// 				uint32_t ts = counter_1MHz_read_sync(index);
// 				if (ts - rx_ts_last >= 0x20000000) {
// 					rx_ts_last = ts - 0x20000000;
// 				}

// 				if (ts - tx_ts_last >= 0x20000000) {
// 					tx_ts_last = ts - 0x20000000;
// 				}
// 			}
// #endif
			uint8_t * const tx_beg = usb_can->tx_buffers[usb_can->tx_bank];
			uint8_t * const tx_end = tx_beg + TU_ARRAY_SIZE(usb_can->tx_buffers[usb_can->tx_bank]);
			uint8_t *tx_ptr = tx_beg + usb_can->tx_offsets[usb_can->tx_bank];

			if (send_can_status) {
				struct sc_msg_can_status *msg = NULL;
				if ((size_t)(tx_end - tx_ptr) >= sizeof(*msg)) {
					done = false;
					send_can_status = 0;
					counter_1MHz_request_current_value_lazy();



					msg = (struct sc_msg_can_status *)tx_ptr;
					usb_can->tx_offsets[usb_can->tx_bank] += sizeof(*msg);
					tx_ptr += sizeof(*msg);


					uint16_t rx_lost = __sync_fetch_and_and(&can->rx_lost, 0);
					uint16_t tx_dropped = can->tx_dropped;
					can->tx_dropped = 0;

					// LOG("status ts %lu\n", ts);
					CAN_ECR_Type ecr = can->m_can->ECR;


					msg->id = SC_MSG_CAN_STATUS;
					msg->len = sizeof(*msg);

					msg->rx_lost = rx_lost;
					msg->tx_dropped = tx_dropped;
					msg->flags = __sync_or_and_fetch(&can->int_comm_flags, 0);
					if (can->desync) {
						msg->flags |= SC_CAN_STATUS_FLAG_TXR_DESYNC;
					}

					msg->bus_status = current_bus_status;
					msg->tx_errors = ecr.bit.TEC;
					msg->rx_errors = ecr.bit.REC;
					msg->tx_fifo_size = CAN_TX_FIFO_SIZE - can->m_can->TXFQS.bit.TFFL;
					msg->rx_fifo_size = can->m_can->RXF0S.bit.F0FL;
					msg->timestamp_us = counter_1MHz_wait_for_current_value();


					// LOG("status store %u bytes\n", (unsigned)sizeof(*msg));
					// sc_dump_mem(msg, sizeof(*msg));
				} else {
					if (sc_can_bulk_in_ep_ready(index)) {
						done = false;
						sc_can_bulk_in_submit(index, __func__);
						continue;
					} else {
						xTaskNotifyGive(can->usb_task_handle);
						yield = true;
						break;
					}
				}
			}

			uint16_t status_put_index = __atomic_load_n(&can->status_put_index, __ATOMIC_ACQUIRE);
			if (can->status_get_index != status_put_index) {
				uint16_t fifo_index = can->status_get_index % TU_ARRAY_SIZE(can->status_fifo);
				struct can_status *s = &can->status_fifo[fifo_index];

				done = false;
				bus_activity_tc = xTaskGetTickCount();

				switch (s->type) {
				case CAN_STATUS_FIFO_TYPE_BUS_STATUS: {
					current_bus_status = s->payload;
					LOG("ch%u bus status %#x\n", index, current_bus_status);
					send_can_status = 1;
				} break;
				case CAN_STATUS_FIFO_TYPE_BUS_ERROR: {
					struct sc_msg_can_error *msg = NULL;

					has_bus_error = true;

					if ((size_t)(tx_end - tx_ptr) >= sizeof(*msg)) {
						msg = (struct sc_msg_can_error *)tx_ptr;
						usb_can->tx_offsets[usb_can->tx_bank] += sizeof(*msg);
						tx_ptr += sizeof(*msg);


						msg->id = SC_MSG_CAN_ERROR;
						msg->len = sizeof(*msg);
						msg->error = s->payload;
						msg->timestamp_us = s->ts;
						msg->flags = 0;
						if (s->tx) {
							msg->flags |= SC_CAN_ERROR_FLAG_RXTX_TX;
						}
						if (s->data_part) {
							msg->flags |= SC_CAN_ERROR_FLAG_NMDT_DT;
						}
					} else {
						if (sc_can_bulk_in_ep_ready(index)) {
							sc_can_bulk_in_submit(index, __func__);
							continue;
						} else {
							// LOG("ch%u dropped CAN bus error msg\n", index);
							// break;
							xTaskNotifyGive(can->usb_task_handle);
							yield = true;
						}
					}
				} break;
				default:
					LOG("ch%u unhandled CAN status message type %#02x\n", index, s->type);
					break;
				}

				__atomic_store_n(&can->status_get_index, can->status_get_index+1, __ATOMIC_RELEASE);
			}



			uint8_t rx_put_index = __atomic_load_n(&can->rx_put_index, __ATOMIC_ACQUIRE);
			if (can->rx_get_index != rx_put_index) {
				__atomic_thread_fence(__ATOMIC_ACQUIRE);
				uint8_t rx_count = rx_put_index - can->rx_get_index;
				if (unlikely(rx_count > CAN_RX_FIFO_SIZE)) {
					LOG("ch%u rx count %u\n", index, rx_count);
					SC_ASSERT(rx_put_index - can->rx_get_index <= CAN_RX_FIFO_SIZE);
				}

				has_bus_error = false;
				bus_activity_tc = xTaskGetTickCount();
				uint8_t get_index = can->rx_get_index & (CAN_RX_FIFO_SIZE-1);
				uint8_t bytes = sizeof(struct sc_msg_can_rx);
				CAN_RXF0E_0_Type r0 = can->rx_frames[get_index].R0;
				CAN_RXF0E_1_Type r1 = can->rx_frames[get_index].R1;
				uint8_t can_frame_len = dlc_to_len(r1.bit.DLC);
				if (!r0.bit.RTR) {
					bytes += can_frame_len;
				}

				// align
				if (bytes & (SC_MSG_CAN_LEN_MULTIPLE-1)) {
					bytes += SC_MSG_CAN_LEN_MULTIPLE - (bytes & (SC_MSG_CAN_LEN_MULTIPLE-1));
				}


				if ((size_t)(tx_end - tx_ptr) >= bytes) {
					done = false;

					// LOG("rx %u bytes\n", bytes);
					struct sc_msg_can_rx *msg = (struct sc_msg_can_rx *)tx_ptr;
					usb_can->tx_offsets[usb_can->tx_bank] += bytes;
					tx_ptr += bytes;


					msg->id = SC_MSG_CAN_RX;
					msg->len = bytes;
					msg->dlc = r1.bit.DLC;
					msg->flags = 0;
					uint32_t id = r0.bit.ID;
					if (r0.bit.XTD) {
						msg->flags |= SC_CAN_FRAME_FLAG_EXT;
					} else {
						id >>= 18;
					}
					msg->can_id = id;


					uint32_t ts = can->rx_frames[get_index].ts;
// #if SUPERCAN_DEBUG
// 					uint32_t delta = (ts - rx_ts_last) & CLOCK_MAX;
// 					bool rx_ts_ok = delta <= CLOCK_MAX / 4;
// 					if (unlikely(!rx_ts_ok)) {
// 						taskDISABLE_INTERRUPTS();
// 						// SC_BOARD_CAN_init_begin(can);
// 						LOG("ch%u rx gi=%u ts=%lx prev=%lx\n", index, get_index, ts, rx_ts_last);
// 						for (unsigned i = 0; i < CAN_RX_FIFO_SIZE; ++i) {
// 							LOG("ch%u rx gi=%u ts=%lx\n", index, i, can->rx_frames[i].ts);
// 						}

// 					}
// 					SC_ASSERT(rx_ts_ok);
// 					// LOG("ch%u rx gi=%u d=%lx\n", index, get_index, ts - rx_ts_last);
// 					rx_ts_last = ts;
// #endif

					msg->timestamp_us = ts;

					// LOG("ch%u rx i=%u rx hi=%x lo=%x tscv hi=%x lo=%x\n", index, get_index, rx_high, rx_low, tscv_high, tscv_low);
					// LOG("ch%u rx i=%u rx=%lx\n", index, get_index, ts);


					// LOG("ch%u rx ts %lu\n", index, msg->timestamp_us);

					if (r1.bit.FDF) {
						msg->flags |= SC_CAN_FRAME_FLAG_FDF;
						if (r1.bit.BRS) {
							msg->flags |= SC_CAN_FRAME_FLAG_BRS;
						}

						memcpy(msg->data, can->rx_frames[get_index].data, can_frame_len);
					} else {
						if (r0.bit.RTR) {
							msg->flags |= SC_CAN_FRAME_FLAG_RTR;
						} else {
							memcpy(msg->data, can->rx_frames[get_index].data, can_frame_len);
						}
					}

					// LOG("rx store %u bytes\n", bytes);
					// sc_dump_mem(msg, bytes);

					__atomic_store_n(&can->rx_get_index, can->rx_get_index+1, __ATOMIC_RELEASE);
				} else {
					if (sc_can_bulk_in_ep_ready(index)) {
						done = false;
						sc_can_bulk_in_submit(index, __func__);
						continue;
					} else {
						xTaskNotifyGive(can->usb_task_handle);
						yield = true;
						break;
					}
				}
			}

			uint8_t tx_put_index = __atomic_load_n(&can->tx_put_index, __ATOMIC_ACQUIRE);
			if (can->tx_get_index != tx_put_index) {
				SC_DEBUG_ASSERT(tx_put_index - can->tx_get_index <= CAN_TX_FIFO_SIZE);

				has_bus_error = false;
				bus_activity_tc = xTaskGetTickCount();
				uint8_t get_index = can->tx_get_index & (CAN_TX_FIFO_SIZE-1);
				struct sc_msg_can_txr *msg = NULL;
				if ((size_t)(tx_end - tx_ptr) >= sizeof(*msg)) {
					// LOG("1\n");
					__atomic_thread_fence(__ATOMIC_ACQUIRE);
					done = false;


					msg = (struct sc_msg_can_txr *)tx_ptr;
					usb_can->tx_offsets[usb_can->tx_bank] += sizeof(*msg);
					tx_ptr += sizeof(*msg);

					CAN_TXEFE_0_Type t0 = can->tx_frames[get_index].T0;
					CAN_TXEFE_1_Type t1 = can->tx_frames[get_index].T1;

					msg->id = SC_MSG_CAN_TXR;
					msg->len = sizeof(*msg);
					msg->track_id = t1.bit.MM;

					uint32_t ts = can->tx_frames[get_index].ts;
// #if SUPERCAN_DEBUG
// 					// bool tx_ts_ok = ts >= tx_ts_last || (TS_HI(tx_ts_last) == 0xffff && TS_HI(ts) == 0);
// 					uint32_t delta = (ts - tx_ts_last) & CLOCK_MAX;
// 					bool tx_ts_ok = delta <= CLOCK_MAX / 4;
// 					if (unlikely(!tx_ts_ok)) {
// 						taskDISABLE_INTERRUPTS();
// 						// SC_BOARD_CAN_init_begin(can);
// 						LOG("ch%u tx gi=%u ts=%lx prev=%lx\n", index, get_index, ts, tx_ts_last);
// 						for (unsigned i = 0; i < CAN_TX_FIFO_SIZE; ++i) {
// 							LOG("ch%u tx gi=%u ts=%lx\n", index, i, can->tx_frames[i].ts);
// 						}

// 					}
// 					SC_ASSERT(tx_ts_ok);
// 					// LOG("ch%u tx gi=%u d=%lx\n", index, get_index, ts - tx_ts_last);
// 					tx_ts_last = ts;
// #endif
					msg->timestamp_us = ts;
					msg->flags = 0;

					// Report the available flags back so host code
					// needs to store less information.
					if (t0.bit.XTD) {
						msg->flags |= SC_CAN_FRAME_FLAG_EXT;
					}

					if (t1.bit.FDF) {
						msg->flags |= SC_CAN_FRAME_FLAG_FDF;

						if (t0.bit.ESI) {
							msg->flags |= SC_CAN_FRAME_FLAG_ESI;
						}

						if (t1.bit.BRS) {
							msg->flags |= SC_CAN_FRAME_FLAG_BRS;
						}
					} else {
						if (t0.bit.RTR) {
							msg->flags |= SC_CAN_FRAME_FLAG_RTR;
						}
					}

					// LOG("2\n");

					__atomic_store_n(&can->tx_get_index, can->tx_get_index+1, __ATOMIC_RELEASE);
					SC_ASSERT(can->tx_available < CAN_TX_FIFO_SIZE);
					++can->tx_available;
					// LOG("3\n");
				} else {
					if (sc_can_bulk_in_ep_ready(index)) {
						done = false;
						sc_can_bulk_in_submit(index, __func__);
						continue;
					} else {
						xTaskNotifyGive(can->usb_task_handle);
						yield = true;
						break;
					}
				}
			}
		}

		if (sc_can_bulk_in_ep_ready(index) && usb_can->tx_offsets[usb_can->tx_bank]) {
			sc_can_bulk_in_submit(index, __func__);
		}

		const bool has_bus_activity = xTaskGetTickCount() - bus_activity_tc < pdMS_TO_TICKS(BUS_ACTIVITY_TIMEOUT_MS);
		bool led_change =
			has_bus_activity != had_bus_activity ||
			has_bus_error != had_bus_error;
		if (!led_change) {
			if (previous_bus_status >= SC_CAN_STATUS_ERROR_PASSIVE &&
				current_bus_status < SC_CAN_STATUS_ERROR_PASSIVE) {
				led_change = true;
			} else if (previous_bus_status < SC_CAN_STATUS_ERROR_PASSIVE &&
				current_bus_status >= SC_CAN_STATUS_ERROR_PASSIVE) {
				led_change = true;
			}
		}

		if (led_change) {
			if (has_bus_error || current_bus_status >= SC_CAN_STATUS_ERROR_PASSIVE) {
				canled_set_status(can, has_bus_activity ? CANLED_STATUS_ERROR_ACTIVE : CANLED_STATUS_ERROR_PASSIVE);
			} else {
				canled_set_status(can, has_bus_activity ? CANLED_STATUS_ENABLED_BUS_ON_ACTIVE : CANLED_STATUS_ENABLED_BUS_ON_PASSIVE);
			}
		}

		had_bus_activity = has_bus_activity;
		had_bus_error = has_bus_error;
		previous_bus_status = current_bus_status;

		xSemaphoreGive(usb_can->mutex_handle);

		// LOG("|");

		if (yield) {
			// yield to prevent this task from eating up the CPU
			// when the USB buffers are full/busy.
			yield = false;
			// taskYIELD();
			vTaskDelay(pdMS_TO_TICKS(1)); // 1ms for USB FS
		}
	}
}
#endif // !SPAM


SC_RAMFUNC static inline void can_frame_bits(
	uint32_t xtd,
	uint32_t rtr,
	uint32_t fdf,
	uint32_t brs,
	uint8_t dlc,
	uint32_t* nmbr_bits,
	uint32_t* dtbr_bits)
{
	uint32_t payload_bits = dlc_to_len(dlc) * UINT32_C(8); /* payload */

	/* For SOF / interframe spacing, see ISO 11898-1:2015(E) 10.4.2.2 SOF
	 *
	 * Since the third bit (if dominant) in the interframe space marks
	 * SOF, there could be sitiuations in which the IFS is only 2 bit times
	 * long. The solution adopted here is to compute including 1 bit time SOF and shorted
	 * IFS to 2.
	 */

	if (fdf) {
		// FD frames have a 3 bit stuff count field and a 1 bit parity field prior to the actual checksum
		// There is a stuff bit at the begin of the stuff count field (always) and then at fixed positions
		// every 4 bits.
		uint32_t crc_bits = dlc <= 10 ? (17+4+5) : (21+4+6);

		if (brs) {
			*dtbr_bits =
				1 /* ESI */
				+ 4 /* DLC */
				+ payload_bits
				+ crc_bits; /* CRC */

			if (xtd) {
				*nmbr_bits =
					1 /* SOF? */
					+ 11 /* ID */
					+ 1 /* SRR */
					+ 1 /* IDE */
					+ 18 /* ID */
					+ 1 /* reserved 0 */
					+ 1 /* EDL */
					+ 1 /* reserved 0 */
					+ 1 /* BRS */
					// + 1 /* ESI */
					// + 4 /* DLC */
					// + dlc_to_len(dlc) * UINT32_C(8) /* payload */
					// + /* CRC */
					+ 1 /* CRC delimiter */
					+ 1 /* ACK slot */
					+ 1 /* ACK delimiter */
					+ 7 /* EOF */
					+ 2; /* INTERFRAME SPACE: INTERMISSION (3) + (SUSPEND TRANSMISSION)? + (BUS IDLE)? */

			} else {
				*nmbr_bits =
					1 /* SOF */
					+ 11 /* ID */
					+ 1 /* reserved 1 */
					+ 1 /* IDE */
					+ 1 /* EDL */
					+ 1 /* reserved 0 */
					+ 1 /* BRS */
					// + 1 /* ESI */
					// + 4 /* DLC */
					// + dlc_to_len(dlc) * UINT32_C(8) /* payload */
					// + /* CRC */
					+ 1 /* CRC delimiter */
					+ 1 /* ACK slot */
					+ 1 /* ACK delimiter */
					+ 7 /* EOF */
					+ 2; /* INTERFRAME SPACE: INTERMISSION (3) + (SUSPEND TRANSMISSION)? + (BUS IDLE)? */
			}
		} else {
			*dtbr_bits = 0;

			if (xtd) {
				*nmbr_bits =
					1 /* SOF */
					+ 11 /* ID */
					+ 1 /* SRR */
					+ 1 /* IDE */
					+ 18 /* ID */
					+ 1 /* reserved 0 */
					+ 1 /* EDL */
					+ 1 /* reserved 0 */
					+ 1 /* BRS */
					+ 1 /* ESI */
					+ 4 /* DLC */
					+ payload_bits
					+ crc_bits /* CRC */
					+ 1 /* CRC delimiter */
					+ 1 /* ACK slot */
					+ 1 /* ACK delimiter */
					+ 7 /* EOF */
					+ 2; /* INTERFRAME SPACE: INTERMISSION (3) + (SUSPEND TRANSMISSION)? + (BUS IDLE)? */
			} else {
				*nmbr_bits =
					1 /* SOF */
					+ 11 /* ID */
					+ 1 /* reserved 1 */
					+ 1 /* IDE */
					+ 1 /* EDL */
					+ 1 /* reserved 0 */
					+ 1 /* BRS */
					+ 1 /* ESI */
					+ 4 /* DLC */
					+ payload_bits
					+ crc_bits /* CRC */
					+ 1 /* CRC delimiter */
					+ 1 /* ACK slot */
					+ 1 /* ACK delimiter */
					+ 7 /* EOF */
					+ 2; /* INTERFRAME SPACE: INTERMISSION (3) + (SUSPEND TRANSMISSION)? + (BUS IDLE)? */
			}
		}
	} else {
		*dtbr_bits = 0;

		if (xtd) {
			*nmbr_bits =
				1 /* SOF */
				+ 11 /* non XTD identifier part */
				+ 1 /* SRR */
				+ 1 /* IDE */
				+ 18 /* XTD identifier part */
				+ 1 /* RTR */
				+ 2 /* reserved */
				+ 4 /* DLC */
				+ (!rtr) * payload_bits
				+ 15 /* CRC */
				+ 1 /* CRC delimiter */
				+ 1 /* ACK slot */
				+ 1 /* ACK delimiter */
				+ 7 /* EOF */
				+ 2; /* INTERFRAME SPACE: INTERMISSION (3) + (SUSPEND TRANSMISSION)? + (BUS IDLE)? */
		} else {
			*nmbr_bits =
				1 /* SOF */
				+ 11 /* ID */
				+ 1 /* RTR */
				+ 1 /* IDE */
				+ 1 /* reserved */
				+ 4 /* DLC */
				+ (!rtr) * payload_bits
				+ 15 /* CRC */
				+ 1 /* CRC delimiter */
				+ 1 /* ACK slot */
				+ 1 /* ACK delimiter */
				+ 7 /* EOF */
				+ 2; /* INTERFRAME SPACE: INTERMISSION (3) + (SUSPEND TRANSMISSION)? + (BUS IDLE)? */
		}
	}
}

SC_RAMFUNC static inline uint32_t can_frame_time_us(
	uint8_t index,
	uint32_t nm,
	uint32_t dt)
{
	struct can *can = &cans.can[index];
	return can->nm_us_per_bit * nm + ((can->dt_us_per_bit_factor_shift8 * dt) >> 8);
}

#ifdef SUPERCAN_DEBUG
static volatile uint32_t rx_lost_reported[TU_ARRAY_SIZE(cans.can)];
// static volatile uint32_t rx_ts_last[TU_ARRAY_SIZE(cans.can)];
#endif

SC_RAMFUNC static bool can_poll(
	uint8_t index,
	uint32_t* events,
	uint32_t tsc)
{
	SC_DEBUG_ASSERT(events);

	struct can *can = &cans.can[index];

	bool more = false;
	uint32_t tsv[CAN_RX_FIFO_SIZE];
	uint8_t count = 0;
	uint8_t pi = 0;

	count = can->m_can->RXF0S.bit.F0FL;
	// static volatile uint32_t c = 0;
	// tsc = c++;
	// if (!counter_1MHz_is_current_value_ready()) {
	// 	LOG("ch%u counter not ready\n", index);
	// }
	//tsc = counter_1MHz_wait_for_current_value(index);
	// counter_1MHz_request_current_value();
	//tsc = counter_1MHz_read_unsafe(index);



	if (count) {
		more = true;
// #ifdef SUPERCAN_DEBUG
// 		uint32_t us = tsc - rx_ts_last[index];
// 		rx_ts_last[index] = tsc;
// 		LOG("ch%u rx dt=%lu\n", index, (unsigned long)us);
// #endif
		// reverse loop reconstructs timestamps
		uint32_t ts = tsc;
		uint8_t get_index;
		for (uint8_t i = 0, gio = can->m_can->RXF0S.bit.F0GI; i < count; ++i) {
			get_index = (gio + count - 1 - i) & (CAN_RX_FIFO_SIZE-1);
			// LOG("ch%u ts rx count=%u gi=%u\n", index, count, get_index);

			tsv[get_index] = ts & CLOCK_MAX;

			uint32_t nmbr_bits, dtbr_bits;
			can_frame_bits(
				can->rx_fifo[get_index].R0.bit.XTD,
				can->rx_fifo[get_index].R0.bit.RTR,
				can->rx_fifo[get_index].R1.bit.FDF,
				can->rx_fifo[get_index].R1.bit.BRS,
				can->rx_fifo[get_index].R1.bit.DLC,
				&nmbr_bits,
				&dtbr_bits);

			// LOG("ch%u rx gi=%u xtd=%d rtr=%d fdf=%d brs=%d dlc=%d nmbr_bits=%lu dtbr_bits=%lu ts=%lx data us=%lu\n",
			// 	index, get_index, can->rx_fifo[get_index].R0.bit.XTD,
			// 	can->rx_fifo[get_index].R0.bit.RTR,
			// 	can->rx_fifo[get_index].R1.bit.FDF,
			// 	can->rx_fifo[get_index].R1.bit.BRS,
			// 	can->rx_fifo[get_index].R1.bit.DLC, nmbr_bits, dtbr_bits,
			// 	(unsigned long)ts,
			// 	(unsigned long)((can->dt_us_per_bit_factor_shift8 * dtbr_bits) >> 8));

			ts -= can_frame_time_us(index, nmbr_bits, dtbr_bits);
		}

		// forward loop stores frames and notifies usb task
		pi = can->rx_put_index;

		for (uint8_t i = 0, gio = can->m_can->RXF0S.bit.F0GI; i < count; ++i) {
			get_index = (gio + i) & (CAN_RX_FIFO_SIZE-1);

			uint8_t rx_get_index = __atomic_load_n(&can->rx_get_index, __ATOMIC_ACQUIRE);
			uint8_t used = pi - rx_get_index;
			SC_ASSERT(used <= CAN_RX_FIFO_SIZE);

			if (unlikely(used == CAN_RX_FIFO_SIZE)) {
				//__atomic_add_fetch(&can->rx_lost, 1, __ATOMIC_ACQ_REL);
				can_inc_sat_rx_lost(index);

#ifdef SUPERCAN_DEBUG
				{
					if (rx_lost_reported[index] + UINT32_C(1000000) <= tsc) {
						rx_lost_reported[index] = tsc;
						LOG("ch%u rx lost %lx\n", index, ts);
					}
				}
#endif
			} else {
				uint8_t put_index = pi & (CAN_RX_FIFO_SIZE-1);
				can->rx_frames[put_index].R0 = can->rx_fifo[get_index].R0;
				can->rx_frames[put_index].R1 = can->rx_fifo[get_index].R1;
				can->rx_frames[put_index].ts = tsv[get_index];
				if (likely(!can->rx_frames[put_index].R0.bit.RTR)) {
					uint8_t can_frame_len = dlc_to_len(can->rx_frames[put_index].R1.bit.DLC);
					if (likely(can_frame_len)) {
						memcpy(can->rx_frames[put_index].data, can->rx_fifo[get_index].data, can_frame_len);
					}
				}

				++pi;

				// NOTE: This code is too slow to have here for some reason.
				// NOTE: If called outside this function, it is fast enough.
				// NOTE: Likely because of register / cache thrashing.

				// xTaskNotifyGive(can->usb_task_handle);
				// BaseType_t xHigherPriorityTaskWoken = pdFALSE;
				// vTaskNotifyGiveFromISR(can->usb_task_handle, &xHigherPriorityTaskWoken);
				// portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
				++*events;
			}
		}

		// removes frames from rx fifo
		can->m_can->RXF0A.reg = CAN_RXF0A_F0AI(get_index);

		// atomic update of rx put index
		__atomic_store_n(&can->rx_put_index, pi, __ATOMIC_RELEASE);
	}

	count = can->m_can->TXEFS.bit.EFFL;
	if (count) {
		more = true;

		// reverse loop reconstructs timestamps
		uint32_t ts = tsc;
		uint8_t get_index;
		uint32_t txp = can->m_can->CCCR.bit.TXP * 2;
		for (uint8_t i = 0, gio = can->m_can->TXEFS.bit.EFGI; i < count; ++i) {
			get_index = (gio + count - 1 - i) & (CAN_TX_FIFO_SIZE-1);
			// LOG("ch%u poll tx count=%u gi=%u\n", index, count, get_index);

			tsv[get_index] = ts & CLOCK_MAX;

			uint32_t nmbr_bits, dtbr_bits;
			can_frame_bits(
				can->tx_event_fifo[get_index].T0.bit.XTD,
				can->tx_event_fifo[get_index].T0.bit.RTR,
				can->tx_event_fifo[get_index].T1.bit.FDF,
				can->tx_event_fifo[get_index].T1.bit.BRS,
				can->tx_event_fifo[get_index].T1.bit.DLC,
				&nmbr_bits,
				&dtbr_bits);

			ts -= can_frame_time_us(index, nmbr_bits + txp, dtbr_bits);
		}

		// forward loop stores frames and notifies usb task
		pi = can->tx_put_index;

		for (uint8_t i = 0, gio = can->m_can->TXEFS.bit.EFGI; i < count; ++i) {
			get_index = (gio + i) & (CAN_TX_FIFO_SIZE-1);

			uint8_t put_index = pi & (CAN_TX_FIFO_SIZE-1);
			// if (unlikely(target_put_index == can->rx_get_index)) {
			// 	__atomic_add_fetch(&can->rx_lost, 1, __ATOMIC_ACQ_REL);
			// } else {
				can->tx_frames[put_index].T0 = can->tx_event_fifo[get_index].T0;
				can->tx_frames[put_index].T1 = can->tx_event_fifo[get_index].T1;
				can->tx_frames[put_index].ts = tsv[get_index];
				// LOG("ch%u tx place MM %u @ index %u\n", index, can->tx_frames[put_index].T1.bit.MM, put_index);

				//__atomic_store_n(&can->tx_put_index, target_put_index, __ATOMIC_RELEASE);
				//++can->tx_put_index;
				++pi;
			// }

			// xTaskNotifyGive(can->usb_task_handle);
			// BaseType_t xHigherPriorityTaskWoken = pdFALSE;
			// vTaskNotifyGiveFromISR(can->usb_task_handle, &xHigherPriorityTaskWoken);
			// portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
			++*events;
		}

		// removes frames from tx fifo
		can->m_can->TXEFA.reg = CAN_TXEFA_EFAI(get_index);
        // LOG("ch%u poll tx count=%u done\n", index, count);

		// atomic update of tx put index
		__atomic_store_n(&can->tx_put_index, pi, __ATOMIC_RELEASE);
	}

	// Because this task runs at a higher priority, it is ok to do this last
	//__atomic_thread_fence(__ATOMIC_RELEASE);

	return more;
}
