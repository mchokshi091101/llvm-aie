; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
;
; This file is licensed under the Apache License v2.0 with LLVM Exceptions.
; See https://llvm.org/LICENSE.txt for license information.
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
;
; (c) Copyright 2023-2024 Advanced Micro Devices, Inc. or its affiliates
; RUN: llc -mtriple=aie -verify-machineinstrs < %s \
; RUN:   | FileCheck %s

define i32 @indirectbr(i8* %target) nounwind {










; CHECK-LABEL: indirectbr:
; CHECK:       // %bb.0:
; CHECK-NEXT:    mov cb0, p0
; CHECK-NEXT:    mov r12, p0
; CHECK-NEXT:    ja cb0
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB0_1: // %test_label
; CHECK-NEXT:    // Label of block must be emitted
; CHECK-NEXT:    mov.u20 r0, #0
; CHECK:         ret lr
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
  indirectbr i8* %target, [label %test_label]
test_label:
  br label %ret
ret:
  ret i32 0
}

define i32 @indirectbr_with_offset(i8* %a) nounwind {










; CHECK-LABEL: indirectbr_with_offset:
; CHECK:       // %bb.0:
; CHECK-NEXT:    mov r12, p0
; CHECK-NEXT:    mov.u20 r13, #1380
; CHECK-NEXT:    add r12, r12, r13
; CHECK-NEXT:    mov cb0, r12
; CHECK-NEXT:    ja cb0
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
; CHECK-NEXT:    .p2align 4
; CHECK-NEXT:  .LBB1_1: // %test_label
; CHECK-NEXT:    // Label of block must be emitted
; CHECK-NEXT:    mov.u20 r0, #0
; CHECK:         ret lr
; CHECK-NEXT:    nop // Delay Slot 5
; CHECK-NEXT:    nop // Delay Slot 4
; CHECK-NEXT:    nop // Delay Slot 3
; CHECK-NEXT:    nop // Delay Slot 2
; CHECK-NEXT:    nop // Delay Slot 1
  %target = getelementptr inbounds i8, i8* %a, i32 1380
  indirectbr i8* %target, [label %test_label]
test_label:
  br label %ret
ret:
  ret i32 0
}
