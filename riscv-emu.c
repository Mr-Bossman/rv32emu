#include "riscv-emu.h"

#define RV32_CAST4B(ofs)       *(uint32_t*)(state->mem + ofs)
#define RV32_CAST2B(ofs)       *(uint16_t*)(state->mem + ofs)
#define RV32_CAST1B(ofs)       *(uint8_t*)(state->mem + ofs)

#define CONCAT(A, B) A##B
#define CSR(x)       state->csr[CONCAT(csr_, x)]
#define REG(x) state->regs[x]

static uint32_t get_pc(RV32_CPU* state) { return CSR(pc) - state->base_ofs; }
static uint32_t op_branch(RV32_CPU* state);
static uint32_t op_load(RV32_CPU* state, uint32_t* rrval);
static uint32_t op_store(RV32_CPU* state, uint32_t* rval);
static uint32_t op_arithmetic(RV32_CPU* state, uint32_t* rrval);
static uint32_t op_csr(RV32_CPU* state, uint32_t* rval);
static uint32_t op_amo(RV32_CPU* state, uint32_t* rval);
static uint32_t handle_op(RV32_CPU* state);

int32_t RV32_step(RV32_CPU* state, int count) {

	// Handle Timer interrupt.
	if ((CSR(timerh) > CSR(timermatchh) ||
	     (CSR(timerh) == CSR(timermatchh) && CSR(timerl) > CSR(timermatchl))) &&
	    (CSR(timermatchh) || CSR(timermatchl))) {
		CSR(extraflags) &= ~4; // Clear WFI
		CSR(mip) |= 1 << 7; // MTIP of MIP // https://stackoverflow.com/a/61916199/2926815
		                    // Fire interrupt.
	} else
		CSR(mip) &= ~(1 << 7);

	// If WFI, don't run processor.
	if (CSR(extraflags) & 4)
		return 1;

	for (int icount = 0; icount < count; icount++) {
		uint32_t trap = 0;
		uint32_t rval = 0;
		// Increment both wall-clock and instruction count time.  (NOTE: Not
		// strictly needed to run Linux)
		CSR(cyclel)++;
		if (CSR(cyclel) == 0)
			CSR(cycleh)++;

		uint32_t ofs_pc = get_pc(state);

		if (ofs_pc >= state->total_mem)
			trap = 1 + 1; // Handle access violation on instruction read.
		else if (ofs_pc & 3)
			trap = 1 + 0; // Handle PC-misaligned access
		else
			trap = handle_op(state);
		// Handle traps and interrupts.
		if (trap) {
			if (trap & 0x80000000) // If prefixed with 1 in MSB, it's an interrupt,
			                       // not a trap.
			{
				CSR(extraflags) &= ~8;
				CSR(mcause) = trap;
				CSR(mtval) = 0;
				CSR(pc) += 4; // PC needs to point to where the PC will return to.
			} else {
				CSR(mcause) = trap - 1;
				CSR(mtval) = (trap > 5 && trap <= 8) ? rval : CSR(pc);
			}
			CSR(mepc) = CSR(pc); // TRICKY: The kernel advances mepc automatically.
			// CSR( mstatus ) & 8 = MIE, & 0x80 = MPIE
			//  On an interrupt, the system moves current MIE into MPIE
			CSR(mstatus) = ((CSR(mstatus) & 0x08) << 4) | ((CSR(extraflags) & 3) << 11);
			CSR(pc) = (CSR(mtvec) - 4);

			// XXX TODO: Do we actually want to check here? Is this correct?
			if (!(trap & 0x80000000))
				CSR(extraflags) |= 3;
		}

		CSR(pc) += 4;
	}
	return 0;
}

