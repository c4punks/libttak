CC ?= gcc
CXX ?= g++
AR ?= ar
NVCC ?= nvcc
HIPCC ?= hipcc
EMBEDDED ?= 0
USE_CUDA ?= 0
USE_OPENCL ?= 0
USE_ROCM ?= 0

# Toolchain selection: pass TOOLCHAIN=msvc on the command line to use cl/lib/link.
# GNU (GCC/Clang) is the default on all platforms.
TOOLCHAIN ?= gnu

COMMON_WARNINGS ?= -Wall -std=c17 -pthread -Iinclude -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT
DEPFLAGS ?= -MD -MF $(@:.o=.d)
LDFLAGS_BASE = -pthread -lm
UNAME_S := $(shell uname -s)

# Detect which compiler family we are using.
BUILD_PROFILE = perf
ifneq (,$(findstring tcc,$(notdir $(CC))))
BUILD_PROFILE = tcc
endif

# If CC=tcc is requested but tcc is not installed in the runner,
# transparently fall back to clang so "tcc" benchmark lanes can still run.
ifeq ($(BUILD_PROFILE),tcc)
TCC_BIN_OK := $(shell command -v tcc >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(TCC_BIN_OK),0)
$(warning [libttak] tcc not found; falling back to clang for CC=tcc build lane)
override CC := clang
endif
endif

TCC_STACK_FLAGS ?= -O3 -g \
                  -fno-inline \
                  -fno-omit-frame-pointer \
                  -fno-optimize-sibling-calls \
				  -fno-semantic-interposition \
				  -fno-trapping-math \
				  -falign-functions=32 \
				  -fno-plt \
				  -fno-math-errno \

PERF_WARNINGS ?= -Wextra -Wshadow -Wstrict-prototypes -Wswitch-enum
PERF_STACK_FLAGS ?= -O3 -march=native -mtune=native -pipe -flto \
                   -fomit-frame-pointer -funroll-loops \
                   -fstrict-aliasing -ffunction-sections -fdata-sections \
                   -fvisibility=hidden -DNDEBUG

# GCC can retain portable LTO objects; AppleClang rejects that GCC-only flag.
# Keep it out of the default profile on every Clang/macOS build rather than
# requiring each caller (CI, users, package managers) to override CFLAGS.
ifeq ($(UNAME_S),Darwin)
PERF_STACK_FLAGS := $(filter-out -ffat-lto-objects,$(PERF_STACK_FLAGS))
else
PERF_STACK_FLAGS += -ffat-lto-objects
endif

ifeq ($(BUILD_PROFILE),tcc)
CFLAGS = $(COMMON_WARNINGS) $(TCC_STACK_FLAGS)
LDFLAGS = $(LDFLAGS_BASE)
else
CFLAGS = $(COMMON_WARNINGS) $(PERF_WARNINGS) $(PERF_STACK_FLAGS)  -fPIC -ftls-model=global-dynamic
ifeq ($(UNAME_S),Darwin)
LDFLAGS = $(LDFLAGS_BASE) -flto -Wl,-dead_strip
else
LDFLAGS = $(LDFLAGS_BASE) -flto -Wl,--gc-sections
endif
endif

CFLAGS += $(EXTRA_CFLAGS) -DEMBEDDED=$(EMBEDDED)
LDFLAGS += $(EXTRA_LDFLAGS)

# ---- MSVC toolchain overrides (Windows / cl.exe) -----------------------
# Replaces GCC-only flags with cl-compatible equivalents.
ifeq ($(TOOLCHAIN),msvc)
CC         = cl
AR         = lib
LD         = link
DEPFLAGS   =
OBJEXT     = obj
LIBEXT     = lib
AR_EXTRA   =
AR_OUTFLAG = /OUT:
CFLAGS     = /O2 /W4 /GL /DNDEBUG /std:c17 /Iinclude $(EXTRA_CFLAGS) /DEMBEDDED=$(EMBEDDED)
LDFLAGS    = /LTCG $(EXTRA_LDFLAGS)
else
OBJEXT     = o
LIBEXT     = a
AR_EXTRA   = rcs
AR_OUTFLAG =
ifeq ($(OS),Windows_NT)
LDFLAGS += -lws2_32
endif
endif
# ------------------------------------------------------------------------

