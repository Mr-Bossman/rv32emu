CFLAGS = -Wno-unused-function  -Wall -pedantic -std=c2x

all : rv32emu

sixtyfourmb.o :
	objcopy -I binary -O elf64-x86-64 --rename-section .data=.rodata sixtyfourmb.dtb sixtyfourmb.o

rv32emu : rv32emu.c riscv-emu.c sixtyfourmb.o
	gcc ${CFLAGS} -o $@ sixtyfourmb.o riscv-emu.c rv32emu.c -g -O3 -Wall

testkern : rv32emu
	./rv32emu -f Image

clean :
	rm -rf rv32emu *.o
