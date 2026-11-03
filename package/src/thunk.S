@ SPDX-License-Identifier: GPL-2.0-or-later
@ The game is old-ABI ARM code: its stack is only 4-byte aligned. Everything it
@ calls here is EABI code, which may assume 8. Each import thunk (built at load
@ time in main.c) loads the target into ip and comes here: align the stack,
@ copy the words stack arguments could occupy (printf-style calls), call, undo.
    .arm
    .text
    .global liar_thunk_common
    .type liar_thunk_common, %function
liar_thunk_common:
    push {r4, lr}
    mov r4, sp
    sub sp, sp, #48
    bic sp, sp, #7
    ldr lr, [r4, #8]
    str lr, [sp, #0]
    ldr lr, [r4, #12]
    str lr, [sp, #4]
    ldr lr, [r4, #16]
    str lr, [sp, #8]
    ldr lr, [r4, #20]
    str lr, [sp, #12]
    ldr lr, [r4, #24]
    str lr, [sp, #16]
    ldr lr, [r4, #28]
    str lr, [sp, #20]
    ldr lr, [r4, #32]
    str lr, [sp, #24]
    ldr lr, [r4, #36]
    str lr, [sp, #28]
    ldr lr, [r4, #40]
    str lr, [sp, #32]
    ldr lr, [r4, #44]
    str lr, [sp, #36]
    ldr lr, [r4, #48]
    str lr, [sp, #40]
    ldr lr, [r4, #52]
    str lr, [sp, #44]
    blx ip
    mov sp, r4
    pop {r4, pc}
    .size liar_thunk_common, .-liar_thunk_common

@ void liar_enter(entry, sp): start the game's _start on the given stack.
    .global liar_enter
    .type liar_enter, %function
liar_enter:
    mov sp, r1
    mov ip, r0
    mov r0, #0
    mov fp, #0
    mov lr, #0
    bx ip
    .size liar_enter, .-liar_enter
    .section .note.GNU-stack,"",%progbits
