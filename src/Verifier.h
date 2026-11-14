//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace caide {
namespace internal {

struct VerificationDiagnostic {
    std::string file;
    unsigned line = 0;
    unsigned column = 0;
    std::string message;
};

struct VerificationResult {
    bool passed = false;
    bool judgeFailed = false;
    std::string reason;
    std::vector<VerificationDiagnostic> diagnostics;
    // Canonical absolute path, resolved along with diagnostic paths before the
    // temporary candidate is deleted (including macOS /var symlinks).
    std::string candidatePath;
    // Rendered embedded errors or the judge's stdout/stderr, for reporting.
    std::string output;
};

// Check one candidate, without modifying it. Locations refer to this candidate
// (or its system headers), so repair can use them before retrying.
// Temporary files are removed before returning, including on exceptions.
VerificationResult verifyCode(const std::string& code,
    const std::vector<std::string>& compilationOptions,
    const std::vector<std::string>& judgeCommand,
    const std::string& temporaryDirectory);

// Check the fallback with the judge alone, to distinguish optimizer failures
// from stage-1 or environment problems. An empty command means success.
VerificationResult verifyWithJudge(const std::string& code,
    const std::vector<std::string>& judgeCommand,
    const std::string& temporaryDirectory);

struct BehaviourVerificationResult {
    bool useUnoptimized = false;
    std::string warnings;
};

// Run only after compile verification passes. Runtime differences cannot guide
// repair; compare the fallback too, on the first differing input, for diagnosis.
// All temporary sources, executables and captured output are removed on return.
BehaviourVerificationResult verifyBehaviour(const std::string& code,
    const std::string& unoptimizedCode,
    const std::vector<std::string>& cppFilePaths,
    const std::vector<std::string>& compilationOptions,
    const std::vector<std::string>& judgeCommand,
    const std::vector<std::string>& inputs,
    const std::string& temporaryDirectory);

// Identifier names on candidate error/note lines that the optimizer removed.
// Uses whole lines, ignoring columns, literals and comments.
std::unordered_set<std::string> findRepairNames(const std::string& code,
    const VerificationResult& result,
    const std::unordered_set<std::string>& removedNames);

} // namespace internal
} // namespace caide
