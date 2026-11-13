// WMVSlop's hook on the dashboard's VdSwap import.
//
// The dashboard calls the kernel's VdSwap through a four-instruction thunk:
//     lis r11, hi ; ori r11, r11, lo ; mtctr r11 ; bctr
// The plugin rewrites only the first word, so the thunk jumps to (sled's high half | VdSwap's low half).
// The sled is 64 KB of branches to the hook, one per possible low half, which makes the change one aligned
// 32-bit store: the thunk is never half-written while the dashboard's thread runs it.

	.section .text.wmvslop_sled, "ax"
	.balign 0x10000
	.globl	wmvslop_sled
wmvslop_sled:
	.rept 0x4000
	b	wmvslop_vdswap_hook
	.endr

	.section .text.wmvslop_hook, "ax"
	.balign 16
	.globl	wmvslop_vdswap_hook
	.type	wmvslop_vdswap_hook, @function
// Runs on the thread that presents the dashboard's frame, with VdSwap's ten arguments in r3-r10 and the
// caller's frame. Calls wmvslop_tick, then goes on to the real VdSwap with everything as it was.
wmvslop_vdswap_hook:
	mflr	r12
	stw	r12, -8(r1)
	stwu	r1, -0x90(r1)
	std	r3, 0x40(r1)
	std	r4, 0x48(r1)
	std	r5, 0x50(r1)
	std	r6, 0x58(r1)
	std	r7, 0x60(r1)
	std	r8, 0x68(r1)
	std	r9, 0x70(r1)
	std	r10, 0x78(r1)
	bl	wmvslop_tick
	ld	r3, 0x40(r1)
	ld	r4, 0x48(r1)
	ld	r5, 0x50(r1)
	ld	r6, 0x58(r1)
	ld	r7, 0x60(r1)
	ld	r8, 0x68(r1)
	ld	r9, 0x70(r1)
	ld	r10, 0x78(r1)
	addi	r1, r1, 0x90
	lwz	r12, -8(r1)
	mtlr	r12
	lis	r11, wmvslop_real_vdswap@ha
	lwz	r11, wmvslop_real_vdswap@l(r11)
	mtctr	r11
	bctr
	.size	wmvslop_vdswap_hook, . - wmvslop_vdswap_hook

// Stands in for the dashboard's call to its general video-player creator (r3 device, r4 parameter block,
// r5 where the player goes). Lets the plugin change the block (processors, and a file to stream in place
// of the buffer), then goes on to the creator with the caller's arguments and return address untouched.
//
// The block (dash 16197, decomp/player2.c, player3.c, player5.c): +4 flags, bit 1 = use the four numbers
// at +8, +0xC, +0x10, +0x14 for the player's four threads; without it they go to hardware threads 3, 1, 3, 2.
	.balign 16
	.globl	wmvslop_player_hook
	.type	wmvslop_player_hook, @function
wmvslop_player_hook:
	// wmvslop_adjust_player(block) in wmvslop.c changes the block; the creator then gets the caller's own
	// three arguments back.
	mflr	r12
	stw	r12, -8(r1)
	stwu	r1, -0x70(r1)
	std	r3, 0x40(r1)
	std	r4, 0x48(r1)
	std	r5, 0x50(r1)
	mr	r3, r4
	bl	wmvslop_adjust_player
	ld	r3, 0x40(r1)
	ld	r4, 0x48(r1)
	ld	r5, 0x50(r1)
	addi	r1, r1, 0x70
	lwz	r12, -8(r1)
	mtlr	r12
	lis	r11, wmvslop_real_create_player@ha
	lwz	r11, wmvslop_real_create_player@l(r11)
	mtctr	r11
	bctr
	.size	wmvslop_player_hook, . - wmvslop_player_hook

// void wmvslop_flush_code(void *address): makes a rewritten instruction word visible to every core.
	.balign 16
	.globl	wmvslop_flush_code
	.type	wmvslop_flush_code, @function
wmvslop_flush_code:
	dcbst	0, r3
	sync
	icbi	0, r3
	sync
	isync
	blr
	.size	wmvslop_flush_code, . - wmvslop_flush_code