static uint32_t handle_op(RV32_CPU* state) {
	uint32_t rval = 0, trap = 0, ir;
	ir = RV32_CAST4B(get_pc(state));
	uint32_t rdid = (ir >> 7) & 0x1f;

	switch (ir & 0x7f) {
	case 0b0110111: // LUI
		rval = (ir & 0xfffff000);
		break;
	case 0b0010111: // AUIPC
		rval = CSR(pc) + (ir & 0xfffff000);
		break;
	case 0b1101111: // JAL
	{
		int32_t reladdy = ((ir & 0x80000000) >> 11) | ((ir & 0x7fe00000) >> 20) |
		                  ((ir & 0x00100000) >> 9) | ((ir & 0x000ff000));
		if (reladdy & 0x00100000)
			reladdy |= 0xffe00000; // Sign extension.
		rval = CSR(pc) + 4;
		CSR(pc) = CSR(pc) + reladdy - 4;
		break;
	}
	case 0b1100111: // JALR
	{
		uint32_t imm = ir >> 20;
		int32_t imm_se = imm | ((imm & 0x800) ? 0xfffff000 : 0);
		rval = CSR(pc) + 4;
		CSR(pc) = ((REG((ir >> 15) & 0x1f) + imm_se) & ~1) - 4;
		break;
	}
	case 0b1100011: // Branch
		rdid = 0;
		trap = op_branch(state);
		break;
	case 0b0000011: // Load
		trap = op_load(state, &rval);
		break;
	case 0b0100011: // Store
		rdid = 0;
		trap = op_store(state, &rval);
		break;
	case 0b0010011: // Op-immediate
	case 0b0110011: // Op
		trap = op_arithmetic(state, &rval);
		break;
	case 0b1110011: // Zifencei+Zicsr
	{
		if (!((ir >> 12) & 0b111))
			rdid = 0;
		trap = op_csr(state, &rval);
		break;
	}
	case 0b0001111: // Fence
	//	rdid = 0;
	//break;
	case 0b0101111:
	//	trap = op_amo(state, &rval);
	//	break;
	default:
		return (2 + 1); // Fault: Invalid opcode.
	}

	if (rdid && rdid != -1) {
		REG(rdid) = rval;
	} else if ((CSR(mip) & (1 << 7)) && (CSR(mie) & (1 << 7) /*mtie*/) &&
	           (CSR(mstatus) & 0x8 /*mie*/) ) {
		trap = 0x80000007; // Timer interrupt.
	}
	return trap;
}

static uint32_t op_branch(RV32_CPU* state) {
	uint32_t ir = RV32_CAST4B(get_pc(state));
	uint32_t immm4 = ((ir & 0xf00) >> 7) | ((ir & 0x7e000000) >> 20) | ((ir & 0x80) << 4) |
	                 ((ir >> 31) << 12);
	if (immm4 & 0x1000)
		immm4 |= 0xffffe000;
	int32_t rs1 = REG((ir >> 15) & 0x1f);
	int32_t rs2 = REG((ir >> 20) & 0x1f);
	immm4 = CSR(pc) + immm4 - 4;
	switch ((ir >> 12) & 0x7) {
	// BEQ, BNE, BLT, BGE, BLTU, BGEU
	case 0b000:
		if (rs1 == rs2)
			CSR(pc) = immm4;
		break;
	case 0b001:
		if (rs1 != rs2)
			CSR(pc) = immm4;
		break;
	case 0b100:
		if (rs1 < rs2)
			CSR(pc) = immm4;
		break;
	case 0b101:
		if (rs1 >= rs2)
			CSR(pc) = immm4;
		break; // BGE
	case 0b110:
		if ((uint32_t)rs1 < (uint32_t)rs2)
			CSR(pc) = immm4;
		break; // BLTU
	case 0b111:
		if ((uint32_t)rs1 >= (uint32_t)rs2)
			CSR(pc) = immm4;
		break; // BGEU
	default:
		return 3;
	}
	return 0;
}

static uint32_t op_load(RV32_CPU* state, uint32_t* rrval) {
	uint32_t rval = 0;
	uint32_t ir = RV32_CAST4B(get_pc(state));
	uint32_t rs1 = REG((ir >> 15) & 0x1f);
	uint32_t imm = ir >> 20;
	int32_t imm_se = imm | ((imm & 0x800) ? 0xfffff000 : 0);
	uint32_t rsval = rs1 + imm_se;

	rsval -= state->base_ofs;
	if (rsval >= state->total_mem - 3) {
		rsval -= state->base_ofs;
		if (rsval >= 0x10000000 && rsval < 0x12000000) // UART, CLNT
		{
			if (rsval ==
			    0x1100bffc) // https://chromitem-soc.readthedocs.io/en/latest/clint.html
				rval = CSR(timerh);
			else if (rsval == 0x1100bff8)
				rval = CSR(timerl);
			else
				rval = HandleControlLoad(rsval);
		} else if (rsval >= 0x400 && rsval < 0x400 + (18 * 4)) // CSR
		{
			rval = state->csr[(rsval >> 2) & 0xff];
		} else {
			return 6;
			rval = rsval;
		}
	} else {
		switch ((ir >> 12) & 0x7) {
		// LB, LH, LW, LBU, LHU
		case 0b000:
			rval = (int8_t)RV32_CAST1B(rsval);
			break;
		case 0b001:
			rval = (int16_t)RV32_CAST2B(rsval);
			break;
		case 0b010:
			rval = RV32_CAST4B(rsval);
			break;
		case 0b100:
			rval = RV32_CAST1B(rsval);
			break;
		case 0b101:
			rval = RV32_CAST2B(rsval);
			break;
		default:
			return 3;
		}
	}
	*rrval = rval;
	return 0;
}

