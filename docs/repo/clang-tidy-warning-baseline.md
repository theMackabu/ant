# Clang-Tidy Warning Baseline

Status: generated
Generated: 2026-08-17
Owner: theMackabu

This report records every warning occurrence emitted by the full,
report-only Ant C lint baseline. Repeated warnings from shared headers are
kept under each translation unit that emitted them.

## Baseline

- Translation units analyzed: 197
- Warning occurrences: 1731
- Error occurrences: 0
- Clang-Tidy version: 21.1.8
- Executable: `/nix/store/rr64nnycczvx7s1b110qmvqrlfcb6lsm-clang-21.1.8/bin/clang-tidy`
- Mode: report-only; warnings do not make `maid lint_c_all` fail

Reproduce the analysis with:

```sh
ANT_CLANG_TIDY=/nix/store/rr64nnycczvx7s1b110qmvqrlfcb6lsm-clang-21.1.8/bin/clang-tidy maid lint_c_all
```

## Summary by Check

| Check | Occurrences |
| --- | ---: |
| `cert-err33-c` | 1119 |
| `readability-redundant-casting` | 166 |
| `clang-analyzer-unix.Malloc` | 110 |
| `bugprone-misplaced-widening-cast` | 102 |
| `clang-analyzer-deadcode.DeadStores` | 94 |
| `clang-analyzer-core.NullDereference` | 41 |
| `clang-analyzer-core.uninitialized.Branch` | 34 |
| `clang-analyzer-core.uninitialized.Assign` | 28 |
| `clang-analyzer-core.CallAndMessage` | 22 |
| `clang-analyzer-core.UndefinedBinaryOperatorResult` | 7 |
| `bugprone-macro-parentheses` | 5 |
| `clang-analyzer-core.uninitialized.UndefReturn` | 3 |

## Warning Inventory

Each bullet below is one emitted warning occurrence. The heading names
the translation unit being analyzed; the bullet location can point into a
shared header included by that unit.

### `src/ant.c` (46)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:174:33` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/ant.c:866:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'size_t' (aka 'unsigned long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:1954:13` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'ok' is never read
- `src/ant.c:1964:11` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'ok' is never read
- `src/ant.c:6189:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/ant.c:6442:30` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:6478:43` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'doff' is never read
- `src/ant.c:7829:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'r' during its initialization is never read
- `src/ant.c:10451:17` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'elem' during its initialization is never read
- `src/ant.c:10517:19` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'str_val' during its initialization is never read
- `src/ant.c:11936:43` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:11941:28` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:11948:33` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:11949:32` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:11957:27` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:11970:30` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12035:30` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12061:28` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12062:31` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12077:28` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12078:31` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12097:50` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12142:41` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12143:29` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12147:41` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12148:29` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:12166:47` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/ant.c:13226:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'attr_val' during its initialization is never read
- `src/ant.c:13653:5` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'pos' is never read
- `src/ant.c:13806:16` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'base_off' during its initialization is never read
- `src/ant.c:13816:16` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/ant.c:13981:17` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'chunk' during its initialization is never read
- `src/ant.c:17417:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/ant.c:18378:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'new_base'
- `src/ant.c:18378:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'new_named'
- `src/ant.c:18378:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'new_names'
- `src/ant.c:18452:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'new_ptrs'
- `src/ant.c:18452:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'new_vals'
- `src/ant.c:19046:14` — `bugprone-misplaced-widening-cast` — either cast from 'unsigned int' to 'size_t' (aka 'unsigned long') is ineffective, or there is loss of precision before the conversion

### `src/bootstrap.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/cli/compile.c` (12)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:294:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:308:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:309:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:120:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:126:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:175:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:192:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:264:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:416:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/compile.c:524:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/cli/misc.c` (1)

- `src/cli/misc.c:48:6` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/cli/pkg.c` (116)

- `include/progress.h:294:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:308:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:309:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:167:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:191:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:192:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:193:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:280:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:286:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:534:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:541:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:569:55` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:570:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:572:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:653:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:719:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:730:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:734:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:745:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:749:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:759:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:771:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:807:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:819:12` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:868:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:881:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:936:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:940:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:941:10` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:949:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:950:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:991:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:998:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1008:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1020:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1079:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1099:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1110:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1125:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1183:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1226:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1233:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1246:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1284:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1295:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1338:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1346:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1360:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1390:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1401:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1449:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1484:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1503:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1522:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1608:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1632:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1642:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1659:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1735:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1778:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1804:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1809:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1848:21` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1922:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1923:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1947:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1956:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1983:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1988:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:1997:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2003:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2019:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2027:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2220:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2221:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2244:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2255:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2257:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2263:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2293:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2308:33` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/cli/pkg.c:2341:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2352:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2365:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2377:38` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2378:16` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2393:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2406:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2412:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2419:16` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2420:8` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2441:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2457:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2463:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2470:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2477:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2484:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2489:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2494:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2501:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2540:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2601:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2651:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2666:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2730:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2737:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2759:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2765:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2788:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2824:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2825:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2860:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2875:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/pkg.c:2904:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/cli/registry.c` (39)

