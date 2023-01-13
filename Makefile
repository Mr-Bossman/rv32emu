BUILD_DIR = .
CC = gcc
CFLAGS = -Wno-unused-function  -Wall -pedantic -std=c2x -O3
C_SOURCES = rv32emu.c riscv-emu.c
LDFLAGS = -z noexecstack

OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o))) sixtyfourmb.o
vpath %.c $(sort $(dir $(C_SOURCES)))

all : rv32emu

sixtyfourmb.o :
	objcopy -I binary -O elf64-x86-64 --rename-section .data=.rodata sixtyfourmb.dtb sixtyfourmb.o

$(BUILD_DIR)/%.o: %.c
	${CC} -c $(CFLAGS) $< -o $@

rv32emu : $(OBJECTS)
	${CC} $(CFLAGS) $(LDGLAGS) -o rv32emu $(OBJECTS)

testkern : rv32emu
	./rv32emu -k Image

clean :
	rm -rf rv32emu ${BUILD_DIR}/*.o
