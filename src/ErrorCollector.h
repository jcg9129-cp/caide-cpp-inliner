//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include "Verifier.h"

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/SmallVector.h>

#include <string>
#include <utility>
#include <vector>

namespace caide {
namespace internal {

// Shared by optimization and verification. Like TextDiagnosticBuffer, but
// resolves locations eagerly, while the diagnostic's SourceManager is alive.
class ErrorCollector : public clang::DiagnosticConsumer {
public:
    void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                          const clang::Diagnostic& info) override {
        DiagnosticConsumer::HandleDiagnostic(level, info);
        if (level >= clang::DiagnosticsEngine::Error) {
            std::string message;
            VerificationDiagnostic diagnostic;
            if (info.hasSourceManager()) {
                // Preserve the optimizer's existing rendered error messages.
                message = info.getLocation().printToString(info.getSourceManager());
                message += ": ";
                if (info.getLocation().isValid()) {
                    const auto location = info.getSourceManager().getPresumedLoc(info.getLocation());
                    if (location.isValid()) {
                        diagnostic.file = location.getFilename();
                        diagnostic.line = location.getLine();
                        diagnostic.column = location.getColumn();
                    }
                }
            }
            llvm::SmallVector<char, 256> buffer;
            info.FormatDiagnostic(buffer);
            diagnostic.message.assign(buffer.data(), buffer.size());
            message += diagnostic.message;
            errors.push_back(std::move(message));
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    void clear() override {
        DiagnosticConsumer::clear();
        errors.clear();
        diagnostics.clear();
    }

    const std::vector<std::string>& getErrors() const { return errors; }
    const std::vector<VerificationDiagnostic>& getDiagnostics() const { return diagnostics; }

private:
    std::vector<std::string> errors;
    std::vector<VerificationDiagnostic> diagnostics;
};

} // namespace internal
} // namespace caide
