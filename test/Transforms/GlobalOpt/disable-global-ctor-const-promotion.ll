; Test -disable-global-ctor-const-promotion
;
; The global variable optimization will try to evaluate the constant
; global variable and remove the constructors.
;
; However, some legacy code will try to perform const_cast<>() and
; assign to the "constant" to avoid static initialization order fiasco.
; To workaround those old code, we would like to provide an option to
; disable this optimization.
;
; This test will check whether the -disable-global-ctor-const-promotion
; is working as expected or not.

; RUN: opt < %s -globalopt -S -o - | FileCheck %s --check-prefix=CHECK-DEFAULT
; RUN: opt < %s -globalopt -disable-global-ctor-const-promotion -S -o - \
; RUN:   | FileCheck %s --check-prefix=CHECK-DISABLE

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%class.ConstantValue = type { i32 }

@_ZL3one = internal global %class.ConstantValue zeroinitializer, align 4
; CHECK-DEFAULT: @_ZL3one = internal constant %class.ConstantValue { i32 1 }, align 4
; CHECK-DISABLE: @_ZL3one = internal global %class.ConstantValue { i32 1 }, align 4

@llvm.global_ctors = appending global [1 x { i32, void ()* }] [{ i32, void ()* } { i32 65535, void ()* @_GLOBAL__I_a }]

@_ZN13ConstantValueC1Ei = alias weak_odr void (%class.ConstantValue*, i32)* @_ZN13ConstantValueC2Ei

define internal void @__cxx_global_var_init() section ".text.startup" {
entry:
  call void @_ZN13ConstantValueC1Ei(%class.ConstantValue* @_ZL3one, i32 1)
  %0 = call {}* @llvm.invariant.start(i64 4, i8* bitcast (%class.ConstantValue* @_ZL3one to i8*))
  ret void
}

declare {}* @llvm.invariant.start(i64, i8* nocapture) nounwind

define void @_Z3barv() nounwind uwtable {
entry:
  call void @_Z3fooRK13ConstantValue(%class.ConstantValue* @_ZL3one) nounwind
  ret void
}

declare void @_Z3fooRK13ConstantValue(%class.ConstantValue*) nounwind

define linkonce_odr void @_ZN13ConstantValueC2Ei(%class.ConstantValue* %this, i32 %i) unnamed_addr nounwind uwtable align 2 {
entry:
  %value_ = getelementptr inbounds %class.ConstantValue* %this, i32 0, i32 0
  store i32 %i, i32* %value_, align 4
  ret void
}

define internal void @_GLOBAL__I_a() section ".text.startup" {
entry:
  call void @__cxx_global_var_init()
  ret void
}