- `src/cli/registry.c:145:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:150:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:155:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:160:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:336:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:349:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:450:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:458:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:472:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:491:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:504:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:507:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:518:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:528:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:585:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:625:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:631:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:635:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:738:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:747:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:755:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:764:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:776:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:784:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:793:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:805:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:814:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:829:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:852:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:865:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:896:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1122:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1128:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1148:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1167:14` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1168:10` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1180:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1199:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/registry.c:1208:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/cli/version.c` (17)

- `include/progress.h:294:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:308:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:309:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:229:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:241:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:246:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:253:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:259:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:270:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:271:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:277:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:278:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:284:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:285:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:321:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/cli/version.c:336:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/crash.c` (26)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:510:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:575:28` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:617:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:720:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:723:32` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:725:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:728:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:744:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:751:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:757:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:760:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:773:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:774:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:776:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:778:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:797:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:873:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/crash.c:983:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/descriptors.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/download.c` (4)

- `include/progress.h:294:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:308:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:309:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/errors.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/errors.c:499:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/errors.c:679:7` — `clang-analyzer-core.NullDereference` — Dereference of null pointer

### `src/esm/commonjs.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/esm/exports.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/esm/hooks.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/esm/library.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/esm/loader.c` (21)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/loader.c:198:9` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'root_end' during its initialization is never read
- `src/esm/loader.c:1339:3` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The left operand of '<<' is a garbage value
- `src/esm/loader.c:1339:3` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/esm/loader.c:1392:3` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The left operand of '<<' is a garbage value
- `src/esm/loader.c:1392:3` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/esm/loader.c:1403:7` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/esm/loader.c:1422:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/loader.c:1424:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/loader.c:1428:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/loader.c:1432:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/loader.c:1433:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/loader.c:1446:26` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/esm/loader.c:1456:24` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/esm/loader.c:1481:21` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value

### `src/esm/loader_cache.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/esm/loader_cache.c:277:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/esm/loader_cache.c:285:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/esm/loader_cache.c:293:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/esm/loader_cache.c:301:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/esm/loader_cache.c:309:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/esm/remote.c` (11)

- `src/esm/remote.c:176:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:178:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:182:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:186:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:187:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:220:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:221:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:231:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:232:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:233:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/esm/remote.c:234:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/esm/trace.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/gc/bigints.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/gc/gc.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/gc/gc.c:230:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/gc/objects.c` (11)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/gc/objects.c:926:40` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/gc/objects.c:937:40` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/gc/objects.c:957:40` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/gc/objects.c:1190:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/gc/roots.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/gc/ropes.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/gc/ropes.c:237:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/gc/strings.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/gc/weak.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/gc/weak.c:459:7` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/http/eventsource.c` (1)

- `src/http/eventsource.c:186:38` — `clang-analyzer-core.NullDereference` — Array access (via field 'line') results in a null pointer dereference

### `src/http/http1_writer.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/inspector/debugger.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/inspector/debugger.c:273:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/inspector/debugger.c:278:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/inspector/debugger.c:282:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/inspector/debugger.c:288:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/inspector/debugger.c:293:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/inspector/router.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/inspector/router.c:198:46` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting

### `src/inspector/runtime.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/inspector/safe_eval.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/inspector/safe_eval.c:544:45` — `bugprone-misplaced-widening-cast` — either cast from 'size_t' (aka 'unsigned long') to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion

### `src/inspector/server.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/inspector/server.c:371:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/main.c` (14)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/main.c:322:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:372:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:412:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:444:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:496:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:631:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:735:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:751:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:757:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:769:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:774:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main.c:843:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/main_runtime.c` (12)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/main_runtime.c:41:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:67:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:77:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:83:19` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:89:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:96:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:103:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:111:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:131:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/main_runtime.c:144:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/abort.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/assert.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/async_hooks.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/atomics.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/bigint.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/bigint.c:80:24` — `clang-analyzer-core.NullDereference` — Array access (from variable 'limbs') results in a null pointer dereference
- `src/modules/bigint.c:167:21` — `clang-analyzer-core.NullDereference` — Array access (via field 'limbs') results in a null pointer dereference
- `src/modules/bigint.c:168:17` — `clang-analyzer-core.NullDereference` — Access to field 'sign' results in a dereference of a null pointer (loaded from variable 'payload')
- `src/modules/bigint.c:703:35` — `clang-analyzer-core.NullDereference` — Array access (from variable 'result') results in a null pointer dereference
- `src/modules/bigint.c:750:15` — `clang-analyzer-core.NullDereference` — Array access (from variable 'result') results in a null pointer dereference
- `src/modules/bigint.c:1110:16` — `clang-analyzer-core.CallAndMessage` — 1st function call argument is an uninitialized value
- `src/modules/bigint.c:1370:19` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'limbs' during its initialization is never read

