/* State shared between the host-side hardware stand-ins (host_stubs.c) and the fuzz harness. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "can.h"
#include "hal_include.h"
#include "usbd_gs_can.h"

#define MOCK_EPS 8

struct mock_can_channel {
	bool hw_enabled;
	bool bus_off;
	bool rx_pending;
	bool bus_error_pending;
	bool tx_full;
	uint8_t tec;
	uint8_t rec;
	uint8_t lec;
	uint8_t tdcv;
	uint8_t rx_dlc;
	uint8_t rx_flags;
	uint32_t rx_can_id;
	uint8_t rx_data[64];
	uint8_t last_rx_bytes_read;
};

struct host_state {
	uint32_t tick_ms;
	uint32_t tick_us;
	struct mock_can_channel can[NUM_CAN_CHANNEL];
	uint8_t fake_usb_regs[0x800] __attribute__((aligned(8)));
	uint8_t tx_len_known[MOCK_EPS];
	bool tx_pending[MOCK_EPS];
	bool rx_armed[MOCK_EPS];
	uint32_t tx_len[MOCK_EPS];
	uint32_t rx_len[MOCK_EPS];
	uint8_t *tx_buf[MOCK_EPS];
	uint32_t rx_count[MOCK_EPS];
	volatile uint32_t tx_sink;
};

extern struct host_state hs;
extern PCD_HandleTypeDef hpcd_USB_FS;

void host_reset_mmio(void);
bool host_ep_rx_nak(unsigned ep);
