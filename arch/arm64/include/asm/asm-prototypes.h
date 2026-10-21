/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_PROTOTYPES_H
#define __ASM_PROTOTYPES_H
/*
 * CONFIG_MODVERSIONS requires a C declaration to generate the appropriate CRC
 * for each symbol. Since commit:
 *
 *   4efca4ed05cbdfd1 ("kbuild: modversions for EXPORT_SYMBOL() for asm")
 *
 * ... kbuild will automatically pick these up from <asm/asm-prototypes.h> and
 * feed this to genksyms when building assembly files.
 */
#include <linux/arm-smccc.h>

#include <asm/ftrace.h>
#include <asm/page.h>
#include <asm/string.h>
#include <asm/uaccess.h>

/*
 * dcache_clean_inval_poc() is exported for the vendor audio stack,
 * whose MDF driver hands a kernel buffer to the secure world
 * through a non-coherent SMMU and has to clean it first.  The
 * declaration is needed here because an exported assembly symbol
 * needs a C prototype for CONFIG_MODVERSIONS and for the
 * gendwarfksyms pass.
 */
extern void dcache_clean_inval_poc(unsigned long start, unsigned long end);

#include <asm-generic/asm-prototypes.h>

long long __ashlti3(long long a, int b);
long long __ashrti3(long long a, int b);
long long __lshrti3(long long a, int b);

/*
 * This function uses a custom calling convention and cannot be called from C so
 * this prototype is not entirely accurate.
 */
void __hwasan_tag_mismatch(unsigned long addr, unsigned long access_info);

#endif /* __ASM_PROTOTYPES_H */
