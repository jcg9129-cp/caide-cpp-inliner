//                        Caide C++ inliner
//
// This file is distributed under the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version. See LICENSE.TXT for details.

#include "DependenciesCollector.h"
#include "Report.h"
#include "clang_compat.h"
#include "clang_version.h"
#include "sema_utils.h"
#include "SourceInfo.h"
#include "util.h"

// #define CAIDE_DEBUG_MODE
#include "caide_debug.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/ExprCXX.h>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <clang/AST/RawCommentList.h>
#include <clang/Basic/SourceManager.h>

#include <ostream>
#include <sstream>
#include <string>


using namespace clang;

namespace caide {
namespace internal {

bool DependenciesCollector::TraverseDecl(Decl* decl) {
    declStack.push(decl);
    bool ret = RecursiveASTVisitor<DependenciesCollector>::TraverseDecl(decl);
    declStack.pop();
    return ret;
}

void DependenciesCollector::traverseSugaredSignature(const SugaredSignature& sig, bool traverseTypeLocs) {
    for (const TemplateArgumentLoc& argLoc : sig.templateArgLocs)
        TraverseTemplateArgumentLoc(argLoc);

    for (TypeSourceInfo* t : sig.argTypes) {
        if (traverseTypeLocs) {
            TraverseTypeLoc(t->getTypeLoc());
        } else {
            TraverseType(t->getType());
        }
    }

    for (Expr* expr : sig.associatedConstraints)
        TraverseStmt(expr);
}

void DependenciesCollector::traverseTemplateSpecializationTypeImpl(
        const TemplateSpecializationType* tempSpecType, bool traverseTypeLocs)
{
    // Template specialized by this type.
    TemplateDecl* tempDecl = nullptr;

    llvm::ArrayRef<TemplateArgument> args;

    if (tempSpecType->isTypeAlias()) {
        // TODO: This type might have been traversed or not.
        // TODO: TraverseTypeLoc?
        TraverseType(tempSpecType->getAliasedType());

        // This will be TypeAliasTemplateDecl.
        tempDecl = tempSpecType->getTemplateName().getAsTemplateDecl();
    } else if (auto* specDecl = dyn_cast_or_null<ClassTemplateSpecializationDecl>(tempSpecType->getAsTagDecl())) {
        args = specDecl->getTemplateInstantiationArgs().asArray();
        llvm::PointerUnion<ClassTemplateDecl*, ClassTemplatePartialSpecializationDecl*>
            instantiatedFrom = specDecl->getSpecializedTemplateOrPartial();

        // This cannot be a partial specialization.
        tempDecl = dyn_cast_if_present<ClassTemplateDecl*>(instantiatedFrom);
    }

    if (tempDecl) {
        // These will be arguments as written at the point from where the
        // reference comes (e.g. a variable declaration).
        llvm::ArrayRef<TemplateArgument> writtenArgs{
            getArgs(*tempSpecType), getNumArgs(*tempSpecType)};
        SugaredSignature sig = substituteTemplateArguments(sema, tempDecl, writtenArgs, args);
        traverseSugaredSignature(sig, traverseTypeLocs);
    }
}

bool DependenciesCollector::TraverseTemplateSpecializationTypeLoc(TemplateSpecializationTypeLoc tempSpecTypeLoc
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
        , bool TraverseQualifier
#endif
        ) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseTemplateSpecializationTypeLoc(tempSpecTypeLoc
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
        , TraverseQualifier
#endif
        );

    traverseTemplateSpecializationTypeImpl(tempSpecTypeLoc.getTypePtr(), true);
    return true;
}

// We duplicate this for Type and TypeLoc, as not everything in clang traversal uses TypeLocs yet.
bool DependenciesCollector::TraverseTemplateSpecializationType(TemplateSpecializationType* tempSpecType
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
        , bool TraverseQualifier
#endif
        ) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseTemplateSpecializationType(tempSpecType
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
        , TraverseQualifier
#endif
        );

    traverseTemplateSpecializationTypeImpl(tempSpecType, false);
    return true;
}

#if CAIDE_CLANG_VERSION_AT_LEAST(10,0)
bool DependenciesCollector::TraverseAutoType(AutoType* autoType
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
        , bool TraverseQualifier
#endif
        ) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseAutoType(autoType
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
        , TraverseQualifier
#endif
        );

#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
    // getTypeConstraintConcept() now returns TemplateDecl* (may be a ConceptDecl).
    if (ConceptDecl* conceptDecl = dyn_cast_if_present<ConceptDecl>(autoType->getTypeConstraintConcept())) {
#else
    if (ConceptDecl* conceptDecl = autoType->getTypeConstraintConcept()) {
#endif
        // XXX: We might need this in stdlib for transitive dependencies, but
        // substituteTemplateArguments takes too long.
        if (sourceManager.isInMainFile(getBeginLoc(conceptDecl))) {
            llvm::SmallVector<TemplateArgument, 4> writtenArgs;
            writtenArgs.push_back(TemplateArgument(autoType->getDeducedType()));
            writtenArgs.append(autoType->getTypeConstraintArguments().begin(), autoType->getTypeConstraintArguments().end());
            SugaredSignature sig = substituteTemplateArguments(sema, conceptDecl, writtenArgs, {});
            traverseSugaredSignature(sig, /*traverseTypeLocs=*/false);
        }
    }
    return true;
}

