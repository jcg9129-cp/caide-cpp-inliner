# Clang 22 / 23 support — migration runbook

**Status: Clang 22 DONE and verified; Clang 23 pending toolchain availability.**
Branch `clang-22-23-support` now builds against clang 22.1.8 (system mode) and
passes the full suite (48/48, incl. 5 new C++23/26 cases). The source changes are
committed and verified — no longer speculative. Clang 23 was not attempted because
no clang-23 toolchain exists in this environment; the recipe below applies verbatim
when it does.

## Executed against Clang 22 — actual results

The toolchain was obtained without root by downloading the `llvm-22-dev` /
`libclang-22-dev` / `libclang-cpp22-dev` `.deb`s from `apt.llvm.org` and extracting
them (`dpkg -x`) into a user-writable prefix merged (symlink farm) with the
read-only system `/usr/lib/llvm-22` install. `-DClang_ROOT=<prefix>` +
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` then configured cleanly.

**Actual Clang 22 API deltas found (4, all in 2 files):**
1. `RecursiveASTVisitor::Traverse<X>Type` / `Traverse<X>TypeLoc` gained a
   `bool TraverseQualifier` parameter → extended the 3 overrides in
   `DependenciesCollector` (commit "Support building against Clang 22").
2. `AutoType::getTypeConstraintConcept()` returns `TemplateDecl*` (was `ConceptDecl*`).
3. `Sema::SubstDefaultTemplateArgumentIfAvailable()` gained a `TemplateKWLoc` param.
4. `Sema::InstantiatingTemplate(ClassTemplatePartialSpecializationDecl*)` dropped its
   trailing `TemplateDeductionInfo&` param.

**Prediction accuracy:** R2/R3 (the Sema constraint/deduction cluster in
`sema_utils.cpp`) were correctly flagged as high-risk and did break. R1
(`PPCallbacks::InclusionDirective`) did **not** break at 22 — its signature was
unchanged. The `RecursiveASTVisitor` Traverse-signature change (#1 above) was the
biggest surprise and was **not** on the pre-flight risk list; note it for future
bumps. No test rebaselines were needed (zero output drift); no semantic regressions
(zero `DependenciesCollector` reachability bugs on the existing suite).

**Also needed (environment, not a Clang delta):** the project's
`cmake_minimum_required(2.8.12)` is rejected by CMake ≥ 4. **Fixed** by bumping it
to `cmake_minimum_required(VERSION 3.5...4.0)` in `src/CMakeLists.txt` (works on
CI's CMake 3.20 through CMake 4.x; no `-DCMAKE_POLICY_VERSION_MINIMUM` flag needed
anymore). The tool's own `-std=c++17` (LLVM≥16 branch in `src/CMakeLists.txt`)
still consumes LLVM 22 headers fine — no bump required.

---

## Original worklist (retained for the Clang 23 pass)

Everything below was the pre-flight static analysis. It remains the procedure for
the next bump (23). Where it said "cannot be verified without a toolchain", that no
longer applies for 22 — see the results above.

Locked scope (confirmed): targets **Clang 22 + 23**, **system-clang only**
(`-DCAIDE_USE_SYSTEM_CLANG=ON`; bundled `src/llvm-project` submodule untouched),
with a **broad C++26 test sweep**.

---

## 0. Environment requirement (why this wasn't executed in place)

Building requires the LLVM/Clang **development** packages, which provide the C++
headers (`clang/AST/*.h`, `llvm/ADT/*.h`), the static libs (`libclangTooling.a`,
…), `LLVMConfig.cmake`, and `llvm-config`. A machine with only the clang *runtime*
+ tools cannot configure the project — `find_package(Clang REQUIRED CONFIG)` at
`src/CMakeLists.txt:18` fails because `ClangConfig.cmake` immediately does
`find_package(LLVM ... REQUIRED CONFIG)` and `LLVMConfig.cmake` is absent.

On a Debian/Ubuntu host, install (as root):

```
apt-get install -y clang-22 libclang-22-dev llvm-22-dev
# and for clang 23 once released:
apt-get install -y clang-23 libclang-23-dev llvm-23-dev
```

Verify before starting: `ls /usr/lib/llvm-22/lib/cmake/llvm/LLVMConfig.cmake` and
`/usr/lib/llvm-22/lib/libclangTooling.a` must both exist.

---

## 1. How the version shim works (so edits are mechanical)

`src/clang_version.h` defines the only mechanism:

```c
#define CAIDE_CLANG_VERSION_AT_LEAST(major, minor) \
    ((major) < CLANG_VERSION_MAJOR || \
    (major) == CLANG_VERSION_MAJOR && (minor) <= CLANG_VERSION_MINOR)
```

It reads `CLANG_VERSION_MAJOR/MINOR` from `<clang/Basic/Version.h>`. **Do not edit
this macro.** Supporting a new Clang means adding new
`#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0) … #else … #endif` branches *next to* the
existing 19/20/21 branches at each site the API changed. All edits are additive —
never modify or delete an existing older-version branch (that would break the
11–21 support the CI matrix still tests).

There are ~60 guard sites across 14 files. Full inventory in §5.

---

## 2. Turnkey procedure (run on a toolchain-equipped machine)

```bash
# --- Phase 1: compile against clang-22, fix API deltas ---
git checkout clang-22-23-support
mkdir -p build && cd build
cmake -G Ninja -DCAIDE_USE_SYSTEM_CLANG=ON -DCMAKE_BUILD_TYPE=Release \
      -DClang_ROOT=/usr/lib/llvm-22 ../src
ninja 2>&1 | tee ../build-22.log
# For each compile error: locate the API, wrap the new form in
#   #if CAIDE_CLANG_VERSION_AT_LEAST(22, 0) ... #else ... #endif
# Prioritise the high-risk sites in §3. Re-run ninja until clean.

# --- Phase 2: run the existing suite ---
ctest --output-on-failure 2>&1 | tee ../ctest-22.log
# Triage each failure per §4 (output-drift vs semantic-regression).

# --- Phase 3: broad C++26 sweep ---
# Add the cases in §6; regenerate expected outputs with the built tool and
# eyeball each for correctness before committing (see §6 "generating expected").

# --- Regression guard ---
# Rebuild+ctest against clang-21 and clang-20 (-DClang_ROOT=/usr/lib/llvm-21|20)
# to confirm the additive branches didn't disturb older versions.

# --- Repeat Phase 1-2 for clang-23 once available ---
```

---

## 3. High-risk sites, ranked (fix these first)

Ranked by churn history and blast radius. Line numbers are on
`clang-22-23-support`.

### R1 — `PPCallbacks::InclusionDirective` signature (highest risk)
Overridden in **two** places that must stay in lockstep:
- `src/inliner.cpp:64` (`TrackMacro::InclusionDirective`, marked `override`)
- `src/RemoveInactivePreprocessorBlocks.cpp:390`

This signature changed at Clang 7 (`CharacteristicKind`), 15 (`Optional<FileEntryRef>`),
16 (`OptionalFileEntryRef`), and 19 (added `bool ModuleImported`). It is the single
most fragile spot. If Clang 22 changes it again (e.g. the `const Module*
SuggestedModule` parameter, which upstream has been reworking around modules), both
overrides will **fail to compile** rather than silently no-op — **verified**: both
declarations carry `override` (`inliner.cpp:85` and
`RemoveInactivePreprocessorBlocks.h:91`). Good: a signature drift is a hard compile
error at both sites, not a silent "inactive `#if` blocks not removed = wrong
output" bug. Just extend both in lockstep.

Pattern to extend (both sites), append before the closing `)`:
```c
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
    /* new/changed parameter(s) as clang 22 declares them */
