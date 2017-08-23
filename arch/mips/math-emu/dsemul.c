#include <asm/branch.h>
#include <asm/cacheflush.h>
#include <asm/fpu_emulator.h>
#include <asm/inst.h>
#include <asm/mipsregs.h>
#include <asm/vdso.h>
#include <asm/uaccess.h>

#include "ieee754.h"

/*
 * Emulate the arbritrary instruction ir at xcp->cp0_epc.  Required when
 * we have to emulate the instruction in a COP1 branch delay slot.  Do
 * not change cp0_epc due to the instruction
 *
 * According to the spec:
 * 1) it shouldn't be a branch :-)
 * 2) it can be a COP instruction :-(
 * 3) if we are tring to run a protected memory space we must take
 *    special care on memory access instructions :-(
 */

/*
 * "Trampoline" return routine to catch exception following
 *  execution of delay-slot instruction execution.
 */

struct emuframe {
	mips_instruction	emul;
	mips_instruction	badinst;
	mips_instruction	cookie;
	unsigned long		epc;
	unsigned long           bpc;
	unsigned long           r31;
};
/* round structure size to N*8 to force a fit two instructions in a single cache line */
#define EMULFRAME_ROUNDED_SIZE  ((sizeof(struct emuframe) + 0x7) & ~0x7)

int mips_dsemul(struct pt_regs *regs, mips_instruction ir, unsigned long cpc,
		unsigned long bpc, unsigned long r31)
{
	struct emuframe __user *fr;
	int err;
	unsigned char *pg_addr;

	/* NOP is easy */
	if (ir == 0)
		return -1;

	/* microMIPS instructions */
	if (get_isa16_mode(regs->cp0_epc)) {
		union mips_instruction insn = { .word = ir };

		/* NOP16 aka MOVE16 $0, $0 */
		if ((ir >> 16) == MM_NOP16)
			return -1;

		/* ADDIUPC */
		if (insn.mm_a_format.opcode == mm_addiupc_op) {
			unsigned int rs;
			s32 v;

			rs = (((insn.mm_a_format.rs + 0xe) & 0xf) + 2);
			v = regs->cp0_epc & ~3;
			v += insn.mm_a_format.simmediate << 2;
			regs->regs[rs] = (long)v;
			return -1;
		}
	}

	pr_debug("dsemul %lx %lx\n", regs->cp0_epc, cpc);

	/*
	 * The strategy is to push the instruction onto the user stack/VDSO page
	 * and put a trap after it which we can catch and jump to
	 * the required address any alternative apart from full
	 * instruction emulation!!.
	 *
	 * Algorithmics used a system call instruction, and
	 * borrowed that vector.  MIPS/Linux version is a bit
	 * more heavyweight in the interests of portability and
	 * multiprocessor support.  For Linux we use a BREAK 514
	 * instruction causing a breakpoint exception.
	 */

	if (current_thread_info()->vdso_page) {
		/*
		 * Use VDSO page and fill structure via kernel VA,
		 * user write is disabled
		 */
		pg_addr = (unsigned char *)page_address(current_thread_info()->vdso_page);
		fr = (struct emuframe __user *)
			    (pg_addr + current_thread_info()->vdso_offset -
			     EMULFRAME_ROUNDED_SIZE);

		/* verify that we don't overflow into trampoline areas */
		if ((unsigned char *)fr < (unsigned char *)(((struct mips_vdso *)pg_addr) + 1)) {
			MIPS_FPU_EMU_INC_STATS(errors);
			return SIGBUS;
		}

		current_thread_info()->vdso_offset -= EMULFRAME_ROUNDED_SIZE;

		if (get_isa16_mode(regs->cp0_epc)) {
			*(u16 *)&fr->emul = (u16)(ir >> 16);
			*((u16 *)(&fr->emul) + 1) = (u16)(ir & 0xffff);
			*((u16 *)(&fr->emul) + 2) = (u16)(BREAK_MATH >> 16);
			*((u16 *)(&fr->emul) + 3) = (u16)(BREAK_MATH &0xffff);
		} else {
			fr->emul = ir;
			fr->badinst = (mips_instruction)BREAK_MATH;
		}
		fr->cookie = (mips_instruction)BD_COOKIE;
		fr->epc = cpc;
		fr->bpc = bpc;
		fr->r31 = r31;

		/* fill CP0_EPC with user VA */
		regs->cp0_epc = ((unsigned long)(current->mm->context.vdso +
				current_thread_info()->vdso_offset)) |
				get_isa16_mode(regs->cp0_epc);
		if (cpu_has_dc_aliases)
			mips_flush_data_cache_range(current->mm->context.vdso_vma,
				regs->cp0_epc, current_thread_info()->vdso_page,
				(unsigned long)fr, sizeof(struct emuframe));
		else
			/* it is a less expensive on CPU with correct SYNCI */
			flush_cache_sigtramp((unsigned long)fr);
	} else {
		/* Ensure that the two instructions are in the same cache line */
		fr = (struct emuframe __user *)
			((regs->regs[29] - sizeof(struct emuframe)) & ~0x7);

		/* Verify that the stack pointer is not competely insane */
		if (unlikely(!access_ok(VERIFY_WRITE, fr, sizeof(struct emuframe))))
			return SIGBUS;

		if (get_isa16_mode(regs->cp0_epc)) {
			err = __put_user(ir >> 16, (u16 __user *)(&fr->emul));
			err |= __put_user(ir & 0xffff, (u16 __user *)((long)(&fr->emul) + 2));
			err |= __put_user(BREAK_MATH >> 16, (u16 __user *)(&fr->badinst));
			err |= __put_user(BREAK_MATH & 0xffff, (u16 __user *)((long)(&fr->badinst) + 2));
		} else {
			err = __put_user(ir, &fr->emul);
			err |= __put_user((mips_instruction)BREAK_MATH, &fr->badinst);
		}

		err |= __put_user((mips_instruction)BD_COOKIE, &fr->cookie);
		err |= __put_user(cpc, &fr->epc);

		if (unlikely(err)) {
			MIPS_FPU_EMU_INC_STATS(errors);
			return SIGBUS;
		}

		regs->cp0_epc = ((unsigned long) &fr->emul) |
			get_isa16_mode(regs->cp0_epc);

		flush_cache_sigtramp((unsigned long)&fr->emul);
	}

	return 0;
}