bool DependenciesCollector::TraverseVarDecl(VarDecl* valueDecl) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseVarDecl(valueDecl);

    if (AutoType* autoType = valueDecl->getType().getTypePtr()->getContainedAutoType()) {
        // AutoType within AutoTypeLoc is missing deduced type: https://github.com/llvm/llvm-project/issues/42259
        // So we traverse the AutoType in addition to AutoTypeLoc.
        TraverseAutoType(autoType);
    }

    return true;
}
#endif

Decl* DependenciesCollector::getCurrentDecl() const {
    return declStack.empty() ? nullptr : declStack.top();
}

FunctionDecl* DependenciesCollector::getCurrentFunction(Decl* decl) const {
    DeclContext* declCtx = decl->getLexicalDeclContext();
    return dyn_cast_or_null<FunctionDecl>(declCtx);
}

Decl* DependenciesCollector::getParentDecl(Decl* decl) const {
    return dyn_cast_or_null<Decl>(decl->getLexicalDeclContext());
}

// A template's parameters are lexically part of it, so a kept template must keep
// its parameters. This matters for a type-constraint such as
// `template<binary_operation<T> Op>`: the reference to the concept is attributed
// to the parameter (Op), and without this edge the parameter -- hence the concept
// -- is unreachable from the template. An explicit specialization gets the concept
// from a different path (its substituted constraints), but a template used only
// via CTAD does not, so the concept would be wrongly removed while the constraint
// naming it stays. Note getParentDecl(param) is the enclosing namespace, not the
// template, which is why the link must be made from the template side.
void DependenciesCollector::recordTemplateParameters(TemplateDecl* templateDecl) {
    TemplateParameterList* params = templateDecl->getTemplateParameters();
    if (!params)
        return;
    Decl* templated = templateDecl->getTemplatedDecl();
    for (NamedDecl* param : *params) {
        insertReference(templateDecl, param);
        insertReference(templated, param);
    }
}

// A declaration in a templated context (such as a method or a field of a class template) is present
// as multiple instances in the AST: once for the template itself and once for each *implicit*
// instantiation. Clearly, it doesn't make sense to 'remove' a Decl coming from an implicit
// instantiation: what if another implicit instantiation of the same code is used? (And we don't:
// OptimizerVisitor doesn't visit implicit code.)
// We, therefore, need to add a dependency from each Decl that comes from an implicit instantiation to
// the one Decl that comes from the template itself; then if the one Decl is unreachable
// we remove it.
Decl* DependenciesCollector::getCorrespondingDeclInNonInstantiatedContext(clang::Decl* semanticDecl) const {
    // The implementation is HACKY. It relies on the assumption that a Decl inside an implicit
    // instantiation and the corresponding Decl in the non-instantiated context have the same
    // source range.
    auto key = SourceInfo::makeKey(semanticDecl);
    auto it = srcInfo.nonImplicitDecls.find(key);
    if (it != srcInfo.nonImplicitDecls.end())
        return it->second;
    return nullptr;
}

void DependenciesCollector::insertReference(Decl* from, Decl* to) {
    if (!from || !to)
        return;
    from = from->getCanonicalDecl();
    to = to->getCanonicalDecl();
    if (from == to)
        return;
    srcInfo.uses[from].insert(to);
    dbg("Reference   FROM    " << from->getDeclKindName() << " " << from
        << "<" << toString(sourceManager, from).substr(0, 20) << ">"
        << toString(sourceManager, from->getSourceRange())
        << "     TO     " << to->getDeclKindName() << " " << to
        << "<" << toString(sourceManager, to).substr(0, 20) << ">"
        << toString(sourceManager, to->getSourceRange())
        << std::endl);
}

bool DependenciesCollector::VisitType(Type* T) {
#ifdef CAIDE_DEBUG_MODE
    dbg("VisitType " << T->getTypeClassName()
        << " " << QualType(T, 0).getAsString() << std::endl);
    // T->dump();
#endif
    insertReference(getCurrentDecl(), T->getAsTagDecl());
    return true;
}

bool DependenciesCollector::VisitTypedefType(TypedefType* typedefType) {
    insertReference(getCurrentDecl(), typedefType->getDecl());
    return true;
}

void DependenciesCollector::linkClassTemplateMembers(ClassTemplateDecl* templateDecl,
        DeclarationName name, Decl* from) {
    auto linkIn = [&](CXXRecordDecl* record) {
        if (!record || !record->hasDefinition())
            return;
        for (NamedDecl* decl : record->lookup(name)) {
            insertReference(from, decl);
            if (auto* memberTemplate = dyn_cast<TemplateDecl>(decl))
                insertReference(from, memberTemplate->getTemplatedDecl());
        }
    };
    linkIn(templateDecl->getTemplatedDecl());
    llvm::SmallVector<ClassTemplatePartialSpecializationDecl*, 4> partials;
    templateDecl->getPartialSpecializations(partials);
    for (ClassTemplatePartialSpecializationDecl* partial : partials)
        linkIn(partial);
}

