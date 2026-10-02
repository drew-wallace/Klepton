// A saved context must belong to the guest's caller, not a C wrapper frame.
// Restore the entry stack/LR after preparing mask metadata, then tail-branch
// to Darwin's register-only _setjmp. These are ordinary signed AOT symbols.
.text
.p2align 2
.globl _klb_setjmp
_klb_setjmp:
    mov w1, #1
    b _klb_sigsetjmp
.globl _klb__setjmp
_klb__setjmp:
    mov w1, #0
    b _klb_sigsetjmp
.globl _klb_sigsetjmp
_klb_sigsetjmp:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl _kl_jump_prepare
    ldp x29, x30, [sp], #16
    b __setjmp