### `src/modules/blob.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/blob.c:416:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'bd'

### `src/modules/buffer.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/buffer.c:999:12` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'length' during its initialization is never read
- `src/modules/buffer.c:1504:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/buffer.c:1670:10` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'byte_length' during its initialization is never read
- `src/modules/buffer.c:2557:12` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'byte_length' during its initialization is never read
- `src/modules/buffer.c:3159:33` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'owned_needle'

### `src/modules/builtin.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/builtin.c:62:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/child_process.c` (20)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:169:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:176:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:177:19` — `clang-analyzer-core.NullDereference` — Array access (from variable 'ptr') results in a null pointer dereference
- `src/modules/child_process.c:177:41` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:183:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:193:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:205:28` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:211:15` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/child_process.c:1195:5` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'spawn_args'
- `src/modules/child_process.c:1288:5` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'cmd_str' is never read
- `src/modules/child_process.c:1302:13` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'cmd_str'
- `src/modules/child_process.c:2543:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/cjit.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/cjit.c:754:26` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/cjit.c:771:26` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/cjit.c:785:26` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/cjit.c:796:26` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/cjit.c:813:24` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/collections.c` (15)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/collections.c:60:17` — `readability-redundant-casting` — redundant explicit casting to the same type 'uint8_t' (aka 'unsigned char') as the sub-expression, remove this casting
- `src/modules/collections.c:514:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/modules/collections.c:707:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/modules/collections.c:960:17` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The right operand of '<=' is a garbage value
- `src/modules/collections.c:1012:37` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The right operand of '<=' is a garbage value
- `src/modules/collections.c:1077:37` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The right operand of '>' is a garbage value
- `src/modules/collections.c:1111:37` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The right operand of '<' is a garbage value
- `src/modules/collections.c:1137:37` — `clang-analyzer-core.UndefinedBinaryOperatorResult` — The right operand of '<=' is a garbage value

### `src/modules/crypto.c` (20)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/crypto.c:1184:7` — `clang-analyzer-core.NullDereference` — Access to field 'finalized' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1208:7` — `clang-analyzer-core.NullDereference` — Access to field 'finalized' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1282:7` — `clang-analyzer-core.NullDereference` — Access to field 'finalized' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1305:7` — `clang-analyzer-core.NullDereference` — Access to field 'finalized' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1432:7` — `clang-analyzer-core.NullDereference` — Access to field 'finalized' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1470:7` — `clang-analyzer-core.NullDereference` — Access to field 'finalized' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1506:12` — `clang-analyzer-core.NullDereference` — Access to field 'encrypt' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1517:8` — `clang-analyzer-core.NullDereference` — Access to field 'is_gcm' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1536:23` — `clang-analyzer-core.NullDereference` — Access to field 'auth_tag_len' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1548:30` — `clang-analyzer-core.NullDereference` — Access to field 'ctx' results in a dereference of a null pointer (loaded from variable 'state')
- `src/modules/crypto.c:1621:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'state'
- `src/modules/crypto.c:1708:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'password_owned'

### `src/modules/date.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/date.c:143:7` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'd1' is never read
- `src/modules/date.c:150:5` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'd1' is never read
- `src/modules/date.c:325:16` — `clang-analyzer-core.uninitialized.UndefReturn` — Undefined or garbage value returned to caller
- `src/modules/date.c:905:16` — `clang-analyzer-core.uninitialized.UndefReturn` — Undefined or garbage value returned to caller
- `src/modules/date.c:923:16` — `clang-analyzer-core.uninitialized.UndefReturn` — Undefined or garbage value returned to caller

### `src/modules/dns.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/domexception.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/events.c` (15)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/events.c:513:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `src/modules/events.c:909:22` — `clang-analyzer-core.NullDereference` — Access to field 'callback' results in a dereference of a null pointer (loaded from variable 'entry')
- `src/modules/events.c:1044:30` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/events.c:1050:10` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/events.c:1139:22` — `clang-analyzer-core.NullDereference` — Access to field 'callback' results in a dereference of a null pointer (loaded from variable 'entry')
- `src/modules/events.c:1156:40` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/events.c:1157:12` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/events.c:1158:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/eventsource.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/eventsource.c:94:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read