# ---- Bare-metal toolchain (PLATFORM=baremetal | freertos) --------------
# Cross-builds lib/libttak.a for Cortex-M microcontrollers with newlib.
# MCU_FLAGS selects the core; the default is Cortex-M7 with a
# double-precision FPU (STM32F76x/F77x, e.g. STM32F769I-DISCO).
#
# PLATFORM=freertos adds real threads on FreeRTOS. It needs the kernel
# sources and the application's FreeRTOSConfig.h, which libttak is
# compiled against:
#   FREERTOS_KERNEL_PATH  FreeRTOS-Kernel checkout (V11.x)
#   FREERTOS_PORT         port directory under portable/ (GCC/ARM_CM4F)
#   FREERTOS_CONFIG_DIR   directory holding FreeRTOSConfig.h
#
# NET=lwip (with PLATFORM=freertos) also builds the socket and I/O modules
# on lwIP's BSD socket API:
#   LWIP_PATH             lwIP checkout (2.1 or later)
#   LWIP_CONFIG_DIR       directory holding lwipopts.h and arch/cc.h
PLATFORM ?= hosted
ifneq ($(filter baremetal freertos,$(PLATFORM)),)
CROSS_COMPILE ?= arm-none-eabi-
ifneq ($(filter default undefined environment,$(origin CC)),)
CC = $(CROSS_COMPILE)gcc
endif
ifneq ($(filter default undefined environment,$(origin AR)),)
AR = $(CROSS_COMPILE)ar
endif
MCU_FLAGS ?= -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16
BAREMETAL_OPT ?= -Os -g
override EMBEDDED := 1
CFLAGS = -std=c17 -Wall -Wextra -Iinclude $(MCU_FLAGS) $(BAREMETAL_OPT) \
         -ffunction-sections -fdata-sections \
         -DTTAK_PLATFORM_BAREMETAL=1 -DEMBEDDED=1 $(EXTRA_CFLAGS)
LDFLAGS = $(MCU_FLAGS) -Wl,--gc-sections $(EXTRA_LDFLAGS)
endif
ifeq ($(PLATFORM),freertos)
FREERTOS_PORT ?= GCC/ARM_CM4F
FREERTOS_CONFIG_DIR ?= tests/baremetal/freertos
ifeq ($(FREERTOS_KERNEL_PATH),)
$(error PLATFORM=freertos needs FREERTOS_KERNEL_PATH=<FreeRTOS-Kernel checkout>)
endif
FREERTOS_INCLUDES = -I$(FREERTOS_CONFIG_DIR) -I$(FREERTOS_KERNEL_PATH)/include \
                    -I$(FREERTOS_KERNEL_PATH)/portable/$(FREERTOS_PORT)
CFLAGS += -DTTAK_PLATFORM_FREERTOS=1 $(FREERTOS_INCLUDES)
endif
ifeq ($(NET),lwip)
ifneq ($(PLATFORM),freertos)
$(error NET=lwip needs PLATFORM=freertos)
endif
ifeq ($(LWIP_PATH),)
$(error NET=lwip needs LWIP_PATH=<lwIP checkout>)
endif
LWIP_CONFIG_DIR ?= tests/baremetal/lwip
LWIP_INCLUDES = -I$(LWIP_CONFIG_DIR) -I$(LWIP_PATH)/src/include \
                -I$(LWIP_PATH)/contrib/ports/freertos/include
CFLAGS += -DTTAK_NET_LWIP=1 $(LWIP_INCLUDES)
endif
# ------------------------------------------------------------------------

PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCDIR = $(PREFIX)/include

SRC_DIRS = src/ht src/thread src/timing src/mem src/async src/priority \
           src/atomic src/sync src/math src/tree src/container \
           src/security src/status src/mem_tree src/limit src/stats src/log \
           src/unsafe src/shared src/mask src/phys/dimless src/io \
           src/net src/net/core src/phys/mem src/script src/arch