// `typename Tmpl<...>::member` where the qualifier is a dependent class-template
// specialization is a DependentNameType: it names no member decl (the specialization
// is only chosen at instantiation, which we don't visit), so the member -- e.g. a
// nested `using scalar = ...;` reached only through such an alias -- is otherwise
// pruned while the alias that needs it stays. Resolve the name against the
// template's primary pattern and every partial specialization.
bool DependenciesCollector::VisitDependentNameType(DependentNameType* dependentName) {
    const IdentifierInfo* identifier = dependentName->getIdentifier();
    if (!identifier)
        return true;
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
    NestedNameSpecifier qualifier = dependentName->getQualifier();
    const Type* qualifierType =
        qualifier.getKind() == NestedNameSpecifier::Kind::Type ? qualifier.getAsType() : nullptr;
#else
    NestedNameSpecifier* qualifier = dependentName->getQualifier();
    const Type* qualifierType = qualifier ? qualifier->getAsType() : nullptr;
#endif
    if (!qualifierType)
        return true;
    const auto* specType = qualifierType->getAs<TemplateSpecializationType>();
    if (!specType)
        return true;
    if (auto* templateDecl =
            dyn_cast_or_null<ClassTemplateDecl>(specType->getTemplateName().getAsTemplateDecl()))
        linkClassTemplateMembers(templateDecl, DeclarationName(identifier), getCurrentDecl());
    return true;
}

bool DependenciesCollector::VisitTemplateSpecializationType(TemplateSpecializationType* tempSpecType) {
    TemplateName templateName = tempSpecType->getTemplateName();
    insertReference(getCurrentDecl(), templateName.getAsTemplateDecl());
    return true;
}

#if CAIDE_CLANG_VERSION_AT_LEAST(10,0)
bool DependenciesCollector::VisitAutoType(AutoType* autoType) {
    insertReference(getCurrentDecl(), autoType->getTypeConstraintConcept());
    return true;
}
#endif

DependenciesCollector::DependenciesCollector(SourceManager& srcMgr,
        Sema& sema_,
        const std::unordered_set<std::string>& identifiersToKeep_,
        const std::unordered_set<std::string>& namesToRestore_,
        SourceInfo& srcInfo_)
    : sourceManager(srcMgr)
    , sema(sema_)
    , identifiersToKeep(identifiersToKeep_)
    , namesToRestore(namesToRestore_)
    , srcInfo(srcInfo_)
{
}

bool DependenciesCollector::shouldVisitImplicitCode() const { return true; }
bool DependenciesCollector::shouldVisitTemplateInstantiations() const { return true; }
bool DependenciesCollector::shouldWalkTypesOfTypeLocs() const { return true; }

bool DependenciesCollector::VisitDecl(Decl* decl) {
    dbg("DECL " << decl->getDeclKindName() << " " << decl
        << "<" << toString(sourceManager, decl).substr(0, 30) << ">"
        << toString(sourceManager, getExpansionRange(sourceManager, decl))
        << std::endl);

    // Mark dependence on enclosing (semantic) class/namespace.
    Decl* ctx = dyn_cast_or_null<Decl>(decl->getDeclContext());
    if (ctx && !isa<FunctionDecl>(ctx))
        insertReference(decl, ctx);

    if (!sourceManager.isInMainFile(getBeginLoc(decl)))
        return true;

    // If this declaration is inside a template instantiation, mark dependence on the corresponding
    // declaration in a non-instantiated context.
    insertReference(decl, getCorrespondingDeclInNonInstantiatedContext(decl));

    // Remainder of the function processes special comments.
    const RawComment* comment = getRawCommentForDecl(decl);
    if (!comment)
        return true;

    bool invalid = false;
    const char* beg = sourceManager.getCharacterData(getBeginLoc(comment), &invalid);
    if (!beg || invalid)
        return true;

    const char* end =
        sourceManager.getCharacterData(getEndLoc(comment), &invalid);
    if (!end || invalid)
        return true;

    StringRef haystack(beg, end - beg + 1);

    {
        static const std::string caideKeepComment = "caide keep";
        StringRef needle(caideKeepComment);
        dbg(toString(sourceManager, decl) << ": " << haystack.str() << std::endl);
        if (haystack.find(needle) != StringRef::npos)
            keepRoot(srcInfo, decl, RootReason::KeepComment);
    }

    if (ctx) {
        // The following is useful for classes implementing a standard C++ concept.
        // For instance, a custom iterator passed to std::shuffle must be a RandomAccessIterator,
        // which means that it must provide certain member functions/type aliases.
        // However, there is no way to detect this requirement, unless both the language and
        // the standard library have full concept support (which we don't want to rely on).
        // That's because some of the functions/type aliases required by the concept might
        // not actually be used by a particular implementation of std::shuffle. If we remove
        // them, the program becomes technically illegal and may not even compile in another
        // compiler.
        //
        // To work around that, it's possible to mark declarations required by some Concept
        // with a comment '/// caide concept'. This will ensure that these declarations don't
        // get removed as long as the class containing them is used.
        static const std::string caideConceptComment = "caide concept";
        StringRef needle(caideConceptComment);
        dbg(toString(sourceManager, decl) << ": " << haystack.str() << std::endl);
        if (haystack.find(needle) != StringRef::npos)
            insertReference(ctx, decl);
    }

    return true;
}

bool DependenciesCollector::VisitNamedDecl(clang::NamedDecl* decl) {
    if (sourceManager.isInMainFile(getExpansionStart(sourceManager, decl)) &&
            (isa<FunctionDecl>(decl) || isa<VarDecl>(decl)) &&
            (decl->hasAttr<ConstructorAttr>() || decl->hasAttr<DestructorAttr>() ||
             decl->hasAttr<UsedAttr>()))
        keepRoot(srcInfo, decl, RootReason::Attribute);

    if (!sourceManager.isInMainFile(getBeginLoc(decl)))
        return true;

    if (identifiersToKeep.count(decl->getQualifiedNameAsString()))
        keepRoot(srcInfo, decl, RootReason::Identifier);

    // Repair intentionally restores all overloads and all scopes by name.
    if (decl->getDeclName().isIdentifier() && namesToRestore.count(decl->getNameAsString()))
        keepRoot(srcInfo, decl, RootReason::Repair);

    return true;
}

