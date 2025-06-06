/*
 * Copyright (C) 2004 Jeff Dike (jdike@addtoit.com)
 * Licensed under the GPL
 */

#ifndef __SYSDEP_STUB_H
#define __SYSDEP_STUB_H

#include <stddef.h>
#include <sysdep/ptrace_user.h>
#include <generated/asm-offsets.h>
#include <linux/stddef.h>
#include <asm/prctl.h>

#define STUB_MMAP_NR __NR_mmap
#define MMAP_OFFSET(o) (o)

#define __syscall_clobber "r11","rcx","memory"
#define __syscall "syscall"

static __always_inline long stub_syscall0(long syscall)
{
	long ret;

	__asm__ volatile (__syscall
		: "=a" (ret)
		: "0" (syscall) : __syscall_clobber );

	return ret;
}

static __always_inline long stub_syscall1(long syscall, long arg1)
{
	long ret;

	__asm__ volatile (__syscall
		: "=a" (ret)
		: "0" (syscall), "D" (arg1) : __syscall_clobber );

	return ret;
}

static __always_inline long stub_syscall2(long syscall, long arg1, long arg2)
{
	long ret;

	__asm__ volatile (__syscall
		: "=a" (ret)
		: "0" (syscall), "D" (arg1), "S" (arg2) : __syscall_clobber );

	return ret;
}

static __always_inline long stub_syscall3(long syscall, long arg1, long arg2,
					  long arg3)
{
	long ret;

	__asm__ volatile (__syscall
		: "=a" (ret)
		: "0" (syscall), "D" (arg1), "S" (arg2), "d" (arg3)
		: __syscall_clobber );

	return ret;
}

static __always_inline long stub_syscall4(long syscall, long arg1, long arg2, long arg3,
				 long arg4)
{
	long ret;

	__asm__ volatile ("movq %5,%%r10 ; " __syscall
		: "=a" (ret)
		: "0" (syscall), "D" (arg1), "S" (arg2), "d" (arg3),
		  "g" (arg4)
		: __syscall_clobber, "r10" );

	return ret;
}

static __always_inline long stub_syscall5(long syscall, long arg1, long arg2,
					  long arg3, long arg4, long arg5)
{
	long ret;

	__asm__ volatile ("movq %5,%%r10 ; movq %6,%%r8 ; " __syscall
		: "=a" (ret)
		: "0" (syscall), "D" (arg1), "S" (arg2), "d" (arg3),
		  "g" (arg4), "g" (arg5)
		: __syscall_clobber, "r10", "r8" );

	return ret;
}

static __always_inline long stub_syscall6(long syscall, long arg1, long arg2,
					  long arg3, long arg4, long arg5,
					  long arg6)
{
	long ret;

	__asm__ volatile ("movq %5,%%r10 ; movq %6,%%r8 ; movq %7,%%r9 ; "
		__syscall
		: "=a" (ret)
		: "0" (syscall), "D" (arg1), "S" (arg2), "d" (arg3),
		  "g" (arg4), "g" (arg5), "g" (arg6)
		: __syscall_clobber, "r10", "r8", "r9");

	return ret;
}

static __always_inline void trap_myself(void)
{
	__asm("int3");
}

static __always_inline void *get_stub_data(void)
{
	unsigned long ret;

	asm volatile (
		"lea 0(%%rip), %0;"
		"andq %1, %0 ;"
		"addq %2, %0 ;"
		: "=a" (ret)
		: "g" (~(UM_KERN_PAGE_SIZE - 1)),
		  "g" (UM_KERN_PAGE_SIZE));

	return (void *)ret;
}

#define stub_start(fn)							\
	asm volatile (							\
		"subq %0,%%rsp ;"					\
		"movq %1,%%rax ;"					\
		"call *%%rax ;"						\
		:: "i" ((1 + STUB_DATA_PAGES) * UM_KERN_PAGE_SIZE),	\
		   "i" (&fn))

static __always_inline void
stub_seccomp_restore_state(struct stub_data_arch *arch)
{
	/*
	 * We could use _writefsbase_u64/_writegsbase_u64 if the host reports
	 * support in the hwcaps (HWCAP2_FSGSBASE).
	 */
	if (arch->sync & STUB_SYNC_FS_BASE)
		stub_syscall2(__NR_arch_prctl, ARCH_SET_FS, arch->fs_base);
	if (arch->sync & STUB_SYNC_GS_BASE)
		stub_syscall2(__NR_arch_prctl, ARCH_SET_GS, arch->gs_base);

	arch->sync = 0;
}

#endif
