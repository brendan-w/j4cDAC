PREFIX = /usr/local/arm-2012.03/bin/arm-none-eabi-
CC = $(PREFIX)gcc
BUILDID=`git describe --dirty`

INCLUDES = -Iinc -I../common/inc -Ilib/lpc17xx -Idrivers/include -I../common
CFLAGS += -fno-common -mcpu=cortex-m3 -mthumb -O3 -g -fno-unroll-loops
CFLAGS += -Wall -Werror -fdata-sections -fshort-enums -Wno-unused -Wno-address
LDFLAGS = -Tlib/lpc17xx/lpc1758.lds -mcpu=cortex-m3 -mthumb -fwhole-program -Wl,-Map=j4cDAC.map

# Cortex-M3 support code
CFLAGS += -Ilib/lpc17xx
BRD_SRCS += ../common/lib/core_cm3.c lib/lpc17xx/startup_LPC17xx.s \
	drivers/source/lpc17xx_clkpwr.c ../common/lib/serial.c main.c

# Mach printf. Much more lightweight than newlib's.
APP_SRCS += ../common/lib/vsnprintf.c

BRD_SRCS += ../common/lib/hardware.c

# Application
BRD_SRCS += lib/dac.c lib/dac_asm.s lib/dmx.c
APP_SRCS += lib/panic.c lib/lightengine.c lib/playback.c

# lwIP
LWIP = lwip-1.3.2/src
INCLUDES += -Ilwip-1.3.2/src/include -Ilwip-1.3.2/src/include/ipv4
APP_SRCS += $(LWIP)/core/dns.c $(LWIP)/core/init.c $(LWIP)/core/netif.c \
        $(LWIP)/core/pbuf.c $(LWIP)/core/raw.c $(LWIP)/core/stats.c \
        $(LWIP)/core/sys.c $(LWIP)/core/tcp.c $(LWIP)/core/tcp_in.c \
        $(LWIP)/core/tcp_out.c $(LWIP)/core/udp.c $(LWIP)/core/ipv4/autoip.c \
	$(LWIP)/core/ipv4/icmp.c $(LWIP)/core/ipv4/igmp.c $(LWIP)/core/ipv4/inet.c \
	$(LWIP)/core/ipv4/inet_chksum.c $(LWIP)/core/ipv4/ip_addr.c \
        $(LWIP)/core/ipv4/ip.c $(LWIP)/core/ipv4/ip_frag.c \
        $(LWIP)/netif/etharp.c $(LWIP)/core/dhcp.c

APP_SRCS += lib/skub.c file/ild-player.c file/playback.c lib/fixpoint.c \
	file/autoplay.c

APP_SRCS += abstract/render.c abstract/io.c

# Ethernet driver
BRD_SRCS += lib/ether.c lib/network.c drivers/source/mdio.c ../common/lib/eeprom.c

# FAT
APP_SRCS += ../common/lib/ff.c ../common/lib/ccsbcs.c ../common/lib/diskio.c
APP_SRCS += lib/fatfs.c
BRD_SRCS += ../common/lib/sdcard.c 
INCLUDES += -Ilib/fatfs

CFLAGS += $(INCLUDES)

# HTTP
# SRCS += net/http/httpd.c net/http/fs.c

# LPCUSB
BRD_SRCS += lib/lpcusb/usbhw_lpc.c lib/lpcusb/usbinit.c ../common/lib/usbstdreq.c \
	../common/lib/usbcontrol.c ../common/lib/usbhw_lpc.c lib/lpcusb/usbtest.c net/bpm.c

APP_SRCS += net/point-stream.c net/broadcast.c net/sink.c net/osc.c \
	net/ilda-osc.c net/correction-osc.c net/abstract-osc.c lib/transform.c net/ifconfig-osc.c

SRCS = $(BRD_SRCS) $(APP_SRCS)

OBJS1 = $(SRCS:.c=.o)
OBJS = $(OBJS1:.s=.o)

all: j4cDAC.hex j4cDAC.bin size

PC_SRCS = pc.c unix/rawif.c unix/sdcard.c unix/dac.c

pc: $(PC_SRCS) $(APP_SRCS)
	gcc $(PC_SRCS) $(APP_SRCS) $(INCLUDES) -DPC_BUILD '-DTABLE_PREFIX=".fini_array.table."' -o pc -Wall -g -m32 -Wl,-Map,pc.map

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

../boot/boot.hex:
	make -C ../boot

j4cDAC-full.hex: j4cDAC.bin ../boot/boot.hex
	grep '^:......00' ../boot/boot.hex | cat - j4cDAC.binhex > j4cDAC-full.hex

j4cDAC.hex: j4cDAC.elf
	$(PREFIX)objcopy -O ihex -R .eeprom -R .fuse -R .lock $< $@

j4cDAC.bin: j4cDAC.hex
	../tools/image.py $< $@

j4cDAC.elf: $(OBJS)
	$(CC) $(CFLAGS) -c ../common/lib/build_id.c -o build_id.o -DBUILDID=\"$(BUILDID)\"
	$(CC) $(LDFLAGS) $(OBJS) build_id.o -o j4cDAC.elf

.PHONY: clean flash bl size term

clean:
	rm -f j4cDAC.elf j4cDAC.hex j4cDAC.map j4cDAC.bin j4cDAC.binhex j4cDAC-full.hex build-id.o pc $(OBJS)

flash: j4cDAC-full.hex
	~/lpc21isp/lpc21isp -debug3 j4cDAC-full.hex /dev/ttyUSB0 230400 14746

bl:
	sudo ../tools/f0ad/f0ad
	sleep 3
	sudo dfu-util -D j4cDAC.bin -R

size:
	$(PREFIX)size -A j4cDAC.elf
	$(PREFIX)nm -anS --size-sort -t d j4cDAC.elf | grep ^2 | tail -20
	@echo
	@echo "->" $(BUILDID)

term:
	../tools/miniterm/miniterm.py -b 230400 -p /dev/ttyUSB0 --rts=0
