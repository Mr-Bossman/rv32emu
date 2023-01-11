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

#define MINIRV32_RAM_IMAGE_OFFSET 0x80000000
static int is_eofd;

static void DumpState(struct MiniRV32IMAState* core, uint8_t* ram_image);
uint32_t HandleControlLoad(uint32_t addy);
uint32_t HandleControlStore(uint32_t addy, uint32_t val);
static int IsKBHit();
static int ReadKBByte();
static uint64_t GetTimeMicroseconds();
static void CtrlC();

uint8_t* ram_image = 0;
struct MiniRV32IMAState* core;

int main(int argc, char** argv) {
	size_t binary_sixtyfourmb_dtb_size = _binary_sixtyfourmb_dtb_end - _binary_sixtyfourmb_dtb_start;
	int i;
	int show_help = 0;
	int dtb_ptr = 0;
	uint32_t ram_amt = 64 * 1024 * 1024;
	const char* image_file_name = 0;
	const char* dtb_file_name = 0;
	signal(SIGINT, CtrlC);
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

restart : {
	FILE* f = fopen(image_file_name, "rb");
	if (!f || ferror(f)) {
		fprintf(stderr, "Error: \"%s\" not found\n", image_file_name);
		return -5;
	}
	fseek(f, 0, SEEK_END);
	long flen = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (flen > ram_amt) {
		fprintf(stderr, "Error: Could not fit RAM image (%ld bytes) into %d\n", flen,
		        ram_amt);
		return -6;
	}

	memset(ram_image, 0, ram_amt);
	if (fread(ram_image, flen, 1, f) != 1) {
		fprintf(stderr, "Error: Could not load image.\n");
		return -7;
	}
	fclose(f);

	if (dtb_file_name) {
		if (strcmp(dtb_file_name, "disable") == 0) {
			// No DTB reading.
		} else {
			f = fopen(dtb_file_name, "rb");
			if (!f || ferror(f)) {
				fprintf(stderr, "Error: \"%s\" not found\n", dtb_file_name);
				return -5;
			}
			fseek(f, 0, SEEK_END);
			long dtblen = ftell(f);
			fseek(f, 0, SEEK_SET);
			dtb_ptr = ram_amt - dtblen - sizeof(struct MiniRV32IMAState);
			if (fread(ram_image + dtb_ptr, dtblen, 1,
			          f) != 1) {
				fprintf(stderr, "Error: Could not open dtb \"%s\"\n",
				        dtb_file_name);
				return -9;
			}
			fclose(f);
		}
	} else {
		// Load a default dtb.
		dtb_ptr = ram_amt -  binary_sixtyfourmb_dtb_size - sizeof(struct MiniRV32IMAState);
		memcpy(ram_image + dtb_ptr, _binary_sixtyfourmb_dtb_start, binary_sixtyfourmb_dtb_size);
	}
}

	// The core lives at the end of RAM.
	core = (struct MiniRV32IMAState*)(ram_image + ram_amt - sizeof(struct MiniRV32IMAState));
	core->total_mem = ram_amt;
	core->mem = ram_image;
	core->base_ofs = MINIRV32_RAM_IMAGE_OFFSET;
	core->csr[csr_pc] = MINIRV32_RAM_IMAGE_OFFSET;
	core->regs[10] = 0x00; // hart ID
	core->regs[11] = dtb_ptr ? (dtb_ptr + MINIRV32_RAM_IMAGE_OFFSET)
	                         : 0;   // dtb_pa (Must be valid pointer) (Should be pointer to dtb)
	core->csr[csr_mvendorid] = 0xff0ff0ff; // mvendorid
	core->csr[csr_misa] = 0x40401101; // marchid
	core->csr[csr_extraflags] |= 3; // Machine-mode.

	if (dtb_file_name == 0) {
		// Update system ram size in DTB (but if and only if we're using the default
		// DTB) Warning - this will need to be updated if the skeleton DTB is ever
		// modified.
		uint32_t* dtb = (uint32_t*)(ram_image + dtb_ptr);
		if (dtb[0x13c / 4] == 0x00c0ff03) {
			uint32_t validram = dtb_ptr;
			dtb[0x13c / 4] = (validram >> 24) | (((validram >> 16) & 0xff) << 8) |
			                 (((validram >> 8) & 0xff) << 16) |
			                 ((validram & 0xff) << 24);
		}
	}

	// Image is loaded.
	uint64_t rt;
	uint64_t time_start = GetTimeMicroseconds();
	int instrs_per_flip = 10000;
	for (rt = 0;; rt += instrs_per_flip) {
		uint64_t time_n = GetTimeMicroseconds() - time_start;
		core->csr[csr_timerl] = time_n & UINT32_MAX;
		core->csr[csr_timerh] = time_n >> 32;
		int ret = MiniRV32IMAStep(
		    core, 0,
		    instrs_per_flip); // Execute upto 1024 cycles before breaking out.
		switch (ret) {
		case 0:
			break;
		case 1:
			uint64_t this_ccount = core->csr[csr_cyclel] | ((uint64_t)core->csr[csr_cycleh] << 32);
			this_ccount += instrs_per_flip;
			core->csr[csr_cyclel] = this_ccount & UINT32_MAX;
			core->csr[csr_cycleh] = this_ccount >> 32;
			break;
		case 3:
			DumpState(core, ram_image);
			return 0;
			break;
		case 0x7777:
			goto restart; // syscon code for restart
		default:
			printf("Unknown failure\n");
			break;
		}
	}

	DumpState(core, ram_image);
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

static void CtrlC() {
	DumpState(core, ram_image);
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
	int rread = read(fileno(stdin), (char*)&rxchar, 1);

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
	if (!byteswaiting && write(fileno(stdin), 0, 0) != 0) {
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
