//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#pragma once

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace clang {
    class Decl;
    class SourceLocation;
    class SourceManager;
}

namespace caide {
namespace internal {

struct SourceInfo;
class SmartRewriter;

enum class RootReason { Main, KeepComment, Identifier, SideEffect, Attribute, Repair };

// Insert a graph root as usual; record its explanation only when reporting is enabled.
void keepRoot(SourceInfo& info, clang::Decl* decl, RootReason reason);

// Extract the fallback explanation from existing runtime-verification warnings.
std::string runtimeFallbackReason(const std::string& warnings);

// One optimizer attempt. AST pointers are used only while the compiler is alive;
// finishDeclarations/addMacro copy all names and presumed locations into text.
class Report {
public:
    void recordRoot(clang::Decl* decl, RootReason reason);
    void recordParent(clang::Decl* decl, clang::Decl* parent);
    void consider(clang::Decl* decl, clang::SourceManager& sourceManager);
    void finishDeclarations(clang::SourceManager& sourceManager,
        const std::unordered_set<clang::Decl*>& removed,
        const std::unordered_set<std::string>& forced,
        const SmartRewriter& rewriter);
    void addMacro(const std::string& name, clang::SourceLocation location,
                  clang::SourceManager& sourceManager);
    void write(const std::string& path, const std::string& outcome) const;

private:
    std::map<clang::Decl*, RootReason> roots;
    std::map<clang::Decl*, clang::Decl*> parents;
    std::vector<clang::Decl*> declarations;
    std::vector<std::string> lines;
    std::vector<std::string> macros;
    unsigned keptCount = 0;
    unsigned removedCount = 0;
};

} // namespace internal
} // namespace caide