SRCS = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
ifeq ($(BUILD_PROFILE),tcc)
C_BASE_ACCEL_SRCS = src/accel/ttak_accel_dispatch.c
else
C_BASE_ACCEL_SRCS = src/accel/accel_cpu.c src/accel/ttak_accel_dispatch.c
endif
SRCS += $(C_BASE_ACCEL_SRCS)

# Bare-metal has no /proc or __int128 (accel CPU backend), and no sockets
# or poll() unless NET=lwip.
BAREMETAL_SOCKET_SRCS = src/net/endpoint.c src/net/poller.c src/net/session.c \
                        src/net/view.c src/io/async.c src/io/io.c src/io/zerocopy.c
BAREMETAL_EXCLUDED_SRCS = src/stats/system_usage.c \
                          $(if $(filter lwip,$(NET)),,$(BAREMETAL_SOCKET_SRCS))
ifneq ($(filter baremetal freertos,$(PLATFORM)),)
SRC_DIRS += src/platform
SRCS = $(filter-out $(BAREMETAL_EXCLUDED_SRCS),$(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c)))
endif
C_OPTIONAL_SRCS =
CUDA_SRCS =
ROCM_SRCS =

ifeq ($(USE_CUDA),1)
CUDA_SRCS += src/accel/accel_cuda.cu src/accel/bigint_cuda.cu src/accel/ntt_cuda.cu
CFLAGS += -DENABLE_CUDA
ifeq ($(TOOLCHAIN),msvc)
NVCCFLAGS = -std=c++14 -Iinclude -DENABLE_CUDA -ccbin cl
else
NVCCFLAGS ?= -std=c++14 -Iinclude -DENABLE_CUDA
endif
endif

ifeq ($(USE_OPENCL),1)
C_OPTIONAL_SRCS += src/accel/accel_opencl.c src/accel/bigint_opencl.c src/accel/ntt_opencl.c
CFLAGS += -DENABLE_OPENCL
OPENCL_LIBS ?= -lOpenCL
LDFLAGS += $(OPENCL_LIBS)
endif

ifeq ($(USE_ROCM),1)
ROCM_SRCS += src/accel/accel_rocm.cpp src/accel/bigint_rocm.cpp src/accel/ntt_rocm.cpp
CFLAGS += -DENABLE_ROCM
HIPCCFLAGS ?= -std=c++17 -Iinclude -DENABLE_ROCM
endif

C_SRCS_ALL = $(SRCS) $(C_OPTIONAL_SRCS)
OBJS = $(patsubst src/%.c,obj/%.$(OBJEXT),$(SRCS))
OBJS += $(patsubst src/%.c,obj/%.$(OBJEXT),$(C_OPTIONAL_SRCS))
ASMS = $(patsubst src/%.c,obj/%.s,$(C_SRCS_ALL))

CUDA_OBJS = $(patsubst src/%.cu,obj/%.$(OBJEXT),$(CUDA_SRCS))
ROCM_OBJS = $(patsubst src/%.cpp,obj/%.$(OBJEXT),$(ROCM_SRCS))
OBJS += $(CUDA_OBJS) $(ROCM_OBJS)

DEPS = $(patsubst %.$(OBJEXT),%.d,$(OBJS))

LIB = lib/libttak.$(LIBEXT)

TEST_SRCS = $(wildcard tests/test_*.c)
TEST_BINS = $(TEST_SRCS:.c=)

EXAMPLE_BINS = examples/mersenne-prime/mersenne_explorer examples/shared-memory/shared_example

all: directories $(LIB)

$(LIB): $(OBJS)
	@mkdir -p lib
	$(AR) $(AR_EXTRA) $(AR_OUTFLAG)$@ $(OBJS)

asm: directories $(ASMS)

# Default rule for all other directories
obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

obj/%.obj: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) /c $< /Fo$@

obj/%.o: src/%.cu
	@mkdir -p $(dir $@)
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

obj/%.obj: src/%.cu
	@mkdir -p $(dir $@)
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(HIPCC) $(HIPCCFLAGS) -c $< -o $@

obj/%.s: obj/%.o
	@mkdir -p $(dir $@)
	objdump -d $< > $@

