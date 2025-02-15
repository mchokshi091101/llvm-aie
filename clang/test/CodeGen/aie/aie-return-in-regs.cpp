// NOTE: Assertions have been autogenerated by utils/update_cc_test_checks.py UTC_ARGS: --function-signature --return-type --skip-function-body
//===- aie-return-in-regs.cpp -----------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2024 Advanced Micro Devices, Inc. or its affiliates
//
//===----------------------------------------------------------------------===//
// RUN: %clang --target=aie -S -emit-llvm %s -o - | FileCheck %s

#include <stdint.h>

extern "C" {


struct largeStruct_InStack {
    v4int32 a;
    v4int32 b;
};

struct largeStruct_InRegs {
    v4int32 a;
    v4int32 b;
} __attribute__((return_in_regs));

// CHECK-LABEL: define {{[^@]*}}void @ret_in_stack
// CHECK-SAME: (ptr noalias sret([[STRUCT_LARGESTRUCT_INSTACK:%.*]]) align 16 [[AGG_RESULT:%.*]]) #[[ATTR0:[0-9]+]] {
largeStruct_InStack ret_in_stack(void) { return {}; }

// CHECK-LABEL: define {{[^@]*}}%struct.largeStruct_InRegs @ret_in_regs
// CHECK-SAME: () #[[ATTR0:[0-9]+]] {
largeStruct_InRegs ret_in_regs(void) { return {}; }

}
