#ifndef KL_JUMP_H
#define KL_JUMP_H
#define KL_BIONIC_JUMP_BYTES 256
int klb_setjmp(void *) __attribute__((returns_twice));
int klb__setjmp(void *) __attribute__((returns_twice));
int klb_sigsetjmp(void *, int) __attribute__((returns_twice));
_Noreturn void klb_longjmp(void *, int);
#endif