### `src/modules/fetch.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fetch.c:736:3` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/modules/ffi.c` (22)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/ffi.c:210:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'returns_val' during its initialization is never read
- `src/modules/ffi.c:211:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'args_val' during its initialization is never read
- `src/modules/ffi.c:869:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'fn' during its initialization is never read
- `src/modules/ffi.c:870:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/ffi.c:879:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/ffi.c:890:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/ffi.c:896:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/ffi.c:902:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/ffi.c:946:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'path_val' during its initialization is never read
- `src/modules/ffi.c:1006:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'fn' during its initialization is never read
- `src/modules/ffi.c:1040:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'fn' during its initialization is never read
- `src/modules/ffi.c:1325:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'signature_val' during its initialization is never read
- `src/modules/ffi.c:1326:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'fn' during its initialization is never read
- `src/modules/ffi.c:1414:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'suffix' during its initialization is never read

### `src/modules/formdata.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/fs.c` (29)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:215:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/fs.c:1006:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'persistent_val' during its initialization is never read
- `src/modules/fs.c:1036:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'persistent_val' during its initialization is never read
- `src/modules/fs.c:2086:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2088:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2091:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2098:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2104:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2212:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2256:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2267:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2267:17` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2272:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2272:15` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2354:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2363:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2364:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2368:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:2369:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/fs.c:3531:10` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/modules/fs.c:4543:10` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/modules/fs.c:4586:10` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/modules/generator.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/generator.c:593:39` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'data'

### `src/modules/globals.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/headers.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/http_metadata.c` (1)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/http_parser.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/http_writer.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/intl.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/io.c` (86)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:265:22` — `bugprone-macro-parentheses` — macro argument should be enclosed in parentheses
- `src/modules/io.c:266:13` — `bugprone-macro-parentheses` — macro argument should be enclosed in parentheses
- `src/modules/io.c:272:10` — `bugprone-macro-parentheses` — macro argument should be enclosed in parentheses
- `src/modules/io.c:559:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/io.c:999:35` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1019:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1023:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1024:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1025:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1026:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1027:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1029:36` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1030:14` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1034:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1035:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1051:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1059:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1063:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1070:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1076:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1084:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1088:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1094:41` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1096:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1099:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1100:21` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1102:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1105:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1106:21` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1108:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1111:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1117:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1125:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1129:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1138:44` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1140:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1146:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1151:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1152:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1154:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1159:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1161:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1164:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1168:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1174:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1175:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1176:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1177:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1178:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1187:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1192:21` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1200:23` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1210:15` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1211:10` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1213:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1214:24` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1215:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1220:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1229:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1234:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1238:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1246:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1251:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1254:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1258:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1260:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1266:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1270:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1273:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1282:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1284:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1289:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1293:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1297:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1300:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1304:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1312:16` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/io.c:1316:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/iterator.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/iterator.c:1103:61` — `readability-redundant-casting` — redundant explicit casting to the same type 'double' as the sub-expression, remove this casting
- `src/modules/iterator.c:1194:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/modules/json.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/json.c:27:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/modules/json.c:999:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'root_holder' during its initialization is never read

### `src/modules/lmdb.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/localstorage.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/localstorage.c:89:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/modules/math.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/module.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/multipart.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/multipart.c:152:5` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'cap' is never read

### `src/modules/napi.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/napi.c:884:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'ret' during its initialization is never read
- `src/modules/napi.c:2155:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'del_result' during its initialization is never read
- `src/modules/napi.c:2303:17` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'key' during its initialization is never read
- `src/modules/napi.c:2319:17` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'value' during its initialization is never read
- `src/modules/napi.c:2802:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/navigator.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/navigator.c:219:7` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'req' is never read
- `src/modules/navigator.c:243:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'options' during its initialization is never read

### `src/modules/net.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/net.c:166:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read

### `src/modules/observable.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/observable.c:26:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/observable.c:107:33` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/observable.c:136:35` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/observable.c:166:35` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/observable.c:262:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/os.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/path.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/path.c:373:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'result'

### `src/modules/performance.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/process.c` (12)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/process.c:1310:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/process.c:1517:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/process.c:1518:13` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/process.c:1520:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/process.c:1521:15` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/process_plan.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/readline.c` (12)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/readline.c:221:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:222:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:353:50` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:403:53` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:409:53` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:545:48` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:594:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:745:45` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:756:45` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/readline.c:1083:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'value' during its initialization is never read

### `src/modules/reflect.c` (9)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1159:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/regex.c` (15)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/regex.c:1200:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/regex.c:2324:21` — `readability-redundant-casting` — redundant explicit casting to the same type 'size_t' (aka 'unsigned long') as the sub-expression, remove this casting
- `src/modules/regex.c:2818:15` — `readability-redundant-casting` — redundant explicit casting to the same type 'size_t' (aka 'unsigned long') as the sub-expression, remove this casting
- `src/modules/regex.c:3075:25` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'str_off' during its initialization is never read
- `src/modules/regex.c:3152:52` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion
- `src/modules/regex.c:3391:25` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'str_off' during its initialization is never read
- `src/modules/regex.c:3725:7` — `clang-analyzer-unix.Malloc` — Attempt to free released memory

