#include <stdint.h>

#ifndef __RISCV_EMU_H__
#define __RISCV_EMU_H__

typedef struct MiniRV32IMAState {
	uint32_t regs[32];
	uint32_t csr[18];
	uint32_t total_mem;
	uint32_t base_ofs;
	uint8_t *mem;
} MiniRV32IMAState;

uint32_t HandleControlLoad(uint32_t addy);
uint32_t HandleControlStore(uint32_t addy, uint32_t val);
int32_t MiniRV32IMAStep(MiniRV32IMAState* state, int count);

enum {
	csr_mstatus,
	csr_cyclel,
	csr_mscratch,
	csr_mtvec,
	csr_mie,
	csr_mip,
	csr_mepc,
	csr_mtval,
	csr_mcause,
	csr_mvendorid,
	csr_misa,
	csr_pc,
	csr_extraflags,
	csr_cycleh,
	csr_timerl,
	csr_timerh,
	csr_timermatchl,
	csr_timermatchh,
};

#endif
