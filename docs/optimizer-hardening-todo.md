# Optimizer hardening — candidate TODO

Running catalog of *implicit-dependency* over-removal risks in the second-stage
optimizer, plus what has already been checked. Pick an item, verify it with the
recipe at the bottom, and only then implement.

## The one root cause

Every over-removal bug fixed so far has the same shape: the C++ program creates a
dependency between two declarations that Clang's AST does **not** expose as a plain
name reference, so `DependenciesCollector` never adds the graph edge — the target
looks unused and gets pruned while the construct that needs it stays. The risk area
is therefore *implicit / dependent / synthesized* references, concentrated around
templates, concepts, CTAD, and dependent base classes.

Guaranteed-safe fallback for any file that trips a new instance: `-n`
/`--no-optimize` (inline without removal).

## Already fixed this cycle (`src/DependenciesCollector.{h,cpp}`)

- **Concept named only as a template-parameter type-constraint** — `recordTemplateParameters`
  links each template to its parameters (also covers non-type parameters).
- **CTAD deduction guides pruned** — `VisitCXXDeductionGuideDecl` ties each guide to
  the class template it deduces.
- **`using base::member;` on a dependent base** — `VisitUnresolvedUsingValueDecl` +
  `baseTemplatePattern` resolve the name against the base's pattern.
- (earlier) nested-namespace merge corruption, `read_float`/`OverloadExpr`
  over-removal, `static_assert` in a braceless discarded branch, the invalid-brace
  crash.

## Verified handled — do NOT re-investigate

Each was reproduced as a minimal case (member reachable *only* through the implicit
path), inlined, compiled `-Wall`, and run for the correct value:

| Construct | Result |
|---|---|
| C++20 rewritten comparisons (`!=` from `==`, `<` from `<=>`) | kept, correct |
| `std::hash<UserType>` specialization used only via `unordered_set` | kept, correct |
| Class-template **partial** specialization selected implicitly | kept, correct (no silent miscompile) |
| User-defined literal operator (`operator""_x`) | kept, correct |
| Structured-binding customization (`tuple_size`/`tuple_element`/`get`) | kept, correct |
| Implicit conversion operator (`operator T()`) | kept, correct |
| Named concept used only in a function template's `requires`-clause | kept, correct |
| Concept constraining a **non-type** template parameter (`C auto N`) | kept, correct |
| CRTP: base template calls a derived method via a dependent expression | kept, correct |
| Template passed as a template *template* argument (`wrapper<my_seq>`) | kept, correct |
| `using enum E;` (C++20) with unqualified enumerators | kept, correct |
| Defaulted `operator<=>` / `operator==` (`= default`) + compared fields | kept, correct |
| Concept named only in a requires-expression return-type-requirement | kept, correct |
| `std::formatter<UserType>` specialization used via `std::format` | kept, correct |
| Variable-template partial specialization selected implicitly | kept, correct (no silent miscompile) |
| Explicit specialization of a class-template **member** function | kept, correct (no silent miscompile) |
| Coroutine `promise_type` referenced implicitly by a coroutine | kept, correct |

## Candidate TODO — swept 2026-08 (all clear except the one fixed)

The full candidate list below was verified with the recipe. Exactly one reproduced
(the grandparent `using`); the rest were already handled and have moved to the table
above.

- [x] **`using base::x;` where `x` lives in a *grandparent* dependent base.**
      FIXED: `lookupInBasesTransitively` walks the base hierarchy, and the
      using-declaration's qualifier type (the `using base = ...;` alias) is now kept
      too. Test: `tests/cases/using-base-grandparent`.
- [x] Template passed as a template template argument — already handled.
- [x] `using enum E;` (C++20) — already handled.
- [x] Defaulted `operator<=>` / `operator==` — already handled.
- [x] Concept in a requires-expression return-type-requirement — already handled.
- [x] `std::formatter` specialization via `std::format` — already handled.
- [x] Variable-template partial specialization — already handled.
- [x] Explicit specialization of a class-template member — already handled.
- [x] Coroutine `promise_type` — already handled.

No open candidates remain. New over-removal reports should be reproduced with the
recipe below and, if real, added here.

## How to verify a candidate

1. Write a minimal `main.cpp` where the suspect declaration is reachable **only**
   through the implicit path (call/use nothing else that names it).
2. Inline it:
   `cmd @clang.rsp -- -a -d out -o out/result.cpp -l 2 main.cpp`
3. `grep` the output to see whether the declaration survived.
4. `g++ -std=c++23 -O2 -Wall out/result.cpp -o out/sol` — must compile clean.
5. Run it and check the value — a construct can survive compilation yet be
   *miscompiled* if a partial specialization was dropped (silent-miscompile class).

If a candidate reproduces, the fix almost always belongs in `DependenciesCollector`
(add the missing edge), keyed on the *templated* decl where removal keys on it (see
the `getTemplatedDecl()` note in `VisitOverloadExpr` / `recordTemplateParameters`),
plus a regression case under `tests/cases/` registered in
`src/test-tool/CMakeLists.txt`.
