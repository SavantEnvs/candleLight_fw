#!/usr/bin/env bash
#
# mayhem/build.sh — build the candleLight_fw fuzz harnesses and the oracle probe.
#
# The firmware targets STM32 (arm-none-eabi, CMake) and cannot run on the host, so the harness builds the
# firmware's own C sources (gs_usb class driver, CAN glue, USB device core, descriptors, LEDs) natively
# with clang and the real vendored ST headers, and links them against host stand-ins for the USB
# controller and CAN peripheral (mayhem/host/). Nothing upstream is modified.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

B=mayhem/build
rm -rf "$B"
mkdir -p "$B"

INC="-Imayhem/host -Iinclude \
 -Ilibs/STM32_USB_Device_Library/Core/Inc -Ilibs/STM32_USB_Device_Library/config \
 -Ilibs/STM32_HAL/include -Ilibs/STM32_HAL/include/cmsis -Ilibs/STM32_HAL/include/cmsis/device \
 -Ilibs/STM32_HAL/config"
# The CMSIS compiler header only knows arm-none-eabi toolchains; mayhem/host/cmsis_compiler.h replaces it.
HOST="-D__CMSIS_COMPILER_H -include mayhem/host/cmsis_compiler.h"

# Firmware sources that run unmodified on the host.
FW_SRC="src/usbd_gs_can.c src/can_common.c src/usbd_conf.c src/usbd_desc.c src/util.c src/led.c src/gpio.c src/boards/legacy.c
 libs/STM32_USB_Device_Library/Core/Src/usbd_core.c
 libs/STM32_USB_Device_Library/Core/Src/usbd_ctlreq.c
 libs/STM32_USB_Device_Library/Core/Src/usbd_ioreq.c"

# compile_all <out-dir> <flags> <hal-driver-dir> <defines...>: compile the firmware sources, the host
# stand-ins and $HARNESS_SRC into <out-dir> (objects left in $OBJS).
HARNESS_SRC=mayhem/fuzz_gs_usb.c
compile_all() {
  local out="$1" flags="$2" hal="$3"; shift 3
  mkdir -p "$out"
  local objs=()
  local f
  for f in $FW_SRC mayhem/host/host_stubs.c "$HARNESS_SRC"; do
    local o="$out/$(echo "$f" | tr '/' '_').o"
    "$CC" $flags -c $HOST "$@" $INC -Ilibs/STM32_HAL/"$hal"/Inc "$f" -o "$o" &
    objs+=("$o")
    while [ "$(jobs -rp | wc -l)" -ge "$MAYHEM_JOBS" ]; do wait -n; done
  done
  wait
  OBJS=("${objs[@]}")
}

"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$B/lsan_off.o"
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -x c -c "$STANDALONE_FUZZ_MAIN" -o "$B/standalone_main.o"

# name | HAL driver dir | defines
build_fuzzer() {
  local name="$1" hal="$2"; shift 2
  compile_all "$B/$name" "$SANITIZER_FLAGS $DEBUG_FLAGS -O1 -fsanitize=fuzzer-no-link" "$hal" "$@"
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "${OBJS[@]}" "$B/lsan_off.o" -o "/mayhem/$name"
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "${OBJS[@]}" "$B/standalone_main.o" "$B/lsan_off.o" -o "/mayhem/$name-standalone"
}

build_fuzzer fuzz_gs_usb_f0 stm32f0xx-hal-driver -DBOARD_candleLight -DSTM32F0 -DSTM32F042x6 -DCONFIG_BXCAN
build_fuzzer fuzz_gs_usb_g0 stm32g0xx-hal-driver -DBOARD_budgetcan -DSTM32G0 -DSTM32G0B1xx -DCONFIG_M_CAN

# Oracle probe: the same firmware sources with the project's normal flags (no sanitizers, no -gdwarf-3),
# dynamically linked, driven through a fixed USB conversation by mayhem/test.sh.
HARNESS_SRC=mayhem/kat_probe.c
compile_all "$B/kat" "-O1 $COVERAGE_FLAGS" stm32f0xx-hal-driver -DBOARD_candleLight -DSTM32F0 -DSTM32F042x6 -DCONFIG_BXCAN
"$CC" -O1 $COVERAGE_FLAGS "${OBJS[@]}" -o /mayhem/kat_probe
