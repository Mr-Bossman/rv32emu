// Copyright 2022 Charles Lohr, you may use this file or any portions herein
// under any of the BSD, MIT, or CC0 licenses.

#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include "riscv-emu.h"

extern const unsigned char _binary_sixtyfourmb_dtb_start[], _binary_sixtyfourmb_dtb_end[];
/* Hack to get around gcc weirdness */
#define _binary_sixtyfourmb_dtb_size (_binary_sixtyfourmb_dtb_end - _binary_sixtyfourmb_dtb_start)

#define MINIRV32_RAM_IMAGE_OFFSET 0x80000000
static int is_eofd;

static void DumpState(struct MiniRV32IMAState* core, uint8_t* ram_image);
static int IsKBHit();
static int ReadKBByte();
static uint64_t GetTimeMicroseconds();
static void exit_now();

static size_t get_Fsize(FILE* fno);
static int populate_ram(struct MiniRV32IMAState* core, const char* F_dtb, const char* F_kern, size_t *dtb_location);
uint8_t* ram_image = 0;
struct MiniRV32IMAState core;

int main(int argc, char** argv) {
	int i;
	int show_help = 0;
	size_t dtb_location = 0;
	uint32_t ram_amt = 64 * 1024 * 1024;
	const char* image_file_name = 0;
	const char* dtb_file_name = 0;
	signal(SIGINT, exit_now);
	for (i = 1; i < argc; i++) {
		const char* param = argv[i];
		int param_continue = 0; // Can combine parameters, like -lpt x
		do {
			if (param[0] == '-' || param_continue) {
				switch (param[1]) {
				case 'f':
					image_file_name = (++i < argc) ? argv[i] : 0;
					break;
				case 'b':
					dtb_file_name = (++i < argc) ? argv[i] : 0;
					break;
				default:
					break;
				}
			} else {
				show_help = 1;
				break;
			}
			param++;
		} while (param_continue);
	}
	if (show_help || image_file_name == 0) {
		fprintf(stderr,
		        "./mini-rv32imaf [parameters]\n\t-m [ram amount]\n\t-f [running "
		        "image]\n\t-b [dtb file, or 'disable']\n\t-c instruction count\n\t-s "
		        "single step with full processor state\n\t-t time divion base\n\t-l "
		        "lock time base to instruction count\n\t-p disable sleep when "
		        "wfi\n\t-d fail out immediately on all faults\n");
		return 1;
	}

	ram_image = malloc(ram_amt);
	if (!ram_image) {
		fprintf(stderr, "Error: could not allocate system image.\n");
		return -4;
	}

restart :

	// The core lives at the end of RAM.
	core.total_mem = ram_amt;
	core.mem = ram_image;
	core.base_ofs = MINIRV32_RAM_IMAGE_OFFSET;
	core.csr[csr_pc] = MINIRV32_RAM_IMAGE_OFFSET;
	populate_ram(&core, dtb_file_name,image_file_name,&dtb_location);
	core.regs[10] = 0x00; // hart ID
	core.regs[11] = dtb_location ? (dtb_location + MINIRV32_RAM_IMAGE_OFFSET)
	                         : 0;   // dtb_pa (Must be valid pointer) (Should be pointer to dtb)
	core.csr[csr_mvendorid] = 0xff0ff0ff; // mvendorid
	core.csr[csr_misa] = 0x40401101; // marchid
	core.csr[csr_extraflags] |= 3; // Machine-mode.
	if (dtb_file_name == 0) {
		// Update system ram size in DTB (but if and only if we're using the default
		// DTB) Warning - this will need to be updated if the skeleton DTB is ever
		// modified.
		uint32_t* dtb = (uint32_t*)(ram_image + dtb_location);
		if (dtb[0x13c / 4] == 0x00c0ff03) {
			uint32_t validram = dtb_location;
			dtb[0x13c / 4] = (validram >> 24) | (((validram >> 16) & 0xff) << 8) |
			                 (((validram >> 8) & 0xff) << 16) |
			                 ((validram & 0xff) << 24);
		}
	}

	// Image is loaded.
	uint64_t time_start = GetTimeMicroseconds();
	uint64_t instrs_per_flip = 100000;
	for (uint64_t rt = 0;;rt += instrs_per_flip) {
		uint64_t time_n = (GetTimeMicroseconds() - time_start);
		core.csr[csr_timerl] = time_n & UINT32_MAX;
		core.csr[csr_timerh] = time_n >> 32;
		int ret = MiniRV32IMAStep(&core, instrs_per_flip);
		switch (ret) {
		case 0:
			break;
		case 1:
			uint64_t this_ccount = core.csr[csr_cyclel] | ((uint64_t)core.csr[csr_cycleh] << 32);
			this_ccount += instrs_per_flip;
			core.csr[csr_cyclel] = this_ccount & UINT32_MAX;
			core.csr[csr_cycleh] = this_ccount >> 32;
			break;
		case 3:
			exit_now();
			break;
		case 0x7777:
			goto restart; // syscon code for restart
		default:
			printf("Unknown failure\n");
			break;
		}
	}
	exit_now();
}


 static int populate_ram(struct MiniRV32IMAState* core, const char* F_dtb, const char* F_kern, size_t *dtb_location) {
	FILE* dtb_fno = NULL;
	FILE* kern_fno = NULL;
	size_t dtb_size = (size_t)_binary_sixtyfourmb_dtb_size;
	size_t kern_size = 0;

	if (F_dtb) {
		dtb_fno = fopen(F_dtb, "rb");
		if (!dtb_fno || ferror(dtb_fno)) {
			fprintf(stderr, "Error: Could not open: \"%s\"\n", F_dtb);
			return -5;
		}
		dtb_size = get_Fsize(dtb_fno);
	}

	if (F_kern) {
		kern_fno = fopen(F_kern, "rb");
		if (!kern_fno || ferror(kern_fno)) {
			fprintf(stderr, "Error: Could not open: \"%s\"\n", F_kern);
			return -5;
		}
		kern_size = get_Fsize(kern_fno);
		if((kern_size+dtb_size) > core->total_mem){
			fprintf(stderr, "Error: Could not fit dtb: %ld, kernel: %ld into ram: %d.\n", dtb_size, kern_size, core->total_mem);
			return -6;
		}
		if (fread(ram_image, kern_size, 1, kern_fno) != 1) {
			fprintf(stderr, "Error: Could not load image.\n");
			return -7;
		}
		fclose(kern_fno);
	}

	*dtb_location = core->total_mem - dtb_size - sizeof(struct MiniRV32IMAState);
	if (F_dtb) {
		if (fread(ram_image + *dtb_location, dtb_size, 1, dtb_fno) != 1) {
			fprintf(stderr, "Error: Could not fit dtb: %ld, kernel: %ld into ram: %d.\n", kern_size, dtb_size, core->total_mem);
			return -6;
		}
	} else
		memcpy(ram_image + *dtb_location, _binary_sixtyfourmb_dtb_start, dtb_size);
	return 0;
}