static uint32_t op_store(RV32_CPU* state, uint32_t* rval) {
	uint32_t ir = RV32_CAST4B(get_pc(state));

	uint32_t rs1 = REG((ir >> 15) & 0x1f);
	uint32_t rs2 = REG((ir >> 20) & 0x1f);
	uint32_t addy = ((ir >> 7) & 0x1f) | ((ir & 0xfe000000) >> 20);
	if (addy & 0x800)
		addy |= 0xfffff000;
	addy += rs1 - state->base_ofs;

	if (addy >= state->total_mem - 3) {
		addy -= state->base_ofs;
		if (addy >= 0x10000000 && addy < 0x12000000) {
			// Should be stuff like SYSCON, 8250, CLNT
			if (addy == 0x11004004) // CLNT
				CSR(timermatchh) = rs2;
			else if (addy == 0x11004000) // CLNT
				CSR(timermatchl) = rs2;
			else if (addy == 0x11100000) // SYSCON (reboot,
			                             // poweroff, etc.)
			{
				CSR(pc) += 4;
				return rs2; // NOTE: PC will be PC of
				            // Syscon.
			} else if (HandleControlStore(addy, rs2))
				return rs2;
		} else if (addy >= 0x400 && addy < 0x400 + (18 * 4)) // CSR
		{
			state->csr[(addy >> 2) & 0xff] = rs2;
		} else {
			*rval = addy + state->base_ofs;
			return 8;
		}
	} else {
		switch ((ir >> 12) & 0x7) {
		// SB, SH, SW
		case 0b000:
			RV32_CAST1B(addy) = rs2;
			break;
		case 0b001:
			RV32_CAST2B(addy) = rs2;
			break;
		case 0b010:
			RV32_CAST4B(addy) = rs2;
			break;
		default:
			return (2 + 1);
		}
	}
	return 0;
}

static uint32_t op_arithmetic(RV32_CPU* state, uint32_t* rrval) {
	uint32_t rval = 0;
	uint32_t ir = RV32_CAST4B(get_pc(state));

	uint32_t imm = ir >> 20;
	imm = imm | ((imm & 0x800) ? 0xfffff000 : 0);
	uint32_t rs1 = REG((ir >> 15) & 0x1f);
	uint32_t is_reg = !!(ir & 0b100000);
	uint32_t rs2 = is_reg ? REG(imm & 0x1f) : imm;

	if (is_reg && (ir & 0x02000000)) {
		return (2 + 1);
	} else {
		switch ((ir >> 12) & 7) // These could be either op-immediate or op
		                        // commands.  Be careful.
		{
		case 0b000:
			rval = (is_reg && (ir & 0x40000000)) ? (rs1 - rs2) : (rs1 + rs2);
			break;
		case 0b001:
			rval = rs1 << rs2;
			break;
		case 0b010:
			rval = (int32_t)rs1 < (int32_t)rs2;
			break;
		case 0b011:
			rval = rs1 < rs2;
			break;
		case 0b100:
			rval = rs1 ^ rs2;
			break;
		case 0b101:
			rval = (ir & 0x40000000) ? (((int32_t)rs1) >> rs2) : (rs1 >> rs2);
			break;
		case 0b110:
			rval = rs1 | rs2;
			break;
		case 0b111:
			rval = rs1 & rs2;
			break;
		}
	}
	*rrval = rval;
	return 0;
}

