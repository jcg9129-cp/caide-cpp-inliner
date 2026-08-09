//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "MergeNamespacesVisitor.h"
#include "clang_compat.h"
#include "SmartRewriter.h"
#include "util.h"

#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>

using namespace clang;


namespace caide {
namespace internal {


MergeNamespacesVisitor::MergeNamespacesVisitor(SourceManager& sourceManager_,
        const std::unordered_set<Decl*>& removed_, SmartRewriter& rewriter_)
    : sourceManager(sourceManager_)
    , removed(removed_)
    , rewriter(rewriter_)
{}

bool MergeNamespacesVisitor::shouldVisitImplicitCode() const { return false; }
bool MergeNamespacesVisitor::shouldVisitTemplateInstantiations() const { return false; }


bool MergeNamespacesVisitor::TraverseDecl(Decl* decl) {
    bool ret = RecursiveASTVisitor<MergeNamespacesVisitor>::TraverseDecl(decl);

    if (decl && sourceManager.isInMainFile(getBeginLoc(decl)) && removed.count(decl) == 0) {
        if (auto* nsDecl = dyn_cast<NamespaceDecl>(decl))
            closedNamespaces.push(nsDecl);
        else {
            auto* parentContext = decl->getLexicalDeclContext();
            // Note: a type alias is not a declaration context, so the lexical context of its template
            // arguments is the enclosing namespace/class/function etc. It means that this check may give a
            // false positive. So we skip TemplateTypeParmDecl (it's always attached to another Decl anyway).
            // We also skip non-top-level Decls (they might have been removed as part of a parent Decl).
            if (!isa<TemplateTypeParmDecl>(decl) &&
                (isa<NamespaceDecl>(parentContext) || isa<TranslationUnitDecl>(parentContext)))
            {
                // A non-removed declaration interrupts the chain of closed namespaces
                closedNamespaces = std::stack<NamespaceDecl*>{};
            }
        }
    }

    return ret;
}

// Locate the "{" that opens a plain "namespace <name> {". Returns an invalid
// location for a nested-namespace definition ("namespace A::B { ... }"), where the
// token after the first name is "::" rather than "{". Such a namespace shares a
// single closing brace between its parts (A and B), so it must not take part in a
// merge: removing that shared brace would pull following code into the inner
// namespace. Note the *outer* part (A) is not flagged NamespaceDecl::isNested()
// (only B is), so this structural check is what distinguishes it.
static SourceLocation plainOpeningBrace(NamespaceDecl* nsDecl, ASTContext& astContext) {
    SourceLocation nameStart =
        findTokenAfterLocation(getBeginLoc(nsDecl), astContext, tok::raw_identifier);
    if (nameStart.isInvalid())
        return SourceLocation();
    return findTokenAfterLocation(nameStart, astContext, tok::l_brace);
}

bool MergeNamespacesVisitor::VisitNamespaceDecl(NamespaceDecl* nsDecl) {
    if (!sourceManager.isInMainFile(getBeginLoc(nsDecl)))
        return true;

    if (nsDecl && removed.count(nsDecl) == 0) {
        NamespaceDecl* canonicalDecl = nsDecl->getCanonicalDecl();
        if (!closedNamespaces.empty() && canonicalDecl == closedNamespaces.top()->getCanonicalDecl()) {
            ASTContext& astContext = nsDecl->getASTContext();
            NamespaceDecl* previous = closedNamespaces.top();

            // Merge "namespace X { ... } namespace X { ... }" by removing the first
            // block's closing brace and the second block's "namespace X {" prefix.
            // Only do so when BOTH blocks are plain "namespace <name> { ... }": a
            // nested-namespace definition ("namespace A::B { ... }") shares one brace
            // between A and B, and removing it (or failing to find the current block's
            // brace) would corrupt the nesting or the removed-ranges set.
            SourceLocation thisNamespaceOpeningBrace = plainOpeningBrace(nsDecl, astContext);
            if (thisNamespaceOpeningBrace.isValid() && plainOpeningBrace(previous, astContext).isValid()) {
                rewriter.removeRange(previous->getRBraceLoc(), previous->getRBraceLoc());
                closedNamespaces.pop();
                rewriter.removeRange(getBeginLoc(nsDecl), thisNamespaceOpeningBrace);
            }
        }
    }

    return true;
}


}
}