bool DependenciesCollector::TraverseCallExpr(CallExpr* callExpr) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseCallExpr(callExpr);

    Expr* callee = callExpr->getCallee();
    Decl* calleeDecl = callExpr->getCalleeDecl();

    if (!callee || !calleeDecl)
        return true;

    auto* refExpr = dyn_cast<DeclRefExpr>(callee);
    if (!refExpr) {
        auto* castExpr = dyn_cast<ImplicitCastExpr>(callee);
        if (castExpr && castExpr->getCastKind() == CK_FunctionToPointerDecay)
            refExpr = dyn_cast_if_present<DeclRefExpr>(castExpr->getSubExpr());
        if (!refExpr)
            return true;
    }

    // XXX: We might need the following in stdlib for transitive dependencies, but
    // substituteTemplateArguments takes too long.
    if (!sourceManager.isInMainFile(callExpr->getExprLoc()))
        return true;

    llvm::SmallVector<TemplateArgument, 4> writtenArgs;
    for (TemplateArgumentLoc argLoc : refExpr->template_arguments())
        writtenArgs.push_back(argLoc.getArgument());

    // Specialization called by this expr. Refers to unsugared types.
    auto* fspec = dyn_cast<FunctionDecl>(calleeDecl);
    if (!fspec)
        return true;
    FunctionTemplateSpecializationInfo* specInfo = fspec->getTemplateSpecializationInfo();
    if (!specInfo)
        return true;
    llvm::ArrayRef<TemplateArgument> args = specInfo->TemplateArguments->asArray();

    FunctionTemplateDecl* ftemplate = specInfo->getTemplate();
    SugaredSignature sig = substituteTemplateArguments(sema, ftemplate, writtenArgs, args);
    traverseSugaredSignature(sig);
    return true;
}

bool DependenciesCollector::VisitCallExpr(CallExpr* callExpr) {
    Expr* callee = callExpr->getCallee();
    Decl* calleeDecl = callExpr->getCalleeDecl();

    if (!callee || !calleeDecl || isa<UnresolvedMemberExpr>(callee) || isa<CXXDependentScopeMemberExpr>(callee))
        return true;

    insertReference(getCurrentDecl(), calleeDecl);

    return true;
}

bool DependenciesCollector::VisitOverloadExpr(OverloadExpr* overloadExpr) {
    // A dependent reference to an unresolved name inside a template body -- e.g.
    // `member<T>()`, represented as an UnresolvedMemberExpr, or an unqualified
    // `f<T>()`, an UnresolvedLookupExpr -- is not handled by VisitCallExpr (it
    // skips unresolved callees) and is never resolved through an instantiation
    // whose enclosing `if constexpr` branch is discarded. Without an edge to the
    // looked-up candidates, such a declaration can be pruned even though the
    // template body still names it, yielding output that no longer compiles.
    // Keep every candidate: over-keeping only leaves an extra declaration, while
    // dropping one that is named breaks the output.
    Decl* currentDecl = getCurrentDecl();
    for (NamedDecl* decl : overloadExpr->decls()) {
        insertReference(currentDecl, decl);
        // A template is removed based on the used-ness of its underlying
        // templated declaration (see OptimizerVisitor::VisitFunctionTemplateDecl),
        // so reference that too -- otherwise keeping only the (Function)TemplateDecl
        // would not keep the function/class body it wraps.
        if (auto* templateDecl = dyn_cast<TemplateDecl>(decl))
            insertReference(currentDecl, templateDecl->getTemplatedDecl());
    }

    return true;
}

bool DependenciesCollector::VisitCXXConstructExpr(CXXConstructExpr* constructorExpr) {
    insertReference(getCurrentDecl(), constructorExpr->getConstructor());
    return true;
}

#if CAIDE_CLANG_VERSION_AT_LEAST(10,0)
bool DependenciesCollector::TraverseConceptSpecializationExpr(ConceptSpecializationExpr* conceptExpr) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseConceptSpecializationExpr(conceptExpr);
    // XXX: We might need the following in stdlib for transitive dependencies, but
    // substituteTemplateArguments takes too long.
    if (!sourceManager.isInMainFile(conceptExpr->getExprLoc()))
        return true;

    ConceptDecl* conceptDecl = conceptExpr->getNamedConcept();
    const ASTTemplateArgumentListInfo* argsInfo = conceptExpr->getTemplateArgsAsWritten();
    llvm::SmallVector<TemplateArgument, 4> writtenArgs;
    for (auto argLoc : argsInfo->arguments())
        writtenArgs.push_back(argLoc.getArgument());

    SugaredSignature sig = substituteTemplateArguments(sema, conceptDecl, writtenArgs, {});
    traverseSugaredSignature(sig);
    return true;
}

bool DependenciesCollector::VisitConceptSpecializationExpr(ConceptSpecializationExpr* conceptExpr) {
    insertReference(getCurrentDecl(), conceptExpr->getNamedConcept());
    return true;
}
#endif

