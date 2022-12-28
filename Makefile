all : mini-rv32ima

mini-rv32ima : mini-rv32ima.c riscv-emu.c
	gcc -o $@  riscv-emu.c mini-rv32ima.c -g -O4 -Wall

testkern : mini-rv32ima
	./mini-rv32ima -f ~/linux/linux/arch/riscv/boot/Image

clean :
	rm -rf mini-rv32ima mini-rv32ima.flt

