# Caide C++ inliner

[![Build
status](https://gitlab.com/slycelote/caide-cpp-inliner/badges/master/pipeline.svg)](https://gitlab.com/slycelote/caide-cpp-inliner/-/pipelines)

*Caide C++ inliner* transforms a C++ program consisting of multiple source
files and headers into a single self-contained source file without any
external dependencies (except for standard system headers). Unused code is not
included in the resulting file.

This project is available at the following URLs:

* [Codeberg](https://codeberg.org/slycelote/caide-cpp-inliner)
* [GitHub mirror](https://github.com/slycelote/caide-cpp-inliner)

## Demo

The directory [doc/demo](./../../tree/master/doc/demo) contains a small sample
application. We will write a program using caideInliner library that processes
the source code of this application:


    #include "caideInliner.hpp"
    #include <iostream>
    #include <stdexcept>
    #include <string>
    #include <vector>

    int main() {
        using namespace std;
        try {
            // (1)
            const string tempDirectory = "/tmp";
            caide::CppInliner inliner(tempDirectory);
            // (2)
            inliner.clangCompilationOptions.push_back("-std=c++11");
            inliner.clangCompilationOptions.push_back("-I");
            inliner.clangCompilationOptions.push_back(".");
            // (3)
            inliner.clangCompilationOptions.push_back("-isystem");
    #ifdef _WIN32
            inliner.clangCompilationOptions.push_back("C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.10150.0\\ucrt");
    #else
            inliner.clangCompilationOptions.push_back("../../src/llvm-project/clang/lib/Headers");
    #endif

            // (4)
            vector<string> files;
            files.push_back("main.cpp");
            files.push_back("ascii_graphics.cpp");

            // (5)
            inliner.inlineCode(files, "result.cpp");
        } catch (const exception& e) {
            cerr << e.what() << endl;
            return 1;
        }
        return 0;
    }


The above code, after including the [caideInliner
header](src/caideInliner.hpp):

1. specifies a directory for temporary files (it must already exist)
2. sets up compilation options (they may vary depending on the application)
3. adds additional directories containing system header files. It's either a
   hardcoded path for VS 2015, or a path to built-in clang headers (available
   in clang source tree included in this repository). You may need to tweak
   these options further depending on your system.
4. lists all c++ source files of the application.
5. finally, runs the inliner and outputs the result in a single c++ file.

Compile and run this code. (You will need to link against caideInliner library
and several clang and LLVM static libraries.) Alternatively, a [simple
command-line utility](./../../tree/master/src/cmd) is included that you can
run like this: `./cmd -std=c++11 -I . -isystem ../../src/clang/lib/Headers/ --
main.cpp ascii_graphics.cpp`.

Inspect the resulting code in the file `result.cpp`. Note how unused parts of
the code (e.g. one of the constructors of `CommandLineParserException` or
inactive preprocessor blocks from `main.cpp`) were eliminated.

Now add a preprocessor definition `OPT_FRAME`:

            inliner.clangCompilationOptions.push_back("-DOPT_FRAME");

and rerun the program. Observe how the output file changes accordingly.


## Build

clang library is a required dependency. You can either build it from scratch,
or use a version installed in your system.

* To build from scratch:

        git submodule update --init --recursive
        mkdir build
        cd build
        cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ../src


Replace the last argument with the full path to [src](./../../tree/master/src)
directory. Replace "Unix Makefiles" with "Visual Studio 16 2019" or another
[generator](https://cmake.org/cmake/help/latest/manual/cmake-generators.7.html)
suitable for your platform. Refer to [cmake
documentation](https://cmake.org/cmake/help/latest/manual/cmake.1.html) for
details.

Once build files are generated, build as usual (run `make`, open the VS
solution etc.)

* To use clang libraries from your system you need to first install them
  (in Linux, search for packages clang-22 and libclang-22-dev or similar).


        mkdir build
        cd build
        cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release -DCAIDE_USE_SYSTEM_CLANG=ON ../src


## Running the tests

From the repository root, configure a native build with a judge compiler to
compile and run each test's output as well as checking the inliner:

```sh
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release \
    -DCAIDE_USE_SYSTEM_CLANG=ON -DCAIDE_TEST_CXX="g++ -std=c++23"
cmake --build build --target test-tool --parallel 4
ctest --test-dir build --output-on-failure
```

Choose an installed judge compiler in `CAIDE_TEST_CXX`; add
`-DClang_ROOT=/path/to/llvm` if CMake cannot find your Clang installation.

To build caide and run the whole suite on Linux with g++ in Docker:

```sh
ci/docker-test.sh
```

This runs `docker build --target test` and fails if any test fails. Defaults are
LLVM 22, g++ 16, and 4 build jobs; override them with environment variables, for
example `LLVM_VERSION=22 GXX_VERSION=16 JOBS=2 ci/docker-test.sh`.
The `gcc-autodetect` and `gcc-bits-stdc++` tests run only on Linux and are skipped
on macOS.


## Command-line utility

The [command-line utility](./../../tree/master/src/cmd) `cmd` runs the inliner
without writing any code. Everything before `--` is passed to clang as
compilation options; everything after `--` is the tool's own options and the
input source files:

        ./cmd [clang options...] [@response-file ...] -- [options] <source files...>

Options after `--`:

* `-d <dir>` — temporary directory for intermediate files (default `./caide-tmp`,
  which must already exist)
* `-o <file>` — output file (default `./caide-tmp/result.cpp`)
* `-k <macro>` — keep preprocessor branches guarded by `<macro>` (repeatable)
* `-i <name>` — keep the fully-qualified declaration `<name>` even if it is
  unreachable from `main` (repeatable)
* `-l <n>` — collapse runs of blank lines left by removed code to at most `<n>`;
  a negative value disables removal (default 2)
* `-a`, `--auto-detect` — auto-detect system include paths before applying the
  clang options (see below)
* `-n`, `--no-optimize` — only combine the sources into a single file; do **not**
  remove code unreachable from `main` (see below)
* `-c`, `--strip-comments` — remove all comments from the output (see below)
* `-h`, `--help` — print usage and exit

An argument of the form `@FILE` (before `--`) reads additional clang options,
one per line, from `FILE`. On error, `cmd` prints the message to stderr and
exits with a non-zero status.

### Inlining without removing code

By default the tool runs in two stages: it first combines all sources and user
headers into one file, then removes everything unreachable from `main`. Pass
`-n` / `--no-optimize` to run only the first stage — you get a self-contained
single file with **nothing removed**.

This is useful when you only want a merged file, or to sidestep the dead-code
analysis entirely: because it never deletes a declaration, it cannot
accidentally drop something the output still needs (for example a member
function named only in an `if constexpr` branch that is never instantiated).
In this mode `-k`, `-i` and the reachability analysis have no effect.

### Removing comments

Pass `-c` / `--strip-comments` to delete every `//` and `/* … */` comment from
the output. String and raw-string literals are left untouched, so `"http://…"`
or `R"(/* not a comment */)"` survive. It is independent of `-n`, so
`-n -c` produces a merged, comment-free file with no code removed.

### Choosing a C++ standard

The standard is whatever you pass via `-std=...` before `--` (e.g. `-std=c++17`
or `-std=c++2c`); the tool forwards it to clang verbatim and does not restrict
it. Two things to keep in mind:

* The value must be recognized by the clang version the tool was built against,
  and that clang must implement enough of the feature to *parse* your code.
  Unimplemented features surface as ordinary compilation errors (on stderr, with
  a non-zero exit) — never silently.
* Library features (for example `<print>`) additionally require standard-library
  headers that provide them, which depends on the installed libstdc++/libc++, not
  on `-std`.

### Match the analysis flags to your build

The tool is not a text processor: it parses your program with a real C++
front-end and deletes whatever is unreachable from `main`. That reachability
depends on the exact preprocessor configuration it parses under, because macro
definitions decide which code is even visible. **So the flags you pass before
`--` (`-std`, `-D…`, `-I…`) must match the flags you later use to compile the
result.** If they differ, the tool prunes one version of the program while you
build a different one, and the output can reference something that was removed.

For example, suppose your code contains:

        #ifdef ASSERT
            check(index < size());   // the only use of size()
        #endif

If you inline **without** `-DASSERT` but compile the result **with** `-DASSERT`,
the tool parses the branch as absent, concludes `size()` is unused, deletes it —
and then your build turns the branch back on and references the missing
`size()`. Analyzing with `-DASSERT` keeps `size()`.

Note that `-k` does **not** do this. `-k MACRO` only preserves `#ifdef MACRO`
blocks in the output *text*; it does not define the macro for the analysis.
Preserving both branches (`-k`) and choosing which branch is live for pruning
(`-D`) are separate things:

        # analysis flags (before --) mirror the g++ flags used on the result
        ./cmd -std=c++17 -DASSERT -I./include -- -a -o result.cpp solution.cpp
        g++ -std=c++17 -DASSERT -I./include result.cpp -o solution

If you produce more than one build from the same source (say a local build with
`-DDEBUG` and a submission build without it), inline once per configuration, each
with flags matching that build — a single pruned file cannot be correct for two
different macro configurations at once. Alternatively, `-n` / `--no-optimize`
skips pruning entirely and sidesteps the whole issue.

### Inlining code written for GCC

The inliner always parses with clang, but it is normally pointed at GCC's
standard library, so code written for g++ works — including the libstdc++-only
`<bits/stdc++.h>` and compiler extensions such as `__int128` and `__builtin_*`.
The simplest approach is `-a`, which runs `g++` to discover its include search
paths and passes them to clang:

        ./cmd -std=c++17 -- -a -o result.cpp solution.cpp

To target a particular GCC version, ensure the matching `g++` (or
`g++-<version>`) is installed; `-a` prefers `g++`. Alternatively, list the
include directories yourself with `-isystem` before `--`. System headers are
preserved verbatim in the output, so the single-file result still compiles with
g++.


## Docker

A [Dockerfile](Dockerfile) is included that builds the [command-line
utility](./../../tree/master/src/cmd) `cmd` directly from this source tree — no
cloning required. It is a multi-stage build: the final image contains only the
`cmd` binary and the clang/LLVM runtime it needs, not the build toolchain.

Build the image from the repository root:

        docker build -t caide-inliner .

The clang/LLVM version is pinned through a build argument. Override it with, for
example, `--build-arg LLVM_VERSION=21` (Clang 11 through 22 are supported).

The image's entry point is `cmd`, so its arguments are passed directly. As on
the command line, everything before `--` goes to clang, while source files and
output options (`-o`, `-d`, ...) come after. Mount the directory holding your
sources so the tool can read them and write the result back:

        docker run --rm -v "$PWD":/data caide-inliner \
            -std=c++17 -I /data -- -o /data/out.cpp /data/main.cpp

Instead of listing `-isystem` directories yourself, you can pass `-a` (after
`--`) to auto-detect the system include paths inside the container:

        docker run --rm -v "$PWD":/data caide-inliner \
            -std=c++17 -I /data -- -a -o /data/out.cpp /data/main.cpp

Run `docker run --rm caide-inliner --help` for the full list of options,
including `-i <name>` to keep a declaration that is otherwise unused.


## Documentation

Refer to Doxygen comments in [caideInliner header](src/caideInliner.hpp).


## Known issues

* Non-standard features, such as defining a function without return type, may
  be unsupported.
* Defining a class and a variable of the same class simultaneously (`struct A
  {} a;`) is not supported.
* Unused global variables are removed. This is fine, unless you use such a
  variable for side effects. In that case, mark the variable with a comment
  `/// caide keep` or `/** caide keep **/` (note the triple slash and the
  double asterisk). For example,

        struct SideEffect {...};
        /// caide keep
        SideEffect instance;

  In general, if you find that a declaration is removed incorrectly, mark this
  declaration with `caide keep` (and file an issue :)).

