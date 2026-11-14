# Redesign progress

Design: `docs/redesign.md`. Roles: Codex (`codex-run work`) writes code, Opus
reviews diffs; Haiku runs builds/tests. Resource limits: this container has a
6 GB memory cap shared with other sessions; no Docker, no sudo, no clang dev
headers here, so builds and test runs happen on the user's Mac (`build/`,
Homebrew LLVM 23.1.3). Judge compiler on the Mac: Homebrew `g++-16`
(plain `g++` there is Apple Clang); matches the Dockerfile's `GXX_VERSION=16`.

## M0 — confirm suspected bugs (done, 2026-11-14)

New cases (registered in `src/test-tool/CMakeLists.txt`; `etalon.cpp` is the
*desired* output, identical to the input, so they fail while the bug exists):

| Case | Suspected bug | g++ result of input |
|---|---|---|
| `global-side-effect-ctor` | unreferenced global with side-effecting ctor removed | exit 7 |
| `field-layout-sizeof` | field read only via `sizeof` removed | exit 0 |
| `compiler-macro-branch` | `#ifdef __clang__ … #else` resolved for Clang | exit 2 |

**Result (2026-11-14, Mac build, LLVM 23.1.3): all three confirmed.** Each
output compiles cleanly with g++ and silently changes the result:

| Case | What the optimizer did | g++ exit: input → output |
|---|---|---|
| `global-side-effect-ctor` | removed `Boot` and `boot` | 7 → 0 |
| `field-layout-sizeof` | removed `reserved` | 0 → 1 |
| `compiler-macro-branch` | kept only the `__clang__` branch | 2 → 1 |

The three cases stay registered and fail until fixed: the first two in M2, the
third needs a new item (keep `#if` blocks that test compiler-identification
macros such as `__clang__`, `__GNUC__`, `_MSC_VER`; workaround today:
`-k __clang__`).

Next: M1 (g++ compile + run check in test-tool), Codex writes, Opus reviews.

### LLVM 23 build fix (2026-11-14)

Homebrew LLVM on the Mac is 23.1.3; `ASTContext::getRawCommentForDeclNoCache`
became private in 23. Codex added `getRawCommentForDecl` in `clang_compat`
(LLVM 23: `getRawCommentForAnyRedecl`, accepted only if the comment belongs to
the decl or its described template); reviewed by Opus, applied to the working
tree. Compiles on the Mac with LLVM 23.1.3 (test-tool target).

## M1 — judge check in test-tool (done, 2026-11-14)

Codex wrote it, Opus reviewed; one review fix (a missing judge compiler now
fails instead of passing silently). `CAIDE_TEST_CXX` (env var or CMake cache
variable) = judge command; unset = old behaviour. Per test: build the original
(per-test `-I/-isystem/-iquote/-D/-U/-std=` only) and `result.cpp`
(`-isystem/-D/-U/-std=` only), run both under 10 s with optional `input.txt`, compare
exit code + stdout. Original not building = skipped; output not building or
behaving differently = fail. Uses `llvm::sys::ExecuteAndWait`.
Result on the Mac (LLVM 22.1.8, `CAIDE_TEST_CXX="g++ -std=c++23"`, `g++` →
Homebrew g++-16): only the three M0 cases fail. Review fix: `-isystem` paths go
to the output's build too (system `#include`s stay in the output).
`gcc-autodetect` / `gcc-bits-stdc++` are skipped on macOS (`if(NOT APPLE)`):
`<bits/stdc++.h>` is libstdc++-only, and detection from Homebrew GCC fails
there (Clang can't parse GCC's own `stddef.h`: `__float128`). Not fixed; only
matters for running caide natively on macOS with GCC.

Next: M2. Codex worktrees start from HEAD, so the uncommitted work must be
committed first.

## M2 — coarse units and side-effect roots (done, not committed, 2026-11-14)

Codex wrote it, Opus reviewed. Applied:
- record → every non-static field and every user-declared special member
  (default/copy/move ctor, copy/move assignment, destructor) in
  `DependenciesCollector` (`VisitFieldDecl`, `VisitCXXMethodDecl`);
- roots: non-template globals / static data members with dynamic init or a
  non-trivial destructor (`VisitVarDecl`), and decls with
  `constructor`/`destructor`/`used` attributes (`VisitNamedDecl`);
- test-tool saves failing outputs to `tests/temp/actual/<test>.cpp`;
- `unused-fields` etalon keeps all fields; new cases
  `global-side-effect-roots`, `record-layout-special-members`.

