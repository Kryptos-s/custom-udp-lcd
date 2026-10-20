.syntax unified
.cpu cortex-m3
.thumb
.section .text,"ax",%progbits
.balign 4

.global indexed_current_handle
.thumb_func
indexed_current_handle:
ldr r0, =0x2000D6D8
ldr r0, [r0]
cbz r0, .Lcurrent_done
ldr.w r0, [r0, #0x4F0]
cbz r0, .Lcurrent_done
ldr r1, [r0]
ldr r2, =0x080CDB90
cmp r1, r2
bne .Lcurrent_none
ldr r0, [r0, #8]
bx lr
.Lcurrent_none:
movs r0, #0
.Lcurrent_done:
bx lr
.ltorg

.global indexed_binary_entry
.thumb_func
indexed_binary_entry:
push {r0-r3, r4, lr}
ldr r1, [r1]
bl indexed_target_binary_begin
cmp r0, #0
beq .Lbinary_fallback
pop {r0-r3, r4, lr}
movs r0, #1
bx lr
.Lbinary_fallback:
pop {r0-r3, r4, lr}
ldr r12, =0x0802B879
bx r12
.ltorg

.global indexed_binary_feed_entry
.thumb_func
indexed_binary_feed_entry:
b.w indexed_target_binary_feed

.global indexed_comm_idle_entry
.thumb_func
indexed_comm_idle_entry:
push {r0-r3, r4, lr}
bl indexed_target_idle_ticks
str r0, [sp]
pop {r0-r3, r4, lr}
ldr r12, =0x08001F69
bx r12
.ltorg

.global indexed_gui_lock_entry
.thumb_func
indexed_gui_lock_entry:
push {r4, lr}
movs r4, #0
.Lgui_lock_retry:
ldr r3, =0x20001320
ldr r0, [r3]
mov r1, r4
ldr r3, =0x08002A31
blx r3
cmp r0, #0
bne .Lgui_lock_acquired
movs r4, #1
b .Lgui_lock_retry
.Lgui_lock_acquired:
pop {r4, pc}
.ltorg

.global indexed_gui_ready
.thumb_func
indexed_gui_ready:
ldr r0, =0x2000D349
ldrb r0, [r0]
bx lr
.ltorg

.global indexed_color_dispose_entry
.thumb_func
indexed_color_dispose_entry:
push {r0-r7, lr}
sub sp, #4
bl frame_stream_dispose
add sp, #4
pop {r0-r7, lr}
push {r4, lr}
mov r4, r0
ldr r12, =0x08037229
bx r12
.ltorg

.global indexed_heap_word
.thumb_func
indexed_heap_word:
ldr r0, [r0]
bx lr

.global indexed_heap_lock
.thumb_func
indexed_heap_lock:
ldr r12, =0x08024445
bx r12
.ltorg

.global indexed_heap_unlock
.thumb_func
indexed_heap_unlock:
ldr r12, =0x08024449
bx r12
.ltorg

.global indexed_native_invalidate
.thumb_func
indexed_native_invalidate:
ldr r12, =0x0800D2F1
bx r12
.ltorg

.global indexed_native_eligible
.thumb_func
indexed_native_eligible:
push {r4, lr}
movs r0, #0
ldr r12, =0x08021DA5
blx r12
mov r4, r0
ldr r12, =0x08021DA5
blx r12
and r0, r4, #1
eor r0, r0, #1
pop {r4, pc}
.ltorg

.global indexed_native_clip
.thumb_func
indexed_native_clip:
push {r4-r8, lr}
mov r4, r0
cmp r4, #0
beq .Lnative_clip_unknown
tst r4, #3
bne .Lnative_clip_unknown
ldr r0, =0x2000D360
ldr r0, [r0]
cmp r0, #0
bne .Lnative_clip_unknown
ldr r5, =0x200002B0
ldr r5, [r5]
cmp r5, #0
beq .Lnative_clip_unknown
ldrb r0, [r5, #0x54]
cmp r0, #0
bne .Lnative_clip_nested
ldrb r0, [r5, #0x55]
cmp r0, #0
beq .Lnative_clip_unknown
b .Lnative_clip_fields
.Lnative_clip_nested:
ldr r0, =0x2000C7E0
ldr r0, [r0]
cmp r0, #0
beq .Lnative_clip_unknown
ldr r1, =0x7FFFFFFF
cmp r0, r1
bhs .Lnative_clip_unknown
.Lnative_clip_fields:
ldr r6, [r5, #0x4C]
ldr r7, [r5, #0x50]
sxth r0, r6
cmp r0, r6
bne .Lnative_clip_unknown
sxth r0, r7
cmp r0, r7
bne .Lnative_clip_unknown
ldrsh r0, [r5, #8]
ldrsh r1, [r5, #10]
ldrsh r2, [r5, #12]
ldrsh r3, [r5, #14]
cmp r0, r2
bgt .Lnative_clip_unknown
cmp r1, r3
bgt .Lnative_clip_unknown
subs r0, r0, r6
subs r1, r1, r7
subs r2, r2, r6
subs r3, r3, r7
stm r4, {r0-r3}
movs r0, #1
pop {r4-r8, pc}
.Lnative_clip_unknown:
movs r0, #0
pop {r4-r8, pc}
.ltorg

.global indexed_native_bitmap
.thumb_func
indexed_native_bitmap:
push {r4-r11, lr}
sub sp, #44
mov r4, r0
mov r5, r1
mov r6, r2
mov r7, r3
ldr r8, [sp, #80]
ldr r9, [sp, #84]
cmp r4, #0
beq .Lindexed_bitmap_fail
cmp r9, #0
beq .Lindexed_bitmap_fail
tst r9, #3
bne .Lindexed_bitmap_fail
cmp r5, #1
blo .Lindexed_bitmap_fail
cmp r5, #240
bhi .Lindexed_bitmap_fail
cmp r6, #1
blo .Lindexed_bitmap_fail
cmp r6, #240
bhi .Lindexed_bitmap_fail
cmp r7, #239
bhi .Lindexed_bitmap_fail
cmp r8, #239
bhi .Lindexed_bitmap_fail
rsb r0, r7, #240
cmp r5, r0
bhi .Lindexed_bitmap_fail
rsb r0, r8, #240
cmp r6, r0
bhi .Lindexed_bitmap_fail
subs r0, r6, #1
movs r1, #240
mla r0, r0, r1, r5
adds r0, r4, r0
bcs .Lindexed_bitmap_fail
ldr r12, =0x0801759D
blx r12
ldr r0, =0x200002B0
ldr r0, [r0]
cmp r0, #0
beq .Lindexed_bitmap_unlock_fail
ldr r10, [r0, #0x4C]
ldr r11, [r0, #0x50]
add r10, r10, r7
add r11, r11, r8
sxth r0, r10
cmp r0, r10
bne .Lindexed_bitmap_unlock_fail
sxth r0, r11
cmp r0, r11
bne .Lindexed_bitmap_unlock_fail
strh r10, [sp, #24]
strh r11, [sp, #26]
add r0, r10, r5
subs r0, #1
sxth r1, r0
cmp r0, r1
bne .Lindexed_bitmap_unlock_fail
strh r0, [sp, #28]
add r0, r11, r6
subs r0, #1
sxth r1, r0
cmp r0, r1
bne .Lindexed_bitmap_unlock_fail
strh r0, [sp, #30]
movs r0, #0
ldr r12, =0x08021DA5
blx r12
str r0, [sp, #32]
tst r0, #1
bne .Lindexed_bitmap_mode_fail
bic r0, r0, #2
ldr r12, =0x08021DA5
blx r12
movs r0, #1
str r0, [sp]
str r0, [sp, #4]
movs r0, #8
str r0, [sp, #8]
movs r0, #240
str r0, [sp, #12]
str r4, [sp, #16]
str r9, [sp, #20]
add r0, sp, #24
ldr r12, =0x0801095D
blx r12
cbz r0, .Lindexed_bitmap_success
.Lindexed_bitmap_loop:
mov r0, r10
mov r1, r11
mov r2, r5
mov r3, r6
ldr r12, =0x08022255
blx r12
ldr r12, =0x080107F9
blx r12
cmp r0, #0
bne .Lindexed_bitmap_loop
.Lindexed_bitmap_success:
movs r4, #1
b .Lindexed_bitmap_mode_restore
.Lindexed_bitmap_mode_fail:
movs r4, #0
.Lindexed_bitmap_mode_restore:
ldr r0, [sp, #32]
ldr r12, =0x08021DA5
blx r12
b .Lindexed_bitmap_unlock
.Lindexed_bitmap_unlock_fail:
movs r4, #0
.Lindexed_bitmap_unlock:
ldr r12, =0x08017589
blx r12
mov r0, r4
b .Lindexed_bitmap_return
.Lindexed_bitmap_fail:
movs r0, #0
.Lindexed_bitmap_return:
add sp, #44
pop {r4-r11, pc}
.ltorg