### `src/modules/request.c` (14)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/request.c:1046:14` — `clang-analyzer-core.NullDereference` — Access to field 'mode' results in a dereference of a null pointer (loaded from variable 'req')
- `src/modules/request.c:1269:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'step' during its initialization is never read
- `src/modules/request.c:1277:28` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'req'
- `src/modules/request.c:1285:12` — `clang-analyzer-core.NullDereference` — Access to field 'mode' results in a dereference of a null pointer (loaded from variable 'req')
- `src/modules/request.c:1346:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'step' during its initialization is never read
- `src/modules/request.c:1353:28` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'req'
- `src/modules/request.c:1361:12` — `clang-analyzer-core.NullDereference` — Access to field 'mode' results in a dereference of a null pointer (loaded from variable 'req')

### `src/modules/response.c` (11)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/response.c:329:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'combined' during its initialization is never read
- `src/modules/response.c:518:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'data'
- `src/modules/response.c:722:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'step' during its initialization is never read
- `src/modules/response.c:763:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'resp'

### `src/modules/rpc.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/sandbox.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/server.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/server.c:511:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/server.c:560:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'event_s' during its initialization is never read
- `src/modules/server.c:561:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'id_s' during its initialization is never read
- `src/modules/server.c:562:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'retry_s' during its initialization is never read
- `src/modules/server.c:861:9` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'wr'
- `src/modules/server.c:1210:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/sessionstorage.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/sessionstorage.c:64:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/modules/shell.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/stream.c` (11)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/stream.c:295:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'value' during its initialization is never read
- `src/modules/stream.c:624:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/stream.c:1213:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'next' during its initialization is never read
- `src/modules/stream.c:1949:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'next_result' during its initialization is never read

### `src/modules/string_decoder.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/string_decoder.c:221:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/modules/structured-clone.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/structured-clone.c:47:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/modules/symbol.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/syntax.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/core.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/duration.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/instant.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/now.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/temporal/now.c:8:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/temporal/plain_date.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/plain_datetime.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/plain_monthday.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/plain_time.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/plain_yearmonth.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/temporal/zoned_datetime.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/textcodec.c` (4)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/textcodec.c:282:36` — `clang-analyzer-core.NullDereference` — Array access (from variable 'src') results in a null pointer dereference
- `src/modules/textcodec.c:418:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/modules/timer.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/timer.c:524:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'value' during its initialization is never read
- `src/modules/timer.c:536:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'reason' during its initialization is never read
- `src/modules/timer.c:564:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'signal' during its initialization is never read
- `src/modules/timer.c:852:11` — `clang-analyzer-deadcode.DeadStores` — Although the value stored to 'batch' is used in the enclosing expression, the value is never actually read from 'batch'
- `src/modules/timer.c:856:11` — `clang-analyzer-deadcode.DeadStores` — Although the value stored to 'batch' is used in the enclosing expression, the value is never actually read from 'batch'

### `src/modules/tls.c` (10)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/tls.c:158:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/tls.c:716:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/tls.c:1062:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'str_value' during its initialization is never read

### `src/modules/tty.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/uri.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/url/legacy.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/url/url_internal.h:38:18` — `clang-analyzer-core.NullDereference` — Array access (via field 'buf') results in a null pointer dereference

### `src/modules/url.c` (13)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/url.c:423:19` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'buf'
- `src/modules/url.c:684:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'base_sv' during its initialization is never read
- `src/modules/url.c:733:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'base_sv' during its initialization is never read
- `src/modules/url.c:755:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'base_sv' during its initialization is never read
- `src/modules/url.c:1360:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 's'

### `src/modules/util.c` (12)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/util.c:344:84` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/modules/util.c:657:64` — `bugprone-macro-parentheses` — macro argument should be enclosed in parentheses
- `src/modules/util.c:1061:17` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'value' during its initialization is never read
- `src/modules/util.c:1302:14` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/v8.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/modules/wasi.c` (4)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/modules/wasi.c:342:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'fenv'
- `src/modules/wasi.c:460:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'handle'

### `src/modules/wasm.c` (17)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/wasm.c:434:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'handle'
- `src/modules/wasm.c:495:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'handle'
- `src/modules/wasm.c:543:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/wasm.c:623:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'handle'
- `src/modules/wasm.c:869:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/modules/wasm.c:947:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'import_error' during its initialization is never read
- `src/modules/wasm.c:948:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'instance_obj' during its initialization is never read
- `src/modules/wasm.c:1136:7` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'owned_host_funcs'
- `src/modules/wasm.c:1149:18` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'inst_handle'

