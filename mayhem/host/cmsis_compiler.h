/* Host (x86) stand-in for CMSIS cmsis_compiler.h: keeps the Cortex-M intrinsics inert. */
#ifndef MAYHEM_HOST_CMSIS_COMPILER_H
#define MAYHEM_HOST_CMSIS_COMPILER_H
#include <stdint.h>
#define __ASM __asm
#define __INLINE inline
#define __STATIC_INLINE static inline
#define __STATIC_FORCEINLINE static inline
#define __NO_RETURN __attribute__((noreturn))
#define __USED __attribute__((used))
#define __WEAK __attribute__((weak))
#define __PACKED __attribute__((packed))
#define __PACKED_STRUCT struct __attribute__((packed))
#define __PACKED_UNION union __attribute__((packed))
#define __ALIGNED(x) __attribute__((aligned(x)))
#define __RESTRICT __restrict
#define __COMPILER_BARRIER() __asm volatile("" ::: "memory")
#define __UNALIGNED_UINT32(x) (((struct T_UINT32 *)(x))->v)
#define __UNALIGNED_UINT16_WRITE(addr, val) (void)((((struct T_UINT16_WRITE *)(void *)(addr))->v) = (val))
#define __UNALIGNED_UINT16_READ(addr) (((const struct T_UINT16_READ *)(const void *)(addr))->v)
#define __UNALIGNED_UINT32_WRITE(addr, val) (void)((((struct T_UINT32_WRITE *)(void *)(addr))->v) = (val))
#define __UNALIGNED_UINT32_READ(addr) (((const struct T_UINT32_READ *)(const void *)(addr))->v)
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t mayhem_primask;
static inline void __enable_irq(void) { mayhem_primask = 0; }
static inline void __disable_irq(void) { mayhem_primask = 1; }
static inline uint32_t __get_PRIMASK(void) { return mayhem_primask; }
static inline void __set_PRIMASK(uint32_t v) { mayhem_primask = v; }
static inline void __ISB(void) {}
static inline void __DSB(void) {}
static inline void __DMB(void) {}
static inline void __NOP(void) {}
static inline void __WFI(void) {}
static inline void __WFE(void) {}
static inline void __SEV(void) {}
#include <stdlib.h>
static inline void __BKPT(int v) { (void)v; abort(); }
static inline uint32_t __get_CONTROL(void) { return 0; }
static inline uint32_t __get_MSP(void) { return 0; }
static inline uint32_t __get_PSP(void) { return 0; }
static inline void __set_MSP(uint32_t v) { (void)v; }
static inline void __set_PSP(uint32_t v) { (void)v; }
static inline void __set_CONTROL(uint32_t v) { (void)v; }
static inline uint32_t __get_IPSR(void) { return 0; }
static inline uint32_t __get_xPSR(void) { return 0; }
static inline uint32_t __REV(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t __REV16(uint32_t v) { return ((v & 0xff00ff00u) >> 8) | ((v & 0x00ff00ffu) << 8); }
static inline uint32_t __RBIT(uint32_t v) { uint32_t r = 0; for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }
static inline uint8_t __CLZ(uint32_t v) { return v ? (uint8_t)__builtin_clz(v) : 32; }
#ifdef __cplusplus
}
#endif
#endif
