/*
 * Host-side stand-ins for the STM32 peripherals so the firmware's gs_usb class driver, CAN glue,
 * USB device core and LED logic run unmodified on x86 under the fuzzer.
 *
 *  - HAL PCD (USB device controller): endpoint bookkeeping only; usbd_conf.c's real USBD_LL_* wrappers sit on top.
 *  - CAN driver (can_drv_*, can_send, can_receive): bxcan.c/m_can.c busy-wait on peripheral flags that a
 *    fake register file can never raise, so the driver entry points are modelled from fuzz-controlled state.
 *    The constants below mirror the ones in src/can/bxcan.c / src/can/m_can.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "board.h"
#include "can_common.h"
#include "can_drv.h"
#include "gs_usb.h"
#include "host.h"
#include "timer.h"

struct host_state hs;
uint32_t mayhem_primask;

/* ---- memory-mapped registers the firmware reads or writes directly ---- */

#if defined(STM32G0)
#define USB_REGS_BASE USB_DRD_BASE
#else
#define USB_REGS_BASE USB_BASE
#endif
#define EPR_STAT_RX_MASK  0x3000u
#define EPR_STAT_RX_NAK	  0x2000u
#define EPR_STAT_RX_VALID 0x3000u

static uint32_t rx_stat_shadow[MOCK_EPS];

static volatile uint32_t *usb_epr(unsigned ep)
{
	return (volatile uint32_t *)(USB_REGS_BASE + 4u * ep);
}

