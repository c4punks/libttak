# libttak network service on the STM32F769I-DISCO (FreeRTOS + lwIP + HAL ETH).
#
#   make FREERTOS_KERNEL_PATH=... LWIP_PATH=... STM32_HAL_PATH=... \
#        STM32_CMSIS_DEVICE_PATH=... CMSIS_CORE_PATH=... LAN8742_PATH=...
#   ../../scripts/baremetal/stm32_eth_demo.py build/stm32f769i_disco_eth.elf
#
# The ST sources are the STM32CubeF7 components, also published separately:
#   STM32_HAL_PATH           github.com/STMicroelectronics/stm32f7xx-hal-driver
#   STM32_CMSIS_DEVICE_PATH  github.com/STMicroelectronics/cmsis-device-f7
#   CMSIS_CORE_PATH          github.com/STMicroelectronics/cmsis-core (CMSIS/Core/Include)
#   LAN8742_PATH             github.com/STMicroelectronics/stm32-lan8742
# Inside an STM32CubeF7 package they are Drivers/STM32F7xx_HAL_Driver,
# Drivers/CMSIS/Device/ST/STM32F7xx, Drivers/CMSIS and Drivers/BSP/Components/lan8742.

LIBTTAK    := $(abspath ../..)
HERE       := $(abspath .)
BUILD      := build
CROSS      ?= arm-none-eabi-
CC         := $(CROSS)gcc
MCU_FLAGS  := -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16

require_var = $(if $($(1)),,$(error set $(1)))
$(foreach v,FREERTOS_KERNEL_PATH LWIP_PATH STM32_HAL_PATH STM32_CMSIS_DEVICE_PATH CMSIS_CORE_PATH LAN8742_PATH,$(call require_var,$(v)))

CMSIS_CORE_INC = $(firstword $(wildcard $(CMSIS_CORE_PATH)/CMSIS/Core/Include $(CMSIS_CORE_PATH)/Include))

DEFS = -DSTM32F769xx -DUSE_HAL_DRIVER -DTTAK_PLATFORM_BAREMETAL=1 -DTTAK_PLATFORM_FREERTOS=1 \
       -DTTAK_NET_LWIP=1 -DEMBEDDED=1
INCS = -I$(HERE) -I$(LIBTTAK)/include -I$(LIBTTAK)/tests/baremetal \
       -I$(STM32_HAL_PATH)/Inc -I$(STM32_CMSIS_DEVICE_PATH)/Include -I$(CMSIS_CORE_INC) \
       -I$(LAN8742_PATH) \
       -I$(FREERTOS_KERNEL_PATH)/include -I$(FREERTOS_KERNEL_PATH)/portable/GCC/ARM_CM7/r0p1 \
       -I$(LWIP_PATH)/src/include -I$(LWIP_PATH)/contrib/ports/freertos/include
CFLAGS = $(MCU_FLAGS) -std=gnu17 -Os -g -Wall -ffunction-sections -fdata-sections -MMD -MP $(DEFS) $(INCS)
# Third-party code builds without warnings enabled.
VENDOR_CFLAGS = $(filter-out -Wall,$(CFLAGS))
LDFLAGS = $(MCU_FLAGS) -nostartfiles -T$(HERE)/stm32f769i_disco_eth.ld -L$(LIBTTAK)/tests/baremetal \
          --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=$(BUILD)/stm32f769i_disco_eth.map

APP_SRCS = main.c ethernetif.c $(LIBTTAK)/tests/baremetal/syscalls.c
HAL_SRCS = $(addprefix $(STM32_HAL_PATH)/Src/stm32f7xx_,hal.c hal_cortex.c hal_rcc.c hal_rcc_ex.c \
             hal_gpio.c hal_pwr.c hal_pwr_ex.c hal_eth.c hal_dma.c hal_flash.c) \
           $(STM32_CMSIS_DEVICE_PATH)/Source/Templates/system_stm32f7xx.c \
           $(LAN8742_PATH)/lan8742.c
STARTUP  = $(STM32_CMSIS_DEVICE_PATH)/Source/Templates/gcc/startup_stm32f769xx.s
RTOS_SRCS = $(addprefix $(FREERTOS_KERNEL_PATH)/,tasks.c queue.c list.c timers.c event_groups.c \
              stream_buffer.c portable/GCC/ARM_CM7/r0p1/port.c portable/MemMang/heap_4.c)
LWIP_SRCS = $(wildcard $(LWIP_PATH)/src/core/*.c $(LWIP_PATH)/src/core/ipv4/*.c $(LWIP_PATH)/src/api/*.c) \
            $(LWIP_PATH)/src/netif/ethernet.c $(LWIP_PATH)/contrib/ports/freertos/sys_arch.c

obj = $(BUILD)/obj/$(notdir $(basename $(1))).o
APP_OBJS    = $(foreach s,$(APP_SRCS),$(call obj,$(s)))
VENDOR_OBJS = $(foreach s,$(HAL_SRCS) $(RTOS_SRCS) $(LWIP_SRCS) $(STARTUP),$(call obj,$(s)))
LIB = $(BUILD)/libttak/lib/libttak.a
ELF = $(BUILD)/stm32f769i_disco_eth.elf

all: $(ELF)

$(ELF): $(APP_OBJS) $(VENDOR_OBJS) $(LIB)
	$(CC) $(APP_OBJS) $(VENDOR_OBJS) $(LIB) $(LDFLAGS) -lm -o $@
	$(CROSS)size $@

define compile_rule
$(call obj,$(1)): $(1) | $(BUILD)/obj
	$(CC) $(2) -c $$< -o $$@
endef
$(foreach s,$(APP_SRCS),$(eval $(call compile_rule,$(s),$(CFLAGS))))
$(foreach s,$(HAL_SRCS) $(RTOS_SRCS) $(LWIP_SRCS) $(STARTUP),$(eval $(call compile_rule,$(s),$(VENDOR_CFLAGS))))

$(BUILD)/obj:
	mkdir -p $@

# libttak itself, compiled against this example's FreeRTOSConfig.h and
# lwipopts.h, from a private copy of the tree so lib/ in the checkout is
# left alone.
$(LIB): FORCE
	mkdir -p $(BUILD)/libttak
	rsync -a --delete --exclude obj --exclude lib $(addprefix $(LIBTTAK)/,Makefile include internal src) \
	      $(BUILD)/libttak/
	$(MAKE) -C $(BUILD)/libttak PLATFORM=freertos NET=lwip \
	        FREERTOS_KERNEL_PATH=$(FREERTOS_KERNEL_PATH) FREERTOS_PORT=GCC/ARM_CM7/r0p1 \
	        FREERTOS_CONFIG_DIR=$(HERE) LWIP_PATH=$(LWIP_PATH) LWIP_CONFIG_DIR=$(HERE) lib/libttak.a

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/obj/*.d)

.PHONY: all clean FORCE