int do_dsemulret(struct pt_regs *xcp)
{
	struct emuframe __user *fr;
	unsigned long epc;
	u32 insn, cookie;
	int err = 0;
	u16 instr[2];

	fr = (struct emuframe __user *)
		(msk_isa16_mode(xcp->cp0_epc) - sizeof(mips_instruction));

	/*
	 * If we can't even access the area, something is very wrong, but we'll
	 * leave that to the default handling
	 */
	if (!access_ok(VERIFY_READ, fr, sizeof(struct emuframe)))
		return 0;

	/*
	 * Do some sanity checking on the stackframe:
	 *
	 *  - Is the instruction pointed to by the EPC an BREAK_MATH?
	 *  - Is the following memory word the BD_COOKIE?
	 */
	if (get_isa16_mode(xcp->cp0_epc)) {
		err = __get_user(instr[0], (u16 __user *)(&fr->badinst));
		err |= __get_user(instr[1], (u16 __user *)((long)(&fr->badinst) + 2));
		insn = (instr[0] << 16) | instr[1];
	} else {
		err = __get_user(insn, &fr->badinst);
	}
	err |= __get_user(cookie, &fr->cookie);

	if (unlikely(err || (insn != BREAK_MATH) || (cookie != BD_COOKIE) ||
	    (current_thread_info()->vdso_page &&
	     ((xcp->cp0_epc & PAGE_MASK) !=
			(unsigned long)current->mm->context.vdso)))) {
		MIPS_FPU_EMU_INC_STATS(errors);
		return 0;
	}

	/*
	 * At this point, we are satisfied that it's a BD emulation trap.  Yes,
	 * a user might have deliberately put two malformed and useless
	 * instructions in a row in his program, in which case he's in for a
	 * nasty surprise - the next instruction will be treated as a
	 * continuation address!  Alas, this seems to be the only way that we
	 * can handle signals, recursion, and longjmps() in the context of
	 * emulating the branch delay instruction.
	 */

	pr_debug("dsemulret\n");

	if (__get_user(epc, &fr->epc)) {		/* Saved EPC */
		/* This is not a good situation to be in */
		force_sig(SIGBUS, current);

		return 0;
	}

	if (current_thread_info()->vdso_page) {
		/* restore VDSO stack level */
		current_thread_info()->vdso_offset += EMULFRAME_ROUNDED_SIZE;
		if (current_thread_info()->vdso_offset > PAGE_SIZE) {
			/* This is not a good situation to be in */
			current_thread_info()->vdso_offset -= EMULFRAME_ROUNDED_SIZE;
			force_sig(SIGBUS, current);

			return 0;
		}
	}

	/* Set EPC to return to post-branch instruction */
	xcp->cp0_epc = epc;
	MIPS_FPU_EMU_INC_STATS(ds_emul);
	return 1;
}

/* check and adjust an emulation stack before start a signal handler */
void vdso_epc_adjust(struct pt_regs *xcp)
{
	struct emuframe __user *fr;
	unsigned long epc;
	unsigned long r31;

	while (current_thread_info()->vdso_offset < PAGE_SIZE) {
		epc = msk_isa16_mode(xcp->cp0_epc);
		if ((epc >= ((unsigned long)current->mm->context.vdso + PAGE_SIZE)) ||
		    (epc < (unsigned long)((struct mips_vdso *)current->mm->context.vdso + 1)))
			return;

		fr = (struct emuframe __user *)
			((unsigned long)current->mm->context.vdso +
			 current_thread_info()->vdso_offset);

		/*
		 * epc must point to emul instruction or badinst
		 * in case of emul - it is not executed, so return to start
		 *                   and restore GPR31
		 * in case of badinst - instruction is executed, return to destination
		 */
		if (epc == (unsigned long)&fr->emul) {
			__get_user(r31, &fr->r31);
			xcp->regs[31] = r31;
			__get_user(epc, &fr->bpc);
		} else {
			__get_user(epc, &fr->epc);
		}
		xcp->cp0_epc = epc;
		current_thread_info()->vdso_offset += EMULFRAME_ROUNDED_SIZE;
	}
}
