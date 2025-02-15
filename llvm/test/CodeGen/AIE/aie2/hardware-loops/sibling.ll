; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
; RUN: llc -O2 -mtriple=aie2 --issue-limit=1 --enable-aie-hardware-loops %s -o - | FileCheck %s

define void @sibling(ptr nocapture %out, ptr nocapture readonly %in, i32 noundef %size, i32 noundef %size2) {
; CHECK-LABEL: sibling:
; CHECK:         .p2align 4
; CHECK-NEXT:  // %bb.0: // %for.body.lr.ph
; CHECK-NEXT:    mova r2, #0; nopxm
; CHECK-NEXT:    add.nc r0, r0, #-1
; CHECK-NEXT:    mova r4, #2
; CHECK-NEXT:    movxm p2, #.LBB0_1
; CHECK-NEXT:    mova r5, #0
; CHECK-NEXT:    lda r3, [p0, #0]
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB0_1: // %for.body
; CHECK-NEXT:    // =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    nopa ; lshl r6, r5, r4
; CHECK-NEXT:    mov dj0, r6
; CHECK-NEXT:    lda r6, [p1, dj0]
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    add r5, r5, #1
; CHECK-NEXT:    add r3, r3, r6
; CHECK-NEXT:    jnzd r0, r0, p2
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    st r3, [p0, #0] // Delay Slot 1
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  // %bb.2: // %for.body6.lr.ph
; CHECK-NEXT:    nopb ; nopa ; nops ; nopx ; add.nc r1, r1, #-1; nopv
; CHECK-NEXT:    mova r3, #2; nopx
; CHECK-NEXT:    movxm p2, #.LBB0_3
; CHECK-NEXT:    lda r0, [p0, #0]
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB0_3: // %for.body6
; CHECK-NEXT:    // =>This Inner Loop Header: Depth=1
; CHECK-NEXT:    nopa ; lshl r4, r2, r3
; CHECK-NEXT:    mov dj0, r4
; CHECK-NEXT:    lda r4, [p1, dj0]
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    nop
; CHECK-NEXT:    add r2, r2, #1
; CHECK-NEXT:    add r0, r0, r4
; CHECK-NEXT:    jnzd r1, r1, p2
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    st r0, [p0, #0] // Delay Slot 1
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  // %bb.4: // %for.cond.cleanup5
; CHECK-NEXT:    nopa ; ret lr
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
for.body.lr.ph:
  %out.promoted = load i32, ptr %out, align 4
  br label %for.body

for.body6.lr.ph:
  %out.promoted25 = load i32, ptr %out, align 4
  br label %for.body6

for.body:
  %add22 = phi i32 [ %out.promoted, %for.body.lr.ph ], [ %add, %for.body ]
  %i.021 = phi i32 [ 0, %for.body.lr.ph ], [ %inc, %for.body ]
  %0 = trunc i32 %i.021 to i20
  %arrayidx = getelementptr inbounds i32, ptr %in, i20 %0
  %1 = load i32, ptr %arrayidx, align 4
  %add = add nsw i32 %add22, %1
  store i32 %add, ptr %out, align 4
  %inc = add nuw nsw i32 %i.021, 1
  %exitcond.not = icmp eq i32 %inc, %size
  br i1 %exitcond.not, label %for.body6.lr.ph, label %for.body

for.cond.cleanup5:
  ret void

for.body6:
  %add826 = phi i32 [ %out.promoted25, %for.body6.lr.ph ], [ %add8, %for.body6 ]
  %j.024 = phi i32 [ 0, %for.body6.lr.ph ], [ %inc10, %for.body6 ]
  %2 = trunc i32 %j.024 to i20
  %arrayidx7 = getelementptr inbounds i32, ptr %in, i20 %2
  %3 = load i32, ptr %arrayidx7, align 4
  %add8 = add nsw i32 %add826, %3
  store i32 %add8, ptr %out, align 4
  %inc10 = add nuw nsw i32 %j.024, 1
  %exitcond27.not = icmp eq i32 %inc10, %size2
  br i1 %exitcond27.not, label %for.cond.cleanup5, label %for.body6
}