test: all $(TEST_BINS)
	@echo "Starting internal test suite..."
	@for bin in $(TEST_BINS); do \
		echo "[RUNNING] $$bin"; \
		./$$bin; \
		if [ $$? -eq 0 ]; then \
			echo "[PASSED] $$bin"; \
		else \
			echo "[FAILED] $$bin with exit code $$?"; \
			exit 1; \
		fi \
	done
	@echo "All tests completed successfully."

tests: test examples_run

examples_run: $(EXAMPLE_BINS)
	@echo "Starting example suite..."
	@for bin in $(EXAMPLE_BINS); do \
		if [ -f $$bin ]; then \
			echo "[RUNNING EXAMPLE] $$bin"; \
			timeout 10s $$bin; \
			ret=$$?; \
			if [ $$ret -eq 0 ]; then \
				echo "[PASSED] $$bin"; \
			elif [ $$ret -eq 124 ]; then \
				echo "[PASSED] $$bin (timed out as expected for long-running demo)"; \
			else \
				echo "[FAILED] $$bin with exit code $$ret"; \
				exit 1; \
			fi \
		fi \
	done
	@echo "All examples completed successfully."

examples/mersenne-prime/mersenne_explorer: examples/mersenne-prime/mersenne_explorer.c $(LIB)
	$(CC) $(CFLAGS) -Iexamples/mersenne-prime $< -o $@ -Llib -lttak $(LDFLAGS)

examples/shared-memory/shared_example: examples/shared-memory/shared_example.c $(LIB)
	$(CC) $(CFLAGS) -Iinclude $< -o $@ -Llib -lttak $(LDFLAGS)

tests/test_%: tests/test_%.c $(LIB)
	$(CC) $(CFLAGS) $< -o $@ $(LIB) $(LDFLAGS)

# ---- Bare-metal unit tests (PLATFORM=baremetal | freertos) -------------
# Builds one firmware image per unit test for BOARD, using the startup and
# syscall harness in tests/baremetal. Run them with
# scripts/baremetal/run_qemu.sh or scripts/baremetal/run_stm32.py.
BOARD ?= mps2_an500
BM_DIR = tests/baremetal
BM_OUT = $(BM_DIR)/build/$(BOARD)$(if $(filter freertos,$(PLATFORM)),-freertos)$(if $(filter lwip,$(NET)),-lwip)
BM_HARNESS = $(BM_DIR)/startup_cortex_m.c $(BM_DIR)/syscalls.c $(BM_DIR)/board_$(BOARD).c
BM_LDFLAGS = $(MCU_FLAGS) -nostartfiles -T$(BM_DIR)/$(BOARD).ld -L$(BM_DIR) \
             --specs=nosys.specs -Wl,--gc-sections
# Tests that fit a 512 KiB MCU. Never built for bare metal:
#  - need desktop-sized memory: test_bigscript (~340 KiB) test_new_mem (1 MiB)
#  - need an OS: test_system_usage, and test_net_* other than
#    BAREMETAL_NET_TESTS
# BAREMETAL_THREAD_TESTS need real threads and only run with FreeRTOS;
# BAREMETAL_NET_TESTS need sockets and only run with NET=lwip.
BAREMETAL_TESTS = test_aria test_async test_atomic test_buddy test_chacha20_poly1305 \
                  test_detachable_mode test_epoch_gc test_future_release test_ht \
                  test_limit test_logger test_lsh test_math test_math_advanced \
                  test_mem test_nice \
                  test_owner_complex test_pool test_priority test_security \
                  test_security_hardening test_security_util test_seed test_shared \
                  test_simple_ds test_smart_sched test_stats test_status test_sync \
                  test_table test_timing test_track_mem
BAREMETAL_THREAD_TESTS = test_epoch_grace test_ringbuf test_sharded_sched \
                         test_thread test_worker_abort test_worker_dirty_recovery
BAREMETAL_NET_TESTS = test_net_tcp
BM_TEST_CFLAGS ?= -DTEST_PQ_STRESS_N=1000
ifeq ($(PLATFORM),freertos)
BM_TESTS ?= $(BAREMETAL_TESTS) $(BAREMETAL_THREAD_TESTS) \
            $(if $(filter lwip,$(NET)),$(BAREMETAL_NET_TESTS))