Dropped in review: keeping every *explicit* specialization of a used template
(design §4.1 template families). Used explicit specializations are already
reached through instantiations (see the hardening TODO's verified table), and
five existing tests check that unused ones are removed. Design doc to be
adjusted: a template family = primary + partial specializations + deduction
guides.

Result on the Mac (LLVM 22.1.8, judge g++-16): builds cleanly; only
`compiler-macro-branch` fails (out of M2 scope). `field-layout-sizeof` and
`global-side-effect-ctor` now pass. `inliner2` etalon updated: its used
structs now keep their fields (`int a;` ×2, `T t;`), judge check passed.

## compiler-macro-branch — harness bug, not a cmd bug (2026-11-14)

`cmd` already keeps both branches of `#ifdef __clang__`: the library's default
`macrosToKeep` lists `__clang__`, `__GNUC__`, `_MSC_VER`, `_WIN32`,
`__linux__`, `__APPLE__`, … and `cmd -k` appends to it. test-tool replaced
the defaults with the test's `macrosToKeep.txt` (usually empty), so the M0
"confirmation" of this case tested the harness, not the tool. Fixed:
test-tool now appends, like `cmd`. Expected etalon change: `macros` keeps the
`#if __cplusplus` block in `b()` (confirmed on the Mac, etalon updated; full suite
passes). The C interface (`caideInliner.h`) still
replaces the defaults; callers such as the caide app must pass them.
Decision (user, 2026-11-14): `ONLINE_JUDGE` stays out of the default
`macrosToKeep`; pass `-k ONLINE_JUDGE` when needed.

## M3a — verification + fallback (done, not committed, 2026-11-14)

Codex wrote it, Opus reviewed. `src/Verifier.{h,cpp}`, `src/ErrorCollector.h`
(moved out of optimizer.cpp). After optimizing, `inlineCode` checks the final
text with the embedded Clang (syntax-only, optimizer's options) and, if
`verifyCommand` is set, compiles+links it with the judge (60 s). On failure it
writes the unoptimized stage-1 text and warns on stderr. API:
`CppInliner::verifyOutput` (default true), `CppInliner::verifyCommand`;
C: new `caideInlineCppCodeVerified` (old struct/function unchanged; the old
function now also runs the embedded check); CLI: `--verify-with "<cmd>"`,
`--no-verify`. Tests: `verify-fallback-judge-fails`,
`verify-fallback-judge-missing` (test-tool reads `verifyCommand.txt`).
`VerificationResult.diagnostics` carries file:line:col for M3b's repair loop.

Result on the Mac (LLVM 22.1.8): all 72 tests pass. Manual `cmd` check:
`--verify-with "g++ -std=c++23"` writes the optimized output silently;
`--verify-with false` warns and writes the unoptimized output, exit 0.

## M3b — repair loop (done, not committed, 2026-11-14)

Codex wrote it, Opus reviewed. On a failed check, `inlineCode` reruns the
optimizer on the same stage-1 file with `namesToRestore` (unqualified names →
extra roots in `DependenciesCollector::VisitNamedDecl`), up to 3 repair rounds.
Names = identifiers on candidate lines referenced by errors/notes (embedded
notes now collected; judge `file:line:col` lines), skipping comments and
literals, ∩ names of declarations the optimizer removed. No new name → stop and
fall back as in M3a. Success after repair prints a note with the restored
names. Macros are not restored. Test hook: `CAIDE_TESTING_FORCE_REMOVE`
(test-tool marker `forceRemove.txt`; `expectStderr.txt` checks stderr).
Tests: `repair-restores-function`, `repair-restores-type`,
`repair-restores-overloads`, `repair-gives-up`.

Result on the Mac (LLVM 22.1.8): builds cleanly, all 76 tests pass.

## M8 — runtime verification on sample inputs (done, 2026-11-14)

Codex wrote it, Opus reviewed. `CppInliner::verifyInputs`; CLI
`--verify-run <file|dir of *.in>` (repeatable, needs `--verify-with`). After
compile verification passes, `verifyBehaviour` (Verifier.cpp) builds the
original (judge command + `-I/-iquote/-D/-U` from the clang options) and the
candidate, runs both per input (10 s), compares exit status + stdout bytes;
first difference → unoptimized output, and the fallback is run too to tell a
stage-1/nondeterminism problem apart. Original not building → warn, skip.
C interface unchanged. Tests: `verify-run-passes`,
`verify-run-detects-difference` (force-removes a printing global).

## M9 — quick fixes (done, 2026-11-14)

Codex wrote it, Opus reviewed: C interface appends to the default
`macrosToKeep`; `cmd` creates its temp dir; `inlineCode` returns
`InlineOutcome` (`Optimized` / `Unoptimized` / `FellBack`), `cmd --strict`
exits 3 on `FellBack`, `caideInlineCppCodeVerified` returns 3 (the old C
function still returns 0); CMake checks each compiler flag with its own cached
result (no more `-Wlogical-op` on Clang); detection trial parses are silent.
test-tool marker `expectOutcome.txt` (added to five existing cases).


## M10 — repair restores macros (done, 2026-11-14)

Codex wrote it, Opus reviewed. `RemoveInactivePreprocessorBlocks` gets
`namesToRestore` (a restored macro's `#define`/`#undef` are never removed;
`#if` branch handling unchanged) and reports removed macro names into
`removedNames`. Test hook `CAIDE_TESTING_FORCE_REMOVE` also force-removes
macro definitions. Test: `repair-restores-macro`.

## M12 part 1 — Linux suite in Docker (applied, not yet run, 2026-11-14)

Codex wrote it, Opus reviewed (fix: `ARG JOBS` redeclared in the test stage;
build args don't cross stages). `docker build --target test` builds caide,
installs g++-16 as `g++`, and runs the whole suite with
`CAIDE_TEST_CXX="g++ -std=c++23"`; `ci/docker-test.sh` wraps it. All stages
build with `-j${JOBS}` (default 4). `.dockerignore` keeps local build output
out of the context. README: "Running the tests".

## M4 — keep/remove report (done, 2026-11-14)

Codex wrote it, Opus reviewed. `CppInliner::reportFile`, CLI `--report <path>`
(C interface unchanged). `src/Report.{h,cpp}`: header (outcome: optimized /
after N repair rounds / fell back + reason; counts), then one line per
main-file declaration: `kept|removed  <Kind> <qualified name>  <file>:<line>
<reason>` (root kind, `used by X (file:line)` from the BFS parent,
`unreachable`, `forced (testing hook)`), then removed macros. Locations point
into the temp `inlined.cpp` (stage 1 doesn't keep `#line`). Zero cost without
`--report`. test-tool marker `expectReport.txt` (6 cases).


## Check results 2026-11-14 (Mac, LLVM 22.1.8, fresh build)

- M9: no `-Wlogical-op` / `__float128` lines (count 0); CMake now reports
  `-Wlogical-op` unsupported. `cmd --strict --verify-with false` exits 3. ✔
- M4: report as expected for `sizeof`. ✔
- **M9 regression, fixed by Opus:** detection used `IgnoringDiagConsumer`,
  which doesn't count errors, so every trial parse "succeeded" and GCC 16's
  headers were accepted → 6 tests failed with `__float128` (stl,
  strip-comments-all, template-partial-spec-dependent,
  using-base-member-template, concept-constrained-ctad, concept-ctor-requires).
  Now uses a plain `DiagnosticConsumer` (counts, prints nothing). Recheck.
- M8 manual run failed only because the command lacked `-a` (no system
  include paths). Recheck.

## Check results 2026-11-14, second run (Mac, LLVM 22.1.8)

Detection fix ✔ (no `__float128` / `-Wlogical-op` lines), **79/79 tests
pass** (covers M8, M9, M10, M4 and the fix). `cmd -a --verify-with
--verify-run` exits 0, silent, `unused()` removed ✔. M8, M9, M10, M4 are done.
Leftover noise: `cmd -a` still prints "2 errors generated." twice: Clang's
own summary line from the failing GCC-16 detection trials (printed by
CompilerInstance when caret diagnostics are on). Fixed with M7: the trial
parses in `testOptions` get `-fno-caret-diagnostics`.

## Pending checks for the user (run on the Mac, paste the output)

Work continues without waiting for these; they're run whenever convenient.
Remove an entry once its result is recorded above.

1. **M12** (Docker, heavy; when the VM is idle):
   `ci/docker-test.sh 2>&1 | tail -30` → last line
   `Linux tests passed (LLVM 22, g++ 16, JOBS=4).`

2. **M7 + detection noise** (commit "Require Clang/LLVM 21…"):
   ```sh
   make -C build -j4 test-tool cmd 2>&1 | grep -E "error|too recent" | head; \
   (cd build && ctest --output-on-failure 2>&1 | grep -E "Failed|tests passed" | head -20); \
   build/cmd/cmd -std=c++17 -- -a -d tests/temp -o /tmp/m7-out.cpp tests/cases/verify-run-passes/1.cpp; \
   echo "m7 exit=$?"
   ```
   Expect: no output from the first grep (no compile errors; the "too recent"
   CMake warning is gone, if CMake reconfigures); 79/79 pass; `cmd -a` prints
   nothing (no "2 errors generated."), exit 0. Optionally also with LLVM 23:
   `cmake -S src -B build23 -DCAIDE_USE_SYSTEM_CLANG=ON -DClang_ROOT=/opt/homebrew/opt/llvm && make -C build23 -j4 test-tool && (cd build23 && ctest 2>&1 | tail -3)`.
