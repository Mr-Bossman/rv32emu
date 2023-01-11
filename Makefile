all : mini-rv32ima

sixtyfourmb.o :
	objcopy -I binary -O elf64-x86-64 --rename-section .data=.rodata sixtyfourmb.dtb sixtyfourmb.o

mini-rv32ima : mini-rv32ima.c riscv-emu.c sixtyfourmb.o
	gcc -o $@ sixtyfourmb.o riscv-emu.c mini-rv32ima.c -g -O3 -Wall

testkern : mini-rv32ima
	./mini-rv32ima -f Image

clean :
	rm -rf mini-rv32ima *.o
