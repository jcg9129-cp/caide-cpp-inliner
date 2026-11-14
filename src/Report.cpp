//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "Report.h"
#include "clang_version.h"
#include "SmartRewriter.h"
#include "SourceInfo.h"
#include "util.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Basic/SourceManager.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace clang;

namespace caide {
namespace internal {

void keepRoot(SourceInfo& info, Decl* decl, RootReason reason) {
    info.declsToKeep.insert(decl);
    if (info.report)
        info.report->recordRoot(decl, reason);
}

void Report::recordRoot(Decl* decl, RootReason reason) {
    const auto inserted = roots.emplace(decl->getCanonicalDecl(), reason);
    // Prefer repair over other roots, and main over incidental keep rules.
    if (reason == RootReason::Repair ||
            (reason == RootReason::Main && inserted.first->second != RootReason::Repair))
        inserted.first->second = reason;
}

void Report::recordParent(Decl* decl, Decl* parent) {
    parents.emplace(decl->getCanonicalDecl(), parent->getCanonicalDecl());
}

static bool isInstantiation(TemplateSpecializationKind kind) {
    return kind == TSK_ImplicitInstantiation || kind == TSK_ExplicitInstantiationDeclaration ||
        kind == TSK_ExplicitInstantiationDefinition;
}

void Report::consider(Decl* decl, SourceManager& sm) {
    const auto location = getExpansionStart(sm, decl);
    if (decl->isImplicit() || location.isInvalid() || !sm.isInMainFile(location))
        return;
    // RAV can still call VisitDecl for an explicit instantiation while skipping
    // its instantiated children. Specializations written by the user remain.
    if (const auto* f = dyn_cast<FunctionDecl>(decl)) {
        if (isInstantiation(f->getTemplateSpecializationKind()))
            return;
    } else if (const auto* record = dyn_cast<CXXRecordDecl>(decl)) {
        if (isInstantiation(record->getTemplateSpecializationKind()))
            return;
    } else if (const auto* var = dyn_cast<VarDecl>(decl)) {
        if (isInstantiation(var->getTemplateSpecializationKind()))
            return;
    }
#if CAIDE_CLANG_VERSION_AT_LEAST(23, 0)
    // Clang 23 represents explicit instantiation syntax with a separate node.
    if (isa<ExplicitInstantiationDecl>(decl))
        return;
#endif
    declarations.push_back(decl);
}

static std::string nameOf(const Decl* decl) {
    if (const auto* named = dyn_cast<NamedDecl>(decl)) {
        if (!named->getDeclName().isEmpty())
            return named->getQualifiedNameAsString();
    }
    return "<unnamed>";
}

static std::string locationOf(SourceManager& sm, SourceLocation location) {
    const PresumedLoc presumed = sm.getPresumedLoc(sm.getExpansionLoc(location));
    if (presumed.isInvalid())
        return "<unknown>:0";
    return std::string(presumed.getFilename()) + ":" + std::to_string(presumed.getLine());
}

static const char* rootText(RootReason reason) {
    switch (reason) {
    case RootReason::Main: return "root: main";
    case RootReason::KeepComment: return "root: caide keep comment";
    case RootReason::Identifier: return "root: identifiersToKeep";
    case RootReason::SideEffect: return "root: side-effecting global";
    case RootReason::Attribute: return "root: constructor/destructor/used attribute";
    case RootReason::Repair: return "root: restored by repair";
    }
    return "root: main";
}

void Report::finishDeclarations(SourceManager& sm,
        const std::unordered_set<Decl*>& removed,
        const std::unordered_set<std::string>& forced,
        const SmartRewriter& rewriter) {
    // Physical source order, even when #line changes the presumed file or line.
    std::stable_sort(declarations.begin(), declarations.end(), [&](Decl* a, Decl* b) {
        return sm.isBeforeInTranslationUnit(sm.getExpansionLoc(a->getLocation()),
                                             sm.getExpansionLoc(b->getLocation()));
    });
    std::unordered_set<Decl*> seen;
    for (Decl* decl : declarations) {
        if (!seen.insert(decl).second)
            continue;
        const auto location = sm.getExpansionLoc(decl->getLocation());
        // A local, parameter or enumerator can disappear with its enclosing
        // declaration without being explicitly entered in the removal set.
        const bool isRemoved = removed.count(decl) ||
            rewriter.isPartOfRangeRemoved(SourceRange(location, location));
        std::string reason;
        if (isRemoved) {
            const auto* named = dyn_cast<NamedDecl>(decl);
            reason = named && named->getDeclName().isIdentifier() &&
                forced.count(named->getNameAsString()) ? "forced (testing hook)" : "unreachable";
            ++removedCount;
        } else {
            ++keptCount;
            Decl* canonical = decl->getCanonicalDecl();
            const auto root = roots.find(canonical);
            const auto parent = parents.find(canonical);
            Decl* from = nullptr;
            if (root != roots.end())
                reason = rootText(root->second);
            else if (parent != parents.end())
                from = parent->second;
            else {
                // Some lexical declarations (e.g. defaulted members) are kept
                // with their context rather than reached as graph vertices.
                from = dyn_cast_or_null<Decl>(decl->getLexicalDeclContext());
                if (isa_and_nonnull<TranslationUnitDecl>(from))
                    from = nullptr;
            }
            if (reason.empty()) {
                // Syntax-only declarations need not have a graph vertex. Do
                // not invent a dependency edge when the visitor keeps one.
                reason = from ? "used by " + nameOf(from) +
                    " (" + locationOf(sm, from->getLocation()) + ")" :
                    "retained by optimizer policy (no dependency graph parent)";
            }
        }
        lines.push_back(std::string(isRemoved ? "removed  " : "kept  ") +
            decl->getDeclKindName() + " " + nameOf(decl) + "  " +
            locationOf(sm, decl->getLocation()) + "  " + reason);
    }
    declarations.clear();
    roots.clear();
    parents.clear();
}

void Report::addMacro(const std::string& name, SourceLocation location, SourceManager& sm) {
    macros.push_back("removed  macro " + name + "  " + locationOf(sm, location));
}

std::string runtimeFallbackReason(const std::string& warnings) {
    std::istringstream input(warnings);
    std::string line;
    const std::string prefix = "caide inliner: warning: ";
    const std::string suffix = "; writing unoptimized output";
    while (std::getline(input, line)) {
        const auto end = line.find(suffix);
        if (end == std::string::npos)
            continue;
        const auto start = line.find(prefix) == 0 ? prefix.size() : 0;
        return line.substr(start, end - start);
    }
    return "runtime verification failed";
}

void Report::write(const std::string& path, const std::string& outcome) const {
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot open report file: " + path);
    // For fallback the first line must explicitly say this attempt was not used.
    if (outcome.find("fell back to unoptimized output:") == 0)
        out << "# optimizer output was not used: " << outcome << '\n';
    out << "# caide inliner report\n" << outcome << '\n'
        << "# declarations: " << keptCount << " kept, " << removedCount << " removed\n";
    for (const auto& line : lines)
        out << line << '\n';
    auto sortedMacros = macros;
    std::sort(sortedMacros.begin(), sortedMacros.end());
    for (const auto& macro : sortedMacros)
        out << macro << '\n';
    out.close();
    if (!out)
        throw std::runtime_error("Cannot write report file: " + path);
}

} // namespace internal
} // namespace caide
