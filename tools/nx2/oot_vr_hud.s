// Called from main+0x340A4B4 in place of `bl main+0x338B4C0`; hud_params {f32 size, u32 on, f32 pull, f32 minimap y, f32 text size, f32 prompt y, f32 talk box y, f32 x shift for this frame's eye, u32 item menu open} is written by svc_nx2_oot_vr.cpp; the panes' last positions follow at +0x30.
    .text
    .globl hud
hud:
    stp x29, x30, [sp, #-0x60]!
    mov x29, sp
    stp x19, x20, [sp, #0x10]
    stp x21, x22, [sp, #0x20]
    stp x23, x24, [sp, #0x30]
    stp d8, d9, [sp, #0x40]
    str d10, [sp, #0x50]
    bl task_flush
    adrp x22, hud_params
    add x22, x22, :lo12:hud_params
    ldr w8, [x22, #4]
    cbz w8, done
    adrp x8, ui_manager_ptr
    ldr x8, [x8, :lo12:ui_manager_ptr]
    ldr x19, [x8]
    cbz x19, done
    mov x0, x19
    movz w1, #0x1c3c
    movk w1, #0x5c7f, lsl #16
    bl ui_screen_by_hash
    mov w21, wzr
    cbz x0, 1f
    bl ui_hud_shown
    and w21, w0, #1
1:
    // Item menu (ShortCut_00) shown: byte +0x1A6 not 0 (opening 3, open 1) or +0x1AF 2 (closing). Read every frame, into hud_params +32.
    mov x0, x19
    movz w1, #0xfb65
    movk w1, #0x42fe, lsl #16
    bl screen
    mov w9, wzr
    cbz x0, 13f
    ldrb w8, [x0, #0x1a6]
    ldrb w10, [x0, #0x1af]
    cmp w8, #0
    cset w9, ne
    cmp w10, #2
    cset w11, eq
    orr w9, w9, w11
13:
    str w9, [x22, #32]
    // svc 0xF8 tells the headset whether the item menu is shown, every frame (svc_nx2_oot_vr.cpp).
    mov x0, x22
    mov w1, w9
    svc #0xf8
    adr x20, table
    add x23, x22, #0x30
next_pane:
    ldr w1, [x20]
    cbz w1, done
    mov x0, x19
    bl screen
    cbz x0, skip
    ldr x0, [x0, #0x28]
    cbz x0, skip
    add x1, x20, #0x18
    bl find_pane
    cbz x0, skip
    mov x24, x0
    ldp s0, s1, [x20, #8]
    ldr w8, [x20, #4]
    tbz w8, #6, 11f
    ldr w9, [x22, #32]
    mov x0, x24
    fmov s0, #1.0
    fmov s1, #1.0
    fmov s2, #1.0
    cbz w9, 12f
    fmov s0, wzr
    fmov s1, wzr
12:
    bl set_scale
    b skip
11:
    tbz w8, #5, 14f
    ldr w9, [x22, #32]
    cbz w9, 14f
    mov x0, x24
    fmov s0, wzr
    fmov s1, wzr
    fmov s2, #1.0
    bl set_scale
    b skip
14:
    tbz w8, #0, 2f
    cbz w21, 3f
2:
    ldr s2, [x22]
    tbz w8, #1, 7f
    ldr s2, [x22, #16]
7:
    ldr s3, [x22, #8]
    fmov s4, #1.0
    fsub s5, s4, s3
    fsub s6, s5, s2
    ldp s16, s17, [x20, #0x10]
    fmul s0, s0, s5
    fmul s1, s1, s5
    fmadd s0, s16, s6, s0
    fmadd s1, s17, s6, s1
    tbz w8, #4, 9f
    fmul s0, s16, s5
    fmul s1, s17, s5
9:
    ubfx w9, w8, #8, #8
    cbz w9, 6f
    ldr s7, [x22, x9]
    fadd s1, s1, s7
6:
    ldr s7, [x22, #28]
    fadd s0, s0, s7
    fmov s10, s2
    b 4f
3:
    fmov s10, #1.0
4:
    fmov s8, s0
    fmov s9, s1
    ldr w12, [x20, #4]
    tbnz w12, #7, 5f
    ldp w8, w9, [x24, #0x6c]
    ldp w10, w11, [x20, #8]
    cmp w8, w10
    ccmp w9, w11, #0, eq
    b.eq 5f
    ldp w10, w11, [x23]
    cmp w8, w10
    ccmp w9, w11, #0, eq
    b.ne skip
5:
    mov x0, x24
    fmov s0, s8
    fmov s1, s9
    bl set_translate
    mov x0, x24
    fmov s0, s10
    fmov s1, s10
    fmov s2, #1.0
    bl set_scale
    stp s8, s9, [x23]
skip:
    add x20, x20, #0x28
    add x23, x23, #8
    b next_pane
done:
    ldr d10, [sp, #0x50]
    ldp d8, d9, [sp, #0x40]
    ldp x23, x24, [sp, #0x30]
    ldp x21, x22, [sp, #0x20]
    ldp x19, x20, [sp, #0x10]
    ldp x29, x30, [sp], #0x60
    ret

// x0: the UI manager, w1: a layout hash. Returns its screen or 0.
screen:
    stp x29, x30, [sp, #-0x20]!
    mov x29, sp
    str x19, [sp, #0x10]
    mov x19, x0
    ldr x0, [x19, #0x48]
    ldr x8, [x0]
    ldr x8, [x8, #0x50]
    blr x8
    ldr w8, [x19, #8]
    cmp w0, w8
    b.hs 1f
    ldr x9, [x19, #0x10]
    ldr x0, [x9, w0, uxtw #3]
    b 2f
1:
    mov x0, xzr
2:
    ldr x19, [sp, #0x10]
    ldp x29, x30, [sp], #0x20
    ret

    .p2align 2
// {u32 layout hash, u32 flags (bit 0: as the game has it while a menu is open, bit 1: text size, bit 6: only the scale, 0 while the item menu is open and 1 after, bit 5: hidden while the item menu is open (hud_params +32, set here every frame), bit 7: moved even if the game moved it, bit 4: placed at the corner x, y times the pull (so it follows the HUD in), bits 8-15: hud_params byte offset of an extra y, 0 none), f32 x, y at rest, f32 x, y of the screen corner it sits in, char name[16]}
    .macro pane hash, menu, x, y, cx, cy, name
    .word \hash, \menu
    .float \x, \y, \cx, \cy
0:
    .asciz "\name"
    .space 16 - (. - 0b)
    .endm
table:
    pane 0x5c7f1c3c, 0, -1920, 1078, 0, 0, N_ScaleL_00
    pane 0x5c7f1c3c, 0x0020, 1920, 1078, 0, 0, N_ScaleR_00
    pane 0x3e737418, 0, 0, 0, 1920, 0, RootPane
    pane 0xc3332ad4, 0x0c01, 0, 0, 1920, -1078, RootPane
    pane 0x2d9798a8, 0x1400, 0, 0, 0, -1078, RootPane
    pane 0xb116cfbc, 0x1400, 0, 0, 0, -1078, RootPane
    pane 0x5528d3d9, 0x1400, 0, 0, 0, -1078, RootPane
    pane 0xf91df64d, 0x1802, 0, 0, 0, -1078, RootPane
    pane 0x8f87396d, 0x1802, 0, 0, 1920, -1078, RootPane
    pane 0x6b8f8fdd, 0x1802, 0, 0, 0, -1078, RootPane
    pane 0x62fedc9b, 0x0002, 0, 0, -1920, -1078, RootPane
    pane 0x0c5498a2, 0x0002, 0, 0, 0, 0, RootPane
    pane 0x42fefb65, 0x0092, 0, 0, 0, 0, RootPane
    pane 0x42fefb65, 0x0040, 0, 0, 0, 0, P_Bg_00
    pane 0xc5a49707, 0x0040, 0, 0, 0, 0, P_Grad_00
    .word 0, 0
