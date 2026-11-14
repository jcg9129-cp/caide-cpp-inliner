# Optimizer redesign — conservative pruning with bounded verification

Status: **proposal**, 2026-11-14; decisions in §7. Nothing here is implemented yet. Reviewed
adversarially by Codex (GPT) on 2026-11-14; its objections are folded in below.

## 1. Goal

Same functionality as today: turn a multi-file C++ program into one
self-contained `.cpp` (system headers stay as `#include`), with unused code
removed. The output must compile with the judge's compiler (usually g++) and
behave like the original.

What changes is the **correctness model**, not the language or the pipeline
shape. The current two-stage pipeline (inline → reparse and prune) stays; we
evolve the existing code rather than rewrite it.

## 2. What is wrong today

The optimizer deletes a declaration unless `DependenciesCollector` found an
explicit edge to it. Every construct whose dependency Clang does not expose as
a plain reference becomes broken output. `docs/optimizer-hardening-todo.md` is a
running list of such holes (deduction guides, constructor requires-clauses,
aggregate-initialized fields, namespace aliases, …). The list never ends,
because C++ keeps adding implicit dependencies.

Problems, with evidence:

| # | Problem | Evidence |
|---|---|---|
| P1 | Delete-by-default: a missed edge means wrong output | the hardening TODO |
| P2 | No check of the result: failures show up when the user compiles, or not at all (silent behaviour change) | — |
| P3 | Fields are removed one by one. `sizeof`, layout, structured bindings and type traits depend on fields nobody names. **Confirmed 2026-11-14** (`tests/cases/field-layout-sizeof`). | `OptimizerVisitor.cpp` `VisitFieldDecl`; no `sizeof` handling in `DependenciesCollector` |
| P4 | Unreferenced globals are removed even when their constructor or destructor has side effects (`Boot boot;`). This is deliberate; `/// caide keep` is the workaround. **Confirmed 2026-11-14** (`tests/cases/global-side-effect-ctor`). | comment in `OptimizerVisitor::VisitVarDecl` |
| P5 | Removal logic is duplicated per declaration kind, and edits are raw source ranges | `OptimizerVisitor` Visit\* methods |
| P6 | The preprocessing stage runs Clang's preprocessor. Blocks such as `#ifdef __clang__ … #else` may be resolved for Clang although the judge uses g++. **Not a bug in `cmd`** (corrected 2026-11-14): `__clang__`, `__GNUC__`, `_MSC_VER` etc. are in the default `macrosToKeep` (`caideInliner.cpp`), which `cmd -k` adds to. The M0 failure came from test-tool *replacing* the defaults; fixed there. The C interface also replaces them, so callers must pass the defaults. | `RemoveInactivePreprocessorBlocks.cpp` |
| P7 | Clang version checks scattered through the code | `CAIDE_CLANG_VERSION_AT_LEAST` usages |

Already handled correctly, kept as is: enums are kept whole
(`DependenciesCollector::VisitEnumDecl`), and using-directives and namespace
aliases have dedicated handlers.

## 3. Principles

1. **Delete in coarse, indivisible units.** Give up some output size to shrink
   the set of semantic and editing risks.
2. **When unsure, keep.** Uncertainty in the analysis must lead to keeping
   code. It must never lead to deleting it.
3. **Check, but don't overclaim.** A bounded check with g++ catches most
   breakage. It does **not** prove the output behaves the same; nothing cheap does.
4. **Always have a safe fallback.** The unoptimized flattened file is the
   fallback. It protects against optimizer mistakes only, not against mistakes
   in the flattening stage (see P6).
5. **Smallest safe output.** The goal is the smallest output that passes the
   checks; there is no "good enough" size threshold.

## 4. Design

### 4.1 Removable units

An optimizer edit removes a whole **unit**, never part of one. A unit is:

- a top-level declaration group (all declarators of `int a, b;`, `typedef A, B;`,
  `struct S {…} obj;`);
- a **record with its complete definition**: all fields, and all members needed
  for its layout and its special member functions. Unused ordinary member
  functions remain removable as sub-units (decided, §7);
