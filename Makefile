PS5_HOST ?= ps5
PS5_PORT ?= 9021
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK does not point to a valid SDK: $(PS5_PAYLOAD_SDK))
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

SYSINFO_ELF := hwinfo_sysinfo.elf
BENCH_ELF := hwinfo_bench.elf
ELF := $(SYSINFO_ELF) $(BENCH_ELF)
SRC := main.c

CFLAGS := -std=c11 -Wall -Wextra -Werror -g
LDLIBS := -lkernel_sys -lSceRegMgr -lpthread

all: $(ELF)

$(SYSINFO_ELF): CFLAGS += -DHWINFO_BUILD_SYSINFO=1 -DHWINFO_BUILD_BENCHMARK=0
$(BENCH_ELF): CFLAGS += -DHWINFO_BUILD_SYSINFO=0 -DHWINFO_BUILD_BENCHMARK=1

$(ELF): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip --strip-all $@

clean:
	rm -f $(ELF)

test: test-sysinfo

test-sysinfo: $(SYSINFO_ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<

test-bench: $(BENCH_ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $<