#endif
```

### R2 — Sema constraint API (`getAssociatedConstraints`)
`src/sema_utils.cpp:84-100` (`getConstraintExprs`). Already has a 16.0 vs 21.0
split (21 switched to `AssociatedConstraint` + `ConstraintExpr` member). Constraint
handling is the most actively churning Clang area for C++20/26 concepts — the
likeliest place for a 22/23 delta. Watch `getAssociatedConstraints` overload shape
and the `AssociatedConstraint` struct layout.

### R3 — Template-argument deduction
`src/sema_utils.cpp:105-130` (`getInjectedTemplateArgs`, `deduceTemplateArguments`,
gated `>= 19.1`) and `:224` (`substituteTemplateArguments`). These call
`Sema::DeduceTemplateArguments`, `ASTContext::getInjectedTemplateArg`,
`MultiLevelTemplateArgumentList::addOuterTemplateArguments`, `Sema::SubstType`,
`Sema::InstantiatingTemplate`. Signatures here moved at 19.1; re-verify each against
22 headers.

### R4 — `clang_compat.{h,cpp}` shim points
Small, self-contained, easy to extend:
- `TemplateSpecializationType::template_arguments()` (13.0 branch, `clang_compat.cpp:72-86`)
- `dyn_cast_if_present` availability (15.0/16.0, `clang_compat.h:38-52`)
- decl/comment `getBeginLoc/getEndLoc` (8.0 — stable, low risk)

### R5 — Preprocessor & tooling misc
- `src/RemoveInactivePreprocessorBlocks.cpp:406` / `.h:85` — 19.0 PP callback params
- `src/inliner.cpp:294` (17.0), `:79` (19.0)
- `src/util.cpp:228` (11.0), diagnostic/`FixedCompilationDatabase` helpers
- `src/optimizer.cpp:213` (12.0), `:278` (10.0)

---

## 4. Test-failure triage (Phase 2)

Every `ctest` failure is one of two kinds — handle them differently:

- **(a) Output drift** — the inlined result is semantically fine but formatting
  differs (whitespace, a printed qualifier, canonical-type spelling). Fix = update
  the expected file in `tests/cases/<name>/`. Legitimate rebaseline.
- **(b) Semantic regression** — a case is now **under-inlined** (kept dead code) or
  **over-inlined** (dropped reachable code). This is a real bug: a new/changed AST
  node isn't being walked, so a dependency edge in `SourceInfo::uses` is missing.
  Fix = add the `Visit*`/`Traverse*` handling in `src/DependenciesCollector.cpp`
  (and possibly removal logic in `src/OptimizerVisitor.cpp`). **Never** rebaseline a
  (b) failure — that would bless incorrect output.

Distinguish them by diffing: if the diff is whitespace/spelling → (a); if whole
declarations appear/disappear → (b).

---

## 5. Full guard-site inventory (append-point map)

Count per file (sites that may need a `(22,0)`/`(23,0)` sibling):

| File | sites | notable thresholds |
|---|---|---|
| `src/inliner.cpp` | 10 | 15,16,19 (InclusionDirective), 17 |
| `src/RemoveInactivePreprocessorBlocks.cpp` | 9 | 15,16,19 |
| `src/RemoveInactivePreprocessorBlocks.h` | 8 | 15,16,19 |
| `src/clang_compat.cpp` | 8 | 8,13 |
| `src/sema_utils.cpp` | 5 | **16, 19.1, 21** (leading edge) |
| `src/util.cpp` | 5 | 7,11 |
| `src/DependenciesCollector.cpp` | 5 | 10,13 |
| `src/clang_compat.h` | 2 | 15,16 |
| `src/DependenciesCollector.h` | 2 | 10 |
| `src/optimizer.cpp` | 2 | 10,12 |
| `src/OptimizerVisitor.{cpp,h}` | 2 | 10 |
| `src/detect_options.cpp` | 1 | 10 |

The highest existing thresholds (21, then 19.1, then 19) mark the leading edge —
those are the sites most likely to need new 22/23 branches. Everything ≤ 16 has
been stable for many releases and rarely needs touching, but will surface at build
time if it does.

---

## 6. Broad C++26 test sweep (Phase 3)

Each case is a directory under `tests/cases/<name>/` plus registration in the
`test_list` in `src/test-tool/CMakeLists.txt:17`. Follow the structure of an
existing case (e.g. `tests/cases/concepts-constraints/`) — input program(s) +
expected inlined output.

**Generating expected output (must be tool-generated, never hand-written):**
run the built `cmd/cmd` (or `test-tool`) on the case input with the appropriate
`-std=c++2c` flag, then **manually review** the produced output for correctness
(compiles standalone; no reachable code dropped; no dead code kept) before saving
it as the expected file. An unreviewed generated file just re-blesses whatever the
tool currently does.

### Candidate feature matrix
Prioritise features that create **new dependency edges** the collector must chase
(these are where correctness is actually at risk), then parsing-only features.

Dependency-relevant (higher priority — likely `DependenciesCollector` work):
- C++20/23 concepts & requires-clauses in more positions (builds on
  `concepts-constraints`, `caide-concept-comment`)
- Deducing `this` (explicit object parameter) — member lookup through the object param
- `if consteval` / `consteval` propagation
- Static `operator()` and static `operator[]`
- Multidimensional `operator[]`
- Pack indexing (`T...[N]`)
- `auto(x)` decay-copy and expanded CTAD
- `constexpr`/`consteval` usage pulling in otherwise-unreferenced helpers
- Structured-binding packs
- User-defined `static_assert` messages (`constexpr` string producers)

Parsing/formatting-oriented (mostly output-drift checks):
- `#embed`
- Attributes on more entities; `[[assume]]`
- `= delete("reason")`
- Named universal character escapes
- `constexpr` cast/`std::bit_cast` edge cases

