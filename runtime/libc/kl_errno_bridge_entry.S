// Signed, ABI-transparent pre-call barrier. The shared trace cells put the
// original target in x16. Restore the original SP before tail-calling it so
// stack arguments, varargs adapters and nonlocal jumps retain their contract.
#if defined(__aarch64__)
.text
.p2align 2
.globl _kl_errno_bridge_entry
_kl_errno_bridge_entry:
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x30, [sp, #64]
    str x16, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    bl _kl_errno_bridge_enter
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x30, [sp, #64]
    ldr x16, [sp, #80]
    add sp, sp, #224
    br x16
#endif
