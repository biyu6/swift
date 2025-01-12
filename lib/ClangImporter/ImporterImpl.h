//===--- ImporterImpl.h - Import Clang Modules - Implementation------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides the implementation class definitions for the Clang
// module loader.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_CLANG_IMPORTER_IMPL_H
#define SWIFT_CLANG_IMPORTER_IMPL_H

#include "SwiftLookupTable.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/Module.h"
#include "swift/AST/Type.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/Basic/StringExtras.h"
#include "clang/APINotes/APINotesReader.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Serialization/ModuleFileExtension.h"
#include "clang/AST/Attr.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <set>

namespace llvm {

class SmallBitVector;

}

namespace clang {
class APValue;
class Decl;
class DeclarationName;
class EnumDecl;
class MacroInfo;
class MangleContext;
class NamedDecl;
class ObjCInterfaceDecl;
class ObjCMethodDecl;
class ObjCPropertyDecl;
class ParmVarDecl;
class Parser;
class QualType;
class TypedefNameDecl;
}

namespace swift {

class ASTContext;
class ClangModuleUnit;
class ClassDecl;
class ConstructorDecl;
class Decl;
class DeclContext;
class Expr;
class ExtensionDecl;
class FuncDecl;
class Identifier;
class Pattern;
class SubscriptDecl;
class ValueDecl;

/// \brief Describes the kind of conversion to apply to a constant value.
enum class ConstantConvertKind {
  /// \brief No conversion required.
  None,
  /// \brief Coerce the constant to the given type.
  Coerce,
  /// \brief Construct the given type from the constant value.
  Construction,
  /// \brief Construct the given type from the constant value, using an
  /// optional initializer.
  ConstructionWithUnwrap,
  /// \brief Perform an unchecked downcast to the given type.
  Downcast
};

/// \brief Describes the kind of type import we're performing.
enum class ImportTypeKind {
  /// \brief Import a type in its most abstract form, without any adjustment.
  Abstract,

  /// \brief Import the underlying type of a typedef.
  Typedef,

  /// \brief Import the type of a literal value.
  Value,

  /// \brief Import the type of a literal value that can be bridged.
  BridgedValue,

  /// \brief Import the declared type of a variable.
  Variable,
  
  /// \brief Import the declared type of an audited variable.
  ///
  /// This is exactly like ImportTypeKind::Variable, except it
  /// disables wrapping CF class types in Unmanaged.
  AuditedVariable,

  /// \brief Import the declared type of a struct or union field.
  RecordField,
  
  /// \brief Import the result type of a function.
  ///
  /// This provides special treatment for 'void', among other things, and
  /// enables the conversion of bridged types.
  Result,

  /// \brief Import the result type of an audited function.
  ///
  /// This is exactly like ImportTypeKind::Result, except it
  /// disables wrapping CF class types in Unmanaged.
  AuditedResult,

  /// \brief Import the type of a function parameter.
  ///
  /// This provides special treatment for C++ references (which become
  /// [inout] parameters) and C pointers (which become magic [inout]-able types),
  /// among other things, and enables the conversion of bridged types.
  /// Parameters are always considered CF-audited.
  Parameter,

  /// \brief Import the type of a parameter declared with
  /// \c CF_RETURNS_RETAINED.
  ///
  /// This ensures that the parameter is not marked as Unmanaged.
  CFRetainedOutParameter,

  /// \brief Import the type of a parameter declared with
  /// \c CF_RETURNS_NON_RETAINED.
  ///
  /// This ensures that the parameter is not marked as Unmanaged.
  CFUnretainedOutParameter,

  /// \brief Import the type pointed to by a pointer or reference.
  ///
  /// This provides special treatment for pointer-to-ObjC-pointer
  /// types, which get imported as pointers to *checked* optional,
  /// *Pointer<NSFoo?>, instead of implicitly unwrapped optional as usual.
  Pointee,

  /// \brief Import the type of an ObjC property.
  ///
  /// This enables the conversion of bridged types. Properties are always
  /// considered CF-audited.
  Property,

  /// \brief Import the type of an ObjC property accessor.
  ///
  /// This behaves exactly like Property except that it accepts Void.
  PropertyAccessor,

