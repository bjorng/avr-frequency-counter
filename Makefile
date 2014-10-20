#    Frequency counter for ATTiny84A
#    Copyright (C) 2014  Bjorn Gustavsson
#
#    This program is free software; you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation; either version 2 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License along
#    with this program; if not, write to the Free Software Foundation, Inc.,
#    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

# Add settings like this to your ~/.avrduderc file to specify which
# hardware to use to upload to the AVR:
#   default_programmer = "avrispmkii";
#   default_serial = "usb";

DEVICE     = attiny84a
AVRDUDE_DEVICE = attiny84
CLOCK      = 20000000
OBJECTS    = main.o
FUSES      = -U lfuse:w:0xef:m -U hfuse:w:0xdf:m -U efuse:w:0xff:m

# ATTiny84A fuse bits used above.
#
# For computing fuse byte values for other devices and options see
# the fuse bit calculator at http://www.engbedded.com/fusecalc/

AVRDUDE = avrdude -p $(AVRDUDE_DEVICE)
COMMON_FLAGS = -Wall -Os -DF_CPU=$(CLOCK) -mmcu=$(DEVICE) \
   -ffunction-sections -fdata-sections -MMD
COMPILE = avr-gcc $(COMMON_FLAGS)

all:	main.hex

.c.o:
	$(COMPILE) -c $< -o $@

.cpp.o:
	$(COMPILE_CPP) -c $< -o $@

.S.o:
	$(COMPILE) -x assembler-with-cpp -c $< -o $@
.c.s:
	$(COMPILE) -S $< -o $@

flash:	all
	$(AVRDUDE) -U flash:w:main.hex:i

fuse:
	$(AVRDUDE) $(FUSES)

clean:
	rm -f main.hex main.elf $(OBJECTS)

main.elf: $(OBJECTS)
	$(COMPILE) -o main.elf -Os -Wl,--gc-sections $(OBJECTS)

main.hex: main.elf
	avr-objcopy -O ihex -R .eeprom main.elf main.hex
	avr-size --format=avr --mcu=$(DEVICE) main.elf

disasm:	main.elf
	avr-objdump -d main.elf

cpp:
	$(COMPILE) -E main.c
