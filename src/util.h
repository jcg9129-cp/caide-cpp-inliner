//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <clang/Basic/TokenKinds.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clang {
    class ASTContext;
    class Decl;
    class LangOptions;
    class SourceLocation;
    class SourceManager;
    class SourceRange;
    class Stmt;

    namespace tooling {
        class FixedCompilationDatabase;
    }
}

namespace caide {
namespace internal {

clang::SourceLocation findTokenAfterLocation(clang::SourceLocation loc, clang::ASTContext& Ctx, clang::tok::TokenKind tokenType);
clang::SourceLocation findSemiAfterLocation(clang::SourceLocation loc, clang::ASTContext& Ctx, bool IsDecl);
clang::SourceLocation findLocationAfterSemi(clang::SourceLocation loc, clang::ASTContext& Ctx, bool IsDecl);

std::pair<const char*, const char*> getCharRange(clang::SourceRange range, const clang::SourceManager& SM, const clang::LangOptions& langOpts);

std::unique_ptr<clang::tooling::FixedCompilationDatabase> createCompilationDatabaseFromCommandLine(const std::vector<std::string>& cmdLine);

/// Remove all C++ comments (// ... and /* ... */) from a source string, leaving
/// string and raw-string literals untouched. Newlines inside a comment are kept
/// so that line numbers are preserved; an inline comment is replaced by a single
/// space to avoid pasting adjacent tokens together.
std::string stripComments(const std::string& code);

std::string rangeToString(clang::SourceManager& sourceManager,
        const clang::SourceLocation& start, const clang::SourceLocation& end);

std::string toString(clang::SourceManager& sourceManager, clang::SourceLocation loc);
std::string toString(clang::SourceManager& sourceManager, clang::SourceRange range, const clang::LangOptions* = nullptr);
std::string toString(clang::SourceManager& sourceManager, const clang::Decl* decl);
std::string toString(clang::ASTContext&, const clang::Stmt&);

clang::SourceLocation getExpansionStart(clang::SourceManager& sourceManager,
        const clang::Decl* decl);
clang::SourceLocation getExpansionEnd(clang::SourceManager& sourceManager,
        const clang::Decl* decl);

clang::SourceRange getExpansionRange(clang::SourceManager& sourceManager,
        const clang::Decl* decl);

}
}

