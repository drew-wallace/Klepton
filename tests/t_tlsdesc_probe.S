.text
.p2align 2
.globl _tlsdesc_probe
_tlsdesc_probe:
    sub sp, sp, #16
    str x30, [sp]
    mov x2, #0x55
    movi v4.16b, #0x5a
    ldr x1, [x0]
    blr x1
    mov x9, #0x1234
    cmp x0, x9
    cset w8, eq
    cmp x2, #0x55
    cset w9, eq
    and w8, w8, w9
    umov x9, v4.d[0]
    movz x10, #0x5a5a
    movk x10, #0x5a5a, lsl #16
    movk x10, #0x5a5a, lsl #32
    movk x10, #0x5a5a, lsl #48
    cmp x9, x10
    cset w9, eq
    and w0, w8, w9
    ldr x30, [sp]
    add sp, sp, #16
    ret
