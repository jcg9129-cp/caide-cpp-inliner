//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <clang/Basic/SourceLocation.h>
#include <clang/Lex/PPCallbacks.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_set>

namespace clang {
    class LangOptions;
    class SourceManager;
    class MacroDirective;
}

namespace caide {
namespace internal {

class SmartRewriter;
class Report;

class RemoveInactivePreprocessorBlocks: public clang::PPCallbacks {
private:
    struct RemoveInactivePreprocessorBlocksImpl;
    std::unique_ptr<RemoveInactivePreprocessorBlocksImpl> impl;

public:
    RemoveInactivePreprocessorBlocks(clang::SourceManager& sourceManager_, const clang::LangOptions& langOptions,
           SmartRewriter& rewriter_, const std::set<std::string>& macrosToKeep_,
           const std::unordered_set<std::string>& namesToRestore_);
    ~RemoveInactivePreprocessorBlocks();

    void MacroDefined(const clang::Token& MacroNameTok, const clang::MacroDirective* MD) override;

    void MacroUndefined(const clang::Token& MacroNameTok, const clang::MacroDefinition& MD, const clang::MacroDirective* Undef) override;

    void MacroExpands(const clang::Token& /*MacroNameTok*/, const clang::MacroDefinition& MD,
                      clang::SourceRange Range, const clang::MacroArgs* /*Args*/) override;

    void If(clang::SourceLocation Loc, clang::SourceRange ConditionRange, ConditionValueKind ConditionValue) override;
    void Ifdef(clang::SourceLocation Loc, const clang::Token& MacroNameTok, const clang::MacroDefinition& /*MD*/) override;
    void Ifndef(clang::SourceLocation Loc, const clang::Token& MacroNameTok, const clang::MacroDefinition& /*MD*/) override;

    void Elif(clang::SourceLocation Loc, clang::SourceRange ConditionRange, ConditionValueKind ConditionValue, clang::SourceLocation /*IfLoc*/ ) override;
    void Else(clang::SourceLocation Loc, clang::SourceLocation /*IfLoc*/) override;
    void Endif(clang::SourceLocation Loc, clang::SourceLocation /*IfLoc*/) override;

    void InclusionDirective(clang::SourceLocation HashLoc,
                            const clang::Token& IncludeTok,
                            llvm::StringRef FileName,
                            bool IsAngled,
                            clang::CharSourceRange FilenameRange,
                            clang::OptionalFileEntryRef File,
                            llvm::StringRef SearchPath,
                            llvm::StringRef RelativePath,
                            const clang::Module* /*SuggestedModule*/
                            , bool /*ModuleImported*/
                            , clang::SrcMgr::CharacteristicKind FileType
                            ) override;

    // EndOfMainFile() is called too late; instead, we call this one manually in the consumer.
    // Reports removed macro definitions alongside the optimizer's removed declarations.
    // The forced-removal set is a testing-only hook; restored names always win.
    void Finalize(std::unordered_set<std::string>& removedNames,
                  const std::unordered_set<std::string>& macrosToForceRemoveForTesting,
                  Report* report = nullptr);
};

}
}