- a **template family**: the primary template plus all partial
  specializations and deduction guides. Explicit (full) specializations stay
  separate: used ones are reached through template instantiations, and unused
  ones are removed (decided in M2 review);
- an enum (already the case);
- a whole macro invocation that produces declarations;
- using-directives, using-declarations and namespace aliases. They are units of
  their own, but keep their current dedicated rules, because lookup depends on
  them without any reference pointing at them.

Each unit records its source extent, the units that own it (out-of-line
definitions, hidden friends), and why it is kept.

### 4.2 Roots

Today only `main`, `/// caide keep` and `identifiersToKeep` are roots. Add:

- globals and static data members with **dynamic initialization or a
  non-trivial destructor** (fixes P4). This changes a deliberate past trade-off
  and may keep more code (decided, §7);
- anything with attributes that have external effects (`constructor`,
  `used`, `section`, …).

### 4.3 Dependency analysis

`DependenciesCollector` stays the main source of edges, with these changes:

- edges go between units, not individual declarations;
- in dependent (uninstantiated) code, where lookup is deferred, fall back to
  **keeping every same-named candidate in scope**. Use shared name buckets
  rather than one edge per pair, so common names like `size` or `begin` don't
  produce a quadratic number of edges. Measure how much each fallback keeps;
- optionally also walk instantiated ASTs, which show dependencies that
  templates hide. Keep walking the templates themselves too.
- Sema's `isUsed()`/`isReferenced()` count as extra evidence for keeping
  something. They are never a reason to delete.

### 4.4 Bounded verification and repair

After pruning:

1. Check the candidate with the embedded Clang (always). If the caller
   supplied a judge compiler command, also compile **and link** with it in the
   **target configuration**: judge compiler (g++), its `-std`, its standard
   library, its defines (§7, item 4).
2. On failure, map the diagnostics back to removed units using the error
   location and the token there, not English message text (details in §7,
   item 4). Restore the
   culprit unit **together with its dependency closure**. A restored unit is
   never removed again in this run. Then repeat.
3. Stop after a small number of rounds, a time budget or a memory budget. If
   the budget runs out, the compiler crashes, or an error can't be mapped back,
   emit the unoptimized flattened file.

Termination is guaranteed: each round restores at least one unit, so there are
at most N rounds. The result is not guaranteed to be minimal. Restoring code
doesn't always move towards a fix: with overloads `f(long)`, `f(double)`,
`f(int)`, restoring a subset can make `f(0)` compile with the wrong overload,
or become ambiguous. So no bisection that assumes it does.

This check catches "doesn't compile" bugs. It does not catch silent behaviour
changes. The coarse units (§4.1) and the extra roots (§4.2) cover those.

### 4.5 Optional finer semantic checks (later)

Added one at a time, each with tests and an explicit scope:

1. which specialization is chosen for each instantiation that survives;
2. the target of each call that survives;
3. results of concept checks, requires-expressions and `if constexpr` branches;
4. record layout (`sizeof`/`alignof`) of records that survive.

These need a **map from output tokens back to original source tokens**. USRs
alone won't do: for some entities they're built from the file name and byte
offset, and some declarations have none. Anything that can't be mapped back
counts as uncertain, so the unit is kept.

### 4.6 Editing engine

One shared engine applies unit removals: it handles overlapping ranges, macro
locations, cleanup of comments and whitespace, and namespace merging. The
syntax-aware rules per declaration kind stay, but they decide *which units* to
remove instead of editing text themselves. Namespace merging and other cleanup
passes run before verification, so the check covers them too.

### 4.7 Reporting

`-v`/debug output lists every unit with its fate and reason: "kept: root
(dynamic initializer)", "kept: dependent-name fallback `begin`", "restored after
g++ error at …". This makes output size and regressions explainable.

### 4.8 Clang versions

Support a bounded range (e.g. Clang 18–23). Move version differences into
`clang_compat.{h,cpp}`. Run the regression suite on each supported version.

## 5. Testing

