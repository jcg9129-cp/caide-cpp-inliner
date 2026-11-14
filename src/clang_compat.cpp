//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "clang_compat.h"
#include "clang_version.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RawCommentList.h>
#include <clang/Basic/SourceManager.h>

using namespace clang;

namespace caide { namespace internal {

CXXConstructorDecl* getInheritedConstructor(CXXConstructorDecl* ctorDecl) {
    return ctorDecl->getInheritedConstructor().getConstructor();
}


SourceLocation getBeginLoc(const Decl* decl) {
    return decl->getBeginLoc();
}

SourceLocation getEndLoc(const Decl* decl) {
    return decl->getEndLoc();
}

SourceLocation getBeginLoc(const RawComment* comment) {
    return comment->getBeginLoc();
}

SourceLocation getEndLoc(const RawComment* comment) {
    return comment->getEndLoc();
}

const RawComment* getRawCommentForDecl(const Decl* decl) {
#if CAIDE_CLANG_VERSION_AT_LEAST(23,0)
    // LLVM 23's redeclaration cache can let one redeclaration's comment mask
    // a different comment directly on this declaration. Return null in that
    // case, rather than returning another declaration's comment.
    const Decl* originalDecl = nullptr;
    const RawComment* comment =
        decl->getASTContext().getRawCommentForAnyRedecl(decl, &originalDecl);
    if (originalDecl == decl ||
        (originalDecl && originalDecl == decl->getDescribedTemplate()))
        return comment;
    return nullptr;
#else
    return decl->getASTContext().getRawCommentForDeclNoCache(decl);
#endif
}

bool isWrittenInBuiltinFile(const SourceManager& srcManager, SourceLocation location) {
    return srcManager.isWrittenInBuiltinFile(location);
}

unsigned getNumArgs(const clang::TemplateSpecializationType& templateSpecType) {
    return templateSpecType.template_arguments().size();
}

const clang::TemplateArgument* getArgs(const TemplateSpecializationType& templateSpecType) {
    return templateSpecType.template_arguments().data();
}
}}

