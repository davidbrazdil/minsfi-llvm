; RUN: pnacl-abicheck < %s | FileCheck %s

; Global variable attributes

; CHECK: Variable var_with_section has disallowed "section" attribute
@var_with_section = global i32 99, section ".some_section"

; PNaCl programs can depend on data alignments in general, so we allow
; "align" on global variables.
; CHECK-NOT: var_with_alignment
@var_with_alignment = global i32 99, align 8

; TLS variables must be expanded out by ExpandTls.
; CHECK-NEXT: Variable tls_var has disallowed "thread_local" attribute
@tls_var = thread_local global i32 99


; Function attributes

; CHECK-NEXT: Function func_with_section has disallowed "section" attribute
define void @func_with_section() section ".some_section" {
  ret void
}

; TODO(mseaborn): PNaCl programs don't know what alignment is
; reasonable for a function, so we should disallow this.
; CHECK-NOT: func_with_alignment
define void @func_with_alignment() align 1 {
  ret void
}

; CHECK-NEXT: Function func_with_gc has disallowed "gc" attribute
define void @func_with_gc() gc "my_gc_func" {
  ret void
}

; CHECK-NOT: disallowed
; If another check is added, there should be a check-not in between each check
