@ Check the ldrd/strd abbrev syntax

@ RUN: llvm-mc < %s -triple thumbv7-linux-gnueabi -filetype=asm -o - \
@ RUN:   | FileCheck %s --check-prefix=CHECK-ASM
@ RUN: llvm-mc < %s -triple thumbv7-linux-gnueabi -filetype=obj -o - \
@ RUN:   | llvm-readobj -s -sd | FileCheck %s --check-prefix=CHECK-OBJ

	.syntax unified

	.text
	.global	main
	.thumb_func
	.type	main, %function
main:
	cmp	r0, r0
	it	ne
	ldrdne	r0, [r3]
	it	ne
	strdne	r0, [r3]
	ldrd	r2, [r3]
	strd	r2, [r3]
	bx	lr

@ CHECK-ASM: ldrdne	r0, r1, [r3]
@ CHECK-ASM: strdne	r0, r1, [r3]
@ CHECK-ASM: ldrd	r2, r3, [r3]
@ CHECK-ASM: strd	r2, r3, [r3]

@ CHECK-OBJ: Name: .text
@ CHECK-OBJ: SectionData (
@ CHECK-OBJ:   0000: 804218BF D3E90001 18BFC3E9 0001D3E9
@ CHECK-OBJ:   0010: 0023C3E9 00237047
@ CHECK-OBJ: )
