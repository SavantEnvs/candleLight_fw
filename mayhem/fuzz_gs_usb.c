/*
 * libFuzzer harness for the candleLight gs_usb firmware's USB/CAN core.
 *
 * Runs the real src/usbd_gs_can.c, src/can_common.c, src/usbd_conf.c, src/usbd_desc.c, src/led.c and the
 * vendored ST USB device core on the host, with the USB device controller and CAN peripheral replaced by
 * the models in mayhem/host/host_stubs.c. The input is a stream of USB-controller events (SETUP packet,
 * EP0/bulk OUT data, IN-complete, SOF, suspend/resume, reset) interleaved with iterations of the firmware's
 * main loop and changes to the mocked CAN bus (frame received, bus errors, TX mailbox full).
 *
 * The hardware model is honest: an IN-complete only follows a pending transmit, an OUT-complete only
 * follows an armed receive and never carries more bytes than were armed.
 */
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "can.h"
#include "can_common.h"
#include "config.h"
#include "gpio.h"
#include "host.h"
#include "led.h"
#include "usbd_conf.h"
#include "usbd_core.h"
#include "usbd_def.h"
#include "usbd_desc.h"
#include "usbd_gs_can.h"

#define MAX_OPS 96

static USBD_GS_CAN_HandleTypeDef hGS_CAN;
static USBD_HandleTypeDef hUSB;

struct reader {
	const uint8_t *p;
	size_t n;
};

static uint8_t rd8(struct reader *r)
{
	if (!r->n)
		return 0;
	r->n--;
	return *r->p++;
}

static size_t rd_bytes(struct reader *r, uint8_t *dst, size_t want)
{
	size_t n = want < r->n ? want : r->n;

	memcpy(dst, r->p, n);
	r->p += n;
	r->n -= n;
	return n;
}

static void firmware_init(void)
{
	memset(&hs, 0, sizeof(hs));
	host_reset_mmio();
	memset(&hGS_CAN, 0, sizeof(hGS_CAN));
	memset(&hUSB, 0, sizeof(hUSB));
	memset(&hpcd_USB_FS, 0, sizeof(hpcd_USB_FS));

	INIT_LIST_HEAD(&hGS_CAN.list_frame_pool);
	INIT_LIST_HEAD(&hGS_CAN.list_to_host);

	for (unsigned i = 0; i < ARRAY_SIZE(hGS_CAN.msgbuf); i++)
		list_add_tail(&hGS_CAN.msgbuf[i].list, &hGS_CAN.list_frame_pool);

	for (unsigned i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++) {
		const struct board_channel_config *channel_config = &config.channel[i];
		can_data_t *channel = &hGS_CAN.channels[i];

		can_channel_set_nr(channel, i);
		INIT_LIST_HEAD(&channel->list_from_host);
		led_init(&channel->leds,
				 LEDRX_GPIO_Port, LEDRX_Pin, LEDRX_Active_High,
				 LEDTX_GPIO_Port, LEDTX_Pin, LEDTX_Active_High);
		can_init(channel, channel_config);
		can_disable(&hGS_CAN, channel);
	}

	/* gpio.c keeps the termination state in a file-scope static; clear it so executions are independent. */
	for (unsigned i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++)
		set_term(&hGS_CAN.channels[i], GS_CAN_TERMINATION_STATE_OFF);

	USBD_Init(&hUSB, (USBD_DescriptorsTypeDef *)&FS_Desc, DEVICE_FS);
	USBD_RegisterClass(&hUSB, &USBD_GS_CAN);
	USBD_GS_CAN_Init(&hGS_CAN, &hUSB);
	USBD_Start(&hUSB);
	HAL_PCD_ResetCallback(&hpcd_USB_FS);
}

static void main_loop_iteration(void)
{
	for (unsigned i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++)
		CAN_SendFrame(&hGS_CAN, &hGS_CAN.channels[i]);

	USBD_GS_CAN_ReceiveFromHost(&hUSB);
	USBD_GS_CAN_SendToHost(&hUSB);

	for (unsigned i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++) {
		can_data_t *channel = &hGS_CAN.channels[i];

		CAN_ReceiveFrame(&hGS_CAN, channel);
		CAN_HandleError(&hGS_CAN, channel);
		led_update(&channel->leds);
	}

	(void)USBD_GS_CAN_DfuDetachRequested(&hUSB);
}