static uint32_t op_csr(RV32_CPU* state, uint32_t* rval) {
	uint32_t ir = RV32_CAST4B(get_pc(state));
	uint32_t i, csrno = ir >> 20;
	int microop = (ir >> 12) & 0b111;
	if ((microop & 3)) // It's a Zicsr function.
	{
		return (2 + 1);
		int rs1imm = (ir >> 15) & 0x1f;
		if (!(microop >> 2))
			rs1imm = REG(rs1imm);
		uint32_t csrnums[18] = {0x300, 0xC00, 0x340, 0x305, 0x304, 0x344,
					0x341, 0x343, 0x342, 0xf11, 0x301,0xf14,0x3a0,0x3b0};
		for (i = 0; i < 18;)
			if (csrno == csrnums[i++])
				break;
		if(i > 13)
			return (2 + 1);
		if(i > 11)
			return 0;

		csrno = i-1;
		uint32_t writeval = state->csr[csrno];
		*rval = writeval;
		switch (microop & 0x3) {
		case 0b01:
			writeval = rs1imm;
			break; // CSRW
		case 0b10:
			writeval |= rs1imm;
			break; // CSRRS
		case 0b11:
			writeval &= ~rs1imm;
			break; // CSRRC
		}
		state->csr[csrno] = writeval;
	} else if (microop == 0b000) // "SYSTEM"
	{
		if (csrno == 0x105) // WFI (Wait for interrupts)
		{
			CSR(mstatus) |= 8;    // Enable interrupts
			CSR(extraflags) |= 4; // Infor environment we want to go to sleep.
			CSR(pc) += 4;
			return 0;
		} else if (((csrno & 0xff) == 0x02)) // MRET
		{
			// https://raw.githubusercontent.com/riscv/virtual-memory/main/specs/663-Svpbmt.pdf
			// Table 7.6. MRET then in mstatus/mstatush sets
			// MPV=0, MPP=0, MIE=MPIE, and MPIE=1. La
			//  Should also update mstatus to reflect correct
			//  mode.
			uint32_t startmstatus = CSR(mstatus);
			uint32_t startextraflags = CSR(extraflags);
			CSR(extraflags) = (startextraflags & ~3) | ((startmstatus >> 11) & 3);
			CSR(mstatus) = ((startmstatus & 0x80) >> 4) | ((startextraflags & 3) << 11) | 0x80;
			CSR(pc) = CSR(mepc) - 4;
		} else {
			switch (csrno) {
			case 0:
				return (CSR(extraflags) & 3) ? (11 + 1) : (8 + 1);
				break; // ECALL; 8 = "Environment call from
				       // U-mode"; 11 = "Environment call
				       // from M-mode"
			case 1:
				return (3 + 1);
				break; // EBREAK 3 = "Breakpoint"
			default:
				return (2 + 1);
				break; // Illegal opcode.
			}
		}
	} else
		return (2 + 1); // Note micrrop 0b100 == undefined.
	return 0;
}

static uint32_t op_amo(RV32_CPU* state, uint32_t* rval)  {
	uint32_t ir = RV32_CAST4B(get_pc(state));
	uint32_t rs1 = REG((ir >> 15) & 0x1f);
	uint32_t rs2 = REG((ir >> 20) & 0x1f);
	uint32_t irmid = (ir >> 27) & 0x1f;

	rs1 -= state->base_ofs;

	if (rs1 >= state->total_mem - 3) {
		*rval = rs1 + state->base_ofs;
		return	(7 + 1); // Store/AMO access fault
	} else {
		*rval = RV32_CAST4B(rs1);

		// Referenced a little bit of
		// https://github.com/franzflasch/riscv_em/blob/master/src/core/core.c
		uint32_t dowrite = 1;
		switch (irmid) {
		case 0b00010:
			dowrite = 0;
			break; // LR.W
		case 0b00011:
			*rval = 0;
			break; // SC.W (Lie and always say it's good)
		case 0b00001:
			break; // AMOSWAP.W
		case 0b00000:
			rs2 += *rval;
			break; // AMOADD.W
		case 0b00100:
			rs2 ^= *rval;
			break; // AMOXOR.W
		case 0b01100:
			rs2 &= *rval;
			break; // AMOAND.W
		case 0b01000:
			rs2 |= *rval;
			break; // AMOOR.W
		case 0b10000:
			rs2 = ((int32_t)rs2 < (int32_t)*rval) ? rs2 : *rval;
			break; // AMOMIN.W
		case 0b10100:
			rs2 = ((int32_t)rs2 > (int32_t)*rval) ? rs2 : *rval;
			break; // AMOMAX.W
		case 0b11000:
			rs2 = (rs2 < *rval) ? rs2 : *rval;
			break; // AMOMINU.W
		case 0b11100:
			rs2 = (rs2 > *rval) ? rs2 : *rval;
			break; // AMOMAXU.W
		default:
			return (2 + 1);
			dowrite = 0;
			break; // Not supported.
		}
		if (dowrite)
			RV32_CAST4B(rs1) = rs2;
	}
	return 0;
}
