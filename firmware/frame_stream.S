.syntax unified
.cpu cortex-m3
.thumb
.section .text,"ax",%progbits
.balign 4
nop
nop
.global stream_native_entry
.thumb_func
stream_native_entry:
push {r4-r11, lr}
sub sp, #28
stmia sp, {r0-r3}
ldr r12, [r4]
cmp r12, #15
bne .Lnative_original
ldr r12, =0xFF554A18
cmp r7, r12
bne .Lnative_original
bl fs_clock_ms
str r0, [sp, #16]
ldmia sp, {r0-r3}
.Lnative_original:
blx r8
str r0, [sp, #20]
ldr r12, [r4]
cmp r12, #15
bne .Lnative_done
ldr r12, =0xFF554A18
cmp r7, r12
bne .Lnative_done
mov r0, r4
ldr r1, [sp, #16]
bl frame_stream_note_native
mov r0, r4
bl frame_stream_paint
.Lnative_done:
ldr r0, [sp, #20]
add sp, #28
pop {r4-r11, lr}
ldr r12, =0x0800D219
bx r12
.ltorg

.global stream_clear_entry
.thumb_func
stream_clear_entry:
push {r0-r4, lr}
ldr r0, =0xFF554A18
cmp r7, r0
bne .Lclear_regular
mov r0, r4
bl frame_stream_has_full_frame
cbnz r0, .Lclear_skip
.Lclear_regular:
pop {r0-r4, lr}
ldr r12, =0x08019FB5
blx r12
ldr r12, =0x0800D1C1
bx r12
.Lclear_skip:
pop {r0-r4, lr}
ldr r12, =0x0800D1C1
bx r12
.ltorg

.global stream_control_entry
.thumb_func
stream_control_entry:
push {r0-r7, lr}
sub sp, #4
cbz r1, .Lordinary
ldr r2, [r1, #4]
ldr r1, [r1]
bl frame_stream_control
cbz r0, .Lordinary
add sp, #4
pop {r0-r7, lr}
movs r0, #1
bx lr
.Lordinary:
add sp, #4
pop {r0-r7, lr}
push {r4, r5, lr}
mov r5, r1
ldr r12, =0x0803726D
bx r12
.ltorg

.global stream_dispose_entry
.thumb_func
stream_dispose_entry:
push {r0-r7, lr}
sub sp, #4
bl frame_stream_dispose
add sp, #4
pop {r0-r7, lr}
push {r3-r7, lr}
mov r4, r0
ldr r12, =0x08037CA9
bx r12
.ltorg

.macro stock_binding name, target
.global \name
.thumb_func
\name:
ldr r12, =\target
bx r12
.ltorg
.endm

stock_binding fs_alloc, 0x0806AE81
stock_binding fs_free, 0x0806AE91
stock_binding fs_gui_alloc, 0x0801DB29
stock_binding fs_gui_free, 0x0801D7B5
stock_binding fs_clock_ms, 0x08001D49
stock_binding fs_lock, 0x0801759D
stock_binding fs_unlock, 0x08017589
stock_binding fs_get_handle, 0x080282DD
stock_binding fs_acquire, 0x0801D6F5
stock_binding fs_release, 0x0801D711
stock_binding fs_invalidate, 0x08025DCD
stock_binding fs_rgb565, 0x08016EE1
stock_binding fs_set_color, 0x08021D65
stock_binding fs_fill_rect, 0x0801EC09

.global fs_state_load
.thumb_func
fs_state_load:
ldr r0, [r0, #0x44]
bx lr
.global fs_state_store
.thumb_func
fs_state_store:
str r1, [r0, #0x44]
bx lr

.global fs_raw_bitmap_selector
.thumb_func
fs_raw_bitmap_selector:
push {r4, lr}
ldr r0, =0x200002B0
ldr r0, [r0]
cbz r0, .Lraw_unknown
ldrb r0, [r0, #0x11]
ldr r12, =0x0801D0A5
blx r12
ldr r1, =0x08016EE1
cmp r0, r1
beq .Lraw_rgb
ldr r1, =0x08018665
cmp r0, r1
beq .Lraw_bgr
.Lraw_unknown:
movs r0, #0
pop {r4, pc}
.Lraw_rgb:
movs r0, #8
pop {r4, pc}
.Lraw_bgr:
movs r0, #9
pop {r4, pc}
.ltorg

.global fs_read_paint_clip
.thumb_func
fs_read_paint_clip:
push {r4-r8, lr}
sub sp, #16
mov r4, r0
cmp r4, #0
beq .Lclip_no_context
ldr r5, =0x200002B0
ldr r5, [r5]
cmp r5, #0
beq .Lclip_no_context
ldr r7, [r5, #0x4C]
ldr r8, [r5, #0x50]
sxth r0, r7
cmp r0, r7
bne .Lclip_origin_range
sxth r0, r8
cmp r0, r8
bne .Lclip_origin_range
movs r0, #0
str r0, [sp]
movs r0, #1
str r0, [sp, #4]
ldrb r0, [r5, #0x54]
cbnz r0, .Lclip_iterator
ldr r0, [r5, #0x48]
cbz r0, .Lclip_inactive
bl fs_acquire
cbz r0, .Lclip_inactive
str r0, [sp]
add r6, sp, #8
ldm r0, {r1, r2}
stm r6, {r1, r2}
movs r0, #5
str r0, [sp, #4]
mov r0, sp
bl fs_release
ldr r0, =0x2000C7B8
ldr r1, [r0]
cbz r1, .Lclip_copy
mov r0, r6
ldr r2, =0x0800FF89
blx r2
b .Lclip_copy
.Lclip_iterator:
ldr r6, =0x2000C7D4
.Lclip_copy:
ldrsh r0, [r6]
subs r0, r0, r7
str r0, [r4]
ldrsh r0, [r6, #2]
subs r0, r0, r8
str r0, [r4, #4]
ldrsh r0, [r6, #4]
subs r0, r0, r7
str r0, [r4, #8]
ldrsh r0, [r6, #6]
subs r0, r0, r8
str r0, [r4, #12]
ldr r0, [sp]
cbz r0, .Lclip_result
mov r0, sp
bl fs_release
.Lclip_result:
ldr r0, [sp, #4]
b .Lclip_return
.Lclip_no_context:
movs r0, #2
b .Lclip_return
.Lclip_inactive:
movs r0, #3
b .Lclip_return
.Lclip_origin_range:
movs r0, #4
.Lclip_return:
add sp, #16
pop {r4-r8, pc}
.ltorg

.global fs_blit_rgb565
.thumb_func
fs_blit_rgb565:
push {r4-r8, lr}
sub sp, #48
mov r4, r0
mov r5, r1
mov r6, r2
mov r7, r3
ldr r8, [sp, #72]
cmp r4, #0
beq .Lblit_fail
tst r4, #1
bne .Lblit_fail
cmp r5, #1
blo .Lblit_fail
cmp r5, #240
bhi .Lblit_fail
cmp r6, #1
blo .Lblit_fail
cmp r6, #16
bhi .Lblit_fail
add r0, r7, r5
cmp r0, #240
bhi .Lblit_fail
cmp r7, #239
bhi .Lblit_fail
add r0, r8, r6
cmp r0, #240
bhi .Lblit_fail
cmp r8, #239
bhi .Lblit_fail
ldr r0, [sp, #76]
cmp r0, #8
beq .Lblit_selector_ok
cmp r0, #9
bne .Lblit_fail
.Lblit_selector_ok:
movw r1, #0x4D42
orr r0, r1, r0, lsl #16
str r0, [sp, #32]
orr r0, r5, r6, lsl #16
str r0, [sp, #36]
mov.w r0, #0x00100000
str r0, [sp, #40]
movs r0, #0
str r0, [sp, #44]
mov r0, sp
add r1, sp, #20
add r2, sp, #32
ldr r12, =0x0801D1B1
blx r12
cbnz r0, .Lblit_fail
str r4, [sp, #8]
mov r0, sp
mov r1, r7
mov r2, r8
ldr r12, =0x0801F0E9
blx r12
movs r0, #1
b .Lblit_return
.Lblit_fail:
movs r0, #0
.Lblit_return:
add sp, #48
pop {r4-r8, pc}
.ltorg

.global fs_emit
.thumb_func
fs_emit:
push {r4-r6, lr}
mov r5, r0
sub sp, #0xD0
mov r4, sp
mov r0, r4
movs r1, #1
adr.w r2, .Lcomponent
ldr r3, =0x0802E895
blx r3
ldr r0, =0x080C7388
str r0, [r4]
add r0, r4, #8
mov r1, r5
ldr r3, =0x08060C31
blx r3
mov r0, r4
ldr r3, =0x08027229
blx r3
add sp, #0xD0
pop {r4-r6, pc}
.ltorg
.Lcomponent:
.asciz "FS1"
.balign 4
