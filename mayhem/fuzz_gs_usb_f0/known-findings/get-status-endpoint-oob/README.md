# GET_STATUS(endpoint) indexes the endpoint arrays out of bounds

`USBD_StdEPReq()` in `libs/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c` takes the endpoint address from
the host-controlled `wIndex` of a standard GET_STATUS request addressed to an endpoint and uses
`ep_addr & 0x7F` (0..127) to index `pdev->ep_in[16]` / `pdev->ep_out[16]`. The class code only validates
`ep_addr & 0x0F`, so an address such as `0x50` passes the `is_used` check on endpoint 0 and then reads
`ep_out[0x50]`, 64 elements past the end of the 16-element array. `USBD_LL_IsStallEP()` in
`src/usbd_conf.c` repeats the pattern against the HAL's endpoint arrays (`hpcd->OUT_ep[ep_addr & 0x7F]`).

- Impact: out-of-bounds read of device RAM driven by an unauthenticated USB host (information disclosure
  of the stall flag, or a fault on parts where the address is unmapped).
- Reproduce: `/mayhem/<target>-standalone mayhem/<target>/known-findings/get-status-endpoint-oob/repro`
  reports `runtime error: index 80 out of bounds for type 'USBD_EndpointTypeDef[16]'`.
- Fix: reject `(ep_addr & 0x7F) >= USBD_MAX_NUM_EP` (and `> dev_endpoints` in `USBD_LL_IsStallEP`) in
  `USBD_StdEPReq()`'s GET_STATUS path before indexing.
