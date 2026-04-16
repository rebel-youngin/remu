/*
 * Minimal AArch64 test: write "REMU OK\n" to PL011 UART at 0x1FF9040000
 * Load at 0x1E00010000 (BL1 reset vector)
 */
    .section .text
    .globl _start
_start:
    /* Build UART base 0x1F_F904_0000 using LDR from literal pool */
    ldr     x1, =0x1FF9040000

    /* Write "REMU OK\r\n" character by character to UART DR (offset 0) */
    mov     w0, #'R'
    strb    w0, [x1]
    mov     w0, #'E'
    strb    w0, [x1]
    mov     w0, #'M'
    strb    w0, [x1]
    mov     w0, #'U'
    strb    w0, [x1]
    mov     w0, #' '
    strb    w0, [x1]
    mov     w0, #'O'
    strb    w0, [x1]
    mov     w0, #'K'
    strb    w0, [x1]
    mov     w0, #'\r'
    strb    w0, [x1]
    mov     w0, #'\n'
    strb    w0, [x1]

    /* Spin forever */
1:  wfe
    b       1b