- **Three-way comparison.** For each case compile and run (a) the original multi-file program,
  (b) the flattened unoptimized file, (c) the optimized file, all with g++ in
  the target configuration, and compare outputs. (a)↔(b) checks the inliner,
  (b)↔(c) the optimizer.
- **Corpus.** atl plus solved problems, and at least one other library, so the
  tests aren't tuned to a single library's habits.
- **Checker tests.** Make deliberately bad removals (a layout field, a
  side-effecting global, a constructor, a deduction guide, a partial
  specialization, a using-directive) and assert that a safeguard catches each.
- **Interaction cases.** Macros × templates, hidden friends × concepts, default
  arguments across redeclarations, globals × destructors.
- **Minimization.** Failures found in the corpus are reduced with a creduce-style
  tool into `tests/cases/`.

## 6. Milestones

Each one is independently useful and shippable.

| M | Work | Fixes |
|---|---|---|
| M0 | Regression cases for `sizeof`+unused field and side-effecting global; check `#ifdef __clang__` handling. Confirms or disproves P3, P4, P6. | — |
| M1 | Three-way comparison in `src/test-tool` | test gap |
| M2 | Keep records whole; treat template families as one unit; root globals with side effects | P3, P4, part of P1 |
| M3 | Bounded g++ verification + repair + fallback to unoptimized | P2, P1 |
| M4 | Keep/remove reason report | diagnosability |
| M5 | Unit-based editing engine | P5 |
| M6 | Finer semantic checks, one at a time (§4.5) | silent miscompiles |
| M7 | Bounded Clang range + compat layer | P7 |
| — | Optional: an "aggressive" mode with runtime differential testing | — |

## 7. Decisions

Decided 2026-11-14:

1. **Side-effecting globals:** rooted by default (§4.2). `/// caide keep` stays
   for anything else the user wants kept.
2. **Record granularity:** fields and special members are kept whole; unused
   ordinary member functions remain removable sub-units (§4.1).
3. **Size:** no size threshold; prune as far as the checks allow (principle 5).
4. **Checking compiler (§4.4).** The embedded Clang always checks the
   output. In addition the caller can pass a **judge compiler command**; when
   it is set, the candidate is also compiled and linked with it:
   - C++ API: `std::vector<std::string> verifyCommand;` on `CppInliner`,
     e.g. `{"g++", "-std=c++23", "-O2"}`. Empty (the default) means embedded
     Clang only. Mirrored in the C interface.
   - CLI (`src/cmd`): `--verify-with "g++ -std=c++23 -O2"`, split with
     `llvm::cl::TokenizeGNUCommandLine`.
   - The library appends `<temp>/candidate.cpp -o <temp>/candidate` (in the
     existing temporary directory) and runs it with
     `llvm::sys::ExecuteAndWait` under a timeout. That is portable and already
     available through LLVM.
   - Errors are read from the plain `file:line:col: error:` lines, which g++
     and Clang both print. The message text is not parsed. The location
     points at the *use* (the removed declaration is no longer in the file), so
     the repair takes the identifier token at that location in the candidate
     and restores removed units with that name. If there is no identifier there
     (e.g. a brace-init mismatch at `{`), it restores the removed parts of the
     records and templates used by the enclosing surviving unit. If that fails
     too, the unoptimized file is emitted.
   - If the command can't be run (not found, timeout), say so on stderr and
     emit the unoptimized flattened file. The output is never left unchecked
     without a warning.

## 8. Rejected

- **Rewrite in Rust.** Clang's full AST is C++-only; libclang leaves out the
  implicit detail this tool depends on.
- **Full equivalence checking via USR comparison** (my first proposal). It is
  unsound: removing a field, a side-effecting global or a deleted constructor
  changes behaviour while every reference still resolves the same way. USRs
  are also not stable IDs.
- **One generic "remove unreferenced declaration" rule.** Using-directives,
  declarations sharing one statement, macros and hidden friends don't fit it.
- **C++20 modules, LLVM IR comparison.** These don't produce a single portable
  source file or validate g++ behaviour.
- **"Under a second per verification round" via PCH.** Unsupported; to be measured, not
  promised.
