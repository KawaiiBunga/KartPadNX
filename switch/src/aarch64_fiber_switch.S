// AArch64 cooperative context switch for kartpad_fiber (see aarch64_fiber.h).
//
// Saved frame layout (160 bytes, 16-byte aligned), offsets from a fiber's
// stored sp. Only the AAPCS64 callee-saved state must survive a switch:
//   0:x19 8:x20 16:x21 24:x22 32:x23 40:x24 48:x25 56:x26 64:x27 72:x28
//   80:x29(fp) 88:x30(lr) 96:d8 104:d9 112:d10 120:d11 128:d12 136:d13
//   144:d14 152:d15
// aarch64_fiber.c primes a new fiber's stack to this exact layout with
// x19=entry, x20=arg, x30=kartpad_fiber_trampoline.

    .text
    .balign 4

// void kartpad_fiber_asm_switch(void** from_sp /*x0*/, void* to_sp /*x1*/)
    .global kartpad_fiber_asm_switch
    .type kartpad_fiber_asm_switch, %function
kartpad_fiber_asm_switch:
    stp x19, x20, [sp, #-160]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x2, sp
    str x2, [x0]            // *from_sp = current sp
    mov sp, x1             // sp = target's saved sp
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8,  d9,  [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret
    .size kartpad_fiber_asm_switch, .-kartpad_fiber_asm_switch

// First-entry trampoline for a freshly created fiber. Reached via the restored
// x30 above, with x19=entry, x20=arg. Calls entry(arg); if it returns, hands
// off to kartpad_fiber_finish (which switches to the fiber's link and never
// returns).
    .global kartpad_fiber_trampoline
    .type kartpad_fiber_trampoline, %function
kartpad_fiber_trampoline:
    mov x29, #0            // clear frame pointer: this is a stack root
    mov x0, x20           // arg
    blr x19               // entry(arg)
    bl kartpad_fiber_finish
.Lhang:                   // kartpad_fiber_finish must not return
    b .Lhang
    .size kartpad_fiber_trampoline, .-kartpad_fiber_trampoline

// No executable stack.
    .section .note.GNU-stack, "", %progbits
