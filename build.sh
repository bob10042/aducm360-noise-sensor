#!/bin/bash
# Build ADuCM360 noise sensor firmware
# Run from WSL2: bash build.sh [target]
# Targets: adc_swd (default), noise_sensor, blink_test, bitbang_test, uart_both

set -e

REPO=~/EVAL-ADICUP360
CMSIS=$REPO/projects/ADuCM360_demo_cn0359_reva/src/system/include/cmsis
LD=$REPO/projects/ADuCM360_demo_cn0359_reva/ld_script/gcc_arm.ld
STARTUP=$REPO/projects/ADuCM360_demo_cn0359_reva/src/system/cmsis
LIBS=$REPO/projects/ADuCM360_demo_blink/IAR/system
SRC=/mnt/c/Users/bob43/Downloads/aducm360_noise_sensor

TARGET=${1:-adc_swd}

# Common source files
COMMON_SRC="$STARTUP/system_ADuCM360.c $STARTUP/startup_ADuCM360.S"
COMMON_FLAGS="-mcpu=cortex-m3 -mthumb -O1 -std=c99 -DADUCM360"
COMMON_FLAGS="$COMMON_FLAGS -I$CMSIS -I$LIBS/include/ADuCM360 -T$LD"
COMMON_FLAGS="$COMMON_FLAGS -nostartfiles -nodefaultlibs -Wl,--gc-sections"

# Select libs and source based on target
case $TARGET in
    adc_swd)
        DEVICE_LIBS="$LIBS/src/ADuCM360/AdcLib.c $LIBS/src/ADuCM360/DioLib.c $LIBS/src/ADuCM360/DmaLib.c"
        MAIN="$SRC/adc_swd.c"
        ;;
    noise_sensor)
        DEVICE_LIBS="$LIBS/src/ADuCM360/UrtLib.c $LIBS/src/ADuCM360/DioLib.c $LIBS/src/ADuCM360/AdcLib.c $LIBS/src/ADuCM360/DmaLib.c"
        MAIN="$SRC/main.c"
        ;;
    blink_test)
        DEVICE_LIBS="$LIBS/src/ADuCM360/DioLib.c"
        MAIN="$SRC/blink_test.c"
        ;;
    bitbang_test)
        DEVICE_LIBS="$LIBS/src/ADuCM360/DioLib.c"
        MAIN="$SRC/bitbang_test.c"
        ;;
    uart_both)
        DEVICE_LIBS="$LIBS/src/ADuCM360/UrtLib.c $LIBS/src/ADuCM360/DioLib.c"
        MAIN="$SRC/uart_both.c"
        ;;
    adc_full)
        DEVICE_LIBS="$LIBS/src/ADuCM360/AdcLib.c $LIBS/src/ADuCM360/DioLib.c $LIBS/src/ADuCM360/DmaLib.c $LIBS/src/ADuCM360/UrtLib.c"
        MAIN="$SRC/adc_full.c"
        ;;
    firmware_full)
        DEVICE_LIBS="$LIBS/src/ADuCM360/AdcLib.c $LIBS/src/ADuCM360/DioLib.c $LIBS/src/ADuCM360/DmaLib.c $LIBS/src/ADuCM360/DacLib.c $LIBS/src/ADuCM360/GptLib.c $LIBS/src/ADuCM360/WdtLib.c $LIBS/src/ADuCM360/UrtLib.c $LIBS/src/ADuCM360/ClkLib.c $LIBS/src/ADuCM360/IntLib.c $LIBS/src/ADuCM360/FeeLib.c $LIBS/src/ADuCM360/SpiLib.c"
        MAIN="$SRC/firmware_full.c"
        ;;
    *)
        echo "Unknown target: $TARGET"
        echo "Usage: bash build.sh [adc_swd|noise_sensor|blink_test|bitbang_test|uart_both|adc_full|firmware_full]"
        exit 1
        ;;
esac

echo "Building $TARGET..."
arm-none-eabi-gcc $COMMON_FLAGS $COMMON_SRC $DEVICE_LIBS $MAIN -o $SRC/$TARGET.elf
arm-none-eabi-objcopy -O binary $SRC/$TARGET.elf $SRC/$TARGET.bin
arm-none-eabi-size $SRC/$TARGET.elf
arm-none-eabi-nm $SRC/$TARGET.elf | grep -E "g_adc|g_magic|g_sample|g_dbg" 2>/dev/null || true
echo "Done: $SRC/$TARGET.bin"