bool DependenciesCollector::VisitCXXConstructorDecl(CXXConstructorDecl* ctorDecl) {
#if CAIDE_CLANG_VERSION_AT_LEAST(3,9)
    CXXConstructorDecl* inheritedCtor = ctorDecl->getInheritedConstructor().getConstructor();
#else
    auto* inheritedCtor = const_cast<CXXConstructorDecl*>(ctorDecl->getInheritedConstructor());
#endif
    insertReference(ctorDecl, inheritedCtor);
    for (auto it = ctorDecl->init_begin(); it != ctorDecl->init_end(); ++it) {
        CXXCtorInitializer* ctorInit = *it;
        if (ctorInit->isWritten())
            insertReference(ctorDecl, ctorInit->getMember());
    }

    return true;
}

bool DependenciesCollector::VisitCXXDeductionGuideDecl(CXXDeductionGuideDecl* guide) {
    // A deduction guide is invoked implicitly by class template argument deduction,
    // which produces no explicit reference to the guide. Tie each guide to the class
    // template it deduces so a kept template keeps its guides; otherwise CTAD in the
    // generated output ("segment_tree seg(...)") fails to compile because the guide
    // that makes the deduction work was removed as apparently unused.
    if (TemplateDecl* deduced = guide->getDeducedTemplate()) {
        insertReference(deduced, guide);
        insertReference(deduced->getTemplatedDecl(), guide);
        if (FunctionTemplateDecl* guideTemplate = guide->getDescribedFunctionTemplate())
            insertReference(deduced, guideTemplate);
    }
    return true;
}

bool DependenciesCollector::VisitTemplateTypeParmDecl(TemplateTypeParmDecl* paramDecl) {
    // Reference from parent function/class template to its parameter, for transitivity.
    // TODO: This won't do the right thing for type aliases, as they're not a declaration context.
    insertReference(getParentDecl(paramDecl), paramDecl);

    return true;
}

bool DependenciesCollector::VisitDeclRefExpr(DeclRefExpr* ref) {
    Decl* currentDecl = getCurrentDecl();
    insertReference(currentDecl, ref->getDecl());
    insertReference(currentDecl, ref->getFoundDecl());
    return true;
}

bool DependenciesCollector::VisitValueDecl(ValueDecl* valueDecl) {
    // Mark any function as depending on its local variables.
    // TODO: detect unused local variables.
    insertReference(getCurrentFunction(valueDecl), valueDecl);
    return true;
}

bool DependenciesCollector::VisitVarDecl(VarDecl* varDecl) {
    insertReference(varDecl, varDecl->getDescribedVarTemplate());

    if (!sourceManager.isInMainFile(getExpansionStart(sourceManager, varDecl)) || !varDecl->isFileVarDecl() ||
            (varDecl->getStorageDuration() != SD_Static && varDecl->getStorageDuration() != SD_Thread) ||
            varDecl->getDescribedVarTemplate() || isa<VarTemplateSpecializationDecl>(varDecl) ||
            varDecl->getDeclContext()->isDependentContext() ||
            varDecl->isThisDeclarationADefinition() == VarDecl::DeclarationOnly)
        return true;

    for (DeclContext* ctx = varDecl->getDeclContext(); ctx; ctx = ctx->getParent()) {
        if (ctx->isFunctionOrMethod())
            return true;
        if (auto* record = dyn_cast<CXXRecordDecl>(ctx)) {
            if (record->getDescribedClassTemplate() || isa<ClassTemplateSpecializationDecl>(record) ||
                    record->getInstantiatedFromMemberClass())
                return true;
        }
    }

    // Initialization and destruction of non-template globals can affect the
    // program without any reference to their names. Keep them conservatively;
    // template variables/members are instantiated only when used, and locals
    // are kept with their function rather than becoming independent roots.
    ASTContext& ctx = varDecl->getASTContext();
#if CAIDE_CLANG_VERSION_AT_LEAST(12, 0)
    const bool dynamicInit = varDecl->hasInit() && !varDecl->hasConstantInitialization();
#else
    const bool dynamicInit = varDecl->hasInit() && !varDecl->checkInitIsICE();
#endif
#if CAIDE_CLANG_VERSION_AT_LEAST(10, 0)
    const bool needsDestructor = varDecl->needsDestruction(ctx) == QualType::DK_cxx_destructor;
#else
    const bool needsDestructor = varDecl->getType().isDestructedType() == QualType::DK_cxx_destructor;
    (void)ctx;
#endif
    if (dynamicInit || needsDestructor)
        keepRoot(srcInfo, varDecl, RootReason::SideEffect);
    return true;
}

// X->F and X.F
bool DependenciesCollector::VisitMemberExpr(MemberExpr* memberExpr) {
    Decl* currentDecl = getCurrentDecl();
    // getFoundDecl() returns either MemberDecl itself or UsingShadowDecl corresponding to a UsingDecl
    insertReference(currentDecl, memberExpr->getFoundDecl().getDecl());
    return true;
}

