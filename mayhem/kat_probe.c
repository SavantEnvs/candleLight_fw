/*
 * Oracle probe: drives the firmware (the same sources the fuzzer runs, built with normal flags) through a
 * fixed gs_usb conversation and prints what it sent to the host, as hex. mayhem/test.sh compares each
 * line with the known answer.
 */
#include <stdio.h>

#include "fuzz_gs_usb.c"

static void dump(const char *name, unsigned ep)
{
	printf("%s:", name);
	for (uint32_t i = 0; i < hs.tx_len[ep]; i++)
		printf(" %02x", hs.tx_buf[ep][i]);
	printf("\n");
}

static void setup_req(uint8_t bm, uint8_t br, uint16_t wv, uint16_t wi, uint16_t wl)
{
	uint8_t pkt[8] = { bm, br, wv & 0xFF, wv >> 8, wi & 0xFF, wi >> 8, wl & 0xFF, wl >> 8 };

	memcpy(hpcd_USB_FS.Setup, pkt, sizeof(pkt));
	HAL_PCD_SetupStageCallback(&hpcd_USB_FS);
}

static void ep0_out(const uint8_t *data, uint8_t len)
{
	uint8_t buf[64] = { len };
	struct reader r = { buf, 1u + len };

	memcpy(buf + 1, data, len);
	op_out_data(&r, 0);
	op_in_complete(0);
}

static void vendor_out(uint8_t br, const void *data, uint8_t len)
{
	setup_req(0x41, br, 0, 0, len);
	ep0_out(data, len);
}

static void vendor_in(const char *name, uint8_t br, uint16_t len)
{
	setup_req(0xC1, br, 0, 0, len);
	dump(name, 0);
	op_in_complete(0);
}

int main(void)
{
	firmware_init();
	main_loop_iteration();

	setup_req(0x80, 6, 0x0100, 0, 18);
	dump("device_descriptor", 0);
	op_in_complete(0);

	setup_req(0x00, 5, 8, 0, 0);
	setup_req(0x00, 9, 1, 0, 0);
	main_loop_iteration();

	const uint32_t byte_order = 0xBEEF;
	const uint32_t bittiming[5] = { 1, 12, 2, 1, 6 };
	const uint32_t mode_start[2] = { GS_CAN_MODE_START, 0 };

	vendor_out(GS_USB_BREQ_HOST_FORMAT, &byte_order, sizeof(byte_order));
	vendor_in("device_config", GS_USB_BREQ_DEVICE_CONFIG, 12);
	vendor_in("bt_const", GS_USB_BREQ_BT_CONST, 40);
	vendor_out(GS_USB_BREQ_BITTIMING, bittiming, sizeof(bittiming));
	vendor_out(GS_USB_BREQ_MODE, mode_start, sizeof(mode_start));
	printf("can_enabled: %d\n", hs.can[0].hw_enabled);

	/* host -> bus: the frame is echoed back to the host with its echo_id */
	uint8_t tx[2 + 20] = { 20, 0 };
	const uint8_t frame_tx[20] = { 7, 0, 0, 0, 0x23, 0x01, 0, 0, 4, 0, 0, 0, 0xde, 0xad, 0xbe, 0xef };
	struct reader r = { tx, sizeof(tx) };

	memcpy(tx + 2, frame_tx, sizeof(frame_tx));
	op_out_data(&r, 2);
	main_loop_iteration();
	dump("echo_frame", 1);
	op_in_complete(1);
	main_loop_iteration();

	/* bus -> host */
	hs.can[0].rx_pending = true;
	hs.can[0].rx_can_id = 0x80000321;
	hs.can[0].rx_dlc = 3;
	hs.can[0].rx_data[0] = 0x11;
	hs.can[0].rx_data[1] = 0x22;
	hs.can[0].rx_data[2] = 0x33;
	main_loop_iteration();
	main_loop_iteration();
	dump("rx_frame", 1);
	return 0;
}
