/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_PTI_H
#define _ASM_X86_PTI_H
#ifndef __ASSEMBLER__

#ifdef CONFIG_MITIGATION_PAGE_TABLE_ISOLATION
extern void pti_init(void);
extern void pti_check_boottime_disable(void);
extern void pti_finalize(void);
#else
static inline void pti_check_boottime_disable(void) { }
#endif

#endif /* __ASSEMBLER__ */
#endif /* _ASM_X86_PTI_H */
