# libvftp -- standalone build, tests and examples.
#
# Three libraries, so each side links only what it runs:
#   libvftp_core.a    src/common: wire codec, CRC-32, shared helpers   (both sides)
#   libvftp_client.a  src/client                                      (+ core)
#   libvftp_server.a  src/server: burst + poll engines                (+ core)
#
#   make            libraries + tests + examples (host gcc)
#   make split-check  link the client and the servers each on their own (no cross dependency)
#   make test       run the test suite (ASan/UBSan where available)
#   make arm-check  compile the core with the OBC's arm-none-eabi-gcc 4.7 and the
#                   firmware's own strict flag set (proves C99 / gcc 4.7)
#   make demo       two-process CCSDS Space Packet over UDP transfer with 20% loss
#   make frames     every message as raw bytes, plus CSP/RDP/KISS framing (docs/protocol.md)
#   make docs       Doxygen HTML into build/docs/html (doxygen >= 1.18 renders the Mermaid diagrams)

CC      ?= gcc
BUILD   ?= build
DOXYGEN ?= doxygen
ARM_CC  ?= ../FSW_devinit/tools/toolchains/gcc-arm-none-eabi-4_7-2014q2/bin/arm-none-eabi-gcc

# Same warning set as FSW_devinit/cmake/*/strict.cmake.
WARN := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align -Wcast-qual -Wwrite-strings \
        -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls -Wundef \
        -Wlogical-op -Wformat=2 -Wswitch-default -Wswitch-enum -Wconversion -Wdouble-promotion \
        -Wjump-misses-init -Wnested-externs -Werror

SAN := $(shell echo 'int main(void){return 0;}' | $(CC) -fsanitize=address,undefined -x c - -o /dev/null 2>/dev/null && echo -fsanitize=address,undefined -fno-sanitize-recover=undefined)

CFLAGS  ?= -std=gnu99 -O1 -g
CFLAGS  += $(WARN) -Iinclude -Iport/storage_posix -Iport/storage_ram -Iport/link_loopback -Iport/ccsds_spp

COMMON  := src/common/vftp_crc32.c src/common/vftp_common.c src/common/vftp_wire.c
CLIENT  := src/client/vftp_client.c
SERVER  := src/server/vftp_burst_server.c src/server/vftp_poll_server.c src/server/vftp_server_util.c
CORE    := $(COMMON) $(CLIENT) $(SERVER)
HDRS    := $(wildcard include/vftp/*.h src/*/*.h)
PORTS   := port/storage_posix/vftp_storage_posix.c port/storage_ram/vftp_storage_ram.c \
           port/link_loopback/vftp_link_loopback.c port/ccsds_spp/vftp_spp.c port/ccsds_spp/vftp_spp_udp.c

.PHONY: all lib test examples arm-check split-check demo frames docs clean

all: lib $(BUILD)/test_vftp examples

LIBS := $(BUILD)/libvftp_core.a $(BUILD)/libvftp_client.a $(BUILD)/libvftp_server.a
lib: $(LIBS)

$(BUILD):
	mkdir -p $(BUILD)/obj

# $(call archive,<sources>): compile each source into build/obj, then archive them into $@.
define archive
	rm -f $@
	for f in $(1); do $(CC) $(CFLAGS) -c $$f -o $(BUILD)/obj/$$(basename $$f .c).o || exit 1; done
	ar rcs $@ $(addprefix $(BUILD)/obj/,$(notdir $(1:.c=.o)))
endef

$(BUILD)/libvftp_core.a: $(COMMON) $(HDRS) | $(BUILD)
	$(call archive,$(COMMON))

$(BUILD)/libvftp_client.a: $(CLIENT) $(HDRS) | $(BUILD)
	$(call archive,$(CLIENT))

$(BUILD)/libvftp_server.a: $(SERVER) $(HDRS) | $(BUILD)
	$(call archive,$(SERVER))

split-check: $(LIBS)
	$(CC) $(CFLAGS) tests/split/client_only.c $(BUILD)/libvftp_client.a $(BUILD)/libvftp_core.a -o $(BUILD)/client_only
	$(CC) $(CFLAGS) tests/split/server_only.c $(BUILD)/libvftp_server.a $(BUILD)/libvftp_core.a -o $(BUILD)/server_only
	$(BUILD)/client_only && $(BUILD)/server_only
	@echo "split check: client and servers each link with libvftp_core alone: OK"

RELAXED := -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter $(SAN) -Iinclude -Isrc/common \
           -Iport/storage_posix -Iport/storage_ram -Iport/link_loopback -Iport/ccsds_spp -Itests/unity \
           -DVFTP_CFG_PACKET_MAX=1100u

$(BUILD)/test_vftp: tests/test_vftp.c $(CORE) $(PORTS) tests/unity/unity.c $(HDRS) $(wildcard port/*/*.h) | $(BUILD)
	mkdir -p $(BUILD)/t
	for f in $(CORE) $(PORTS); do $(CC) $(CFLAGS) $(SAN) -DVFTP_CFG_PACKET_MAX=1100u -c $$f -o $(BUILD)/t/$$(basename $$f .c).o || exit 1; done
	$(CC) $(RELAXED) -c tests/unity/unity.c -o $(BUILD)/t/unity.o
	$(CC) $(RELAXED) -c tests/test_vftp.c -o $(BUILD)/t/test_vftp.o
	$(CC) $(SAN) $(BUILD)/t/*.o -o $@

test: $(BUILD)/test_vftp
	$(BUILD)/test_vftp

examples: $(BUILD)/vftp_spp_server $(BUILD)/vftp_spp_client

$(BUILD)/vftp_spp_server: examples/spp_server.c $(CORE) $(PORTS) | $(BUILD)
	$(CC) $(CFLAGS) -DVFTP_CFG_PACKET_MAX=1100u $^ -o $@

$(BUILD)/vftp_spp_client: examples/spp_client.c $(CORE) $(PORTS) | $(BUILD)
	$(CC) $(CFLAGS) -DVFTP_CFG_PACKET_MAX=1100u $^ -o $@

arm-check: | $(BUILD)
	for f in $(CORE) port/storage_ram/vftp_storage_ram.c port/link_loopback/vftp_link_loopback.c port/ccsds_spp/vftp_spp.c; do \
	  $(ARM_CC) -mcpu=arm926ej-s -marm -mfloat-abi=soft -Os -std=gnu99 $(WARN) -Wstack-usage=3072 -fstack-usage \
	    -Iinclude -Iport/storage_ram -Iport/link_loopback -Iport/ccsds_spp -c $$f -o $(BUILD)/arm_$$(basename $$f .c).o || exit 1; \
	done
	@echo "arm-none-eabi-gcc 4.7 strict build: OK"; cat $(BUILD)/arm_*.su | sort -t'	' -k2 -n -r | head -5

demo: examples
	sh examples/demo.sh

frames: $(BUILD)/vftp_frames
	$(BUILD)/vftp_frames

$(BUILD)/vftp_frames: tools/vftp_frames.c $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

docs: | $(BUILD)
	$(DOXYGEN) Doxyfile

clean:
	rm -rf $(BUILD)