else
BM_TESTS ?= $(BAREMETAL_TESTS)
endif
BM_ELFS = $(patsubst %,$(BM_OUT)/%.elf,$(BM_TESTS))

baremetal-tests: $(LIB) $(BM_ELFS)

ifeq ($(PLATFORM),freertos)
# The test's main() becomes a FreeRTOS task started by freertos_main.c.
FREERTOS_KERNEL_SRCS = $(addprefix $(FREERTOS_KERNEL_PATH)/,tasks.c queue.c list.c \
                         timers.c event_groups.c stream_buffer.c \
                         portable/$(FREERTOS_PORT)/port.c portable/MemMang/heap_4.c)
FREERTOS_KERNEL_OBJS = $(patsubst $(FREERTOS_KERNEL_PATH)/%.c,$(BM_OUT)/kernel/%.o,$(FREERTOS_KERNEL_SRCS))
BM_RTOS_OBJS = $(FREERTOS_KERNEL_OBJS) $(BM_OUT)/freertos_main.o
ifeq ($(NET),lwip)
# freertos_main.c starts lwIP's tcpip thread before the test runs.
LWIP_SRCS = $(wildcard $(LWIP_PATH)/src/core/*.c $(LWIP_PATH)/src/core/ipv4/*.c \
                       $(LWIP_PATH)/src/api/*.c) \
            $(LWIP_PATH)/contrib/ports/freertos/sys_arch.c
LWIP_OBJS = $(patsubst $(LWIP_PATH)/%.c,$(BM_OUT)/lwip/%.o,$(LWIP_SRCS))
BM_RTOS_OBJS += $(LWIP_OBJS)

$(BM_OUT)/lwip/%.o: $(LWIP_PATH)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MCU_FLAGS) $(BAREMETAL_OPT) -ffunction-sections -fdata-sections $(FREERTOS_INCLUDES) $(LWIP_INCLUDES) -c $< -o $@
endif

$(BM_OUT)/kernel/%.o: $(FREERTOS_KERNEL_PATH)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MCU_FLAGS) $(BAREMETAL_OPT) -ffunction-sections -fdata-sections $(FREERTOS_INCLUDES) -c $< -o $@

$(BM_OUT)/freertos_main.o: $(BM_DIR)/freertos_main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BM_OUT)/%.elf: tests/%.c $(BM_HARNESS) $(BM_RTOS_OBJS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu17 $(BM_TEST_CFLAGS) -Itests -Dmain=ttak_test_main -c $< -o $(@:.elf=.o)
	$(CC) $(CFLAGS) -std=gnu17 $(@:.elf=.o) $(BM_HARNESS) $(BM_RTOS_OBJS) -o $@ $(LIB) $(BM_LDFLAGS) -lm
else
# Tests build as gnu17, the arm-none-eabi default most firmware uses; this
# also covers headers under newlib's POSIX visibility.
$(BM_OUT)/%.elf: tests/%.c $(BM_HARNESS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -std=gnu17 $(BM_TEST_CFLAGS) -Itests $< $(BM_HARNESS) -o $@ $(LIB) $(BM_LDFLAGS) -lm
endif

directories:
	@mkdir -p $(foreach dir,$(SRC_DIRS),obj/$(patsubst src/%,%,$(dir))) obj/accel lib

clean:
	rm -rf obj lib tests/*.d $(BM_DIR)/build
	find tests/ -maxdepth 1 -type f ! -name "*.c" ! -name "*.h" -delete
	rm -f examples/mersenne-prime/mersenne_explorer examples/mersenne-prime/*.o
	rm -f examples/shared-memory/shared_example
	rm -rf a.out

asm_clean:
	rm -f $(ASMS)

install: $(LIB)
	install -d $(INCDIR)
	install -d $(LIBDIR)
	cp -r include/* $(INCDIR)/
	install -m 644 $(LIB) $(LIBDIR)/

uninstall:
	rm -rf $(INCDIR)/ttak
	rm -f $(LIBDIR)/libttak.$(LIBEXT)

blueprints:
	@python3 blueprints/scripts/render_blueprints.py

-include $(DEPS)

.PHONY: all clean directories install uninstall blueprints test tests asm asm_clean examples_run