static void map_fixed(uintptr_t addr, size_t len)
{
	const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
	const uintptr_t start = addr & ~(page - 1);
	const uintptr_t end = (addr + len + page - 1) & ~(page - 1);

	if (mmap((void *)start, end - start, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) {
		perror("mmap fake peripheral");
		abort();
	}
}

__attribute__((constructor)) static void host_map_mmio(void)
{
	map_fixed(UID_BASE, 12);          /* device unique ID, read for the USB serial-number string */
	map_fixed(USB_REGS_BASE, 0x400);  /* USB controller registers (EPnR), poked by PCD_SET_EP_RX_STATUS */
}

void host_reset_mmio(void)
{
	memset((void *)USB_REGS_BASE, 0, 0x400);
	memset(rx_stat_shadow, 0, sizeof(rx_stat_shadow));
	for (unsigned i = 0; i < 3; i++)
		((volatile uint32_t *)UID_BASE)[i] = 0x01020304u * (i + 1);
}

/*
 * STAT_RX is a toggle-on-write field: PCD_SET_EP_RX_STATUS writes (current ^ wanted) and the
 * controller turns that back into `wanted`. The fake register keeps whatever the model last stored, so
 * the state the controller would be in is (fake ^ shadow) when firmware wrote it, else the shadow itself.
 */
bool host_ep_rx_nak(unsigned ep)
{
	const uint32_t reg = *usb_epr(ep) & EPR_STAT_RX_MASK;
	const uint32_t state = (reg == rx_stat_shadow[ep]) ? reg : (reg ^ rx_stat_shadow[ep]);

	return state == EPR_STAT_RX_NAK;
}

/* ---- time ---- */

uint32_t HAL_GetTick(void)
{
	return hs.tick_ms;
}

uint32_t timer_get(void)
{
	return hs.tick_us;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
	(void)GPIOx;
	(void)GPIO_Pin;
	(void)PinState;
}

void HAL_GPIO_Init(GPIO_TypeDef *GPIOx, GPIO_InitTypeDef *GPIO_Init)
{
	(void)GPIOx;
	(void)GPIO_Init;
}

#if defined(STM32G0)
void HAL_SYSCFG_StrobeDBattpinsConfig(uint32_t ConfigDeadBattery)
{
	(void)ConfigDeadBattery;
}

void HAL_PWREx_EnableVddUSB(void)
{
}
#endif

/* ---- NVIC (only referenced by usbd_conf.c's MSP init, which the stubbed HAL_PCD_Init never calls) ---- */

void HAL_NVIC_SetPriority(IRQn_Type IRQn, uint32_t PreemptPriority, uint32_t SubPriority)
{
	(void)IRQn;
	(void)PreemptPriority;
	(void)SubPriority;
}

void HAL_NVIC_EnableIRQ(IRQn_Type IRQn)
{
	(void)IRQn;
}

void HAL_NVIC_DisableIRQ(IRQn_Type IRQn)
{
	(void)IRQn;
}

/* ---- HAL PCD ---- */

static PCD_EPTypeDef *pcd_ep(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
	return (ep_addr & 0x80) ? &hpcd->IN_ep[ep_addr & 0x7F] : &hpcd->OUT_ep[ep_addr & 0x7F];
}

HAL_StatusTypeDef HAL_PCD_Init(PCD_HandleTypeDef *hpcd)
{
	(void)hpcd;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_DeInit(PCD_HandleTypeDef *hpcd)
{
	(void)hpcd;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_Start(PCD_HandleTypeDef *hpcd)
{
	(void)hpcd;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_Stop(PCD_HandleTypeDef *hpcd)
{
	(void)hpcd;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_Open(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint16_t ep_mps, uint8_t ep_type)
{
	PCD_EPTypeDef *ep = pcd_ep(hpcd, ep_addr);

	ep->num = ep_addr & 0x7F;
	ep->is_in = (ep_addr & 0x80) != 0;
	ep->maxpacket = ep_mps;
	ep->type = ep_type;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_Close(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
	(void)hpcd;
	if (ep_addr & 0x80)
		hs.tx_pending[ep_addr & 0x7F] = false;
	else
		hs.rx_armed[ep_addr & 0x7F] = false;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_Flush(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
	(void)hpcd;
	(void)ep_addr;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_SetStall(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
	/* The ST HAL rejects endpoint numbers above the configured count. */
	if ((ep_addr & 0x7F) > hpcd->Init.dev_endpoints)
		return HAL_ERROR;
	pcd_ep(hpcd, ep_addr)->is_stall = 1;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_ClrStall(PCD_HandleTypeDef *hpcd, uint8_t ep_addr)
{
	/* The ST HAL rejects endpoint numbers above the configured count. */
	if ((ep_addr & 0x7F) > hpcd->Init.dev_endpoints)
		return HAL_ERROR;
	pcd_ep(hpcd, ep_addr)->is_stall = 0;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_SetAddress(PCD_HandleTypeDef *hpcd, uint8_t address)
{
	hpcd->USB_Address = address;
	return HAL_OK;
}

/* As in the ST HAL, Receive always addresses the OUT endpoint and Transmit the IN endpoint, whatever the direction bit. */
HAL_StatusTypeDef HAL_PCD_EP_Receive(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint8_t *pBuf, uint32_t len)
{
	PCD_EPTypeDef *ep = &hpcd->OUT_ep[ep_addr & 0x7F];
	const unsigned n = ep_addr & 0x7F;

	ep->xfer_buff = pBuf;
	ep->xfer_len = len;
	ep->xfer_count = 0;
	hs.rx_armed[n] = true;
	hs.rx_len[n] = len;
	*usb_epr(n) = (*usb_epr(n) & ~EPR_STAT_RX_MASK) | EPR_STAT_RX_VALID;
	rx_stat_shadow[n] = EPR_STAT_RX_VALID;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_PCD_EP_Transmit(PCD_HandleTypeDef *hpcd, uint8_t ep_addr, uint8_t *pBuf, uint32_t len)
{
	PCD_EPTypeDef *ep = &hpcd->IN_ep[ep_addr & 0x7F];
	const unsigned n = ep_addr & 0x7F;

	ep->xfer_buff = pBuf;
	ep->xfer_len = len;
	ep->xfer_count = 0;

	/* The controller DMA-reads the payload from device memory; touch every byte so ASan sees an over-read. */
	uint32_t sum = 0;
	for (uint32_t i = 0; pBuf && i < len; i++)
		sum += pBuf[i];
	hs.tx_sink += sum;

	hs.tx_pending[n] = true;
	hs.tx_buf[n] = pBuf;
	hs.tx_len[n] = len;
	return HAL_OK;
}

uint32_t HAL_PCD_EP_GetRxCount(PCD_HandleTypeDef const *hpcd, uint8_t ep_addr)
{
	(void)hpcd;
	return hs.rx_count[ep_addr & 0x7F];
}

HAL_StatusTypeDef HAL_PCDEx_PMAConfig(PCD_HandleTypeDef *hpcd, uint16_t ep_addr, uint16_t ep_kind, uint32_t pmaadress)
{
	(void)hpcd;
	(void)ep_addr;
	(void)ep_kind;
	(void)pmaadress;
	return HAL_OK;
}

/* ---- CAN driver stand-ins ---- */

#if defined(CONFIG_BXCAN)
const struct gs_device_bt_const CAN_btconst = {
	.feature =
		GS_CAN_FEATURE_LISTEN_ONLY |
		GS_CAN_FEATURE_LOOP_BACK |
		GS_CAN_FEATURE_ONE_SHOT |
		GS_CAN_FEATURE_HW_TIMESTAMP |
		GS_CAN_FEATURE_IDENTIFY |
		GS_CAN_FEATURE_PAD_PKTS_TO_MAX_PKT_SIZE |
		(IS_ENABLED(CONFIG_TERMINATION) ? GS_CAN_FEATURE_TERMINATION : 0) |
		GS_CAN_FEATURE_BERR_REPORTING |
		GS_CAN_FEATURE_GET_STATE |
		(IS_ENABLED(CONFIG_CAN_FILTER) ? GS_CAN_FEATURE_FILTER : 0) |
		GS_CAN_FEATURE_BUS_OFF_RECOVERY,
	.fclk_can = CAN_CLOCK_SPEED,
	.btc = {
		.tseg1_min = 1,
		.tseg1_max = 16,
		.tseg2_min = 1,
		.tseg2_max = 8,
		.sjw_max = 4,
		.brp_min = 1,
		.brp_max = 1024,
		.brp_inc = 1,
	},
};
#ifdef CONFIG_CAN_FILTER
const struct gs_device_filter_info CAN_filter_info = {
	.dev = GS_DEVICE_FILTER_DEV_BXCAN,
};
#endif
#elif defined(CONFIG_M_CAN)
const struct gs_device_bt_const CAN_btconst = {
	.feature =
		GS_CAN_FEATURE_LISTEN_ONLY |
		GS_CAN_FEATURE_LOOP_BACK |
		GS_CAN_FEATURE_HW_TIMESTAMP |
		GS_CAN_FEATURE_IDENTIFY |
		GS_CAN_FEATURE_PAD_PKTS_TO_MAX_PKT_SIZE |
		(IS_ENABLED(CONFIG_CANFD) ? GS_CAN_FEATURE_FD | GS_CAN_FEATURE_BT_CONST_EXT : 0) |
		(IS_ENABLED(CONFIG_TERMINATION) ? GS_CAN_FEATURE_TERMINATION : 0) |
		GS_CAN_FEATURE_BERR_REPORTING |
		GS_CAN_FEATURE_GET_STATE |
		(IS_ENABLED(CONFIG_CANFD) ? GS_CAN_FEATURE_TDC : 0) |
		GS_CAN_FEATURE_BUS_OFF_RECOVERY,
	.fclk_can = CAN_CLOCK_SPEED,
	.btc = {
		.tseg1_min = 1,
		.tseg1_max = 256,
		.tseg2_min = 1,
		.tseg2_max = 128,
		.sjw_max = 128,
		.brp_min = 1,
		.brp_max = 512,
		.brp_inc = 1,
	},
};
#ifdef CONFIG_CANFD
const struct gs_device_bt_const_extended CAN_btconst_ext = {
	.feature =
		GS_CAN_FEATURE_LISTEN_ONLY |
		GS_CAN_FEATURE_LOOP_BACK |
		GS_CAN_FEATURE_HW_TIMESTAMP |
		GS_CAN_FEATURE_IDENTIFY |
		GS_CAN_FEATURE_PAD_PKTS_TO_MAX_PKT_SIZE |
		GS_CAN_FEATURE_FD | GS_CAN_FEATURE_BT_CONST_EXT |
		(IS_ENABLED(CONFIG_TERMINATION) ? GS_CAN_FEATURE_TERMINATION : 0) |
		GS_CAN_FEATURE_BERR_REPORTING |
		GS_CAN_FEATURE_GET_STATE |
		GS_CAN_FEATURE_TDC |
		GS_CAN_FEATURE_BUS_OFF_RECOVERY,
	.fclk_can = CAN_CLOCK_SPEED,
	.btc = {
		.tseg1_min = 1,
		.tseg1_max = 256,
		.tseg2_min = 1,
		.tseg2_max = 128,
		.sjw_max = 128,
		.brp_min = 1,
		.brp_max = 512,
		.brp_inc = 1,
	},
	.dbtc = {
		.tseg1_min = 1,
		.tseg1_max = 32,
		.tseg2_min = 1,
		.tseg2_max = 16,
		.sjw_max = 16,
		.brp_min = 1,
		.brp_max = 32,
		.brp_inc = 1,
	},
};

const struct gs_device_tdc_const CAN_tdc_const = {
	.tdco_min = 0,
	.tdco_max = 127,
	.tdcf_min = 0,
	.tdcf_max = 127,
	.mode = GS_CAN_TDC_MODE_OFF | GS_CAN_TDC_MODE_AUTO,
};
#endif
#endif

static struct mock_can_channel *mock_of(const struct can_channel *channel)
{
	return &hs.can[can_channel_get_nr(channel)];
}

void can_init(can_data_t *channel, const struct board_channel_config *config)
{
	(void)channel;
	(void)config;
}

#ifdef CONFIG_CAN_FILTER
void can_set_filter(can_data_t *channel, const struct gs_device_filter *filter)
{
	channel->filter = *filter;
}
#endif

void can_drv_enable(struct can_channel *channel)
{
	mock_of(channel)->hw_enabled = true;
}

void can_drv_disable(struct can_channel *channel)
{
	struct mock_can_channel *m = mock_of(channel);

	m->hw_enabled = false;
	m->rx_pending = false;
}

void can_drv_get_device_tdc(const struct can_channel *channel, struct gs_device_tdc *tdc)
{
	memset(tdc, 0, sizeof(*tdc));
	tdc->tdcv = mock_of(channel)->tdcv;
}

void can_drv_read_reg_status(struct can_channel *channel)
{
	const struct mock_can_channel *m = mock_of(channel);

#if defined(CONFIG_BXCAN)
	channel->reg_status.esr = ((uint32_t)m->rec << 24) | ((uint32_t)m->tec << 16) | ((uint32_t)(m->lec & 7) << 4);
#elif defined(CONFIG_M_CAN)
	channel->reg_status.ecr = ((uint32_t)m->rec << 8) | m->tec;
	channel->reg_status.psr = (m->lec & 7);
#endif
}

bool can_drv_bus_error_pending(const struct can_channel *channel)
{
	return mock_of(channel)->bus_error_pending;
}

bool can_drv_handle_bus_error(const struct can_channel *channel, struct gs_host_frame *frame)
{
	const struct mock_can_channel *m = mock_of(channel);

	if (m->tec == 0 && m->rec == 0)
		return false;

	frame->classic_can->data[6] = m->tec;
	frame->classic_can->data[7] = m->rec;
	frame->can_id |= CAN_ERR_PROT | CAN_ERR_BUSERROR | CAN_ERR_CNT;
	can_lec_error_to_frame(frame, m->lec & 7);
	return true;
}

enum gs_can_state can_drv_get_state(const struct can_channel *channel)
{
	const struct mock_can_channel *m = mock_of(channel);

	if (m->bus_off)
		return GS_CAN_STATE_BUS_OFF;
	return can_err_to_state(m->tec > m->rec ? m->tec : m->rec);
}

void can_drv_get_device_state(const struct can_channel *channel, struct gs_device_state *state)
{
	const struct mock_can_channel *m = mock_of(channel);

	state->state = can_drv_get_state(channel);
	state->rxerr = m->rec;
	state->txerr = m->tec;
}

void can_drv_handle_state_change(const struct can_channel *channel, struct gs_host_frame *frame)
{
	const struct mock_can_channel *m = mock_of(channel);
	const enum gs_can_state tx_state = can_err_to_state(m->tec);
	const enum gs_can_state rx_state = can_err_to_state(m->rec);

	if (tx_state >= rx_state)
		frame->classic_can->data[1] |= gs_can_tx_state_to_frame(tx_state);
	if (tx_state <= rx_state)
		frame->classic_can->data[1] |= gs_can_rx_state_to_frame(rx_state);
}

void can_drv_handle_bus_off_recovery(struct can_channel *channel)
{
	struct mock_can_channel *m = mock_of(channel);

	m->bus_off = false;
	m->tec = 0;
	m->rec = 0;
}

bool can_is_rx_pending(can_data_t *channel)
{
	return mock_of(channel)->rx_pending;
}

bool can_receive(can_data_t *channel, struct gs_host_frame *rx_frame)
{
	struct mock_can_channel *m = mock_of(channel);

	if (!m->rx_pending)
		return false;
	m->rx_pending = false;

	rx_frame->can_id = m->rx_can_id;
	rx_frame->can_dlc = m->rx_dlc;
	rx_frame->channel = can_channel_get_nr(channel);
	rx_frame->flags = m->rx_flags;
	if (IS_ENABLED(CONFIG_CANFD) && (m->rx_flags & GS_CAN_FLAG_FD)) {
		memcpy(rx_frame->canfd->data, m->rx_data, sizeof(rx_frame->canfd->data));
		rx_frame->canfd_ts->timestamp_us = timer_get();
	} else {
		memcpy(rx_frame->classic_can->data, m->rx_data, sizeof(rx_frame->classic_can->data));
		rx_frame->classic_can_ts->timestamp_us = timer_get();
	}
	return true;
}

bool can_send(can_data_t *channel, struct gs_host_frame *frame)
{
	struct mock_can_channel *m = mock_of(channel);

	if (m->tx_full)
		return false;

	/* A real mailbox load reads the whole payload out of the host frame. */
	uint8_t sink = 0;
	const unsigned n = (IS_ENABLED(CONFIG_CANFD) && (frame->flags & GS_CAN_FLAG_FD)) ? 64 : 8;
	const uint8_t *src = (IS_ENABLED(CONFIG_CANFD) && (frame->flags & GS_CAN_FLAG_FD)) ?
						 frame->canfd->data : frame->classic_can->data;
	for (unsigned i = 0; i < n; i++)
		sink ^= src[i];
	m->last_rx_bytes_read = sink;
	return true;
}
