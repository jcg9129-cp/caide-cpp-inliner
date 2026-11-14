//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <string>
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
    // Rendered embedded errors or the judge's stdout/stderr, for reporting.
    std::string output;
};

// Check one candidate, without modifying it. Locations refer to this candidate
// (or its system headers), so a future repair loop can use them before retrying.
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

} // namespace internal
} // namespace caide