bool DependenciesCollector::VisitInitListExpr(InitListExpr* initList) {
    // Aggregate (braced) initialization -- `return {a, b, c};` -- writes a
    // record's fields positionally, producing no MemberExpr for each one. A field
    // set only this way and never read by name would therefore get no incoming
    // edge and be removed as unused; the surviving brace list then no longer
    // matches the shrunken struct ("could not convert brace-enclosed initializer
    // list"). Aggregate initialization also depends on the exact field layout
    // (initializers bind to fields in declaration order), so a used aggregate must
    // keep its whole struct: depend on every field of the initialized record.
    if (const RecordDecl* record = initList->getType()->getAsRecordDecl()) {
        Decl* currentDecl = getCurrentDecl();
        for (FieldDecl* field : record->fields())
            insertReference(currentDecl, field);
    }
    return true;
}

// A using declaration has an additional shadow declaration for each declaration
// that it brings into scope.
bool DependenciesCollector::VisitUsingShadowDecl(UsingShadowDecl* usingShadowDecl) {
    // Add dependency on the actually written using declaration.
#if CAIDE_CLANG_VERSION_AT_LEAST(13,0)
    insertReference(usingShadowDecl, usingShadowDecl->getIntroducer());
#else
    insertReference(usingShadowDecl, usingShadowDecl->getUsingDecl());
#endif

    insertReference(usingShadowDecl, usingShadowDecl->getTargetDecl());
    return true;
}

// The primary-template record of a (possibly dependent) base type, or the record
// itself for a non-dependent base -- the definition in which to look up a member
// named by a using-declaration.
static CXXRecordDecl* baseTemplatePattern(QualType baseType) {
    baseType = baseType.getCanonicalType();
    if (const auto* tst = baseType->getAs<TemplateSpecializationType>()) {
        if (auto* ctd = dyn_cast_or_null<ClassTemplateDecl>(tst->getTemplateName().getAsTemplateDecl()))
            return ctd->getTemplatedDecl();
    }
    return baseType->getAsCXXRecordDecl();
}

// Look `name` up in `record` and, transitively, the patterns of its (possibly
// dependent) bases, appending every match. A using-declaration may name a member
// inherited from a grandparent base, and CXXRecordDecl::lookup only reports members
// of the record itself, so the base hierarchy must be walked by hand.
static void lookupInBasesTransitively(CXXRecordDecl* record, DeclarationName name,
        llvm::SmallVectorImpl<NamedDecl*>& out, llvm::SmallPtrSetImpl<const CXXRecordDecl*>& visited) {
    if (!record || !record->hasDefinition() || !visited.insert(record).second)
        return;
    for (NamedDecl* found : record->lookup(name))
        out.push_back(found);
    for (const CXXBaseSpecifier& base : record->bases())
        lookupInBasesTransitively(baseTemplatePattern(base.getType()), name, out, visited);
}

// The type a using-declaration's qualifier names (the `base` in `using base::x;`),
// or null when the qualifier is not a type. NestedNameSpecifier became a value type
// in clang 22.
static const clang::Type* usingQualifierType(const UnresolvedUsingValueDecl* usingDecl) {
#if CAIDE_CLANG_VERSION_AT_LEAST(22, 0)
    NestedNameSpecifier qualifier = usingDecl->getQualifier();
    if (qualifier.getKind() == NestedNameSpecifier::Kind::Type)
        return qualifier.getAsType();
    return nullptr;
#else
    if (NestedNameSpecifier* qualifier = usingDecl->getQualifier())
        return qualifier->getAsType();
    return nullptr;
#endif
}

// A member using-declaration in a class template (`using base::build;`) that names
// a member of a *dependent* base is an UnresolvedUsingValueDecl: at the template
// definition the base is dependent, so the declaration is tied to no target. The
// optimizer keeps such using-declarations but removes base members individually, so
// a member named only through the using-declaration (never called in a reachable
// method of the derived class) is removed while the `using base::X;` that names it
// stays -- and the output no longer compiles. Resolve the name against the base
// hierarchy's patterns and make the derived class depend on the members it finds.
bool DependenciesCollector::VisitUnresolvedUsingValueDecl(UnresolvedUsingValueDecl* usingDecl) {
    auto* derived = dyn_cast_or_null<CXXRecordDecl>(usingDecl->getDeclContext());
    if (!derived || !derived->hasDefinition())
        return true;

    // Keep the qualifier the using-declaration names (e.g. the `using base = ...;`
    // alias in `using base::x;`). Nothing reachable references it when none of the
    // pulled-in members are used, so it would otherwise be removed and the
    // using-declaration left dangling on an undeclared name.
    if (const Type* qualifierType = usingQualifierType(usingDecl)) {
        if (const auto* typedefType = qualifierType->getAs<TypedefType>())
            insertReference(derived, typedefType->getDecl());
        insertReference(derived, qualifierType->getAsCXXRecordDecl());
    }

    DeclarationName name = usingDecl->getNameInfo().getName();
    llvm::SmallVector<NamedDecl*, 4> found;
    llvm::SmallPtrSet<const CXXRecordDecl*, 8> visited;
    for (const CXXBaseSpecifier& base : derived->bases())
        lookupInBasesTransitively(baseTemplatePattern(base.getType()), name, found, visited);

    for (NamedDecl* decl : found) {
        insertReference(derived, decl);
        if (auto* templateDecl = dyn_cast<TemplateDecl>(decl))
            insertReference(derived, templateDecl->getTemplatedDecl());
    }
    return true;
}

bool DependenciesCollector::VisitLambdaExpr(LambdaExpr* lambdaExpr) {
    insertReference(getCurrentDecl(), lambdaExpr->getCallOperator());
    return true;
}