static size_t get_Fsize(FILE* fno) {
	size_t size = 0;
	size_t pos = 0;
	if(!fno || ferror(fno))
		return -0;
	pos = ftell(fno);
	fseek(fno, 0, SEEK_END);
	size = ftell(fno);
	fseek(fno, pos, SEEK_SET);
	return size;
}

uint32_t HandleControlStore(uint32_t addy, uint32_t val) {
	if (addy == 0x10000000) // UART 8250 / 16550 Data Buffer
	{
		printf("%c", val);
		fflush(stdout);
	}
	return 0;
}

uint32_t HandleControlLoad(uint32_t addy) {
	// Emulating a 8250 / 16550 UART
	if (addy == 0x10000005)
		return 0x60 | IsKBHit();
	else if (addy == 0x10000000 && IsKBHit())
		return ReadKBByte();
	return 0;
}

static void exit_now() {
	DumpState(&core, ram_image);
	exit(0);
}

static uint64_t GetTimeMicroseconds() {
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_usec + ((uint64_t)(tv.tv_sec)) * 1000000LL;
}

static int ReadKBByte() {
	if (is_eofd)
		return 0xffffffff;
	char rxchar = 0;
	int rread = read(0, (char*)&rxchar, 1);

	if (rread > 0) // Tricky: getchar can't be used with arrow keys.
		return rxchar;
	else
		return -1;
}

static int IsKBHit() {
	if (is_eofd)
		return -1;
	int byteswaiting;
	ioctl(0, FIONREAD, &byteswaiting);
	if (!byteswaiting && write(0, 0, 0) != 0) {
		is_eofd = 1;
		return -1;
	} // Is end-of-file for
	return !!byteswaiting;
}

static void DumpState(struct MiniRV32IMAState* core, uint8_t* ram_image) {
	uint32_t pc = core->csr[csr_pc];
	uint32_t pc_offset = pc - MINIRV32_RAM_IMAGE_OFFSET;
	uint32_t ir = 0;

	printf("PC: %08x ", pc);
	if (pc_offset >= 0 && pc_offset < (64*1024*1024) - 3) {
		ir = *((uint32_t*)(&((uint8_t*)ram_image)[pc_offset]));
		printf("[0x%08x] ", ir);
	} else
		printf("[xxxxxxxxxx] ");
	uint32_t* regs = core->regs;
	printf("Z:%08x ra:%08x sp:%08x gp:%08x tp:%08x t0:%08x t1:%08x t2:%08x "
	       "s0:%08x s1:%08x a0:%08x a1:%08x a2:%08x a3:%08x a4:%08x a5:%08x ",
	       regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], regs[8],
	       regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15]);
	printf("a6:%08x a7:%08x s2:%08x s3:%08x s4:%08x s5:%08x s6:%08x s7:%08x "
	       "s8:%08x s9:%08x s10:%08x s11:%08x t3:%08x t4:%08x t5:%08x t6:%08x\n",
	       regs[16], regs[17], regs[18], regs[19], regs[20], regs[21], regs[22], regs[23],
	       regs[24], regs[25], regs[26], regs[27], regs[28], regs[29], regs[30], regs[31]);
}
