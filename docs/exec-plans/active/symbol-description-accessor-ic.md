# Symbol description accessor IC

Status: active
Last reviewed: 2026-09-15
Owner: theMackabu

## Goal

Restore Symbol description read performance after removing the incorrect virtual
property. Targets per million benchmark iterations: primitive named 17 ms;
primitive computed, boxed named and boxed computed 22 ms each.

## Implementation

Extend the existing field IC with primitive and boxed Symbol-description kinds.
Only direct Symbol.prototype access to the original native getter is cached.
Boxed receivers must be ordinary objects with no own shadowing property and the
expected direct prototype. Other chains and proxy receivers fall back.

The cache retains its existing shape slot and size. Primitive entries retain the
prototype shape; boxed entries retain the receiver shape. The holder is compared
with the isolate-rooted Symbol.prototype before use. The existing IC epoch
invalidates descriptor replacement/deletion; boxed shape and prototype guards
invalidate own-property additions and prototype changes. Registry failure must
not install a valid cache entry. The cached result is getter identity, never a
particular Symbol's description string.

The handler executes outside no-effect probes because constructing its result
may allocate. The shared extraction helper roots the actual Symbol during string
allocation. Computed reads still perform ordinary lookup; once that resolves to
the original native getter, accessor dispatch can use the shared extraction
helper instead of entering the VM. It does not optimize by key spelling.

Symbol is also admitted to the existing primitive data/missing IC lookup.
Custom getters and invalid native-getter receivers retain ordinary VM dispatch.
No MIR emitter or bytecode layout change is included in this first experiment.

## Validation and comparison

User will build. No build, runtime test or benchmark executed for this change.
Added tests/test_jit_symbol_description_ic.cjs; retain the previous missing-field
and Symbol spec mutation coverage. After building, run those and the existing
unboxing/missing-field codegen test, then compare the unchanged baseline fixture
against both pinned binaries in alternating order.

Baseline: /tmp/ant-symbol-description-baseline-20260915-111350
Cleanup comparison: /tmp/ant-symbol-description-comparison-20260915-112417
No speed target is claimed met before measurement. Computed reads still pay
lookup costs, so their 22 ms goal remains an explicit open question.
