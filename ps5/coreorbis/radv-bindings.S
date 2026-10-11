// PS5 port, RADV link only (link-radv.sh, vk-285-115): two of the platform layer's libc bindings as functions instead of
// --defsym. A --defsym takes effect after the archives are read, so the SDK's libc.a had already put its own qsort_r and
// localtime_r into the eboot for RADV's references (dead code, calls go to the platform's), and with them imports of
// qsort_s and localtime_s that no RADV title on the console makes. Defined here, before libc.a, they never are.
// Hidden: the eboot exports nothing.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

	.text
	.globl	qsort_r
	.hidden	qsort_r
	.type	qsort_r, @function
qsort_r:
	jmp	ps5_qsort_r
	.size	qsort_r, . - qsort_r

	.globl	localtime_r
	.hidden	localtime_r
	.type	localtime_r, @function
localtime_r:
	jmp	ps5_localtime_r
	.size	localtime_r, . - localtime_r

	.section .note.GNU-stack, "", @progbits
