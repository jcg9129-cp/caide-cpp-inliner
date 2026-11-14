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