bool DependenciesCollector::VisitFieldDecl(FieldDecl* field) {
    insertReference(field, field->getParent());
    // Every non-static field contributes to layout, even unnamed bit-fields and
    // anonymous records. Keeping the field also keeps its type and initializer.
    insertReference(field->getParent(), field);
    return true;
}

bool DependenciesCollector::VisitTypeAliasDecl(TypeAliasDecl* aliasDecl) {
    insertReference(aliasDecl, aliasDecl->getDescribedAliasTemplate());
    return true;
}

bool DependenciesCollector::VisitTypeAliasTemplateDecl(TypeAliasTemplateDecl* aliasTemplateDecl) {
    insertReference(aliasTemplateDecl, aliasTemplateDecl->getInstantiatedFromMemberTemplate());
    // Dependency on the single (pattern) TypeAlias associated with this template.
    insertReference(aliasTemplateDecl, aliasTemplateDecl->getTemplatedDecl());
    recordTemplateParameters(aliasTemplateDecl);
    return true;
}

bool DependenciesCollector::VisitClassTemplateDecl(ClassTemplateDecl* templateDecl) {
    insertReference(templateDecl, templateDecl->getTemplatedDecl());
    recordTemplateParameters(templateDecl);

    // A partial specialization is chosen only when the template is instantiated,
    // which we don't visit, so nothing otherwise ties it to the primary. Keep every
    // partial specialization of a used class template: any instantiation may select
    // it, and dropping one silently changes which specialization wins (e.g. a
    // `range_rank_v` used only inside a deduction-guide constraint then reads the
    // primary's value instead of the partial specialization's).
    llvm::SmallVector<ClassTemplatePartialSpecializationDecl*, 4> partials;
    templateDecl->getPartialSpecializations(partials);
    for (ClassTemplatePartialSpecializationDecl* partial : partials)
        insertReference(templateDecl, partial);
    return true;
}

bool DependenciesCollector::VisitVarTemplateDecl(VarTemplateDecl* templateDecl) {
    // Same as class templates: keep every partial specialization of a used variable
    // template, since the one selected is only decided at instantiation time.
    llvm::SmallVector<VarTemplatePartialSpecializationDecl*, 4> partials;
    templateDecl->getPartialSpecializations(partials);
    for (VarTemplatePartialSpecializationDecl* partial : partials)
        insertReference(templateDecl, partial);
    return true;
}

bool DependenciesCollector::TraverseClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* specDecl) {
    dbg(CAIDE_FUNC);
    RecursiveASTVisitor::TraverseClassTemplateSpecializationDecl(specDecl);

    llvm::PointerUnion<ClassTemplateDecl*, ClassTemplatePartialSpecializationDecl*>
        instantiatedFrom = specDecl->getSpecializedTemplateOrPartial();

    if (auto* partial = dyn_cast_if_present<ClassTemplatePartialSpecializationDecl*>(instantiatedFrom)) {
        // template<typename T>
        // class X<Foo<T>, Bar<T>> {...}
        // -> X<Foo<int>, Bar<int>>
        //
        // templateArgsAsWritten == [Foo<T>, Bar<T>]
        // instantiatedWithArgs = [int]
        //
        // To obtain correct dependencies, substitute instantiatedWithArgs into templateArgsAsWritten.
        const TemplateArgumentList& instantiatedWithArgs = specDecl->getTemplateInstantiationArgs();
        SugaredSignature sig = substituteTemplateArguments(sema, partial, instantiatedWithArgs.asArray());
        traverseSugaredSignature(sig);
    }

    return true;
}

bool DependenciesCollector::VisitClassTemplateSpecializationDecl(ClassTemplateSpecializationDecl* specDecl) {
    llvm::PointerUnion<ClassTemplateDecl*, ClassTemplatePartialSpecializationDecl*>
        instantiatedFrom = specDecl->getSpecializedTemplateOrPartial();

    if (auto* tempDecl = dyn_cast_if_present<ClassTemplateDecl*>(instantiatedFrom)) {
        insertReference(specDecl, tempDecl);
    } else if (auto* partial = dyn_cast_if_present<ClassTemplatePartialSpecializationDecl*>(instantiatedFrom)) {
        insertReference(specDecl, partial);
    }

    return true;
}

bool DependenciesCollector::VisitVarTemplateSpecializationDecl(clang::VarTemplateSpecializationDecl* specDecl) {
    llvm::PointerUnion<VarTemplateDecl*, VarTemplatePartialSpecializationDecl*>
        instantiatedFrom = specDecl->getSpecializedTemplateOrPartial();

    if (auto* tempDecl = dyn_cast_if_present<VarTemplateDecl*>(instantiatedFrom)) {
        insertReference(specDecl, tempDecl);
    } else if (auto* partial = dyn_cast_if_present<VarTemplatePartialSpecializationDecl*>(instantiatedFrom)) {
        insertReference(specDecl, partial);
    }

    return true;
}

/*
Every function template is represented as a FunctionTemplateDecl and a FunctionDecl
(or something derived from FunctionDecl). The former contains template properties
(such as the template parameter lists) while the latter contains the actual description
of the template's contents. FunctionTemplateDecl::getTemplatedDecl() retrieves the
FunctionDecl that describes the function template,
FunctionDecl::getDescribedFunctionTemplate() retrieves the FunctionTemplateDecl
from a FunctionDecl.

We only use FunctionDecl's for dependency tracking.
 */