### `src/modules/websocket.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/modules/websocket.c:136:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read

### `src/modules/worker_threads.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/modules/zlib.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/net/connection.c` (1)

- `src/net/connection.c:375:28` — `clang-analyzer-core.NullDereference` — Dereference of null pointer

### `src/pool.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/reactor.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/readline/editor.c` (1)

- `src/readline/editor.c:340:10` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'line'

### `src/readline/history.c` (7)

- `src/readline/history.c:107:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/history.c:145:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/history.c:158:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/history.c:162:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/history.c:163:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/history.c:164:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/history.c:167:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/readline/render.c` (20)

- `src/readline/render.c:211:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:222:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:223:14` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:234:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:239:26` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:309:10` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:311:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:313:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:314:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:315:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:319:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:321:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:322:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:323:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:328:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:329:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:330:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:338:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:345:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/render.c:356:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/readline/terminal.c` (4)

- `src/readline/terminal.c:122:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/terminal.c:126:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/terminal.c:130:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/readline/terminal.c:137:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/repl/inspector.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/repl/repl.c` (26)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:604:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:620:33` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:644:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:650:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:654:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:656:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:669:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:675:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:681:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:686:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:688:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:702:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:703:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:747:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:754:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:770:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:948:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/repl/repl.c:949:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/runtime.c` (5)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/runtime.c:59:3` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/runtime.c:61:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/runtime.c:261:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/sandbox/assets.c` (14)

- `include/progress.h:294:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:308:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:309:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:255:27` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:257:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:259:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:266:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:267:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:917:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:923:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:927:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:937:27` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/assets.c:938:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/backends/darwin/backend.c` (13)

- `src/sandbox/backends/darwin/backend.c:14:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:21:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:77:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:80:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:86:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:87:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:95:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:97:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:192:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:228:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:244:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:471:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/backend.c:643:25` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/backends/darwin/gic.c` (3)

- `src/sandbox/backends/darwin/gic.c:33:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/gic.c:56:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/gic.c:71:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/backends/darwin/mmio.c` (2)

- `src/sandbox/backends/darwin/mmio.c:82:14` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/mmio.c:90:8` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/backends/darwin/uart.c` (3)

- `src/sandbox/backends/darwin/uart.c:48:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/uart.c:49:27` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/darwin/uart.c:61:48` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/backends/shared/common.c` (6)

- `src/sandbox/backends/shared/common.c:25:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/common.c:31:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/common.c:36:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/common.c:126:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/common.c:143:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/common.c:149:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/backends/shared/nat.c` (3)

- `src/sandbox/backends/shared/nat.c:128:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/nat.c:133:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/nat.c:578:14` — `clang-analyzer-core.NullDereference` — Array access (from variable 'fds') results in a null pointer dereference

### `src/sandbox/backends/shared/virtio_9p.c` (1)

- `src/sandbox/backends/shared/virtio_9p.c:803:18` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/sandbox/backends/shared/virtio_vsock.c` (11)

- `src/sandbox/backends/shared/virtio_vsock.c:374:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:375:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:384:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:385:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:386:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:389:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:391:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:412:49` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:413:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:428:49` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/backends/shared/virtio_vsock.c:429:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/cli.c` (19)

- `src/sandbox/cli.c:147:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:165:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:170:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:175:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:211:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:263:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:276:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:280:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:283:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:286:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:292:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:295:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:298:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:303:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:317:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:337:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:344:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:352:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/cli.c:393:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/host.c` (4)

- `include/progress.h:294:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:295:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:308:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/progress.h:309:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/sandbox.c` (17)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:233:3` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'p' is never read
- `src/sandbox/sandbox.c:339:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:344:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:349:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:354:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:360:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:378:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:383:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/sandbox.c:450:3` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'p' is never read
- `src/sandbox/sandbox.c:505:3` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'p' is never read

### `src/sandbox/transport.c` (2)

- `src/sandbox/transport.c:208:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/transport.c:213:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/sandbox/vm_helper.c` (15)

- `src/sandbox/vm_helper.c:408:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:411:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:421:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:424:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:575:18` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:576:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:585:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:586:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:587:20` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:590:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:592:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:615:47` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:616:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:626:47` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sandbox/vm_helper.c:627:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/shapes.c` (3)