static void op_out_data(struct reader *r, unsigned ep)
{
	if (!hs.rx_armed[ep] || (ep == 2 && host_ep_rx_nak(ep)))
		return;

	PCD_EPTypeDef *pep = &hpcd_USB_FS.OUT_ep[ep];
	uint32_t len = rd8(r);

	if (ep != 0)
		len |= (uint32_t)rd8(r) << 8;
	if (len > hs.rx_len[ep])
		len = hs.rx_len[ep];

	/*
	 * Real hardware would fault writing a packet to a NULL-derived buffer, so no completion follows;
	 * UBSan has already reported where that address came from (USBD_GS_CAN_PrepareReceive).
	 */
	if (ep != 0 && (uintptr_t)pep->xfer_buff < 4096)
		return;
	if (!pep->xfer_buff)
		len = 0;
	if (len) {
		memset(pep->xfer_buff, 0, len);
		rd_bytes(r, pep->xfer_buff, len);
	}
	hs.rx_armed[ep] = false;
	hs.rx_count[ep] = len;
	HAL_PCD_DataOutStageCallback(&hpcd_USB_FS, ep);
}

static void op_in_complete(unsigned ep)
{
	if (!hs.tx_pending[ep])
		return;

	hs.tx_pending[ep] = false;
	HAL_PCD_DataInStageCallback(&hpcd_USB_FS, ep);
}

static void op_can_state(struct reader *r)
{
	struct mock_can_channel *m = &hs.can[rd8(r) % NUM_CAN_CHANNEL];
	const uint8_t sel = rd8(r);

	switch (sel & 7) {
	case 0:
		m->rx_pending = true;
		m->rx_can_id = rd8(r) | (rd8(r) << 8) | (rd8(r) << 16) | ((uint32_t)rd8(r) << 24);
		m->rx_dlc = rd8(r) & 0x0F;
		m->rx_flags = rd8(r);
		memset(m->rx_data, 0, sizeof(m->rx_data));
		rd_bytes(r, m->rx_data, sizeof(m->rx_data));
		break;
	case 1:
		m->tec = rd8(r);
		m->rec = rd8(r);
		m->lec = rd8(r);
		m->bus_error_pending = sel & 8;
		break;
	case 2:
		m->bus_off = sel & 8;
		break;
	case 3:
		m->tx_full = sel & 8;
		break;
	default:
		m->tdcv = rd8(r);
		break;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct reader r = { data, size };

	firmware_init();
	/* main() is already looping (filling the receive buffers) long before a host finishes enumerating. */
	main_loop_iteration();

	for (unsigned n = 0; n < MAX_OPS && r.n; n++) {
		const uint8_t op = rd8(&r);

		hs.tick_us += 100 + (op >> 4) * 37;
		hs.tick_ms = hs.tick_us / 1000;

		switch (op & 0x0F) {
		case 0:
		case 1:
		case 2:
			memset(hpcd_USB_FS.Setup, 0, sizeof(hpcd_USB_FS.Setup));
			rd_bytes(&r, (uint8_t *)hpcd_USB_FS.Setup, 8);
			HAL_PCD_SetupStageCallback(&hpcd_USB_FS);
			break;
		case 3:
			op_out_data(&r, 0);
			break;
		case 4:
			op_in_complete(0);
			break;
		case 5:
		case 6:
			op_out_data(&r, 2);
			break;
		case 7:
			op_in_complete(1);
			break;
		case 8:
		case 9:
			main_loop_iteration();
			break;
		case 10:
			op_can_state(&r);
			break;
		case 11:
			HAL_PCD_SOFCallback(&hpcd_USB_FS);
			break;
		case 12:
			HAL_PCD_SuspendCallback(&hpcd_USB_FS);
			break;
		case 13:
			HAL_PCD_ResumeCallback(&hpcd_USB_FS);
			break;
		default:
			HAL_PCD_ResetCallback(&hpcd_USB_FS);
			break;
		}
	}

	return 0;
}
