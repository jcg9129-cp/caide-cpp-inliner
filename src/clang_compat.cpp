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

#include <cstring>


using namespace clang;

namespace caide { namespace internal {

CXXConstructorDecl* getInheritedConstructor(CXXConstructorDecl* ctorDecl) {
#if CAIDE_CLANG_VERSION_AT_LEAST(3,9)
    return ctorDecl->getInheritedConstructor().getConstructor();
#else
    return const_cast<CXXConstructorDecl*>(ctorDecl->getInheritedConstructor());
#endif
}


SourceLocation getBeginLoc(const Decl* decl) {
#if CAIDE_CLANG_VERSION_AT_LEAST(8,0)
    return decl->getBeginLoc();
#else
    return decl->getLocStart();
#endif
}

SourceLocation getEndLoc(const Decl* decl) {
#if CAIDE_CLANG_VERSION_AT_LEAST(8,0)
    return decl->getEndLoc();
#else
    return decl->getLocEnd();
#endif
}

SourceLocation getBeginLoc(const RawComment* comment) {
#if CAIDE_CLANG_VERSION_AT_LEAST(8,0)
    return comment->getBeginLoc();
#else
    return comment->getLocStart();
#endif
}

SourceLocation getEndLoc(const RawComment* comment) {
#if CAIDE_CLANG_VERSION_AT_LEAST(8,0)
    return comment->getEndLoc();
#else
    return comment->getLocEnd();
#endif
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
#if CAIDE_CLANG_VERSION_AT_LEAST(8,0)
    return srcManager.isWrittenInBuiltinFile(location);
#else
    // Copied from latest SourceManager::isWrittenInBuiltinFile.
    const char* fileName = srcManager.getPresumedLoc(location).getFilename();
    return std::strcmp(fileName, "<built-in>") == 0;
#endif
}

unsigned getNumArgs(const clang::TemplateSpecializationType& templateSpecType) {
#if CAIDE_CLANG_VERSION_AT_LEAST(13,0)
    return templateSpecType.template_arguments().size();
#else
    return templateSpecType.getNumArgs();
#endif
}

const clang::TemplateArgument* getArgs(const TemplateSpecializationType& templateSpecType) {
#if CAIDE_CLANG_VERSION_AT_LEAST(13,0)
    return templateSpecType.template_arguments().data();
#else
    return templateSpecType.getArgs();
#endif
}
}}

