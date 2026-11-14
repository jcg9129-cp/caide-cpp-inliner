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