  /// \brief Import the underlying type of an enum.
  ///
  /// This provides special treatment for 'NSUInteger'.
  Enum
};

/// \brief Describes the kind of the C type that can be mapped to a stdlib
/// swift type.
enum class MappedCTypeKind {
  UnsignedInt,
  SignedInt,
  UnsignedWord,
  SignedWord,
  FloatIEEEsingle,
  FloatIEEEdouble,
  FloatX87DoubleExtended,
  VaList,
  ObjCBool,
  ObjCSel,
  ObjCId,
  ObjCClass,
  CGFloat,
  Block,
};

/// \brief Describes what to do with the C name of a type that can be mapped to
/// a Swift standard library type.
enum class MappedTypeNameKind {
  DoNothing,
  DefineOnly,
  DefineAndUse
};

/// \brief Describes certain kinds of methods that need to be specially
/// handled by the importer.
enum class SpecialMethodKind {
  Regular,
  Constructor,
  PropertyAccessor,
  NSDictionarySubscriptGetter
};

#define SWIFT_NATIVE_ANNOTATION_STRING "__swift native"

#define SWIFT_PROTOCOL_SUFFIX "Protocol"
#define SWIFT_CFTYPE_SUFFIX "Ref"

namespace api_notes = clang::api_notes;
using api_notes::FactoryAsInitKind;

/// \brief Implementation of the Clang importer.
class LLVM_LIBRARY_VISIBILITY ClangImporter::Implementation 
  : public LazyMemberLoader, public clang::ModuleFileExtension
{
  friend class ClangImporter;

public:
  /// \brief Describes how a particular C enumeration type will be imported
  /// into Swift. All of the possibilities have the same storage
  /// representation, but can be used in different ways.
  enum class EnumKind {
    /// \brief The enumeration type should map to an enum, which means that
    /// all of the cases are independent.
    Enum,
    /// \brief The enumeration type should map to an option set, which means that
    /// the constants represent combinations of independent flags.
    Options,
    /// \brief The enumeration type should map to a distinct type, but we don't
    /// know the intended semantics of the enum constants, so conservatively
    /// map them to independent constants.
    Unknown,
    /// \brief The enumeration constants should simply map to the appropriate
    /// integer values.
    Constants
  };

  Implementation(ASTContext &ctx, const ClangImporterOptions &opts);
  ~Implementation();

  /// \brief Swift AST context.
  ASTContext &SwiftContext;

  const bool ImportForwardDeclarations;
  const bool OmitNeedlessWords;
  const bool InferDefaultArguments;
  const bool UseSwiftLookupTables;

  constexpr static const char * const moduleImportBufferName =
    "<swift-imported-modules>";
  constexpr static const char * const bridgingHeaderBufferName =
    "<bridging-header-import>";

private:
  /// \brief A count of the number of load module operations.
  /// FIXME: Horrible, horrible hack for \c loadModule().
  unsigned ImportCounter = 0;

  /// \brief The value of \c ImportCounter last time when imported modules were
  /// verified.
  unsigned VerifiedImportCounter = 0;

  /// \brief Clang compiler invocation.
  llvm::IntrusiveRefCntPtr<clang::CompilerInvocation> Invocation;

  /// \brief Clang compiler instance, which is used to actually load Clang
  /// modules.
  std::unique_ptr<clang::CompilerInstance> Instance;

  /// \brief Clang compiler action, which is used to actually run the
  /// parser.
  std::unique_ptr<clang::FrontendAction> Action;

  /// \brief Clang parser, which is used to load textual headers.
  std::unique_ptr<clang::Parser> Parser;

  /// \brief Clang parser, which is used to load textual headers.
  std::unique_ptr<clang::MangleContext> Mangler;

  /// The active type checker, or null if there is no active type checker.
  ///
  /// The flag is \c true if there has ever been a type resolver assigned, i.e.
  /// if type checking has begun.
  llvm::PointerIntPair<LazyResolver *, 1, bool> typeResolver;

  /// The Swift lookup table for the bridging header.
  SwiftLookupTable BridgingHeaderLookupTable;

  /// The Swift lookup tables, per module.
  llvm::StringMap<std::unique_ptr<SwiftLookupTable>> LookupTables;

public:
  /// \brief Mapping of already-imported declarations.
  llvm::DenseMap<const clang::Decl *, Decl *> ImportedDecls;

  /// \brief The set of "special" typedef-name declarations, which are
  /// mapped to specific Swift types.
  ///
  /// Normal typedef-name declarations imported into Swift will maintain
  /// equality between the imported declaration's underlying type and the
  /// import of the underlying type. A typedef-name declaration is special
  /// when this is not the case, e.g., Objective-C's "BOOL" has an underlying
  /// type of "signed char", but is mapped to a special Swift struct type
  /// ObjCBool.
  llvm::SmallDenseMap<const clang::TypedefNameDecl *, MappedTypeNameKind, 16>
    SpecialTypedefNames;

  /// Is the given identifier a reserved name in Swift?
  static bool isSwiftReservedName(StringRef name);

  /// Translation API nullability from an API note into an optional kind.
  static OptionalTypeKind translateNullability(clang::NullabilityKind kind);

  /// Retrieve the API notes readers that may contain information for the
  /// given Objective-C container.
  ///
  /// \returns a (name, primary, secondary) tuple containing the name of the
  /// entity to look for and the API notes readers where information could be
  /// found. The "primary" reader is the reader describes the module where the
  /// specific container is defined; the "secondary" reader describes the
  /// module in which the type is originally defined, if it's different from
  /// the primary. Either or both of the readers may be null.
  std::tuple<StringRef, api_notes::APINotesReader*, api_notes::APINotesReader*>
  getAPINotesForContext(const clang::ObjCContainerDecl *container);

  /// Retrieve the API notes reader that contains information for the
  /// given declaration. Note, use getAPINotesForContext to get notes for ObjC
  /// properties and methods.
  api_notes::APINotesReader* getAPINotesForDecl(const clang::Decl *decl);

  /// Retrieve any information known a priori about the given Objective-C
  /// method, if we have it.
  ///
  /// If \p container is specified, we're looking for a method with the same
  /// selector and instance-ness in \p container.
  Optional<api_notes::ObjCMethodInfo>
  getKnownObjCMethod(const clang::ObjCMethodDecl *method,
                     const clang::ObjCContainerDecl *container = nullptr);

  /// For ObjC property accessor, if the property is known, lookup
  /// the property info and merge it in.
  void mergePropInfoIntoAccessor(const clang::ObjCMethodDecl *method,
                                 api_notes::ObjCMethodInfo &methodInfo);

  /// Retrieve information about the given Objective-C context scoped to the
  /// given Swift module.
  Optional<api_notes::ObjCContextInfo>
  getKnownObjCContext(const clang::ObjCContainerDecl *container);

  /// Retrieve any information known a priori about the given Objective-C
  /// property.
  Optional<api_notes::ObjCPropertyInfo>
  getKnownObjCProperty(const clang::ObjCPropertyDecl *property);

  /// Retrieve any information known a priori about the given global variable.
  Optional<api_notes::GlobalVariableInfo>
  getKnownGlobalVariable(const clang::VarDecl *global);

  /// Retrieve any information known a priori about the given global function.
  Optional<api_notes::GlobalFunctionInfo>
  getKnownGlobalFunction(const clang::FunctionDecl *function);

  /// Determine whether the given class has designated initializers,
  /// consulting 
  bool hasDesignatedInitializers(const clang::ObjCInterfaceDecl *classDecl);

  /// Determine whether the given method is a designated initializer
  /// of the given class.
  bool isDesignatedInitializer(const clang::ObjCInterfaceDecl *classDecl,
                               const clang::ObjCMethodDecl *method);

  /// Determine whether the given method is a required initializer
  /// of the given class.
  bool isRequiredInitializer(const clang::ObjCMethodDecl *method);

  /// Determine whether the given class method should be imported as
  /// an initializer.
  FactoryAsInitKind getFactoryAsInit(const clang::ObjCInterfaceDecl *classDecl,
                                     const clang::ObjCMethodDecl *method);

  /// \brief Typedefs that we should not be importing.  We should be importing
  /// underlying decls instead.
  llvm::DenseSet<const clang::Decl *> SuperfluousTypedefs;
  /// Tag decls whose typedefs were imported instead.
  ///
  /// \sa SuperfluousTypedefs
  llvm::DenseSet<const clang::Decl *> DeclsWithSuperfluousTypedefs;

  using ClangDeclAndFlag = llvm::PointerIntPair<const clang::Decl *, 1, bool>;

  /// \brief Mapping of already-imported declarations from protocols, which
  /// can (and do) get replicated into classes.
  llvm::DenseMap<std::pair<ClangDeclAndFlag, DeclContext *>, Decl *>
    ImportedProtocolDecls;

  /// \brief Mapping of already-imported macros.
  llvm::DenseMap<clang::MacroInfo *, ValueDecl *> ImportedMacros;

  /// Keeps track of active selector-basde lookups, so that we don't infinitely
  /// recurse when checking whether a method with a given selector has already
  /// been imported.
  llvm::DenseMap<std::pair<ObjCSelector, char>, unsigned>
    ActiveSelectors;

  // FIXME: An extra level of caching of visible decls, since lookup needs to
  // be filtered by module after the fact.
  SmallVector<ValueDecl *, 0> CachedVisibleDecls;
  enum class CacheState {
    Invalid,
    InProgress,
    Valid
  } CurrentCacheState = CacheState::Invalid;

  /// Whether we should suppress the import of the given Clang declaration.
  static bool shouldSuppressDeclImport(const clang::Decl *decl);

  /// \brief Check if the declaration is one of the specially handled
  /// accessibility APIs.
  ///
  /// These appaer as both properties and methods in ObjC and should be
  /// imported as methods into Swift.
  static bool isAccessibilityDecl(const clang::Decl *objCMethodOrProp);

  /// Determine whether this method is an Objective-C "init" method
  /// that will be imported as a Swift initializer.
  bool isInitMethod(const clang::ObjCMethodDecl *method);

  /// Determine whether this Objective-C method should be imported as
  /// an initializer.
  ///
  /// \param prefixLength Will be set to the length of the prefix that
  /// should be stripped from the first selector piece, e.g., "init"
  /// or the restated name of the class in a factory method.
  ///
  ///  \param kind Will be set to the kind of initializer being
  ///  imported. Note that this does not distinguish designated
  ///  vs. convenience; both will be classified as "designated".
  bool shouldImportAsInitializer(const clang::ObjCMethodDecl *method,
                                 unsigned &prefixLength,
                                 CtorInitializerKind &kind);

private:
  /// \brief Generation number that is used for crude versioning.
  ///
  /// This value is incremented every time a new module is imported.
  unsigned Generation = 1;

  /// \brief A cached set of extensions for a particular Objective-C class.
  struct CachedExtensions {
    CachedExtensions()
      : Extensions(nullptr), Generation(0) { }

    CachedExtensions(const CachedExtensions &) = delete;
    CachedExtensions &operator=(const CachedExtensions &) = delete;

    CachedExtensions(CachedExtensions &&other)
      : Extensions(other.Extensions), Generation(other.Generation)
    {
      other.Extensions = nullptr;
      other.Generation = 0;
    }

    CachedExtensions &operator=(CachedExtensions &&other) {
      delete Extensions;
      Extensions = other.Extensions;
      Generation = other.Generation;
      other.Extensions = nullptr;
      other.Generation = 0;
      return *this;
    }

    ~CachedExtensions() { delete Extensions; }

    /// \brief The cached extensions.
    SmallVector<ExtensionDecl *, 4> *Extensions;

    /// \brief Generation number used to tell when this cache has gone stale.
    unsigned Generation;
  };

  void bumpGeneration() {
    ++Generation;
    SwiftContext.bumpGeneration();
    CachedVisibleDecls.clear();
    CurrentCacheState = CacheState::Invalid;
  }

  /// \brief Cache of the class extensions.
  llvm::DenseMap<ClassDecl *, CachedExtensions> ClassExtensions;

public:
  /// \brief Keep track of subscript declarations based on getter/setter
  /// pairs.
  llvm::DenseMap<std::pair<FuncDecl *, FuncDecl *>, SubscriptDecl *> Subscripts;

  /// \brief Keep track of enum constant name prefixes in enums.
  llvm::DenseMap<const clang::EnumDecl *, StringRef> EnumConstantNamePrefixes;

private:
  class EnumConstantDenseMapInfo {
  public:
    using PairTy = std::pair<const clang::EnumDecl *, llvm::APSInt>;
    using PointerInfo = llvm::DenseMapInfo<const clang::EnumDecl *>;
    static inline PairTy getEmptyKey() {
      return {PointerInfo::getEmptyKey(), llvm::APSInt(/*bitwidth=*/1)};
    }
    static inline PairTy getTombstoneKey() {
      return {PointerInfo::getTombstoneKey(), llvm::APSInt(/*bitwidth=*/1)};
    }
    static unsigned getHashValue(const PairTy &pair) {
      return llvm::combineHashValue(PointerInfo::getHashValue(pair.first),
                                    llvm::hash_value(pair.second));
    }
    static bool isEqual(const PairTy &lhs, const PairTy &rhs) {
      return lhs == rhs;
    }
  };

  /// Retrieve the prefix to be stripped from the names of the enum constants
  /// within the given enum.
  StringRef getEnumConstantNamePrefix(clang::Sema &sema,
                                      const clang::EnumDecl *enumDecl);

public:
  /// \brief Keep track of enum constant values that have been imported.
  llvm::DenseMap<std::pair<const clang::EnumDecl *, llvm::APSInt>,
                 EnumElementDecl *,
                 EnumConstantDenseMapInfo>
    EnumConstantValues;

  /// \brief Keep track of initializer declarations that correspond to
  /// imported methods.
  llvm::DenseMap<std::pair<const clang::ObjCMethodDecl *, DeclContext *>,
                 ConstructorDecl *>
    Constructors;

  /// A mapping from imported declarations to their "alternate" declarations,
  /// for cases where a single Clang declaration is imported to two
  /// different Swift declarations.
  llvm::DenseMap<Decl *, ValueDecl *> AlternateDecls;

  /// Retrieve the alternative declaration for the given imported
  /// Swift declaration.
  ValueDecl *getAlternateDecl(Decl *decl) {
    auto known = AlternateDecls.find(decl);
    if (known == AlternateDecls.end()) return nullptr;
    return known->second;
  }

private:
  /// \brief NSObject, imported into Swift.
  Type NSObjectTy;

  /// A pair containing a ClangModuleUnit,
  /// and whether the adapters of its re-exported modules have all been forced
  /// to load already.
  using ModuleInitPair = llvm::PointerIntPair<ClangModuleUnit *, 1, bool>;

public:
  /// A map from Clang modules to their Swift wrapper modules.
  llvm::SmallDenseMap<const clang::Module *, ModuleInitPair, 16> ModuleWrappers;

  /// A map from Clang modules to their associated API notes.
  llvm::SmallDenseMap<
    const clang::Module *,
    std::unique_ptr<api_notes::APINotesReader>> APINotesReaders;

  /// The module unit that contains declarations from imported headers.
  ClangModuleUnit *ImportedHeaderUnit = nullptr;

  /// The modules re-exported by imported headers.
  llvm::SmallVector<Module::ImportedModule, 8> ImportedHeaderExports;

  /// The modules that requested imported headers.
  ///
  /// These are used to look up Swift classes forward-declared with \@class.
  TinyPtrVector<Module *> ImportedHeaderOwners;

  /// \brief Clang's objectAtIndexedSubscript: selector.
  clang::Selector objectAtIndexedSubscript;

  /// \brief Clang's setObjectAt:indexedSubscript: selector.
  clang::Selector setObjectAtIndexedSubscript;

  /// \brief Clang's objectForKeyedSubscript: selector.
  clang::Selector objectForKeyedSubscript;

  /// \brief Clang's setObject:forKeyedSubscript: selector.
  clang::Selector setObjectForKeyedSubscript;

private:
  Optional<Module *> checkedFoundationModule, checkedSIMDModule;

  /// External Decls that we have imported but not passed to the ASTContext yet.
  SmallVector<Decl *, 4> RegisteredExternalDecls;

  /// Protocol conformances that may be missing witnesses.
  SmallVector<NormalProtocolConformance *, 4> DelayedProtocolConformances;

  unsigned NumCurrentImportingEntities = 0;

  /// Mapping from delayed conformance IDs to the set of delayed
  /// protocol conformances.
  llvm::DenseMap<unsigned, SmallVector<ProtocolConformance *, 4>>
    DelayedConformances;

  /// The next delayed conformance ID to use with \c DelayedConformances.
  unsigned NextDelayedConformanceID = 0;

  /// The set of imported protocols for a declaration, used only to
  /// load all members of the declaration.
  llvm::DenseMap<const Decl *, SmallVector<ProtocolDecl *, 4>>
    ImportedProtocols;

  void startedImportingEntity();
  void finishedImportingEntity();
  void finishPendingActions();
  void finishProtocolConformance(NormalProtocolConformance *conformance);

  struct ImportingEntityRAII {
    Implementation &Impl;

    ImportingEntityRAII(Implementation &Impl) : Impl(Impl) {
      Impl.startedImportingEntity();
    }
    ~ImportingEntityRAII() {
      Impl.finishedImportingEntity();
    }
  };

public:
  /// A predicate that indicates if the given platform should be
  /// considered for availability.
  std::function<bool (StringRef PlatformName)>
    PlatformAvailabilityFilter;

  /// A predicate that indicates if the given platform version should
  /// should be included in the cutoff of deprecated APIs marked unavailable.
  std::function<bool (unsigned major, llvm::Optional<unsigned> minor)>
    DeprecatedAsUnavailableFilter;

  /// The message to embed for implicitly unavailability if a deprecated
  /// API is now unavailable.
  std::string DeprecatedAsUnavailableMessage;

  /// Tracks top level decls from the bridging header.
  std::vector<clang::Decl *> BridgeHeaderTopLevelDecls;
  std::vector<llvm::PointerUnion<clang::ImportDecl *, ImportDecl *>>
    BridgeHeaderTopLevelImports;

  /// Tracks macro definitions from the bridging header.
  std::vector<clang::IdentifierInfo *> BridgeHeaderMacros;
  /// Tracks included headers from the bridging header.
  llvm::DenseSet<const clang::FileEntry *> BridgeHeaderFiles;

  void addBridgeHeaderTopLevelDecls(clang::Decl *D);
  bool shouldIgnoreBridgeHeaderTopLevelDecl(clang::Decl *D);

  /// Add the given named declaration as an entry to the given Swift name
  /// lookup table, including any of its child entries.
  void addEntryToLookupTable(clang::Sema &clangSema, SwiftLookupTable &table,
                             clang::NamedDecl *named);

  /// Add the macros from the given Clang preprocessor to the given
  /// Swift name lookup table.
  void addMacrosToLookupTable(clang::ASTContext &clangCtx,
                              clang::Preprocessor &pp, SwiftLookupTable &table);

public:
  void registerExternalDecl(Decl *D) {
    RegisteredExternalDecls.push_back(D);
  }

  void scheduleFinishProtocolConformance(NormalProtocolConformance *C) {
    DelayedProtocolConformances.push_back(C);
  }

  /// \brief Retrieve the Clang AST context.
  clang::ASTContext &getClangASTContext() const {
    return Instance->getASTContext();
  }

  /// \brief Retrieve the Clang Sema object.
  clang::Sema &getClangSema() const {
    return Instance->getSema();
  }

  /// \brief Retrieve the Clang AST context.
  clang::Preprocessor &getClangPreprocessor() const {
    return Instance->getPreprocessor();
  }
  
  clang::CodeGenOptions &getClangCodeGenOpts() const {
    return Instance->getCodeGenOpts();
  }

  /// Imports the given header contents into the Clang context.
  bool importHeader(Module *adapter, StringRef headerName, SourceLoc diagLoc,
                    bool trackParsedSymbols,
                    std::unique_ptr<llvm::MemoryBuffer> contents);

  /// Returns the redeclaration of \p D that contains its definition for any
  /// tag type decl (struct, enum, or union) or Objective-C class or protocol.
  ///
  /// Returns \c None if \p D is not a redeclarable type declaration.
  /// Returns null if \p D is a redeclarable type, but it does not have a
  /// definition yet.
  Optional<const clang::Decl *>
  getDefinitionForClangTypeDecl(const clang::Decl *D);

  /// Returns the module \p D comes from, or \c None if \p D does not have
  /// a valid associated module.
  ///
  /// The returned module may be null (but not \c None) if \p D comes from
  /// an imported header.
  Optional<clang::Module *>
  getClangSubmoduleForDecl(const clang::Decl *D,
                           bool allowForwardDeclaration = false);

  /// \brief Retrieve the imported module that should contain the given
  /// Clang decl.
  ClangModuleUnit *getClangModuleForDecl(const clang::Decl *D,
                                         bool allowForwardDeclaration = false);

  /// Returns the module \p MI comes from, or \c None if \p MI does not have
  /// a valid associated module.
  ///
  /// The returned module may be null (but not \c None) if \p MI comes from
  /// an imported header.
  Optional<clang::Module *>
  getClangSubmoduleForMacro(const clang::MacroInfo *MI);

  ClangModuleUnit *getClangModuleForMacro(const clang::MacroInfo *MI);

  /// Retrieve the type of an instance of the given Clang declaration context,
  /// or a null type if the DeclContext does not have a corresponding type.
  clang::QualType getClangDeclContextType(const clang::DeclContext *dc);

  /// Determine whether this typedef is a CF type.
  static bool isCFTypeDecl(const clang::TypedefNameDecl *Decl);

  /// Determine the imported CF type for the given typedef-name, or the empty
  /// string if this is not an imported CF type name.
  StringRef getCFTypeName(const clang::TypedefNameDecl *decl,
                          StringRef *secondaryName = nullptr);

  /// Retrieve the type name of a Clang type for the purposes of
  /// omitting unneeded words.
  OmissionTypeName getClangTypeNameForOmission(clang::ASTContext &ctx,
                                               clang::QualType type);

  /// Omit needless words in a function name.
  bool omitNeedlessWordsInFunctionName(
         clang::Sema &clangSema,
         StringRef &baseName,
         SmallVectorImpl<StringRef> &argumentNames,
         ArrayRef<const clang::ParmVarDecl *> params,
         clang::QualType resultType,
         const clang::DeclContext *dc,
         const llvm::SmallBitVector &nonNullArgs,
         const Optional<api_notes::ObjCMethodInfo> &knownMethod,
         Optional<unsigned> errorParamIndex,
         bool returnsSelf,
         bool isInstanceMethod,
         StringScratchSpace &scratch);

  /// \brief Converts the given Swift identifier for Clang.
  clang::DeclarationName exportName(Identifier name);

  /// Information about imported error parameters.
  struct ImportedErrorInfo {
    ForeignErrorConvention::Kind Kind;
    ForeignErrorConvention::IsOwned_t IsOwned;

    /// The index of the error parameter.
    unsigned ParamIndex;

    /// Whether the parameter is being replaced with "void"
    /// (vs. removed).
    bool ReplaceParamWithVoid;
  };

  /// Describes a name that was imported from Clang.
  struct ImportedName {
    /// The imported name.
    DeclName Imported;

    /// An additional alias to the imported name, which should be
    /// recorded in name lookup tables as well.
    DeclName Alias;

    /// Whether this name was explicitly specified via a Clang
    /// swift_name attribute.
    bool HasCustomName = false;

    /// Whether this was one of a special class of Objective-C
    /// initializers for which we drop the variadic argument rather
    /// than refuse to import the initializer.
    bool DroppedVariadic = false;

    /// Whether this declaration is a subscript accessor (getter or setter).
    bool IsSubscriptAccessor = false;

    /// For an initializer, the kind of initializer to import.
    CtorInitializerKind InitKind = CtorInitializerKind::Designated;

    /// For names that map Objective-C error handling conventions into
    /// throwing Swift methods, describes how the mapping is performed.
    Optional<ImportedErrorInfo> ErrorInfo;

    /// Produce just the imported name, for clients that don't care
    /// about the details.
    operator DeclName() const { return Imported; }

    /// Whether any name was imported.
    explicit operator bool() const { return static_cast<bool>(Imported); }
  };

  /// Flags that control the import of names in importFullName.
  enum class ImportNameFlags {
    /// Suppress the factory-method-as-initializer transformation.
    SuppressFactoryMethodAsInit = 0x01,
  };

  /// Options that control the import of names in importFullName.
  typedef OptionSet<ImportNameFlags> ImportNameOptions;

  /// Imports the full name of the given Clang declaration into Swift.
  ///
  /// Note that this may result in a name very different from the Clang name,
  /// so it should not be used when referencing Clang symbols.
  ///
  /// \param D The Clang declaration whose name should be imported.
  ///
  /// \param effectiveContext If non-null, will be set to the effective
  /// Clang declaration context in which the declaration will be imported.
  /// This can differ from D's redeclaration context when the Clang importer
  /// introduces nesting, e.g., for enumerators within an NS_ENUM.
  ImportedName importFullName(const clang::NamedDecl *D,
                              ImportNameOptions options = None,
                              clang::DeclContext **effectiveContext = nullptr,
                              clang::Sema *clangSemaOverride = nullptr);

  /// \brief Import the given Clang identifier into Swift.
  ///
  /// \param identifier The Clang identifier to map into Swift.
  ///
  /// \param removePrefix The prefix to remove from the Clang name to produce
  /// the Swift name. If the Clang name does not start with this prefix,
  /// nothing is removed.
  Identifier importIdentifier(const clang::IdentifierInfo *identifier,
                              StringRef removePrefix = "");

  /// Import an Objective-C selector.
  ObjCSelector importSelector(clang::Selector selector);

  /// Import a Swift name as a Clang selector.
  clang::Selector exportSelector(DeclName name, bool allowSimpleName = true);

  /// Export a Swift Objective-C selector as a Clang Objective-C selector.
  clang::Selector exportSelector(ObjCSelector selector);

  /// \brief Import the given Swift source location into Clang.
  clang::SourceLocation exportSourceLoc(SourceLoc loc);

  /// \brief Import the given Clang source location into Swift.
  SourceLoc importSourceLoc(clang::SourceLocation loc);

  /// \brief Import the given Clang source range into Swift.
  SourceRange importSourceRange(clang::SourceRange loc);

  /// \brief Import the given Clang preprocessor macro as a Swift value decl.
  ///
  /// \returns The imported declaration, or null if the macro could not be
  /// translated into Swift.
  ValueDecl *importMacro(Identifier name, clang::MacroInfo *macro);

  /// Returns true if it is expected that the macro is ignored.
  bool shouldIgnoreMacro(StringRef name, const clang::MacroInfo *macro);

  /// \brief Classify the given Clang enumeration type to describe how it
  /// should be imported 
  static EnumKind classifyEnum(clang::Preprocessor &pp,
                               const clang::EnumDecl *decl);

  /// Import attributes from the given Clang declaration to its Swift
  /// equivalent.
  ///
  /// \param ClangDecl The decl being imported.
  /// \param MappedDecl The decl to attach attributes to.
  /// \param NewContext If present, the Clang node for the context the decl is
  /// being imported into, which may affect info from API notes.
  void importAttributes(const clang::NamedDecl *ClangDecl, Decl *MappedDecl,
                        const clang::ObjCContainerDecl *NewContext = nullptr);

  /// If we already imported a given decl, return the corresponding Swift decl.
  /// Otherwise, return nullptr.
  Decl *importDeclCached(const clang::NamedDecl *ClangDecl);

  Decl *importDeclImpl(const clang::NamedDecl *ClangDecl,
                       bool &TypedefIsSuperfluous,
                       bool &HadForwardDeclaration);

  Decl *importDeclAndCacheImpl(const clang::NamedDecl *ClangDecl,
                               bool SuperfluousTypedefsAreTransparent);

  /// \brief Same as \c importDeclReal, but for use inside importer
  /// implementation.
  ///
  /// Unlike \c importDeclReal, this function for convenience transparently
  /// looks through superfluous typedefs and returns the imported underlying
  /// decl in that case.
  Decl *importDecl(const clang::NamedDecl *ClangDecl) {
    return importDeclAndCacheImpl(ClangDecl,
                                  /*SuperfluousTypedefsAreTransparent=*/true);
  }

  /// \brief Import the given Clang declaration into Swift.  Use this function
  /// outside of the importer implementation, when importing a decl requested by
  /// Swift code.
  ///
  /// \returns The imported declaration, or null if this declaration could
  /// not be represented in Swift.
  Decl *importDeclReal(const clang::NamedDecl *ClangDecl) {
    return importDeclAndCacheImpl(ClangDecl,
                                  /*SuperfluousTypedefsAreTransparent=*/false);
  }

  /// Import the class-method version of the given Objective-C
  /// instance method of a root class.
  Decl *importClassMethodVersionOf(FuncDecl *method);

  /// \brief Import a cloned version of the given declaration, which is part of
  /// an Objective-C protocol and currently must be a method or property, into
  /// the given declaration context.
  ///
  /// \returns The imported declaration, or null if this declaration could not
  /// be represented in Swift.
  Decl *importMirroredDecl(const clang::NamedDecl *decl, DeclContext *dc,
                           ProtocolDecl *proto, bool forceClassMethod = false);

  /// \brief Import the given Clang declaration context into Swift.
  ///
  /// Usually one will use \c importDeclContextOf instead.
  ///
  /// \returns The imported declaration context, or null if it could not
  /// be converted.
  DeclContext *importDeclContextImpl(const clang::DeclContext *dc);

  /// \brief Import the declaration context of a given Clang declaration into
  /// Swift.
  ///
  /// \returns The imported declaration context, or null if it could not
  /// be converted.
  DeclContext *importDeclContextOf(const clang::Decl *D);

  /// \brief Create a new named constant with the given value.
  ///
  /// \param name The name of the constant.
  /// \param dc The declaration context into which the name will be introduced.
  /// \param type The type of the named constant.
  /// \param value The value of the named constant.
  /// \param convertKind How to convert the constant to the given type.
  /// \param isStatic Whether the constant should be a static member of \p dc.
  ValueDecl *createConstant(Identifier name, DeclContext *dc,
                            Type type, const clang::APValue &value,
                            ConstantConvertKind convertKind,
                            bool isStatic,
                            ClangNode ClangN);

  /// \brief Create a new named constant with the given value.
  ///
  /// \param name The name of the constant.
  /// \param dc The declaration context into which the name will be introduced.
  /// \param type The type of the named constant.
  /// \param value The value of the named constant.
  /// \param convertKind How to convert the constant to the given type.
  /// \param isStatic Whether the constant should be a static member of \p dc.
  ValueDecl *createConstant(Identifier name, DeclContext *dc,
                            Type type, StringRef value,
                            ConstantConvertKind convertKind,
                            bool isStatic,
                            ClangNode ClangN);

  /// \brief Create a new named constant using the given expression.
  ///
  /// \param name The name of the constant.
  /// \param dc The declaration context into which the name will be introduced.
  /// \param type The type of the named constant.
  /// \param valueExpr An expression to use as the value of the constant.
  /// \param convertKind How to convert the constant to the given type.
  /// \param isStatic Whether the constant should be a static member of \p dc.
  ValueDecl *createConstant(Identifier name, DeclContext *dc,
                            Type type, Expr *valueExpr,
                            ConstantConvertKind convertKind,
                            bool isStatic,
                            ClangNode ClangN);

  /// \brief Add "Unavailable" annotation to the swift declaration.
  void markUnavailable(ValueDecl *decl, StringRef unavailabilityMsg);

  /// \brief Create a decl with error type and an "unavailable" attribute on it
  /// with the specified message.
  ValueDecl *createUnavailableDecl(Identifier name, DeclContext *dc,
                                   Type type, StringRef UnavailableMessage,
                                   bool isStatic, ClangNode ClangN);

  /// \brief Retrieve the standard library module.
  Module *getStdlibModule();

  /// \brief Retrieve the named module.
  ///
  /// \param name The name of the module.
  ///
  /// \returns The named module, or null if the module has not been imported.
  Module *getNamedModule(StringRef name);

  /// \brief Returns the "Foundation" module, if it can be loaded.
  ///
  /// After this has been called, the Foundation module will or won't be loaded
  /// into the ASTContext.
  Module *tryLoadFoundationModule();

  /// \brief Returns the "SIMD" module, if it can be loaded.
  ///
  /// After this has been called, the SIMD module will or won't be loaded
  /// into the ASTContext.
  Module *tryLoadSIMDModule();

  /// \brief Retrieves the Swift wrapper for the given Clang module, creating
  /// it if necessary.
  ClangModuleUnit *getWrapperForModule(ClangImporter &importer,
                                       const clang::Module *underlying);

  /// Retrieve the API notes reader that corresponds to the given Clang module,
  /// loading it if necessary.
  ///
  /// \returns an unowned pointer to the corresponding API notes reader, or
  /// nullptr if no API notes file exists.
  api_notes::APINotesReader *getAPINotesForModule(const clang::Module *module);

  /// \brief Constructs a Swift module for the given Clang module.
  Module *finishLoadingClangModule(ClangImporter &importer,
                                   const clang::Module *clangModule,
                                   bool preferAdapter);

  /// \brief Retrieve the named Swift type, e.g., Int32.
  ///
  /// \param module The name of the module in which the type should occur.
  ///
  /// \param name The name of the type to find.
  ///
  /// \returns The named type, or null if the type could not be found.
  Type getNamedSwiftType(Module *module, StringRef name);

  /// \brief Retrieve a specialization of the named Swift type, e.g.,
  /// UnsafeMutablePointer<T>.
  ///
  /// \param module The name of the module in which the type should occur.
  ///
  /// \param name The name of the type to find.
  ///
  /// \param args The arguments to use in the specialization.
  ///
  /// \returns The named type, or null if the type could not be found.
  Type getNamedSwiftTypeSpecialization(Module *module, StringRef name,
                                       ArrayRef<Type> args);

  /// \brief Retrieve the NSObject type.
  Type getNSObjectType();

  /// \brief Retrieve the NSObject protocol type.
  Type getNSObjectProtocolType();

  /// \brief Retrieve the NSCopying protocol type.
  Type getNSCopyingType();

  /// \brief Retrieve the CFStringRef typealias.
  Type getCFStringRefType();

  /// \brief Determines whether the given type matches an implicit type
  /// bound of "NSObject", which is used to validate NSDictionary/NSSet.
  bool matchesNSObjectBound(Type type);

  /// \brief Look up and attempt to import a Clang declaration with
  /// the given name.
  Decl *importDeclByName(StringRef name);

  /// \brief Import the given Clang type into Swift.
  ///
  /// \param type The Clang type to import.
  ///
  /// \param kind The kind of type import we're performing.
  ///
  /// \param allowNSUIntegerAsInt If true, NSUInteger will be imported as Int
  ///        in certain contexts. If false, it will always be imported as UInt.
  ///
  /// \param canFullyBridgeTypes True if we can bridge types losslessly.
  ///        This is an additional guarantee on top of the ImportTypeKind
  ///        cases that allow bridging, and applies to the entire type.
  ///
  /// \returns The imported type, or null if this type could
  /// not be represented in Swift.
  Type importType(clang::QualType type,
                  ImportTypeKind kind,
                  bool allowNSUIntegerAsInt,
                  bool canFullyBridgeTypes,
                  OptionalTypeKind optional = OTK_ImplicitlyUnwrappedOptional);

  /// \brief Import the given function type.
  ///
  /// This routine should be preferred when importing function types for
  /// which we have actual function parameters, e.g., when dealing with a
  /// function declaration, because it produces a function type whose input
  /// tuple has argument names.
  ///
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param resultType The result type of the function.
  /// \param params The parameter types to the function.
  /// \param isVariadic Whether the function is variadic.
  /// \param isNoReturn Whether the function is noreturn.
  /// \param bodyPatterns The patterns visible inside the function body.
  ///
  /// \returns the imported function type, or null if the type cannot be
  /// imported.
  Type importFunctionType(const clang::FunctionDecl *clangDecl,
                          clang::QualType resultType,
                          ArrayRef<const clang::ParmVarDecl *> params,
                          bool isVariadic, bool isNoReturn,
                          bool isFromSystemModule,
                          bool hasCustomName,
                          SmallVectorImpl<Pattern*> &bodyPatterns,
                          DeclName &name);

  Type importPropertyType(const clang::ObjCPropertyDecl *clangDecl,
                          bool isFromSystemModule);

  /// Determine whether we can infer a default argument for a parameter with
  /// the given \c type and (Clang) optionality.
  bool canInferDefaultArgument(clang::Preprocessor &pp,
                               clang::QualType type,
                               OptionalTypeKind clangOptionality,
                               Identifier baseName,
                               unsigned numParams,
                               bool isLastParameter);

  /// Retrieve a bit vector containing the non-null argument
  /// annotations for the given declaration.
  llvm::SmallBitVector getNonNullArgs(
                         const clang::Decl *decl,
                         ArrayRef<const clang::ParmVarDecl *> params);

  /// \brief Import the type of an Objective-C method.
  ///
  /// This routine should be preferred when importing function types for
  /// which we have actual function parameters, e.g., when dealing with a
  /// function declaration, because it produces a function type whose input
  /// tuple has argument names.
  ///
  /// \param clangDecl The underlying declaration, if any; should only be
  ///   considered for any attributes it might carry.
  /// \param resultType The result type of the function.
  /// \param params The parameter types to the function.
  /// \param isVariadic Whether the function is variadic.
  /// \param isNoReturn Whether the function is noreturn.
  /// \param isFromSystemModule Whether to apply special rules that only apply
  ///   to system APIs.
  /// \param bodyPatterns The patterns visible inside the function body.
  ///   whether the created arg/body patterns are different (selector-style).
  /// \param importedName The name of the imported method.
  /// \param errorConvention Information about the method's error conventions.
  /// \param kind Controls whether we're building a type for a method that
  ///        needs special handling.
  ///
  /// \returns the imported function type, or null if the type cannot be
  /// imported.
  Type importMethodType(const clang::ObjCMethodDecl *clangDecl,
                        clang::QualType resultType,
                        ArrayRef<const clang::ParmVarDecl *> params,
                        bool isVariadic, bool isNoReturn,
                        bool isFromSystemModule,
                        SmallVectorImpl<Pattern*> &bodyPatterns,
                        ImportedName importedName,
                        DeclName &name,
                        Optional<ForeignErrorConvention> &errorConvention,
                        SpecialMethodKind kind);

  /// \brief Determine whether the given typedef-name is "special", meaning
  /// that it has performed some non-trivial mapping of its underlying type
  /// based on the name of the typedef.
  Optional<MappedTypeNameKind>
  getSpecialTypedefKind(clang::TypedefNameDecl *decl);

  /// \brief Look up a name, accepting only typedef results.
  const clang::TypedefNameDecl *lookupTypedef(clang::DeclarationName);

  /// \brief Return whether a global of the given type should be imported as a
  /// 'let' declaration as opposed to 'var'.
  bool shouldImportGlobalAsLet(clang::QualType type);

  LazyResolver *getTypeResolver() const {
    return typeResolver.getPointer();
  }
  void setTypeResolver(LazyResolver *newResolver) {
    assert((!typeResolver.getPointer() || !newResolver) &&
           "already have a type resolver");
    typeResolver.setPointerAndInt(newResolver, true);
  }
  bool hasBegunTypeChecking() const { return typeResolver.getInt(); }
  bool hasFinishedTypeChecking() const {
    return hasBegunTypeChecking() && !getTypeResolver();
  }

  /// Allocate a new delayed conformance ID with the given set of
  /// conformances.
  unsigned allocateDelayedConformance(
             SmallVector<ProtocolConformance *, 4> &&conformances) {
    unsigned id = NextDelayedConformanceID++;
    DelayedConformances[id] = std::move(conformances);
    return id;
  }

  /// Take the delayed conformances associated with the given id.
  SmallVector<ProtocolConformance *, 4> takeDelayedConformance(unsigned id) {
    auto conformances = DelayedConformances.find(id);
    SmallVector<ProtocolConformance *, 4> result
      = std::move(conformances->second);
    DelayedConformances.erase(conformances);
    return result;
  }

  /// Record the set of imported protocols for the given declaration,
  /// to be used by member loading.
  ///
  /// FIXME: This is all a hack; we should have lazier deserialization
  /// of protocols separate from their conformances.
  void recordImportedProtocols(const Decl *decl,
                               ArrayRef<ProtocolDecl *> protocols) {
    if (protocols.empty())
      return;

    auto &recorded = ImportedProtocols[decl];
    recorded.insert(recorded.end(), protocols.begin(), protocols.end());
  }

  /// Retrieve the imported protocols for the given declaration.
  SmallVector<ProtocolDecl *, 4> takeImportedProtocols(const Decl *decl) {
    SmallVector<ProtocolDecl *, 4> result;

    auto known = ImportedProtocols.find(decl);
    if (known != ImportedProtocols.end()) {
      result = std::move(known->second);
      ImportedProtocols.erase(known);
    }

    return result;
  }

  virtual void
  loadAllMembers(Decl *D, uint64_t unused,
                 bool *hasMissingRequiredMembers) override;

  void
  loadAllConformances(
    const Decl *D, uint64_t contextData,
    SmallVectorImpl<ProtocolConformance *> &Conformances) override;

  template <typename DeclTy, typename ...Targs>
  DeclTy *createDeclWithClangNode(ClangNode ClangN, Targs &&... Args) {
    assert(ClangN);
    void *DeclPtr = allocateMemoryForDecl<DeclTy>(SwiftContext, sizeof(DeclTy),
                                                  true);
    auto D = ::new (DeclPtr) DeclTy(std::forward<Targs>(Args)...);
    D->setClangNode(ClangN);
    D->setEarlyAttrValidation(true);
    if (auto VD = dyn_cast<ValueDecl>(D))
      VD->setAccessibility(Accessibility::Public);
    if (auto ASD = dyn_cast<AbstractStorageDecl>(D))
      ASD->setSetterAccessibility(Accessibility::Public);
    return D;
  }

  // Module file extension overrides

  clang::ModuleFileExtensionMetadata getExtensionMetadata() const override;
  llvm::hash_code hashExtension(llvm::hash_code code) const override;

  std::unique_ptr<clang::ModuleFileExtensionWriter>
  createExtensionWriter(clang::ASTWriter &writer) override;

  std::unique_ptr<clang::ModuleFileExtensionReader>
  createExtensionReader(const clang::ModuleFileExtensionMetadata &metadata,
                        clang::ASTReader &reader,
                        clang::serialization::ModuleFile &mod,
                        const llvm::BitstreamCursor &stream) override;

  /// Find the lookup table that corresponds to the given Clang module.
  ///
  /// \param clangModule The module, or null to indicate that we're talking
  /// about the directly-parsed headers.
  SwiftLookupTable *findLookupTable(const clang::Module *clangModule);

  /// Look for namespace-scope values with the given name in the given
  /// Swift lookup table.
  void lookupValue(SwiftLookupTable &table, DeclName name,
                   VisibleDeclConsumer &consumer);

  /// Look for namespace-scope values in the given Swift lookup table.
  void lookupVisibleDecls(SwiftLookupTable &table,
                          VisibleDeclConsumer &consumer);

  /// Look for Objective-C members with the given name in the given
  /// Swift lookup table.
  void lookupObjCMembers(SwiftLookupTable &table, DeclName name,
                         VisibleDeclConsumer &consumer);

  /// Look for all Objective-C members in the given Swift lookup table.
  void lookupAllObjCMembers(SwiftLookupTable &table,
                            VisibleDeclConsumer &consumer);

  /// Dump the Swift-specific name lookup tables we generate.
  void dumpSwiftLookupTables();
};

}

#endif