bool DependenciesCollector::VisitFunctionDecl(FunctionDecl* f) {
    if (f->isMain())
        keepRoot(srcInfo, f, RootReason::Main);

    if (sourceManager.isInMainFile(getBeginLoc(f)) && f->isLateTemplateParsed())
        srcInfo.delayedParsedFunctions.push_back(f);

    // The base visitor does not traverse the requires-clause on a constructor
    // template's parameter list (a plain function or method template's is fine).
    // A concept named only there produces no dependency edge, so the concept
    // definition is pruned while the surviving requires-clause still references
    // it -> the generated file fails to compile ("undeclared identifier"). Walk
    // it explicitly here (f is the current decl, so VisitConceptSpecializationExpr
    // records the edge from this constructor to the concept).
    if (isa<CXXConstructorDecl>(f)) {
        if (FunctionTemplateDecl* ctorTemplate = f->getDescribedFunctionTemplate())
            if (TemplateParameterList* params = ctorTemplate->getTemplateParameters())
                if (Expr* requiresClause = params->getRequiresClause())
                    TraverseStmt(requiresClause);
    }

    if (f->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate) {
        // skip non-instantiated template function
        return true;
    }

    if (FunctionTemplateSpecializationInfo* specInfo = f->getTemplateSpecializationInfo()) {
        FunctionTemplateDecl* ftemplate = specInfo->getTemplate();
        insertReference(f, ftemplate->getTemplatedDecl());
    }

    insertReference(f, f->getInstantiatedFromMemberFunction());

    if (f->doesThisDeclarationHaveABody() &&
            sourceManager.isInMainFile(getBeginLoc(f)))
    {
        dbg("Moving to ";
            DeclarationName DeclName = f->getNameInfo().getName();
            std::string FuncName = DeclName.getAsString();
            std::cerr << FuncName << " at " <<
                toString(sourceManager, f->getLocation()) << std::endl;
        );
    }

    return true;
}

bool DependenciesCollector::VisitFunctionTemplateDecl(FunctionTemplateDecl* functionTemplate) {
    insertReference(functionTemplate,
            functionTemplate->getInstantiatedFromMemberTemplate());
    recordTemplateParameters(functionTemplate);
    return true;
}

bool DependenciesCollector::VisitCXXMethodDecl(CXXMethodDecl* method) {
    insertReference(method, method->getParent());
    // User-declared special members affect traits, aggregate-ness and implicit
    // members even when never called, including defaulted and deleted members.
    if (!method->isImplicit()) {
        bool specialMember = isa<CXXDestructorDecl>(method) ||
            method->isCopyAssignmentOperator() || method->isMoveAssignmentOperator();
        if (auto* ctor = dyn_cast<CXXConstructorDecl>(method))
            specialMember = ctor->isDefaultConstructor() || ctor->isCopyOrMoveConstructor();
        if (specialMember)
            insertReference(method->getParent(), method);
    }
    if (method->isVirtual()) {
        // Virtual methods may not be called directly. Assume that
        // if we need a class, we need all its virtual methods.
        // TODO: a more detailed analysis (walk the inheritance tree?)
        insertReference(method->getParent(), method);
    }
    return true;
}

bool DependenciesCollector::VisitCXXRecordDecl(CXXRecordDecl* recordDecl) {
    insertReference(recordDecl, recordDecl->getDescribedClassTemplate());
    // No implicit calls to destructors in AST; assume that
    // if a class is used, its destructor is used too.
    insertReference(recordDecl, recordDecl->getDestructor());
    return true;
}

bool DependenciesCollector::VisitEnumDecl(EnumDecl* enumDecl) {
    // Removing an unused enum constant can change values of used constants of the same enum.
    // So we assume that either the whole enum is used or it is unused. For this purpose, insert
    // bidirectional dependency links connecting the enum and each enum constant.
    for (auto it = enumDecl->enumerator_begin(); it != enumDecl->enumerator_end(); ++it) {
        insertReference(enumDecl, *it);
        insertReference(*it, enumDecl);
    }

    return true;
}

bool DependenciesCollector::VisitStmt(clang::Stmt* stmt) {
    (void)stmt;
    dbg(stmt->getStmtClassName() << std::endl);
    // stmt->dump();
    return true;
}

void DependenciesCollector::printGraph(std::ostream& out) const {
    auto locToStr = [&](const SourceLocation loc) {
        std::ostringstream str;
        str << sourceManager.getExpansionLineNumber(loc) << ":"
            << sourceManager.getExpansionColumnNumber(loc);
        return str.str();
    };
    auto getNodeId = [&](const Decl* decl) {
        SourceRange range = decl->getSourceRange();
        std::ostringstream str;
        str << '"' << decl->getDeclKindName() << "\\n";

        if (const auto* namedDecl = dyn_cast<NamedDecl>(decl))
            str << namedDecl->getNameAsString() << "\\n";

        str << decl << "\\n"
            << "<" << locToStr(range.getBegin()) << "-" << locToStr(range.getEnd()) << ">"
            << '"';
        return str.str();
    };

    const auto& graph = srcInfo.uses;
    out << "digraph {\n";
    for (const auto& it : graph) if (it.first) {
        std::string fromStr = getNodeId(it.first);
        out << fromStr << "\n";
        for (const Decl* to : it.second) if (to)
            out << fromStr << " -> " << getNodeId(to) << "\n";
    }

    out << "}\n";
}


}
}

