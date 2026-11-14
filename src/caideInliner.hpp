//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <string>
#include <vector>

/// \file caideInliner.hpp
/// \brief C++ interface for caide inliner API


namespace caide {

/// \brief Outcome of a successful inlineCode call, after writing the output.
enum class InlineOutcome {
    Optimized,   ///< Optimized output was written, possibly after repair.
    Unoptimized, ///< Unoptimized output was requested (removeUnusedCode was false).
    FellBack     ///< Compile or runtime verification forced unoptimized output.
};

/// \brief C++ code inliner and unused code remover
///
/// The C++ inliner transforms a program implemented as multiple C++ source files
/// and headers into a single-file program. Only code reachable from main function
/// is kept in the output file.
///
/// Fairly complex programs are supported, including programs using template metaprogramming
/// and modern C++ features. That said, don't try to inline Boost headers.
///
/// \sa inlineCode()
class CppInliner {
public:
    /// \brief Create an instance of C++ inliner
    /// \param temporaryDirectory path to a directory where intermediate
    /// files will be written. The directory must exist.
    ///
    /// \sa clangCompilationOptions
    /// \sa macrosToKeep
    /// \sa maxConsequentEmptyLines
    explicit CppInliner(const std::string& temporaryDirectory);


    /// \brief Generate a single-file C++ program equivalent to the multiple-file program.
    /// \param cppFilePaths full paths of all C++ files of a program
    /// \param outputFilePath path to a file where the inlined program will be written
    /// \return Whether optimized, requested unoptimized, or fallback output was written.
    ///
    /// All input C++ files and included user headers will be combined into a single C++ file,
    /// and only code reachable from main function will be kept. In addition, declarations
    /// marked with a comment 'caide keep' will be kept too.
    InlineOutcome inlineCode(const std::vector<std::string>& cppFilePaths,
                             const std::string& outputFilePath) const;

    /// \brief Try to detect system include paths automatically and adjust
    /// clangCompilationOptions accordingly.
    ///
    /// \sa clangCompilationOptions
    void autoDetectCompilationOptions();

    /// \brief clang compilation options (see http://clang.llvm.org/docs/CommandGuide/clang.html
    /// and http://clang.llvm.org/docs/UsersManual.html)
    ///
    /// Most commonly used options are:
    ///
    /// \li `-I` to setup include search paths
    /// \li `-isystem` to setup system include search paths. In particular, you will likely need
    ///     to add builtin clang includes as a system path. See this link:
    ///     http://clang.llvm.org/docs/LibTooling.html#libtooling-builtin-includes
    /// \li `-D` for custom symbol definitions
    /// \li `-std=c++11` to specify a version of C++ standard
    /// \li `-fparse-all-comments' to take into account non-documentation comments
    /// \li `-v` for verbose output
    ///
    /// Other options you may need if they are not determined automatically (e.g. if
    /// the library was built in MinGW but you want to use Visual Studio headers, or if
    /// the library was built in a different Linux distribution):
    ///
    /// \li `-target` to specify compilation target (e.g., i386-pc-mingw32, i386-pc-windows-msvc,
    ///     i386-pc-linux)
    /// \li `-fmsc-version` to specify a version of Microsoft compiler installed in the system
    ///
    /// Default value is empty.
    std::vector<std::string> clangCompilationOptions;


    /// \brief the list of macro definitions whose values should be assumed unknown
    ///
    /// The inliner will remove inactive preprocessor blocks. Sometimes it is
    /// not desirable. For example, if you have a Linux-specific code path
    /// and a Windows-specific code path, you want to always keep both of them.
    /// This parameter contains macro names, references to which in an
    /// #if or #ifdef condition will protect the corresponding preprocessor branches
    /// from being removed.
    ///
    /// Default value is a list of common OS-specific and compiler-specific definitions.
    /// Add callers' macros to these defaults, as cmd -k and the C interface do.
    ///
    /// \note The reference is checked by substring search. So, if this parameter
    /// contains a string "_WIN32" and a preprocessor condition is `#ifdef HAVE_WIN32`
    /// or `#if defined(_WIN32) || defined(_WIN64)`, then preprocessor branches
    /// following this condition will not be removed.
    ///
    /// \note Unused code inside an inactive preprocessor block is not removed.
    std::vector<std::string> macrosToKeep;


    /// \brief limit on the number of consecutive empty lines in the output file
    ///
    /// When unused code is removed, it leaves behind large blocks of empty lines.
    /// This parameter controls the number of consequent empty lines that is kept
    /// in the output file.
    ///
    /// Default value is 2. If the parameter is negative, empty lines are not removed.
    int maxConsequentEmptyLines;

    /// \brief List of fully qualified names to preserve in the output, even if
    /// they are not reachable from main function
    ///
    /// Normally, only code reachable from main or marked by special comments
    /// is preserved. This settings provides additional identifiers to preserve.
    /// Identifiers must be fully qualified, e.g. "NamespaceName::ClassName::method".
    std::vector<std::string> identifiersToKeep;

    /// \brief Whether to remove code unreachable from main (the second stage).
    ///
    /// When true (the default), declarations not reachable from main are removed.
    /// When false, only the first stage runs: all source files and user headers
    /// are combined into a single file, but nothing is removed. This is useful
    /// when you only want a self-contained file, or to avoid any risk of the
    /// reachability analysis removing something the output still needs.
    ///
    /// Default value is true.
    bool removeUnusedCode;

    /// \brief Whether to remove all comments from the output.
    ///
    /// When true, every // and /* */ comment is stripped from the final file.
    /// String and raw-string literals are left untouched. This is independent of
    /// removeUnusedCode, so it also applies when only inlining is performed.
    ///
    /// Default value is false.
    bool removeComments;

    /// \brief Whether to verify optimized output and fall back on failure.
    ///
    /// The embedded Clang checks the final output with the optimizer's options.
    /// If verification fails, the unoptimized inlined program is written instead
    /// and a warning is printed. Used only when removeUnusedCode is true.
    ///
    /// Default value is true.
    bool verifyOutput;

    /// \brief Optional judge compiler command for output verification.
    ///
    /// After the embedded check passes, compile and link with this command,
    /// for example {"g++", "-std=c++23", "-O2"}. The candidate source path and
    /// -o executable path are appended. Used only when verifyOutput is true.
    ///
    /// Default value is empty (embedded Clang check only).
    std::vector<std::string> verifyCommand;

    /// \brief Optional sample input paths for runtime output verification.
    ///
    /// After compile verification passes, build the original and optimized
    /// programs with verifyCommand and compare exit status and stdout on each
    /// input, with a 10-second timeout per run. A difference writes the
    /// unoptimized output. Programs depending on time or randomness may always
    /// fall back. Used only when removeUnusedCode and verifyOutput are true
    /// and verifyCommand is non-empty.
    ///
    /// Default value is empty (no runtime checks).
    std::vector<std::string> verifyInputs;

private:
    const std::string temporaryDirectory;
};

} // namespace caide

