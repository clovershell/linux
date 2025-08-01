// SPDX-License-Identifier: GPL-2.0
/*
 * Clang Control Flow Integrity (CFI) error handling.
 *
 * Copyright (C) 2022 Google LLC
 */

#include <linux/bpf.h>
#include <linux/cfi_types.h>
#include <linux/cfi.h>

bool cfi_warn __ro_after_init = IS_ENABLED(CONFIG_CFI_PERMISSIVE);

enum bug_trap_type report_cfi_failure(struct pt_regs *regs, unsigned long addr,
				      unsigned long *target, u32 type)
{
	if (target)
		pr_err("CFI failure at %pS (target: %pS; expected type: 0x%08x)\n",
		       (void *)addr, (void *)*target, type);
	else
		pr_err("CFI failure at %pS (no target information)\n",
		       (void *)addr);

	if (cfi_warn) {
		__warn(NULL, 0, (void *)addr, 0, regs, NULL);
		return BUG_TRAP_TYPE_WARN;
	}

	return BUG_TRAP_TYPE_BUG;
}

/*
 * Declare two non-existent functions with types that match bpf_func_t and
 * bpf_callback_t pointers, and use DEFINE_CFI_TYPE to define type hash
 * variables for each function type. The cfi_bpf_* variables are used by
 * arch-specific BPF JIT implementations to ensure indirectly callable JIT
 * code has matching CFI type hashes.
 */
extern typeof(*(bpf_func_t)0) __bpf_prog_runX;
DEFINE_CFI_TYPE(cfi_bpf_hash, __bpf_prog_runX);

extern typeof(*(bpf_callback_t)0) __bpf_callback_fn;
DEFINE_CFI_TYPE(cfi_bpf_subprog_hash, __bpf_callback_fn);

#ifdef CONFIG_ARCH_USES_CFI_TRAPS
static inline unsigned long trap_address(s32 *p)
{
	return (unsigned long)((long)p + (long)*p);
}

static bool is_trap(unsigned long addr, s32 *start, s32 *end)
{
	s32 *p;

	for (p = start; p < end; ++p) {
		if (trap_address(p) == addr)
			return true;
	}

	return false;
}

#ifdef CONFIG_MODULES
/* Populates `kcfi_trap(_end)?` fields in `struct module`. */
void module_cfi_finalize(const Elf_Ehdr *hdr, const Elf_Shdr *sechdrs,
			 struct module *mod)
{
	char *secstrings;
	unsigned int i;

	mod->kcfi_traps = NULL;
	mod->kcfi_traps_end = NULL;

	secstrings = (char *)hdr + sechdrs[hdr->e_shstrndx].sh_offset;

	for (i = 1; i < hdr->e_shnum; i++) {
		if (strcmp(secstrings + sechdrs[i].sh_name, "__kcfi_traps"))
			continue;

		mod->kcfi_traps = (s32 *)sechdrs[i].sh_addr;
		mod->kcfi_traps_end = (s32 *)(sechdrs[i].sh_addr + sechdrs[i].sh_size);
		break;
	}
}

static bool is_module_cfi_trap(unsigned long addr)
{
	struct module *mod;
	bool found = false;

	guard(rcu)();
	mod = __module_address(addr);
	if (mod)
		found = is_trap(addr, mod->kcfi_traps, mod->kcfi_traps_end);

	return found;
}
#else /* CONFIG_MODULES */
static inline bool is_module_cfi_trap(unsigned long addr)
{
	return false;
}
#endif /* CONFIG_MODULES */

extern s32 __start___kcfi_traps[];
extern s32 __stop___kcfi_traps[];

bool is_cfi_trap(unsigned long addr)
{
	if (is_trap(addr, __start___kcfi_traps, __stop___kcfi_traps))
		return true;

	return is_module_cfi_trap(addr);
}
#endif /* CONFIG_ARCH_USES_CFI_TRAPS */