- `src/shapes.c:120:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/shapes.c:398:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/shapes.c:404:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/silver/ast.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/silver/ast.c:386:7` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'strict_mode' is never read
- `src/silver/ast.c:2435:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/ast.c:2444:32` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/ast.c:2452:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/ast.c:2458:32` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/silver/ast_export.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/silver/compile_ctx.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compile_ctx.c:103:5` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/silver/compiler.c` (71)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1147:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1148:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1149:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1150:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1152:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1162:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1163:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1170:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1181:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:1214:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:3064:49` — `clang-analyzer-core.NullDereference` — Access to field 'str' results in a dereference of a null pointer (loaded from field 'right')
- `src/silver/compiler.c:6961:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6963:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6964:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6965:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6966:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6977:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6979:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6981:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6988:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6989:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6993:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6996:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:6999:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7002:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7005:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7008:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7011:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7016:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7018:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7020:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7027:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7029:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7034:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7039:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7043:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7046:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7051:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7053:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7055:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7059:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7063:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7067:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7070:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7073:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7079:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7083:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7090:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7092:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7096:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7097:12` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7101:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7103:7` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7107:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7108:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7109:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7136:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7146:22` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'top_name' during its initialization is never read
- `src/silver/compiler.c:7180:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7190:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7247:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7264:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/compiler.c:7346:34` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/ops/property.h:398:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/silver/engine.c` (19)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/engine.c:313:19` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'tail' during its initialization is never read
- `src/silver/engine.c:1193:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'vm_result' during its initialization is never read
- `src/silver/engine.c:1336:16` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'tc_this' during its initialization is never read
- `src/silver/engine.c:1344:27` — `bugprone-macro-parentheses` — macro replacement list should be enclosed in parentheses
- `src/silver/ops/iteration.h:338:23` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/ops/iteration.h:384:23` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/ops/iteration.h:397:5` — `clang-analyzer-core.CallAndMessage` — 3rd function call argument is an uninitialized value
- `src/silver/ops/iteration.h:475:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/silver/ops/objects.h:176:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'val' during its initialization is never read
- `src/silver/ops/property.h:398:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/ops/unary.h:29:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'key_str' during its initialization is never read

### `src/silver/eval_env.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/silver/glue.c` (14)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1159:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/glue.c:222:28` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'call.args'
- `src/silver/glue.c:1203:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'key_str' during its initialization is never read
- `src/silver/ops/iteration.h:475:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'result' during its initialization is never read
- `src/silver/ops/objects.h:176:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'val' during its initialization is never read
- `src/silver/ops/property.h:398:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/silver/lexer.c` (3)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/silver/lexer.c:815:11` — `bugprone-misplaced-widening-cast` — either cast from 'long' to 'ant_offset_t' (aka 'unsigned long long') is ineffective, or there is loss of precision before the conversion