For each: create `tests/cases/cpp26-<feature>/`, add input, generate+review
expected, append the name to `test_list`.

---

## 7. CI + docs drafts (apply when Phase 1 builds green)

**Do not apply these until the code actually compiles against the target** — a red
CI job for an unbuilt clang, or a job for an unreleased clang-23, is noise.

### `.gitlab-ci.yml` (line 18) — add 22 (then 23 when released)
```diff
-        CAIDE_CLANG_VERSION: [11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21]
+        CAIDE_CLANG_VERSION: [11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]
```
`ci/gitlab-script.sh` needs no change — it installs `clang-$CAIDE_CLANG_VERSION`
via `apt.llvm.org/llvm.sh` generically. Confirm `llvm.sh` offers 22 on the CI base
image (`debian:bullseye`); if the pinned CMake 3.20.1 is too old for LLVM 22's
CMake package, bump the CMake download in `ci/gitlab-script.sh:19` too.

### `README.md` (~line 115) — refresh the example version
```diff
-  (in Linux, search for packages clang-19 and libclang-19-dev or similar).
+  (in Linux, search for packages clang-22 and libclang-22-dev or similar).
```

### `src/CMakeLists.txt` (108-116) — watch the tool's own `-std`
Currently caps the tool's build standard at `c++17` for `LLVM >= 16`. If LLVM 22/23
raises the C++ standard required to *consume* its headers (LLVM has been trending
toward C++20), add an `elseif(LLVM_PACKAGE_VERSION VERSION_LESS "NN")` branch
selecting `c++20`. Only needed if the build errors on this.

---

## 8. Definition of done
- `ninja` clean against clang-22 (and clang-23 when available), system mode.
- `ctest` green: all existing cases + new C++26 cases.
- clang-21 and clang-20 still build+pass (additive-change regression guard).
- CI matrix includes 22 (23 when released); README refreshed.
- No existing older-version guard branch modified or removed.