### `src/silver/swarm.c` (223)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:563:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:571:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:714:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:723:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:725:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:748:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:757:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:759:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:781:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:938:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:940:11` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:959:15` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:2392:29` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:2457:23` — `readability-redundant-casting` — redundant explicit casting to the same type 'uint16_t' (aka 'unsigned short') as the sub-expression, remove this casting
- `src/silver/swarm.c:2843:15` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2849:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:2861:28` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:2863:9` — `clang-analyzer-core.CallAndMessage` — 3rd function call argument is an uninitialized value
- `src/silver/swarm.c:2868:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2871:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:2880:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2883:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:2890:22` — `clang-analyzer-core.CallAndMessage` — 3rd function call argument is an uninitialized value
- `src/silver/swarm.c:2891:22` — `clang-analyzer-core.CallAndMessage` — 3rd function call argument is an uninitialized value
- `src/silver/swarm.c:2892:22` — `clang-analyzer-core.CallAndMessage` — 3rd function call argument is an uninitialized value
- `src/silver/swarm.c:2893:22` — `clang-analyzer-core.CallAndMessage` — 3rd function call argument is an uninitialized value
- `src/silver/swarm.c:2898:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2905:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2909:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2915:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2919:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2924:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:2926:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2931:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2935:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:2937:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2942:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2946:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:2948:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2953:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2957:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:2959:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2964:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:2969:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:2981:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:2993:15` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:3012:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3015:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:3016:13` — `clang-analyzer-core.CallAndMessage` — 2nd function call argument is an uninitialized value
- `src/silver/swarm.c:3021:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3039:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3051:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3077:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3115:11` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3127:13` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3199:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3200:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3201:9` — `clang-analyzer-core.CallAndMessage` — 4th function call argument is an uninitialized value
- `src/silver/swarm.c:3260:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3261:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3289:13` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3313:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3314:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3367:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3368:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3369:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3378:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3379:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3380:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3389:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3390:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3391:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3400:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3401:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3402:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3412:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3413:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3433:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3434:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3460:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3468:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3471:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3492:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3496:52` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:3499:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3542:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3544:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3547:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3580:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3582:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3585:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3625:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3627:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3630:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3631:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3653:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3656:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3688:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3689:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3691:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3698:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3742:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3743:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3744:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3759:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3760:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3780:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3781:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3782:9` — `clang-analyzer-core.CallAndMessage` — 4th function call argument is an uninitialized value
- `src/silver/swarm.c:3806:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3808:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:3816:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:3968:9` — `clang-analyzer-core.uninitialized.Branch` — Branch condition evaluates to a garbage value
- `src/silver/swarm.c:3969:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:3987:9` — `clang-analyzer-core.uninitialized.Assign` — Assigned value is uninitialized
- `src/silver/swarm.c:4017:9` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:4035:42` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:4183:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:4196:11` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/silver/swarm.c:5036:13` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5247:13` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5287:13` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5346:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5569:28` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:5659:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5677:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5703:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5720:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5739:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5771:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5778:35` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:5805:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5812:35` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:5843:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5861:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5873:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5880:37` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:5913:33` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:5924:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5949:33` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:5960:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:5984:33` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:5995:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:6018:33` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:6029:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:6237:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6237:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6365:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6365:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6492:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6492:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6619:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6619:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6752:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:6760:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:6968:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:6968:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:7121:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:7121:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:7232:48` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:7274:48` — `readability-redundant-casting` — redundant explicit casting to the same type 'int8_t' (aka 'signed char') as the sub-expression, remove this casting
- `src/silver/swarm.c:7556:21` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:7603:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:7646:23` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:7852:23` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:7974:15` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8028:15` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8070:15` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8096:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8272:44` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:8289:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8311:44` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:8328:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8372:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8396:54` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:8439:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8476:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8506:17` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:8517:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8541:23` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8548:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8619:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8620:35` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:8644:23` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:8651:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9299:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9303:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9315:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9317:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9337:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9342:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9355:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9357:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9441:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9827:21` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:9992:21` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10242:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:10242:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:10361:38` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:10361:51` — `readability-redundant-casting` — redundant explicit casting to the same type 'int' as the sub-expression, remove this casting
- `src/silver/swarm.c:10484:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10491:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10592:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10620:21` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10639:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10667:21` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10685:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10721:21` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:10880:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:11043:23` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:11108:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:11172:19` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:11240:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:11330:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion
- `src/silver/swarm.c:11331:35` — `clang-analyzer-core.NullDereference` — Array access (from variable 'local_regs') results in a null pointer dereference
- `src/silver/swarm.c:11350:17` — `bugprone-misplaced-widening-cast` — either cast from 'int' to 'MIR_disp_t' (aka 'long long') is ineffective, or there is loss of precision before the conversion

### `src/snapshot.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/streams/brotli.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/streams/codec.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/streams/compression.c` (4)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/streams/compression.c:224:25` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'
- `src/streams/compression.c:346:25` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/streams/pipes.c` (8)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/streams/pipes.c:738:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/streams/queuing.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/streams/readable.c` (10)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/streams/readable.c:796:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'ctrl'
- `src/streams/readable.c:941:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/streams/transform.c` (12)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/streams/transform.c:955:23` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'rst'
- `src/streams/transform.c:961:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'rcc'
- `src/streams/transform.c:991:22` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'wst'
- `src/streams/transform.c:997:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'wc'

### `src/streams/writable.c` (10)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1139:27` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'plan.ctx.alloc'
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/streams/writable.c:1035:3` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'ctrl'
- `src/streams/writable.c:1092:34` — `clang-analyzer-unix.Malloc` — Potential leak of memory pointed to by 'st'

### `src/sugar.c` (11)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `include/silver/engine.h:366:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:382:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:383:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1302:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/silver/engine.h:1314:31` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/sugar.c:96:3` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/sugar.c:138:3` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/sugar.c:266:3` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed
- `src/sugar.c:268:3` — `clang-analyzer-unix.Malloc` — Use of memory after it is freed

### `src/tty_ctrl.c` (2)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting

### `src/utf8.c` (7)

- `include/common.h:144:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `include/object.h:260:20` — `readability-redundant-casting` — redundant explicit casting to the same type 'ant_private_table_t *' as the sub-expression, remove this casting
- `src/utf8.c:936:7` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'bc' is never read
- `src/utf8.c:936:15` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'cp' is never read
- `src/utf8.c:936:23` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'pp' is never read
- `src/utf8.c:945:3` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'pp' is never read
- `src/utf8.c:952:3` — `clang-analyzer-deadcode.DeadStores` — Value stored to 'cp' is never read

### `src/vfs_bundle.c` (2)

- `src/vfs_bundle.c:279:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/vfs_bundle.c:344:10` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors

### `src/watch.c` (6)

- `src/watch.c:75:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/watch.c:76:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/watch.c:261:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/watch.c:267:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/watch.c:274:5` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
- `src/watch.c:279:3` — `cert-err33-c` — the value returned by this function should not be disregarded; neglecting it may lead to errors
