/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wabt/c-writer.h"

#include <cctype>
#include <cinttypes>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include "wabt/cast.h"
#include "wabt/common.h"
#include "wabt/ir.h"
#include "wabt/literal.h"
#include "wabt/sha256.h"
#include "wabt/stream.h"
#include "wabt/string-util.h"

#define INDENT_SIZE 2

#define UNIMPLEMENTED(x) printf("unimplemented: %s\n", (x)), abort()

// code to be inserted into the generated output
extern const char* s_header_top;
extern const char* s_header_bottom;
extern const char* s_source_includes;
extern const char* s_source_declarations;
extern const char* s_simd_source_declarations;
extern const char* s_atomicops_source_declarations;

namespace wabt {

namespace {

struct Label {
  Label(LabelType label_type,
        const std::string& name,
        const TypeVector& sig,
        size_t type_stack_size,
        size_t try_catch_stack_size,
        bool used = false)
      : label_type(label_type),
        name(name),
        sig(sig),
        type_stack_size(type_stack_size),
        try_catch_stack_size(try_catch_stack_size),
        used(used) {}

  bool HasValue() const { return !sig.empty(); }

  LabelType label_type;
  const std::string& name;
  const TypeVector& sig;
  size_t type_stack_size;
  size_t try_catch_stack_size;
  bool used = false;
};

struct LocalName {
  explicit LocalName(const std::string& name) : name(name) {}
  const std::string& name;
};

struct ParamName : LocalName {
  using LocalName::LocalName;
  ParamName(const Var& var) : LocalName(var.name()) {}
};

struct LabelName : LocalName {
  using LocalName::LocalName;
};

struct GlobalName {
  GlobalName(ModuleFieldType type, const std::string& name)
      : type(type), name(name) {}
  ModuleFieldType type;
  const std::string& name;
};

struct TagSymbol : GlobalName {
  explicit TagSymbol(const std::string& name)
      : GlobalName(ModuleFieldType::Tag, name) {}
};

struct ExternalRef : GlobalName {
  using GlobalName::GlobalName;
};

struct WrapperRef : GlobalName {
  explicit WrapperRef(const std::string& name)
      : GlobalName(ModuleFieldType::Func, name) {}
};

struct TailCallRef : GlobalName {
  explicit TailCallRef(const std::string& name)
      : GlobalName(ModuleFieldType::Func, name) {}
};

struct ExternalInstancePtr : GlobalName {
  using GlobalName::GlobalName;
};

struct ExternalInstanceRef : GlobalName {
  using GlobalName::GlobalName;
};

struct GotoLabel {
  explicit GotoLabel(const Var& var) : var(var) {}
  const Var& var;
};

struct LabelDecl {
  explicit LabelDecl(const std::string& name) : name(name) {}
  std::string name;
};

struct GlobalInstanceVar {
  explicit GlobalInstanceVar(const Var& var) : var(var) {}
  const Var& var;
};

struct StackVar {
  explicit StackVar(Index index, Type type = Type::Any)
      : index(index), type(type) {}
  Index index;
  Type type;
};

struct TypeEnum {
  explicit TypeEnum(Type type) : type(type) {}
  Type type;
};

struct SignedType {
  explicit SignedType(Type type) : type(type) {}
  Type type;
};

struct TryCatchLabel {
  TryCatchLabel(const std::string& name, size_t try_catch_stack_size)
      : name(name), try_catch_stack_size(try_catch_stack_size), used(false) {}
  std::string name;
  size_t try_catch_stack_size;
  bool used;
};

struct FuncTypeExpr {
  const FuncType* func_type;
  FuncTypeExpr(const FuncType* f) : func_type(f) {}
};

struct Newline {};
struct OpenBrace {};
struct CloseBrace {};

int GetShiftMask(Type type) {
  // clang-format off
  switch (type) {
    case Type::I32: return 31;
    case Type::I64: return 63;
    default: WABT_UNREACHABLE; return 0;
  }
  // clang-format on
}

/*
 * This function is the default behavior for name_to_output_file_index_. For
 * single .c output, this function returns a vector filled with 0. For multiple
 * .c outputs, this function sorts all non-imported functions in the module by
 * their names, and then divides all non-imported functions into equal-sized
 * buckets (# of non-imported functions / # of .c outputs) based on the sorting.
 */
static std::vector<size_t> default_name_to_output_file_index(
    std::vector<Func*>::const_iterator func_begin,
    std::vector<Func*>::const_iterator func_end,
    size_t num_imports,
    size_t num_streams) {
  std::vector<size_t> result;
  result.resize(std::distance(func_begin, func_end));
  if (num_streams == 1) {
    return result;
  }

  std::map<std::string, Index> sorted_functions;
  size_t non_imported_funcs = result.size() - num_imports;
  size_t bucket_size = non_imported_funcs / num_streams +
                       (non_imported_funcs % num_streams ? 1 : 0);
  Index func_index = 0;
  for (auto func = func_begin; func != func_end; func++) {
    sorted_functions.insert({(*func)->name, func_index});
    ++func_index;
  }
  Index sorted_func_index = 0;
  for (const auto& [func_name, index] : sorted_functions) {
    bool is_import = index < num_imports;
    if (!is_import) {
      result.at(index) = sorted_func_index / bucket_size;
      ++sorted_func_index;
    }
  }
  return result;
}

class CWriter {
 public:
  CWriter(std::vector<Stream*>&& c_streams,
          Stream* h_stream,
          Stream* h_impl_stream,
          const char* header_name,
          const char* header_impl_name,
          const WriteCOptions& options)
      : options_(options),
        c_streams_(std::move(c_streams)),
        h_stream_(h_stream),
        h_impl_stream_(h_impl_stream),
        header_name_(header_name),
        header_impl_name_(header_impl_name) {
    module_prefix_ = MangleModuleName(options_.module_name);
    if (c_streams_.size() != 1 && options.name_to_output_file_index) {
      name_to_output_file_index_ = options.name_to_output_file_index;
    } else {
      name_to_output_file_index_ = default_name_to_output_file_index;
    }
  }

  Result WriteModule(const Module&);

 private:
  using SymbolSet = std::set<std::string>;
  using SymbolMap = std::map<std::string, std::string>;
  using StackTypePair = std::pair<Index, Type>;
  using StackVarSymbolMap = std::map<StackTypePair, std::string>;

  void WriteCHeader();
  void WriteCSource();

  size_t MarkTypeStack() const;
  void ResetTypeStack(size_t mark);
  Type StackType(Index) const;
  void PushType(Type);
  void PushTypes(const TypeVector&);
  void DropTypes(size_t count);

  void PushLabel(LabelType,
                 const std::string& name,
                 const FuncSignature&,
                 bool used = false);
  const Label* FindLabel(const Var& var, bool mark_used = true);
  bool IsTopLabelUsed() const;
  void PopLabel();

  static constexpr bool AreInitializersAlwaysNull(Type);
  static constexpr char MangleType(Type);
  static constexpr char MangleField(ModuleFieldType);
  static std::string MangleTypes(const TypeVector&);
  static std::string Mangle(std::string_view name, bool double_underscores);
  static std::string MangleName(std::string_view);
  static std::string MangleModuleName(std::string_view);
  static std::string ExportName(std::string_view module_name,
                                std::string_view export_name);
  std::string ExportName(std::string_view export_name) const;
  static std::string TailCallExportName(std::string_view module_name,
                                        std::string_view export_name);
  std::string TailCallExportName(std::string_view export_name) const;
  std::string ModuleInstanceTypeName() const;
  static std::string ModuleInstanceTypeName(std::string_view module_name);
  void ClaimName(SymbolSet& set,
                 SymbolMap& map,
                 char type_suffix,
                 std::string_view wasm_name,
                 const std::string& c_name);
  std::string FindUniqueName(SymbolSet& set,
                             std::string_view proposed_name) const;
  std::string ClaimUniqueName(SymbolSet& set,
                              SymbolMap& map,
                              char type_suffix,
                              std::string_view wasm_name,
                              const std::string& proposed_c_name);
  void DefineImportName(const Import* import,
                        std::string_view module_name,
                        std::string_view field_name);
  void ReserveExportNames();
  void ReserveExportName(std::string_view);
  std::string DefineImportedModuleInstanceName(std::string_view name);
  std::string DefineInstanceMemberName(ModuleFieldType, std::string_view);
  std::string DefineGlobalScopeName(ModuleFieldType, std::string_view);
  std::string DefineLocalScopeName(std::string_view name, bool is_label);
  std::string DefineParamName(std::string_view);
  std::string DefineLabelName(std::string_view);
  std::string DefineStackVarName(Index, Type, std::string_view);

  static void SerializeFuncType(const FuncType&, std::string&);

  std::string GetGlobalName(ModuleFieldType, const std::string&) const;
  std::string GetLocalName(const std::string&, bool is_label) const;
  std::string GetTailCallRef(const std::string&) const;

  void Indent(int size = INDENT_SIZE);
  void Dedent(int size = INDENT_SIZE);
  void NonIndented(std::function<void()> func);
  void WriteIndent();
  void WriteData(const char* src, size_t size);
  void Writef(const char* format, ...);

  template <typename T, typename U, typename... Args>
  void Write(T&& t, U&& u, Args&&... args) {
    Write(std::forward<T>(t));
    Write(std::forward<U>(u));
    Write(std::forward<Args>(args)...);
  }

  static const char* GetReferenceTypeName(const Type& type);
  static const char* GetReferenceNullValue(const Type& type);
  static const char* GetCTypeName(const Type& type);

  const char* InternalSymbolScope() const;

  enum class CWriterPhase {
    Declarations,
    Definitions,
  };

  void Write() {}
  void Write(Newline);
  void Write(OpenBrace);
  void Write(CloseBrace);
  void Write(uint64_t);
  void Write(std::string_view);
  void Write(const ParamName&);
  void Write(const LabelName&);
  void Write(const GlobalName&);
  void Write(const TagSymbol&);
  void Write(const ExternalRef&);
  void Write(const WrapperRef&);
  void Write(const TailCallRef&);
  void Write(const ExternalInstancePtr&);
  void Write(const ExternalInstanceRef&);
  void Write(Type);
  void Write(SignedType);
  void Write(TypeEnum);
  void Write(const GotoLabel&);
  void Write(const LabelDecl&);
  void Write(const GlobalInstanceVar&);
  void Write(const StackVar&);
  void Write(const TypeVector&);
  void Write(const Const&);
  void WriteInitExpr(const ExprList&);
  void WriteInitExprTerminal(const Expr*);
  std::string GenerateHeaderGuard() const;
  void WriteSourceTop();
  void WriteMultiCTop();
  void WriteMultiCTopEmpty();
  void DeclareStruct(const TypeVector&);
  void WriteTailcalleeParamTypes();
  void WriteMultivalueResultTypes();
  void WriteTagTypes();
  void WriteFuncTypeDecls();
  void WriteFuncTypes();
  void Write(const FuncTypeExpr&);
  void WriteTagDecls();
  void WriteTags();
  void ComputeUniqueImports();
  void BeginInstance();
  void WriteImports();
  void WriteTailCallWeakImports();
  void WriteFuncDeclarations();
  void WriteFuncDeclaration(const FuncDeclaration&, const std::string&);
  void WriteTailCallFuncDeclaration(const std::string&);
  void WriteImportFuncDeclaration(const FuncDeclaration&,
                                  const std::string& module_name,
                                  const std::string&);
  void WriteFuncRefWrapper(const Func* func);
  void WriteCallIndirectFuncDeclaration(const FuncDeclaration&,
                                        const std::string&);
  void ComputeSimdScope();
  void WriteHeaderIncludes();
  void WriteV128Decl();
  void WriteModuleInstance();
  void WriteGlobals();
  void WriteGlobal(const Global&, const std::string&);
  void WriteGlobalPtr(const Global&, const std::string&);
  void WriteMemories();
  void WriteMemory(const std::string&, const Memory& memory);
  void WriteMemoryPtr(const std::string&, const Memory& memory);
  void WriteTables();
  void WriteTable(const std::string&, const wabt::Type&);
  void WriteTablePtr(const std::string&, const Table&);
  void WriteTableType(const wabt::Type&);
  void WriteDataInstances();
  void WriteElemInstances();
  void WriteGlobalInitializers();
  void WriteDataInitializerDecls();
  void WriteDataInitializers();
  void WriteElemInitializerDecls();
  void WriteElemInitializers();
  void WriteFuncRefWrappers();
  void WriteElemTableInit(bool, const ElemSegment*, const Table*);
  bool IsSingleUnsharedMemory();
  void InstallSegueBase(Memory* memory, bool save_old_value);
  void RestoreSegueBase();
  void WriteExports(CWriterPhase);
  void WriteTailCallExports(CWriterPhase);
  void WriteInitDecl();
  void WriteFreeDecl();
  void WriteGetFuncTypeDecl();
  void WriteInit();
  void WriteFree();
  void WriteGetFuncType();
  void WriteInitInstanceImport();
  void WriteImportProperties(CWriterPhase);
  void WriteFuncs();
  void BeginFunction(const Func&);
  void FinishFunction(size_t);
  void Write(const Func&);
  void WriteTailCallee(const Func&);
  void WriteParamsAndLocals();
  void WriteParams(const std::vector<std::string>& index_to_name,
                   bool setjmp_safe = false);
  void WriteParamSymbols(const std::vector<std::string>& index_to_name);
  void WriteParamTypes(const FuncDeclaration& decl);
  void WriteLocals(const std::vector<std::string>& index_to_name);
  void WriteStackVarDeclarations();
  void Write(const ExprList&);
  void WriteTailCallAsserts(const FuncSignature&);
  void WriteTailCallStack();
  void WriteUnwindTryCatchStack(const Label*);
  void FinishReturnCall();
  void Spill(const TypeVector&);
  void Unspill(const TypeVector&);

  template <typename Vars, typename TypeOf, typename ToDo>
  void WriteVarsByType(const Vars&, const TypeOf&, const ToDo&, bool);

  template <typename sources>
  void Spill(const TypeVector&, const sources& src);
  template <typename targets>
  void Unspill(const TypeVector&, const targets& tgt);

  enum class AssignOp {
    Disallowed,
    Allowed,
  };

  void WriteSimpleUnaryExpr(Opcode, const char* op);
  void WriteInfixBinaryExpr(Opcode,
                            const char* op,
                            AssignOp = AssignOp::Allowed);
  void WritePrefixBinaryExpr(Opcode, const char* op);
  void WriteSignedBinaryExpr(Opcode, const char* op);
  void Write(const BinaryExpr&);
  void Write(const CompareExpr&);
  void Write(const ConvertExpr&);
  void Write(const LoadExpr&);
  void Write(const StoreExpr&);
  void Write(const UnaryExpr&);
  void Write(const TernaryExpr&);
  void Write(const SimdLaneOpExpr&);
  void Write(const SimdLoadLaneExpr&);
  void Write(const SimdStoreLaneExpr&);
  void Write(const SimdShuffleOpExpr&);
  void Write(const LoadSplatExpr&);
  void Write(const LoadZeroExpr&);
  void Write(const Block&);

  void Write(const AtomicLoadExpr& expr);
  void Write(const AtomicStoreExpr& expr);
  void Write(const AtomicRmwExpr& expr);
  void Write(const AtomicRmwCmpxchgExpr& expr);

  size_t BeginTry(const Block& block);
  void WriteTryCatch(const TryExpr& tryexpr);
  void WriteTryDelegate(const TryExpr& tryexpr);
  void Write(const TryTableExpr& try_table_expr);
  void Write(const Catch& c);
  void Write(const TableCatch& c);
  void WriteThrow();

  void PushTryCatch(const std::string& name);
  void PopTryCatch();

  void PushFuncSection(std::string_view include_condition = "");

  bool IsImport(const std::string& name) const;

  const WriteCOptions& options_;
  const Module* module_ = nullptr;
  const Func* func_ = nullptr;
  Stream* stream_ = nullptr;
  std::vector<Stream*> c_streams_;
  Stream* h_stream_ = nullptr;
  Stream* h_impl_stream_ = nullptr;
  std::string header_name_;
  std::string header_impl_name_;
  Result result_ = Result::Ok;
  int indent_ = 0;
  bool should_write_indent_next_ = false;
  int consecutive_newline_count_ = 0;

  SymbolMap global_sym_map_;
  SymbolMap local_sym_map_;
  SymbolMap import_module_sym_map_;
  StackVarSymbolMap stack_var_sym_map_;
  SymbolSet global_syms_;
  SymbolSet local_syms_;
  TypeVector type_stack_;
  std::vector<Label> label_stack_;
  std::vector<TryCatchLabel> try_catch_stack_;
  std::string module_prefix_;
  SymbolSet typevector_structs_;

  std::vector<const Import*> unique_imports_;
  SymbolSet import_module_set_;       // modules that are imported from
  SymbolSet import_func_module_set_;  // modules that funcs are imported from

  std::vector<std::pair<std::string, MemoryStream>> func_sections_;
  SymbolSet func_includes_;

  std::vector<std::string> unique_func_type_names_;

  std::function<std::vector<size_t>(std::vector<Func*>::const_iterator,
                                    std::vector<Func*>::const_iterator,
                                    size_t,
                                    size_t)>
      name_to_output_file_index_;

  bool simd_used_in_header_;

  bool in_tail_callee_;
};

// TODO: if WABT begins supporting debug names for labels,
// will need to avoid conflict between a label named "$Bfunc" and
// the implicit func label
static constexpr char kImplicitFuncLabel[] = "$Bfunc";

// These should be greater than any ModuleFieldType (used for MangleField).
static constexpr char kParamSuffix =
    'a' + static_cast<char>(ModuleFieldType::Tag) + 1;
static constexpr char kLabelSuffix = kParamSuffix + 1;

static constexpr char kGlobalSymbolPrefix[] = "w2c_";
static constexpr char kWrapperSymbolPrefix[] = "wrap_";
static constexpr char kLocalSymbolPrefix[] = "var_";
static constexpr char kAdminSymbolPrefix[] = "wasm2c_";
static constexpr char kTailCallSymbolPrefix[] = "wasm_tailcall_";
static constexpr char kTailCallFallbackPrefix[] = "wasm_fallback_";
static constexpr unsigned int kTailCallStackSize = 1024;

size_t CWriter::MarkTypeStack() const {
  return type_stack_.size();
}

void CWriter::ResetTypeStack(size_t mark) {
  assert(mark <= type_stack_.size());
  type_stack_.erase(type_stack_.begin() + mark, type_stack_.end());
}

Type CWriter::StackType(Index index) const {
  assert(index < type_stack_.size());
  return *(type_stack_.rbegin() + index);
}

void CWriter::PushType(Type type) {
  type_stack_.push_back(type);
}

void CWriter::PushTypes(const TypeVector& types) {
  type_stack_.insert(type_stack_.end(), types.begin(), types.end());
}

void CWriter::DropTypes(size_t count) {
  assert(count <= type_stack_.size());
  type_stack_.erase(type_stack_.end() - count, type_stack_.end());
}

void CWriter::PushLabel(LabelType label_type,
                        const std::string& name,
                        const FuncSignature& sig,
                        bool used) {
  if (label_type == LabelType::Loop)
    label_stack_.emplace_back(label_type, name, sig.param_types,
                              type_stack_.size(), try_catch_stack_.size(),
                              used);
  else
    label_stack_.emplace_back(label_type, name, sig.result_types,
                              type_stack_.size(), try_catch_stack_.size(),
                              used);
}

const Label* CWriter::FindLabel(const Var& var, bool mark_used) {
  Label* label = nullptr;

  if (var.is_index()) {
    // We've generated names for all labels, so we should only be using an
    // index when branching to the implicit function label, which can't be
    // named.
    assert(var.index() + 1 == label_stack_.size());
    label = &label_stack_[0];
  } else {
    assert(var.is_name());
    for (Index i = label_stack_.size(); i > 0; --i) {
      label = &label_stack_[i - 1];
      if (label->name == var.name())
        break;
    }
  }

  assert(label);
  if (mark_used) {
    label->used = true;
  }
  return label;
}

bool CWriter::IsTopLabelUsed() const {
  assert(!label_stack_.empty());
  return label_stack_.back().used;
}

void CWriter::PopLabel() {
  label_stack_.pop_back();
}

// static
constexpr bool CWriter::AreInitializersAlwaysNull(Type type) {
  // clang-format off
  switch (type) {
    case Type::FuncRef: return false;
    case Type::ExternRef: return true;
    case Type::ExnRef: return true;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

// static
constexpr char CWriter::MangleType(Type type) {
  // clang-format off
  switch (type) {
    case Type::I32: return 'i';
    case Type::I64: return 'j';
    case Type::F32: return 'f';
    case Type::F64: return 'd';
    case Type::V128: return 'o';
    case Type::FuncRef: return 'r';
    case Type::ExternRef: return 'e';
    case Type::ExnRef: return 'x';
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

// static
constexpr char CWriter::MangleField(ModuleFieldType type) {
  assert(static_cast<std::underlying_type<ModuleFieldType>::type>(type) <
         std::numeric_limits<char>::max());
  return 'a' + static_cast<char>(type);
}

// remove risky characters for pasting into a C-style comment
static std::string SanitizeForComment(std::string_view str) {
  std::string result;

  for (const uint8_t ch : str) {
    // escape control chars, DEL, >7-bit chars, trigraphs, and end of comment
    if (ch < ' ' || ch > '~' || ch == '?' || ch == '/') {
      result += "\\" + StringPrintf("%02X", ch);
    } else {
      result += ch;
    }
  }

  return result;
}

// static
std::string CWriter::MangleTypes(const TypeVector& types) {
  assert(types.size() >= 2);
  std::string result = "wasm_multi_";
  for (auto type : types) {
    result += MangleType(type);
  }
  return result;
}

/* The C symbol for an export from this module. */
std::string CWriter::ExportName(std::string_view export_name) const {
  return kGlobalSymbolPrefix + module_prefix_ + '_' + MangleName(export_name);
}

/* The C symbol for an export from an arbitrary module. */
// static
std::string CWriter::ExportName(std::string_view module_name,
                                std::string_view export_name) {
  return kGlobalSymbolPrefix + MangleModuleName(module_name) + '_' +
         MangleName(export_name);
}

/* The C symbol for a tail-callee export from this module. */
std::string CWriter::TailCallExportName(std::string_view export_name) const {
  return kTailCallSymbolPrefix + ExportName(export_name);
}

/* The C symbol for a tail-callee export from an arbitrary module. */
// static
std::string CWriter::TailCallExportName(std::string_view module_name,
                                        std::string_view export_name) {
  return kTailCallSymbolPrefix + ExportName(module_name, export_name);
}

/* The type name of an instance of this module. */
std::string CWriter::ModuleInstanceTypeName() const {
  return kGlobalSymbolPrefix + module_prefix_;
}

/* The type name of an instance of an arbitrary module. */
// static
std::string CWriter::ModuleInstanceTypeName(std::string_view module_name) {
  return kGlobalSymbolPrefix + MangleModuleName(module_name);
}

/*
 * Hardcoded "C"-locale versions of isalpha/isdigit/isalnum/isxdigit for use
 * in CWriter::Mangle(). We don't use the standard isalpha/isdigit/isalnum
 * because the caller might have changed the current locale.
 */
static bool internal_isalpha(uint8_t ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static bool internal_isdigit(uint8_t ch) {
  return (ch >= '0' && ch <= '9');
}

static bool internal_isalnum(uint8_t ch) {
  return internal_isalpha(ch) || internal_isdigit(ch);
}

static bool internal_ishexdigit(uint8_t ch) {
  return internal_isdigit(ch) || (ch >= 'A' && ch <= 'F');  // capitals only
}

// static
std::string CWriter::Mangle(std::string_view name, bool double_underscores) {
  /*
   * Name mangling transforms arbitrary Wasm names into "safe" C names
   * in a deterministic way. To avoid collisions, distinct Wasm names must be
   * transformed into distinct C names.
   *
   * The rules implemented here are:
   * 1) any hex digit ('A' through 'F') that follows the sequence "0x"
   *    is escaped
   * 2) any underscore at the beginning, at the end, or following another
   *    underscore, is escaped
   * 3) if double_underscores is set, underscores are replaced with
   *    two underscores.
   * 4) otherwise, any alphanumeric character is kept as-is,
   *    and any other character is escaped
   *
   * "Escaped" means the character is represented with the sequence "0xAB",
   * where A B are hex digits ('0'-'9' or 'A'-'F') representing the character's
   * numeric value.
   *
   * Module names are mangled with double_underscores=true to prevent
   * collisions between, e.g., a module "alfa" with export
   * "bravo_charlie" vs. a module "alfa_bravo" with export "charlie".
   */

  enum State { Any, Zero, ZeroX, ZeroXHexDigit } state{Any};
  bool last_was_underscore = false;

  std::string result;
  auto append_escaped = [&](const uint8_t ch) {
    result += "0x" + StringPrintf("%02X", ch);
    last_was_underscore = false;
    state = Any;
  };

  auto append_verbatim = [&](const uint8_t ch) {
    result += ch;
    last_was_underscore = (ch == '_');
  };

  for (auto it = name.begin(); it != name.end(); ++it) {
    const uint8_t ch = *it;
    switch (state) {
      case Any:
        state = (ch == '0') ? Zero : Any;
        break;
      case Zero:
        state = (ch == 'x') ? ZeroX : Any;
        break;
      case ZeroX:
        state = internal_ishexdigit(ch) ? ZeroXHexDigit : Any;
        break;
      case ZeroXHexDigit:
        WABT_UNREACHABLE;
        break;
    }

    /* rule 1 */
    if (state == ZeroXHexDigit) {
      append_escaped(ch);
      continue;
    }

    /* rule 2 */
    if ((ch == '_') && ((it == name.begin()) || (std::next(it) == name.end()) ||
                        last_was_underscore)) {
      append_escaped(ch);
      continue;
    }

    /* rule 3 */
    if (double_underscores && ch == '_') {
      append_verbatim(ch);
      append_verbatim(ch);
      continue;
    }

    /* rule 4 */
    if (internal_isalnum(ch) || (ch == '_')) {
      append_verbatim(ch);
    } else {
      append_escaped(ch);
    }
  }

  return result;
}

// static
std::string CWriter::MangleName(std::string_view name) {
  return Mangle(name, false);
}

// static
std::string CWriter::MangleModuleName(std::string_view name) {
  return Mangle(name, true);
}

/*
 * Allocate a C symbol (must be unused) in the SymbolSet,
 * and a mapping from the Wasm name (tagged with
 * the index space of the name) to that C symbol.
 */
void CWriter::ClaimName(SymbolSet& set,
                        SymbolMap& map,
                        char type_suffix,
                        std::string_view wasm_name,
                        const std::string& c_name) {
  const std::string type_tagged_wasm_name =
      std::string(wasm_name) + type_suffix;

  [[maybe_unused]] bool success;
  success = set.insert(c_name).second;
  assert(success);

  success = map.emplace(type_tagged_wasm_name, c_name).second;
  assert(success);
}

/*
 * Make a proposed C symbol unique in a given symbol set by appending
 * an integer to the symbol if necessary.
 */
std::string CWriter::FindUniqueName(SymbolSet& set,
                                    std::string_view proposed_name) const {
  std::string unique{proposed_name};
  if (set.find(unique) != set.end()) {
    std::string base = unique + "_";
    size_t count = 0;
    do {
      unique = base + std::to_string(count++);
    } while (set.find(unique) != set.end());
  }
  return unique;
}

/*
 * Find a unique C symbol in the symbol set and claim it (mapping the
 * type-tagged Wasm name to it).
 */
std::string CWriter::ClaimUniqueName(SymbolSet& set,
                                     SymbolMap& map,
                                     char type_suffix,
                                     std::string_view wasm_name,
                                     const std::string& proposed_c_name) {
  const std::string unique = FindUniqueName(set, proposed_c_name);
  ClaimName(set, map, type_suffix, wasm_name, unique);
  return unique;
}

std::string_view StripLeadingDollar(std::string_view name) {
  assert(!name.empty());
  assert(name.front() == '$');
  name.remove_prefix(1);
  return name;
}

void CWriter::DefineImportName(const Import* import,
                               std::string_view module,
                               std::string_view field_name) {
  std::string name;
  ModuleFieldType type{};

  switch (import->kind()) {
    case ExternalKind::Func:
      type = ModuleFieldType::Func;
      name = cast<FuncImport>(import)->func.name;
      break;
    case ExternalKind::Tag:
      type = ModuleFieldType::Tag;
      name = cast<TagImport>(import)->tag.name;
      break;
    case ExternalKind::Global:
      type = ModuleFieldType::Global;
      name = cast<GlobalImport>(import)->global.name;
      break;
    case ExternalKind::Memory:
      type = ModuleFieldType::Memory;
      name = cast<MemoryImport>(import)->memory.name;
      break;
    case ExternalKind::Table:
      type = ModuleFieldType::Table;
      name = cast<TableImport>(import)->table.name;
      break;
  }

  import_module_sym_map_.emplace(name, import->module_name);

  const std::string mangled = ExportName(module, field_name);
  global_syms_.erase(mangled);  // duplicate imports are allowed
  ClaimName(global_syms_, global_sym_map_, MangleField(type), name, mangled);
}

/*
 * Reserve a C symbol for the public name of a module's export.  The
 * format of these is "w2c_" + the module prefix + "_" + the mangled
 * export name. Reserving the symbol prevents internal functions and
 * other names from shadowing/overlapping the exports.
 */
void CWriter::ReserveExportName(std::string_view name) {
  ClaimName(global_syms_, global_sym_map_, MangleField(ModuleFieldType::Export),
            name, ExportName(name));
}

/*
 * Names for functions, function types, tags, and segments are globally unique
 * across modules (formatted the same as an export, as "w2c_" + module prefix +
 * "_" + the name, made unique if necessary).
 */
std::string CWriter::DefineGlobalScopeName(ModuleFieldType type,
                                           std::string_view name) {
  return ClaimUniqueName(global_syms_, global_sym_map_, MangleField(type), name,
                         ExportName(StripLeadingDollar(name)));
}

std::string CWriter::GetGlobalName(ModuleFieldType type,
                                   const std::string& name) const {
  std::string mangled = name + MangleField(type);
  assert(global_sym_map_.count(mangled) == 1);
  return global_sym_map_.at(mangled);
}

/* Names for params, locals, and stack vars are formatted as "var_" + name. */
std::string CWriter::DefineLocalScopeName(std::string_view name,
                                          bool is_label) {
  return ClaimUniqueName(
      local_syms_, local_sym_map_, is_label ? kLabelSuffix : kParamSuffix, name,
      kLocalSymbolPrefix + MangleName(StripLeadingDollar(name)));
}

std::string CWriter::GetLocalName(const std::string& name,
                                  bool is_label) const {
  std::string mangled = name + (is_label ? kLabelSuffix : kParamSuffix);
  assert(local_sym_map_.count(mangled) == 1);
  return local_sym_map_.at(mangled);
}

std::string CWriter::GetTailCallRef(const std::string& name) const {
  return kTailCallSymbolPrefix + GetGlobalName(ModuleFieldType::Func, name);
}

std::string CWriter::DefineParamName(std::string_view name) {
  return DefineLocalScopeName(name, false);
}

std::string CWriter::DefineLabelName(std::string_view name) {
  return DefineLocalScopeName(name, true);
}

std::string CWriter::DefineStackVarName(Index index,
                                        Type type,
                                        std::string_view name) {
  std::string unique =
      FindUniqueName(local_syms_, kLocalSymbolPrefix + MangleName(name));
  StackTypePair stp = {index, type};
  [[maybe_unused]] bool success =
      stack_var_sym_map_.emplace(stp, unique).second;
  assert(success);
  return unique;
}

/*
 * Members of the module instance (globals, tables, and memories) are formatted
 * as "w2c_" + the mangled name of the element (made unique if necessary).
 */
std::string CWriter::DefineInstanceMemberName(ModuleFieldType type,
                                              std::string_view name) {
  return ClaimUniqueName(
      global_syms_, global_sym_map_, MangleField(type), name,
      kGlobalSymbolPrefix + MangleName(StripLeadingDollar(name)));
}

/*
 * The name of a module-instance member that points to the originating
 * instance of an imported function is formatted as "w2c_" + originating
 * module prefix + "_instance".
 */
std::string CWriter::DefineImportedModuleInstanceName(std::string_view name) {
  return ClaimUniqueName(global_syms_, global_sym_map_,
                         MangleField(ModuleFieldType::Import), name,
                         ExportName(name, "instance"));
}

void CWriter::Indent(int size) {
  indent_ += size;
}

void CWriter::Dedent(int size) {
  indent_ -= size;
  assert(indent_ >= 0);
}

void CWriter::NonIndented(std::function<void()> func) {
  int copy = indent_;
  indent_ = 0;
  func();
  indent_ = copy;
}

void CWriter::WriteIndent() {
  static char s_indent[] =
      "                                                                       "
      "                                                                       ";
  static size_t s_indent_len = sizeof(s_indent) - 1;
  size_t to_write = indent_;
  while (to_write >= s_indent_len) {
    stream_->WriteData(s_indent, s_indent_len);
    to_write -= s_indent_len;
  }
  if (to_write > 0) {
    stream_->WriteData(s_indent, to_write);
  }
}

void CWriter::WriteData(const char* src, size_t size) {
  if (should_write_indent_next_) {
    WriteIndent();
    should_write_indent_next_ = false;
  }
  if (size > 0 && src[0] != '\n') {
    consecutive_newline_count_ = 0;
  }
  stream_->WriteData(src, size);
}

void WABT_PRINTF_FORMAT(2, 3) CWriter::Writef(const char* format, ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  WriteData(buffer, length);
}

void CWriter::Write(Newline) {
  // Allow maximum one blank line between sections
  if (consecutive_newline_count_ < 2) {
    Write("\n");
    consecutive_newline_count_++;
  }
  should_write_indent_next_ = true;
}

void CWriter::Write(OpenBrace) {
  Write("{");
  Indent();
  Write(Newline());
}

void CWriter::Write(CloseBrace) {
  Dedent();
  Write("}");
}

void CWriter::Write(uint64_t val) {
  Writef("%" PRIu64, val);
}

void CWriter::Write(std::string_view s) {
  WriteData(s.data(), s.size());
}

void CWriter::Write(const ParamName& name) {
  Write(GetLocalName(name.name, false));
}

void CWriter::Write(const LabelName& name) {
  Write(GetLocalName(name.name, true));
}

void CWriter::Write(const GlobalName& name) {
  Write(GetGlobalName(name.type, name.name));
}

void CWriter::Write(const TagSymbol& name) {
  if (!IsImport(name.name)) {
    Write("&");
  }
  Write(GlobalName(name));
}

void CWriter::Write(const ExternalInstancePtr& name) {
  if (!IsImport(name.name)) {
    Write("&");
  }
  Write("instance->", GlobalName(name));
}

void CWriter::Write(const ExternalRef& name) {
  if (name.type == ModuleFieldType::Func || !IsImport(name.name)) {
    Write(GlobalName(name));
  } else {
    Write("(*", GlobalName(name), ")");
  }
}

void CWriter::Write(const WrapperRef& name) {
  Write(kWrapperSymbolPrefix, GlobalName(name));
}

void CWriter::Write(const TailCallRef& name) {
  Write(GetTailCallRef(name.name));
}

void CWriter::Write(const ExternalInstanceRef& name) {
  if (IsImport(name.name)) {
    Write("(*instance->", GlobalName(name), ")");
  } else {
    Write("instance->", GlobalName(name));
  }
}

void CWriter::Write(const GotoLabel& goto_label) {
  const Label* label = FindLabel(goto_label.var);
  if (label->HasValue()) {
    size_t amount = label->sig.size();
    assert(type_stack_.size() >= label->type_stack_size);
    assert(type_stack_.size() >= amount);
    assert(type_stack_.size() - amount >= label->type_stack_size);
    Index offset = type_stack_.size() - label->type_stack_size - amount;
    if (offset != 0) {
      for (Index i = 0; i < amount; ++i) {
        Write(StackVar(amount - i - 1 + offset, label->sig[i]), " = ",
              StackVar(amount - i - 1), "; ");
      }
    }
  }

  WriteUnwindTryCatchStack(label);

  if (goto_label.var.is_name()) {
    Write("goto ", LabelName(goto_label.var.name()), ";");
  } else {
    // We've generated names for all labels, so we should only be using an
    // index when branching to the implicit function label, which can't be
    // named.
    Write("goto ", LabelName(kImplicitFuncLabel), ";");
  }
}

void CWriter::Write(const LabelDecl& label) {
  if (IsTopLabelUsed())
    Write(label.name, ":;", Newline());
}

void CWriter::Write(const GlobalInstanceVar& var) {
  assert(var.var.is_name());
  Write(ExternalInstanceRef(ModuleFieldType::Global, var.var.name()));
}

void CWriter::Write(const StackVar& sv) {
  Index index = type_stack_.size() - 1 - sv.index;
  Type type = sv.type;
  if (type == Type::Any) {
    assert(index < type_stack_.size());
    type = type_stack_[index];
  }

  StackTypePair stp = {index, type};
  auto iter = stack_var_sym_map_.find(stp);
  if (iter == stack_var_sym_map_.end()) {
    std::string name = MangleType(type) + std::to_string(index);
    Write(DefineStackVarName(index, type, name));
  } else {
    Write(iter->second);
  }
}

// static
const char* CWriter::GetCTypeName(const Type& type) {
  // clang-format off
  switch (type) {
  case Type::I32: return "u32";
  case Type::I64: return "u64";
  case Type::F32: return "f32";
  case Type::F64: return "f64";
  case Type::V128: return "v128";
  case Type::FuncRef: return "wasm_rt_funcref_t";
  case Type::ExternRef: return "wasm_rt_externref_t";
  case Type::ExnRef: return "wasm_rt_exnref_t";
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

void CWriter::Write(Type type) {
  Write(GetCTypeName(type));
}

void CWriter::Write(TypeEnum type) {
  // clang-format off
  switch (type.type) {
    case Type::I32: Write("WASM_RT_I32"); break;
    case Type::I64: Write("WASM_RT_I64"); break;
    case Type::F32: Write("WASM_RT_F32"); break;
    case Type::F64: Write("WASM_RT_F64"); break;
    case Type::V128: Write("WASM_RT_V128"); break;
    case Type::FuncRef: Write("WASM_RT_FUNCREF"); break;
    case Type::ExternRef: Write("WASM_RT_EXTERNREF"); break;
    case Type::ExnRef: Write("WASM_RT_EXNREF"); break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

void CWriter::Write(SignedType type) {
  // clang-format off
  switch (type.type) {
    case Type::I32: Write("s32"); break;
    case Type::I64: Write("s64"); break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
}

void CWriter::Write(const TypeVector& types) {
  if (types.empty()) {
    Write("void");
  } else if (types.size() == 1) {
    Write(types[0]);
  } else {
    Write("struct ", MangleTypes(types));
  }
}

void CWriter::Write(const Const& const_) {
  switch (const_.type()) {
    case Type::I32:
      Writef("%uu", static_cast<int32_t>(const_.u32()));
      break;

    case Type::I64:
      Writef("%" PRIu64 "ull", static_cast<int64_t>(const_.u64()));
      break;

    case Type::F32: {
      uint32_t f32_bits = const_.f32_bits();
      // TODO(binji): Share with similar float info in interp.cc and literal.cc
      if ((f32_bits & 0x7f800000u) == 0x7f800000u) {
        const char* sign = (f32_bits & 0x80000000) ? "-" : "";
        uint32_t significand = f32_bits & 0x7fffffu;
        if (significand == 0) {
          // Infinity.
          Writef("%sINFINITY", sign);
        } else {
          // Nan.
          Writef("f32_reinterpret_i32(0x%08x) /* %snan:0x%06x */", f32_bits,
                 sign, significand);
        }
      } else if (f32_bits == 0x80000000) {
        // Negative zero. Special-cased so it isn't written as -0 below.
        Writef("-0.f");
      } else {
        Writef("%.9g", Bitcast<float>(f32_bits));
      }
      break;
    }

    case Type::F64: {
      uint64_t f64_bits = const_.f64_bits();
      // TODO(binji): Share with similar float info in interp.cc and literal.cc
      if ((f64_bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) {
        const char* sign = (f64_bits & 0x8000000000000000ull) ? "-" : "";
        uint64_t significand = f64_bits & 0xfffffffffffffull;
        if (significand == 0) {
          // Infinity.
          Writef("%sINFINITY", sign);
        } else {
          // Nan.
          Writef("f64_reinterpret_i64(0x%016" PRIx64 ") /* %snan:0x%013" PRIx64
                 " */",
                 f64_bits, sign, significand);
        }
      } else if (f64_bits == 0x8000000000000000ull) {
        // Negative zero. Special-cased so it isn't written as -0 below.
        Writef("-0.0");
      } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "%.17g", Bitcast<double>(f64_bits));
        // Append .0 if sprint didn't include a decimal point or use the
        // exponent ('e') form.  This is a workaround for an MSVC parsing
        // issue: https://github.com/WebAssembly/wabt/issues/2422
        if (!strchr(buf, '.') && !strchr(buf, 'e')) {
          strcat(buf, ".0");
        }
        Writef("%s", buf);
      }
      break;
    }
    case Type::V128: {
      Writef("v128_const(0x%02x", const_.vec128().u8(0));
      for (int i = 1; i < 16; i++) {
        Writef(", 0x%02x", const_.vec128().u8(i));
      }
      Write(")");
      break;
    }

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::WriteInitDecl() {
  Write("void ", kAdminSymbolPrefix, module_prefix_, "_instantiate(",
        ModuleInstanceTypeName(), "*");
  for (const auto& import_module_name : import_module_set_) {
    Write(", struct ", ModuleInstanceTypeName(import_module_name), "*");
  }
  Write(");", Newline());
}

void CWriter::WriteFreeDecl() {
  Write("void ", kAdminSymbolPrefix, module_prefix_, "_free(",
        ModuleInstanceTypeName(), "*);", Newline());
}

void CWriter::WriteGetFuncTypeDecl() {
  Write("wasm_rt_func_type_t ", kAdminSymbolPrefix, module_prefix_,
        "_get_func_type(uint32_t param_count, uint32_t result_count, ...);",
        Newline());
}

static std::string GetMemoryTypeString(const Memory& memory) {
  return memory.page_limits.is_shared ? "wasm_rt_shared_memory_t"
                                      : "wasm_rt_memory_t";
}

static std::string GetMemoryAPIString(const Memory& memory, std::string api) {
  std::string suffix;
  if (memory.page_limits.is_shared) {
    suffix += "_shared";
  }

  // Memory load and store routines can be optimized for default-page-size,
  // 32-bit memories (by using hardware to bounds-check memory access).
  // Append "_default32" to these function names to choose the (possibly) fast
  // path.
  //
  // We don't need to do this for runtime routines; those can check the
  // wasm_rt_memory_t structure.
  if (api.substr(0, 8) != "wasm_rt_" &&
      memory.page_size == WABT_DEFAULT_PAGE_SIZE &&
      memory.page_limits.is_64 == false) {
    suffix += "_default32";
  }
  return api + suffix;
}

void CWriter::WriteInitExpr(const ExprList& expr_list) {
  if (expr_list.empty()) {
    WABT_UNREACHABLE;
  }

  std::vector<std::string> mini_stack;

  for (const auto& expr : expr_list) {
    if (expr.type() == ExprType::Binary) {
      // Extended const expressions include at least one binary op.
      // This builds a C expression from the operands.
      if (mini_stack.size() < 2) {
        WABT_UNREACHABLE;
      }

      const auto binexpr = cast<BinaryExpr>(&expr);
      char op;
      switch (binexpr->opcode) {
        case Opcode::I32Add:
        case Opcode::I64Add:
        case Opcode::F32Add:
        case Opcode::F64Add:
          op = '+';
          break;

        case Opcode::I32Sub:
        case Opcode::I64Sub:
        case Opcode::F32Sub:
        case Opcode::F64Sub:
          op = '-';
          break;

        case Opcode::I32Mul:
        case Opcode::I64Mul:
        case Opcode::F32Mul:
        case Opcode::F64Mul:
          op = '*';
          break;

        default:
          WABT_UNREACHABLE;
      }

      std::string combination =
          "((" + std::string(GetCTypeName(binexpr->opcode.GetParamType1())) +
          ")" + mini_stack.at(mini_stack.size() - 2) + ")" + op + "((" +
          std::string(GetCTypeName(binexpr->opcode.GetParamType2())) + ")" +
          mini_stack.at(mini_stack.size() - 1) + ")";
      mini_stack.resize(mini_stack.size() - 2);
      mini_stack.push_back(std::move(combination));
    } else {
      // Leaf node (nullary const expression)
      Stream* existing_stream = stream_;
      MemoryStream terminal_stream;
      stream_ = &terminal_stream;
      WriteInitExprTerminal(&expr);
      const auto& buf = terminal_stream.output_buffer();
      mini_stack.emplace_back(reinterpret_cast<const char*>(buf.data.data()),
                              buf.data.size());
      stream_ = existing_stream;
    }
  }

  if (mini_stack.size() != 1) {
    WABT_UNREACHABLE;
  }

  Write(mini_stack.front());
}

void CWriter::WriteInitExprTerminal(const Expr* expr) {
  switch (expr->type()) {
    case ExprType::Const:
      Write(cast<ConstExpr>(expr)->const_);
      break;

    case ExprType::GlobalGet:
      Write(GlobalInstanceVar(cast<GlobalGetExpr>(expr)->var));
      break;

    case ExprType::RefFunc: {
      const Func* func = module_->GetFunc(cast<RefFuncExpr>(expr)->var);
      const FuncDeclaration& decl = func->decl;

      assert(decl.has_func_type);
      const FuncType* func_type = module_->GetFuncType(decl.type_var);

      Write("(wasm_rt_funcref_t){", FuncTypeExpr(func_type), ", ",
            "(wasm_rt_function_ptr_t)", WrapperRef(func->name), ", {");
      if (options_.features.tail_call_enabled() &&
          (IsImport(func->name) || func->features_used.tailcall)) {
        Write(TailCallRef(func->name));
      } else {
        Write("NULL");
      }
      Write("}, ");

      if (IsImport(func->name)) {
        Write("instance->", GlobalName(ModuleFieldType::Import,
                                       import_module_sym_map_[func->name]));
      } else {
        Write("instance");
      }
      Write("}");
    } break;

    case ExprType::RefNull:
      Write(GetReferenceNullValue(cast<RefNullExpr>(expr)->type));
      break;

    default:
      WABT_UNREACHABLE;
  }
}

std::string CWriter::GenerateHeaderGuard() const {
  std::string result;
  for (char c : header_name_) {
    if (isalnum(c) || c == '_') {
      result += toupper(c);
    } else {
      result += '_';
    }
  }
  result += "_GENERATED_";
  return result;
}

void CWriter::WriteSourceTop() {
  Write(s_source_includes);
  Write(Newline(), "#include \"", header_name_, "\"", Newline());

  if (IsSingleUnsharedMemory()) {
    Write("#define IS_SINGLE_UNSHARED_MEMORY 1", Newline());
  }

  Write(s_source_declarations, Newline());

  if (module_->features_used.simd) {
    if (!simd_used_in_header_) {
      WriteV128Decl();
    }
    Write(s_simd_source_declarations);
  }

  if (module_->features_used.threads) {
    Write(s_atomicops_source_declarations);
  }
}

void CWriter::WriteMultiCTop() {
  if (c_streams_.size() > 1) {
    assert(header_impl_name_.size() > 0);
    Write("/* Automatically generated by wasm2c */", Newline());
    Write("#include \"", header_impl_name_, "\"", Newline());
  }
}

void CWriter::WriteMultiCTopEmpty() {
  for (auto& stream : c_streams_) {
    if (stream->offset() == 0) {
      stream_ = stream;
      Write("/* Empty wasm2c generated file */\n");
      Write("typedef int dummy_def;");
    }
  }
}

void CWriter::DeclareStruct(const TypeVector& types) {
  const std::string name = MangleTypes(types);
  if (!typevector_structs_.insert(name).second) {
    return;
  }

  Write(Newline(), "#ifndef ", name, Newline());
  Write("#define ", name, " ", name, Newline());
  Write("struct ", name, " ", OpenBrace());
  for (Index i = 0; i < types.size(); ++i) {
    const Type type = types[i];
    Write(type);
    Writef(" %c%d;", MangleType(type), i);
    Write(Newline());
  }
  Write(CloseBrace(), ";", Newline(), "#endif  /* ", name, " */", Newline());
}

void CWriter::WriteTailcalleeParamTypes() {
  // A structure for the spilled parameters of a tail-callee is needed in
  // three cases: for a function that makes a tail-call (and therefore
  // will have a tail-callee version generated), for any imported function
  // (for which wasm2c generates a weak import that presents the tail-callee
  // interface, in case the exporting module doesn't generate one itself), and
  // for any type entry referenced in a return_call_indirect instruction.

  for (const Func* func : module_->funcs) {
    if (IsImport(func->name) || func->features_used.tailcall) {
      if (func->decl.sig.GetNumParams() > 1) {
        DeclareStruct(func->decl.sig.param_types);
      }
    }
  }

  for (TypeEntry* type : module_->types) {
    FuncType* func_type = cast<FuncType>(type);
    if (func_type->GetNumParams() > 1 && func_type->features_used.tailcall) {
      DeclareStruct(func_type->sig.param_types);
    }
  }
}

void CWriter::WriteMultivalueResultTypes() {
  for (TypeEntry* type : module_->types) {
    FuncType* func_type = cast<FuncType>(type);
    if (func_type->GetNumResults() > 1) {
      DeclareStruct(func_type->sig.result_types);
    }
  }
}

void CWriter::WriteTagTypes() {
  for (const Tag* tag : module_->tags) {
    const FuncDeclaration& tag_type = tag->decl;
    Index num_params = tag_type.GetNumParams();
    if (num_params <= 1) {
      continue;
    }
    DeclareStruct(tag_type.sig.param_types);
  }
}

void CWriter::WriteFuncTypeDecls() {
  if (module_->types.empty()) {
    return;
  }

  Write(Newline());

  std::string serialized_type;
  for (const TypeEntry* type : module_->types) {
    const std::string name =
        DefineGlobalScopeName(ModuleFieldType::Type, type->name);

    if (c_streams_.size() > 1) {
      Write("FUNC_TYPE_DECL_EXTERN_T(", name, ");", Newline());
    }
  }
}

void CWriter::WriteFuncTypes() {
  if (module_->types.empty()) {
    return;
  }

  Write(Newline());

  std::unordered_map<std::string, std::string> type_hash;

  std::string serialized_type;
  for (const TypeEntry* type : module_->types) {
    const std::string name = GetGlobalName(ModuleFieldType::Type, type->name);
    SerializeFuncType(*cast<FuncType>(type), serialized_type);

    auto prior_type = type_hash.find(serialized_type);
    if (prior_type != type_hash.end()) {
      /* duplicate function type */
      unique_func_type_names_.push_back(prior_type->second);
    } else {
      unique_func_type_names_.push_back(name);
      type_hash.emplace(serialized_type, name);
      if (c_streams_.size() > 1) {
        Write("FUNC_TYPE_EXTERN_T(");
      } else {
        Write("FUNC_TYPE_T(");
      }
      Write(name, ") = \"");
      for (uint8_t x : serialized_type) {
        Writef("\\x%02x", x);
      }
      Write("\";", Newline());
    }
  }
}

void CWriter::Write(const FuncTypeExpr& expr) {
  Index func_type_index = module_->GetFuncTypeIndex(expr.func_type->sig);
  Write(unique_func_type_names_.at(func_type_index));
}

// static
void CWriter::SerializeFuncType(const FuncType& func_type,
                                std::string& serialized_type) {
  unsigned int len = func_type.GetNumParams() + func_type.GetNumResults() + 1;

  char* const mangled_signature = static_cast<char*>(alloca(len));
  char* next_byte = mangled_signature;

  // step 1: serialize each param type
  for (Index i = 0; i < func_type.GetNumParams(); ++i) {
    *next_byte++ = MangleType(func_type.GetParamType(i));
  }

  // step 2: separate params and results with a space
  *next_byte++ = ' ';

  // step 3: serialize each result type
  for (Index i = 0; i < func_type.GetNumResults(); ++i) {
    *next_byte++ = MangleType(func_type.GetResultType(i));
  }

  assert(next_byte - mangled_signature == static_cast<ptrdiff_t>(len));

  // step 4: SHA-256 the whole string
  sha256({mangled_signature, len}, serialized_type);
}

void CWriter::WriteTagDecls() {
  Index tag_index = 0;
  for (const Tag* tag : module_->tags) {
    bool is_import = tag_index < module_->num_tag_imports;
    if (!is_import) {
      // Tags are identified and compared solely by their (unique) address.
      // The data stored in this variable is never read.
      if (tag_index == module_->num_tag_imports) {
        Write(Newline());
        Write("typedef char wasm_tag_placeholder_t;", Newline());
      }
      DefineGlobalScopeName(ModuleFieldType::Tag, tag->name);
      if (c_streams_.size() > 1) {
        Write("extern const wasm_tag_placeholder_t ",
              GlobalName(ModuleFieldType::Tag, tag->name), ";", Newline());
      }
    }
    tag_index++;
  }
}

void CWriter::WriteTags() {
  Write(Newline());
  Index tag_index = 0;
  for (const Tag* tag : module_->tags) {
    bool is_import = tag_index < module_->num_tag_imports;
    if (!is_import) {
      Write(InternalSymbolScope(), "const wasm_tag_placeholder_t ",
            GlobalName(ModuleFieldType::Tag, tag->name), ";", Newline());
    }
    tag_index++;
  }
}

void CWriter::ComputeUniqueImports() {
  using modname_name_pair = std::pair<std::string, std::string>;
  std::map<modname_name_pair, const Import*> import_map;
  for (const Import* import : module_->imports) {
    // After emplacing, the returned bool says whether the insert happened;
    // i.e., was there already an import with the same modname and name?
    // If there was, make sure it was at least the same kind of import.
    const auto iterator_and_insertion_bool = import_map.emplace(
        modname_name_pair(import->module_name, import->field_name), import);
    if (!iterator_and_insertion_bool.second) {
      if (iterator_and_insertion_bool.first->second->kind() != import->kind()) {
        UNIMPLEMENTED("contradictory import declaration");
      } else {
        fprintf(stderr, "warning: duplicate import declaration \"%s\" \"%s\"\n",
                import->module_name.c_str(), import->field_name.c_str());
      }
    }
    import_module_set_.insert(import->module_name);
    if (import->kind() == ExternalKind::Func) {
      import_func_module_set_.insert(import->module_name);
    }
  }

  for (const auto& node : import_map) {
    unique_imports_.push_back(node.second);
  }
}

void CWriter::BeginInstance() {
  if (module_->imports.empty()) {
    Write("typedef struct ", ModuleInstanceTypeName(), " ", OpenBrace());
    return;
  }

  ComputeUniqueImports();

  // define names of per-instance imports
  for (const Import* import : module_->imports) {
    DefineImportName(import, import->module_name, import->field_name);
  }

  // Forward declaring module instance types
  for (const auto& import_module : import_module_set_) {
    DefineImportedModuleInstanceName(import_module);
    Write("struct ", ModuleInstanceTypeName(import_module), ";", Newline());
  }

  // Forward declaring module imports
  for (const Import* import : unique_imports_) {
    if ((import->kind() == ExternalKind::Func) ||
        (import->kind() == ExternalKind::Tag)) {
      continue;
    }

    Write("extern ");
    switch (import->kind()) {
      case ExternalKind::Global: {
        const Global& global = cast<GlobalImport>(import)->global;
        Write(global.type);
        break;
      }

      case ExternalKind::Memory: {
        Write(GetMemoryTypeString(cast<MemoryImport>(import)->memory));
        break;
      }

      case ExternalKind::Table:
        WriteTableType(cast<TableImport>(import)->table.elem_type);
        break;

      default:
        WABT_UNREACHABLE;
    }
    Write("* ", ExportName(import->module_name, import->field_name), "(struct ",
          ModuleInstanceTypeName(import->module_name), "*);", Newline());
  }
  Write(Newline());

  // Add pointers to module instances that any func is imported from,
  // so that imported functions can be given their own module instances
  // when invoked
  Write("typedef struct ", ModuleInstanceTypeName(), " ", OpenBrace());
  for (const auto& import_module : import_func_module_set_) {
    Write("struct ", ModuleInstanceTypeName(import_module), "* ",
          GlobalName(ModuleFieldType::Import, import_module), ";", Newline());
  }

  for (const Import* import : unique_imports_) {
    if ((import->kind() == ExternalKind::Func) ||
        (import->kind() == ExternalKind::Tag)) {
      continue;
    }

    Write("/* import: '", SanitizeForComment(import->module_name), "' '",
          SanitizeForComment(import->field_name), "' */", Newline());

    switch (import->kind()) {
      case ExternalKind::Global:
        WriteGlobal(cast<GlobalImport>(import)->global,
                    std::string("*") +
                        ExportName(import->module_name, import->field_name));
        break;

      case ExternalKind::Memory:
        WriteMemory(std::string("*") +
                        ExportName(import->module_name, import->field_name),
                    cast<MemoryImport>(import)->memory);
        break;

      case ExternalKind::Table: {
        const Table& table = cast<TableImport>(import)->table;
        WriteTable(std::string("*") +
                       ExportName(import->module_name, import->field_name),
                   table.elem_type);
      } break;

      default:
        WABT_UNREACHABLE;
    }

    Write(Newline());
  }
}

// Write module-wide imports (funcs & tags), which aren't tied to an instance.
void CWriter::WriteImports() {
  if (unique_imports_.empty())
    return;

  Write(Newline());

  for (const Import* import : unique_imports_) {
    if (import->kind() == ExternalKind::Func) {
      Write(Newline(), "/* import: '", SanitizeForComment(import->module_name),
            "' '", SanitizeForComment(import->field_name), "' */", Newline());
      const Func& func = cast<FuncImport>(import)->func;
      WriteImportFuncDeclaration(
          func.decl, import->module_name,
          ExportName(import->module_name, import->field_name));
      Write(";", Newline());
      if (options_.features.tail_call_enabled()) {
        WriteTailCallFuncDeclaration(GetTailCallRef(func.name));
        Write(";", Newline());
      }
    } else if (import->kind() == ExternalKind::Tag) {
      Write(Newline(), "/* import: '", SanitizeForComment(import->module_name),
            "' '", SanitizeForComment(import->field_name), "' */", Newline());
      Write("extern const wasm_rt_tag_t ",
            ExportName(import->module_name, import->field_name), ";",
            Newline());
    }
  }
}

void CWriter::WriteTailCallWeakImports() {
  for (const Import* import : unique_imports_) {
    if (import->kind() != ExternalKind::Func) {
      continue;
    }
    const Func& func = cast<FuncImport>(import)->func;
    Write(Newline(), "/* handler for missing tail-call on import: '",
          SanitizeForComment(import->module_name), "' '",
          SanitizeForComment(import->field_name), "' */", Newline());
    Write("WEAK_FUNC_DECL(",
          TailCallExportName(import->module_name, import->field_name), ", ",
          kTailCallFallbackPrefix, module_prefix_, "_",
          ExportName(import->module_name, import->field_name), ")", Newline());
    Write(OpenBrace(), "next->fn = NULL;", Newline());

    Index num_params = func.GetNumParams();
    Index num_results = func.GetNumResults();
    if (num_params >= 1) {
      Write(func.decl.sig.param_types, " params;", Newline());
      Write("wasm_rt_memcpy(params, tail_call_stack, sizeof(params));",
            Newline());
    }

    if (num_results >= 1) {
      Write(func.decl.sig.result_types, " result = ");
    }

    Write(ExportName(import->module_name, import->field_name),
          "(*instance_ptr");

    if (num_params == 1) {
      Write(", params");
    } else {
      for (Index i = 0; i < num_params; ++i) {
        Writef(", params.%c%d", MangleType(func.GetParamType(i)), i);
      }
    }

    Write(");", Newline());

    if (num_results >= 1) {
      Write("wasm_rt_memcpy(tail_call_stack, &result, sizeof(result));",
            Newline());
    }

    Write(CloseBrace(), Newline());
  }
}

void CWriter::WriteFuncDeclarations() {
  if (module_->funcs.size() == module_->num_func_imports)
    return;

  Write(Newline());

  Index func_index = 0;
  for (const Func* func : module_->funcs) {
    bool is_import = func_index < module_->num_func_imports;
    if (!is_import) {
      Write(InternalSymbolScope());
      WriteFuncDeclaration(
          func->decl, DefineGlobalScopeName(ModuleFieldType::Func, func->name));
      Write(";", Newline());

      if (func->features_used.tailcall) {
        Write(InternalSymbolScope());
        WriteTailCallFuncDeclaration(GetTailCallRef(func->name));
        Write(";", Newline());
      }
    }
    ++func_index;
  }
}

void CWriter::WriteFuncDeclaration(const FuncDeclaration& decl,
                                   const std::string& name) {
  Write(decl.sig.result_types, " ", name, "(");
  Write(ModuleInstanceTypeName(), "*");
  WriteParamTypes(decl);
  Write(")");
}

void CWriter::WriteTailCallFuncDeclaration(const std::string& mangled_name) {
  Write("void ", mangled_name,
        "(void **instance_ptr, void *tail_call_stack, wasm_rt_tailcallee_t "
        "*next)");
}

void CWriter::WriteImportFuncDeclaration(const FuncDeclaration& decl,
                                         const std::string& module_name,
                                         const std::string& name) {
  Write(decl.sig.result_types, " ", name, "(");
  Write("struct ", ModuleInstanceTypeName(module_name), "*");
  WriteParamTypes(decl);
  Write(")");
}

void CWriter::WriteCallIndirectFuncDeclaration(const FuncDeclaration& decl,
                                               const std::string& name) {
  Write(decl.sig.result_types, " ", name, "(void*");
  WriteParamTypes(decl);
  Write(")");
}

static bool func_uses_simd(const FuncSignature& sig) {
  return std::any_of(sig.param_types.begin(), sig.param_types.end(),
                     [](auto x) { return x == Type::V128; }) ||
         std::any_of(sig.result_types.begin(), sig.result_types.end(),
                     [](auto x) { return x == Type::V128; });
}

void CWriter::ComputeSimdScope() {
  simd_used_in_header_ =
      module_->features_used.simd &&
      (std::any_of(module_->globals.begin(), module_->globals.end(),
                   [](const auto& x) { return x->type == Type::V128; }) ||
       std::any_of(module_->imports.begin(), module_->imports.end(),
                   [](const auto& x) {
                     return x->kind() == ExternalKind::Func &&
                            func_uses_simd(cast<FuncImport>(x)->func.decl.sig);
                   }) ||
       std::any_of(module_->exports.begin(), module_->exports.end(),
                   [&](const auto& x) {
                     return x->kind == ExternalKind::Func &&
                            func_uses_simd(module_->GetFunc(x->var)->decl.sig);
                   }));
}

void CWriter::WriteHeaderIncludes() {
  Write("#include \"wasm-rt.h\"", Newline());

  if (module_->features_used.exceptions) {
    Write("#include \"wasm-rt-exceptions.h\"", Newline(), Newline());
  }

  if (simd_used_in_header_) {
    WriteV128Decl();
  }

  Write(Newline());
}

void CWriter::WriteV128Decl() {
  Write("#include <simde/wasm/simd128.h>", Newline(), Newline());
  Write("#ifndef WASM_RT_SIMD_TYPE_DEFINED", Newline(),
        "#define WASM_RT_SIMD_TYPE_DEFINED", Newline(),
        "typedef simde_v128_t v128;", Newline(), "#endif", Newline(),
        Newline());
}

void CWriter::WriteModuleInstance() {
  BeginInstance();
  WriteGlobals();
  WriteMemories();
  WriteTables();
  WriteDataInstances();
  WriteElemInstances();

  // C forbids an empty struct
  if (module_->globals.empty() && module_->memories.empty() &&
      module_->tables.empty() && import_func_module_set_.empty()) {
    Write("char dummy_member;", Newline());
  }

  Write(CloseBrace(), " ", ModuleInstanceTypeName(), ";", Newline());
  Write(Newline());
}

void CWriter::WriteGlobals() {
  Index global_index = 0;
  if (module_->globals.size() != module_->num_global_imports) {
    for (const Global* global : module_->globals) {
      bool is_import = global_index < module_->num_global_imports;
      if (!is_import) {
        WriteGlobal(*global, DefineInstanceMemberName(ModuleFieldType::Global,
                                                      global->name));
        Write(Newline());
      }
      ++global_index;
    }
  }
}

void CWriter::WriteGlobal(const Global& global, const std::string& name) {
  Write(global.type, " ", name, ";");
}

void CWriter::WriteGlobalPtr(const Global& global, const std::string& name) {
  Write(global.type, "* ", name, "(", ModuleInstanceTypeName(), "* instance)");
}

void CWriter::WriteMemories() {
  if (module_->memories.size() == module_->num_memory_imports)
    return;

  Index memory_index = 0;
  for (const Memory* memory : module_->memories) {
    bool is_import = memory_index < module_->num_memory_imports;
    if (!is_import) {
      WriteMemory(
          DefineInstanceMemberName(ModuleFieldType::Memory, memory->name),
          *memory);
      Write(Newline());
    }
    ++memory_index;
  }
}

void CWriter::WriteMemory(const std::string& name, const Memory& memory) {
  Write(GetMemoryTypeString(memory), " ", name, ";");
}

void CWriter::WriteMemoryPtr(const std::string& name, const Memory& memory) {
  Write(GetMemoryTypeString(memory), "* ", name, "(", ModuleInstanceTypeName(),
        "* instance)");
}

void CWriter::WriteTables() {
  if (module_->tables.size() == module_->num_table_imports) {
    return;
  }

  Index table_index = 0;
  for (const Table* table : module_->tables) {
    bool is_import = table_index < module_->num_table_imports;
    if (!is_import) {
      WriteTable(DefineInstanceMemberName(ModuleFieldType::Table, table->name),
                 table->elem_type);
      Write(Newline());
    }
    ++table_index;
  }
}

void CWriter::WriteTable(const std::string& name, const wabt::Type& type) {
  WriteTableType(type);
  Write(" ", name, ";");
}

void CWriter::WriteTablePtr(const std::string& name, const Table& table) {
  WriteTableType(table.elem_type);
  Write("* ", name, "(", ModuleInstanceTypeName(), "* instance)");
}

void CWriter::WriteTableType(const wabt::Type& type) {
  Write("wasm_rt_", GetReferenceTypeName(type), "_table_t");
}

void CWriter::WriteGlobalInitializers() {
  if (module_->globals.empty())
    return;

  Write(Newline(), "static void init_globals(", ModuleInstanceTypeName(),
        "* instance) ", OpenBrace());
  Index global_index = 0;
  for (const Global* global : module_->globals) {
    bool is_import = global_index < module_->num_global_imports;
    if (!is_import) {
      assert(!global->init_expr.empty());
      Write(ExternalInstanceRef(ModuleFieldType::Global, global->name), " = ");
      WriteInitExpr(global->init_expr);
      Write(";", Newline());
    }
    ++global_index;
  }
  Write(CloseBrace(), Newline());
}

static inline bool is_droppable(const DataSegment* data_segment) {
  return (data_segment->kind == SegmentKind::Passive) &&
         (!data_segment->data.empty());
}

static inline bool is_droppable(const ElemSegment* elem_segment) {
  return (elem_segment->kind == SegmentKind::Passive) &&
         (!elem_segment->elem_exprs.empty());
}

void CWriter::WriteDataInstances() {
  for (const DataSegment* data_segment : module_->data_segments) {
    std::string name =
        DefineGlobalScopeName(ModuleFieldType::DataSegment, data_segment->name);
    if (is_droppable(data_segment)) {
      Write("bool ", "data_segment_dropped_", name, " : 1;", Newline());
    }
  }
}

void CWriter::WriteDataInitializerDecls() {
  if (module_->memories.empty()) {
    return;
  }

  for (const DataSegment* data_segment : module_->data_segments) {
    if (data_segment->data.empty()) {
      continue;
    }

    if (c_streams_.size() > 1) {
      Write(Newline(), "extern const u8 data_segment_data_",
            GlobalName(ModuleFieldType::DataSegment, data_segment->name), "[];",
            Newline());
    }
  }
}

void CWriter::WriteDataInitializers() {
  if (module_->memories.empty()) {
    return;
  }

  for (const DataSegment* data_segment : module_->data_segments) {
    if (data_segment->data.empty()) {
      continue;
    }

    Write(Newline(), InternalSymbolScope(), "const u8 data_segment_data_",
          GlobalName(ModuleFieldType::DataSegment, data_segment->name),
          "[] = ", OpenBrace());
    size_t i = 0;
    for (uint8_t x : data_segment->data) {
      Writef("0x%02x, ", x);
      if ((++i % 12) == 0)
        Write(Newline());
    }
    if (i > 0)
      Write(Newline());
    Write(CloseBrace(), ";", Newline());
  }

  Write(Newline(), "static void init_memories(", ModuleInstanceTypeName(),
        "* instance) ", OpenBrace());
  if (module_->memories.size() > module_->num_memory_imports) {
    Index memory_idx = module_->num_memory_imports;
    for (Index i = memory_idx; i < module_->memories.size(); i++) {
      const Memory* memory = module_->memories[i];
      const uint64_t max =
          memory->page_limits.has_max
              ? memory->page_limits.max
              : WABT_BYTES_TO_MIN_PAGES(
                    (memory->page_limits.is_64 ? UINT64_MAX : UINT32_MAX),
                    memory->page_size);
      std::string func = GetMemoryAPIString(*memory, "wasm_rt_allocate_memory");
      Write(
          func, "(", ExternalInstancePtr(ModuleFieldType::Memory, memory->name),
          ", ", memory->page_limits.initial, ", ", max, ", ",
          memory->page_limits.is_64, ", ", memory->page_size, ");", Newline());
    }
  }

  for (const DataSegment* data_segment : module_->data_segments) {
    if (data_segment->kind != SegmentKind::Active) {
      continue;
    }
    const Memory* memory =
        module_->memories[module_->GetMemoryIndex(data_segment->memory_var)];
    Write("LOAD_DATA(",
          ExternalInstanceRef(ModuleFieldType::Memory, memory->name), ", ");
    WriteInitExpr(data_segment->offset);
    if (data_segment->data.empty()) {
      Write(", NULL, 0");
    } else {
      Write(", data_segment_data_",
            GlobalName(ModuleFieldType::DataSegment, data_segment->name), ", ",
            data_segment->data.size());
    }
    Write(");", Newline());
  }

  Write(CloseBrace(), Newline());

  if (!module_->data_segments.empty()) {
    Write(Newline(), "static void init_data_instances(",
          ModuleInstanceTypeName(), " *instance) ", OpenBrace());

    for (const DataSegment* data_segment : module_->data_segments) {
      if (is_droppable(data_segment)) {
        Write("instance->data_segment_dropped_",
              GlobalName(ModuleFieldType::DataSegment, data_segment->name),
              " = false;", Newline());
      }
    }

    Write(CloseBrace(), Newline());
  }
}

void CWriter::WriteElemInstances() {
  for (const ElemSegment* elem_segment : module_->elem_segments) {
    std::string name =
        DefineGlobalScopeName(ModuleFieldType::ElemSegment, elem_segment->name);
    if (is_droppable(elem_segment)) {
      Write("bool ", "elem_segment_dropped_", name, " : 1;", Newline());
    }
  }
}

void CWriter::WriteElemInitializerDecls() {
  if (module_->tables.empty()) {
    return;
  }

  for (const ElemSegment* elem_segment : module_->elem_segments) {
    if (elem_segment->elem_exprs.empty()) {
      continue;
    }

    if (AreInitializersAlwaysNull(elem_segment->elem_type)) {
      // no need to store these initializers because only ref.null is possible
      continue;
    }

    if (c_streams_.size() > 1) {
      Write(Newline(),
            "extern const wasm_elem_segment_expr_t elem_segment_exprs_",
            GlobalName(ModuleFieldType::ElemSegment, elem_segment->name), "[];",
            Newline());
    }
  }
}

void CWriter::WriteFuncRefWrapper(const Func* func) {
  Write("static ", func->decl.sig.result_types, " ", WrapperRef(func->name));
  Write("(void *instance");
  if (func->GetNumParams() != 0) {
    Indent(4);
    for (Index i = 0; i < func->GetNumParams(); ++i) {
      Write(", ");
      if (i != 0 && (i % 8) == 0) {
        Write(Newline());
      }
      Write(func->GetParamType(i), " ", kLocalSymbolPrefix);
      Writef("%d", i);
    }
    Dedent(4);
  }
  Write(") ", OpenBrace());
  Write("return ");
  Write(ExternalRef(ModuleFieldType::Func, func->name));
  Write("(instance");
  if (func->GetNumParams() != 0) {
    Indent(4);
    for (Index i = 0; i < func->GetNumParams(); ++i) {
      Write(", ");
      if (i != 0 && (i % 8) == 0) {
        Write(Newline());
      }
      Write(kLocalSymbolPrefix);
      Writef("%d", i);
    }
    Dedent(4);
  }
  Write(");", Newline(), CloseBrace(), Newline());
}

void CWriter::WriteFuncRefWrappers() {
  std::set<std::string> unique_func_wrappers;

  for (Index index : module_->used_func_refs) {
    assert(index < module_->funcs.size());
    const Func* func = module_->funcs[index];
    if (unique_func_wrappers.count(func->name) == 0) {
      WriteFuncRefWrapper(func);
      unique_func_wrappers.insert(func->name);
    }
  }
}

void CWriter::WriteElemInitializers() {
  if (module_->tables.empty()) {
    return;
  }

  for (const ElemSegment* elem_segment : module_->elem_segments) {
    if (elem_segment->elem_exprs.empty()) {
      continue;
    }

    if (AreInitializersAlwaysNull(elem_segment->elem_type)) {
      // no need to store these initializers because only ref.null is possible
      continue;
    }

    Write(Newline(), InternalSymbolScope(),
          "const wasm_elem_segment_expr_t elem_segment_exprs_",
          GlobalName(ModuleFieldType::ElemSegment, elem_segment->name),
          "[] = ", OpenBrace());

    for (const ExprList& elem_expr : elem_segment->elem_exprs) {
      assert(elem_expr.size() == 1);
      const Expr& expr = elem_expr.front();
      switch (expr.type()) {
        case ExprType::RefFunc: {
          const Func* func = module_->GetFunc(cast<RefFuncExpr>(&expr)->var);
          const FuncType* func_type = module_->GetFuncType(func->decl.type_var);
          Write("{RefFunc, ", FuncTypeExpr(func_type),
                ", (wasm_rt_function_ptr_t)", WrapperRef(func->name), ", {");
          if (options_.features.tail_call_enabled() &&
              (IsImport(func->name) || func->features_used.tailcall)) {
            Write(TailCallRef(func->name));
          } else {
            Write("NULL");
          }
          Write("}, ");

          if (IsImport(func->name)) {
            Write("offsetof(", ModuleInstanceTypeName(), ", ",
                  GlobalName(ModuleFieldType::Import,
                             import_module_sym_map_[func->name]),
                  ")");
          } else {
            Write("0");
          }
          Write("},", Newline());
        } break;
        case ExprType::RefNull:
          Write("{RefNull, NULL, NULL, {NULL}, 0},", Newline());
          break;
        case ExprType::GlobalGet: {
          const Global* global =
              module_->GetGlobal(cast<GlobalGetExpr>(&expr)->var);
          assert(IsImport(global->name));
          Write("{GlobalGet, NULL, NULL, {NULL}, offsetof(",
                ModuleInstanceTypeName(), ", ",
                GlobalName(ModuleFieldType::Global, global->name), ")},",
                Newline());
        } break;
        default:
          WABT_UNREACHABLE;
      }
    }
    Write(CloseBrace(), ";", Newline());
  }

  Write(Newline(), "static void init_tables(", ModuleInstanceTypeName(),
        "* instance) ", OpenBrace());

  if (module_->tables.size() > module_->num_table_imports) {
    Index table_idx = module_->num_table_imports;
    for (Index i = table_idx; i < module_->tables.size(); i++) {
      const Table* table = module_->tables[i];
      uint32_t max =
          table->elem_limits.has_max ? table->elem_limits.max : UINT32_MAX;
      Write("wasm_rt_allocate_", GetReferenceTypeName(table->elem_type),
            "_table(", ExternalInstancePtr(ModuleFieldType::Table, table->name),
            ", ", table->elem_limits.initial, ", ", max, ");", Newline());
    }
  }

  for (const ElemSegment* elem_segment : module_->elem_segments) {
    if (elem_segment->kind != SegmentKind::Active) {
      continue;
    }

    const Table* table = module_->GetTable(elem_segment->table_var);

    WriteElemTableInit(true, elem_segment, table);
  }

  Write(CloseBrace(), Newline());

  if (!module_->elem_segments.empty()) {
    Write(Newline(), "static void init_elem_instances(",
          ModuleInstanceTypeName(), " *instance) ", OpenBrace());

    for (const ElemSegment* elem_segment : module_->elem_segments) {
      if (is_droppable(elem_segment)) {
        Write("instance->elem_segment_dropped_",
              GlobalName(ModuleFieldType::ElemSegment, elem_segment->name),
              " = false;", Newline());
      }
    }

    Write(CloseBrace(), Newline());
  }
}

void CWriter::WriteElemTableInit(bool active_initialization,
                                 const ElemSegment* src_segment,
                                 const Table* dst_table) {
  assert(dst_table->elem_type.IsRef() &&
         dst_table->elem_type != Type::Reference);
  assert(dst_table->elem_type == src_segment->elem_type);

  Write(GetReferenceTypeName(dst_table->elem_type), "_table_init(",
        ExternalInstancePtr(ModuleFieldType::Table, dst_table->name), ", ");

  // elem segment exprs needed only for funcref tables
  // because externref and exnref tables can only be initialized with ref.null
  if (dst_table->elem_type == Type::FuncRef) {
    if (src_segment->elem_exprs.empty()) {
      Write("NULL, ");
    } else {
      Write("elem_segment_exprs_",
            GlobalName(ModuleFieldType::ElemSegment, src_segment->name), ", ");
    }
  }

  // src_size, dest_addr, src_addr, N
  if (active_initialization) {
    Write(src_segment->elem_exprs.size(), ", ");
    WriteInitExpr(src_segment->offset);
    Write(", 0, ", src_segment->elem_exprs.size());
  } else {
    if (is_droppable(src_segment)) {
      Write("(instance->elem_segment_dropped_",
            GlobalName(ModuleFieldType::ElemSegment, src_segment->name),
            " ? 0 : ", src_segment->elem_exprs.size(), "), ");
    } else {
      Write("0, ");
    }
    Write(StackVar(2), ", ", StackVar(1), ", ", StackVar(0));
  }

  if (dst_table->elem_type == Type::FuncRef) {
    Write(", instance");
  }

  Write(");", Newline());
}

bool CWriter::IsSingleUnsharedMemory() {
  return module_->memories.size() == 1 &&
         !module_->memories[0]->page_limits.is_shared;
}

void CWriter::InstallSegueBase(Memory* memory, bool save_old_value) {
  NonIndented(
      [&] { Write("#if WASM_RT_USE_SEGUE_FOR_THIS_MODULE", Newline()); });
  if (save_old_value) {
    NonIndented([&] { Write("#if !WASM_RT_SEGUE_FREE_SEGMENT", Newline()); });
    Write("void* segue_saved_base = wasm_rt_segue_read_base();", Newline());
    NonIndented([&] { Write("#endif", Newline()); });
  }
  auto primary_memory =
      ExternalInstanceRef(ModuleFieldType::Memory, memory->name);
  Write("wasm_rt_segue_write_base(", primary_memory, ".data);", Newline());
  NonIndented([&] { Write("#endif", Newline()); });
}

void CWriter::RestoreSegueBase() {
  NonIndented([&] {
    Write(
        "#if WASM_RT_USE_SEGUE_FOR_THIS_MODULE && !WASM_RT_SEGUE_FREE_SEGMENT",
        Newline());
  });
  Write("wasm_rt_segue_write_base(segue_saved_base);", Newline());
  NonIndented([&] { Write("#endif", Newline()); });
}

void CWriter::WriteExports(CWriterPhase kind) {
  if (module_->exports.empty())
    return;

  for (const Export* export_ : module_->exports) {
    Write(Newline(), "/* export: '", SanitizeForComment(export_->name), "' */",
          Newline());

    const std::string mangled_name = ExportName(export_->name);
    std::string internal_name;
    std::vector<std::string> index_to_name;

    switch (export_->kind) {
      case ExternalKind::Func: {
        const Func* func = module_->GetFunc(export_->var);
        internal_name = func->name;
        if (kind == CWriterPhase::Declarations) {
          WriteFuncDeclaration(func->decl, mangled_name);
        } else {
          func_ = func;
          local_syms_ = global_syms_;
          local_sym_map_.clear();
          stack_var_sym_map_.clear();
          Write(func_->decl.sig.result_types, " ", mangled_name, "(");
          MakeTypeBindingReverseMapping(func_->GetNumParamsAndLocals(),
                                        func_->bindings, &index_to_name);
          WriteParams(index_to_name);
        }
        break;
      }

      case ExternalKind::Global: {
        const Global* global = module_->GetGlobal(export_->var);
        internal_name = global->name;
        WriteGlobalPtr(*global, mangled_name);
        break;
      }

      case ExternalKind::Memory: {
        const Memory* memory = module_->GetMemory(export_->var);
        internal_name = memory->name;
        WriteMemoryPtr(mangled_name, *memory);
        break;
      }

      case ExternalKind::Table: {
        const Table* table = module_->GetTable(export_->var);
        internal_name = table->name;
        WriteTablePtr(mangled_name, *table);
        break;
      }

      case ExternalKind::Tag: {
        const Tag* tag = module_->GetTag(export_->var);
        internal_name = tag->name;
        if (kind == CWriterPhase::Declarations) {
          Write("extern ");
        }
        Write("const wasm_rt_tag_t ", mangled_name);
        break;
      }

      default:
        WABT_UNREACHABLE;
    }

    if (kind == CWriterPhase::Declarations) {
      Write(";", Newline());
      continue;
    }

    Write(" ");
    switch (export_->kind) {
      case ExternalKind::Func: {
        Write(OpenBrace());
        if (IsSingleUnsharedMemory()) {
          InstallSegueBase(module_->memories[0], true /* save_old_value */);
        }
        auto num_results = func_->GetNumResults();
        if (num_results > 1) {
          Write(func_->decl.sig.result_types, " ret = ");
        } else if (num_results == 1) {
          Write(func_->GetResultType(0), " ret = ");
        }
        Write(ExternalRef(ModuleFieldType::Func, internal_name), "(");

        if (IsImport(internal_name)) {
          Write("instance->",
                GlobalName(ModuleFieldType::Import,
                           import_module_sym_map_[internal_name]));
        } else {
          Write("instance");
        }
        WriteParamSymbols(index_to_name);
        if (IsSingleUnsharedMemory()) {
          RestoreSegueBase();
        }
        if (num_results > 0) {
          Write("return ret;", Newline());
        }
        Write(CloseBrace(), Newline());

        local_sym_map_.clear();
        stack_var_sym_map_.clear();
        func_ = nullptr;
        break;
      }

      case ExternalKind::Global:
        Write(OpenBrace());
        Write("return ",
              ExternalInstancePtr(ModuleFieldType::Global, internal_name), ";",
              Newline());
        Write(CloseBrace(), Newline());
        break;

      case ExternalKind::Memory:
        Write(OpenBrace());
        Write("return ",
              ExternalInstancePtr(ModuleFieldType::Memory, internal_name), ";",
              Newline());
        Write(CloseBrace(), Newline());
        break;

      case ExternalKind::Table:
        Write(OpenBrace());
        Write("return ",
              ExternalInstancePtr(ModuleFieldType::Table, internal_name), ";",
              Newline());
        Write(CloseBrace(), Newline());
        break;

      case ExternalKind::Tag:
        Write("= ", TagSymbol(internal_name), ";", Newline());
        break;

      default:
        WABT_UNREACHABLE;
    }
  }
}

void CWriter::WriteTailCallExports(CWriterPhase kind) {
  for (const Export* export_ : module_->exports) {
    if (export_->kind != ExternalKind::Func) {
      continue;
    }

    const Func* func = module_->GetFunc(export_->var);

    if (!IsImport(func->name) && !func->features_used.tailcall) {
      continue;
    }

    const std::string mangled_name = TailCallExportName(export_->name);

    Write(Newline(), "/* export for tail-call of '",
          SanitizeForComment(export_->name), "' */", Newline());
    if (kind == CWriterPhase::Declarations) {
      WriteTailCallFuncDeclaration(mangled_name);
      Write(";", Newline());
    } else {
      WriteTailCallFuncDeclaration(mangled_name);
      Write(" ", OpenBrace());
      const Func* func = module_->GetFunc(export_->var);
      Write(TailCallRef(func->name), "(instance_ptr, tail_call_stack, next);",
            Newline(), CloseBrace(), Newline());
    }
  }
}

void CWriter::WriteInit() {
  Write(Newline(), "void ", kAdminSymbolPrefix, module_prefix_, "_instantiate(",
        ModuleInstanceTypeName(), "* instance");
  for (const auto& import_module_name : import_module_set_) {
    Write(", struct ", ModuleInstanceTypeName(import_module_name), "* ",
          GlobalName(ModuleFieldType::Import, import_module_name));
  }
  Write(") ", OpenBrace());

  Write("assert(wasm_rt_is_initialized());", Newline());

  if (!import_module_set_.empty()) {
    Write("init_instance_import(instance");
    for (const auto& import_module_name : import_module_set_) {
      Write(", ", GlobalName(ModuleFieldType::Import, import_module_name));
    }
    Write(");", Newline());
  }

  if (!module_->globals.empty()) {
    Write("init_globals(instance);", Newline());
  }
  if (!module_->tables.empty()) {
    Write("init_tables(instance);", Newline());
  }
  if (!module_->memories.empty()) {
    Write("init_memories(instance);", Newline());
    if (IsSingleUnsharedMemory()) {
      InstallSegueBase(module_->memories[0], true /* save_old_value */);
    }
  }
  if (!module_->tables.empty() && !module_->elem_segments.empty()) {
    Write("init_elem_instances(instance);", Newline());
  }
  if (!module_->memories.empty() && !module_->data_segments.empty()) {
    Write("init_data_instances(instance);", Newline());
  }

  for (Var* var : module_->starts) {
    Write(ExternalRef(ModuleFieldType::Func, module_->GetFunc(*var)->name));
    if (IsImport(module_->GetFunc(*var)->name)) {
      Write("(instance->",
            GlobalName(ModuleFieldType::Import,
                       import_module_sym_map_[module_->GetFunc(*var)->name]),
            ");");
    } else {
      Write("(instance);");
    }
    Write(Newline());
  }

  if (IsSingleUnsharedMemory()) {
    RestoreSegueBase();
  }
  Write(CloseBrace(), Newline());
}

void CWriter::WriteGetFuncType() {
  Write(Newline(), "wasm_rt_func_type_t ", kAdminSymbolPrefix, module_prefix_,
        "_get_func_type(uint32_t param_count, uint32_t result_count, "
        "...) ",
        OpenBrace());

  Write("va_list args;", Newline());

  for (const TypeEntry* type : module_->types) {
    const FuncType* func_type = cast<FuncType>(type);
    const FuncSignature& signature = func_type->sig;

    Write(Newline(), "if (param_count == ", signature.GetNumParams(),
          " && result_count == ", signature.GetNumResults(), ") ", OpenBrace());
    Write("va_start(args, result_count);", Newline());
    Write("if (true");
    for (const auto& t : signature.param_types) {
      Write(" && va_arg(args, wasm_rt_type_t) == ", TypeEnum(t));
    }
    for (const auto& t : signature.result_types) {
      Write(" && va_arg(args, wasm_rt_type_t) == ", TypeEnum(t));
    }
    Write(") ", OpenBrace());
    Write("va_end(args);", Newline());
    Write("return ", FuncTypeExpr(func_type), ";", Newline());
    Write(CloseBrace(), Newline());
    Write("va_end(args);", Newline());
    Write(CloseBrace(), Newline());
  }

  Write(Newline(), "return NULL;", Newline());
  Write(CloseBrace(), Newline());
}

void CWriter::WriteInitInstanceImport() {
  if (import_module_set_.empty())
    return;

  Write(Newline(), "static void init_instance_import(",
        ModuleInstanceTypeName(), "* instance");
  for (const auto& import_module_name : import_module_set_) {
    Write(", struct ", ModuleInstanceTypeName(import_module_name), "* ",
          GlobalName(ModuleFieldType::Import, import_module_name));
  }
  Write(") ", OpenBrace());

  for (const auto& import_module : import_func_module_set_) {
    Write("instance->", GlobalName(ModuleFieldType::Import, import_module),
          " = ", GlobalName(ModuleFieldType::Import, import_module), ";",
          Newline());
  }

  for (const Import* import : unique_imports_) {
    switch (import->kind()) {
      case ExternalKind::Func:
      case ExternalKind::Tag:
        break;

      case ExternalKind::Global:
      case ExternalKind::Memory:
      case ExternalKind::Table: {
        Write("instance->", ExportName(import->module_name, import->field_name),
              " = ", ExportName(import->module_name, import->field_name), "(",
              GlobalName(ModuleFieldType::Import, import->module_name), ");",
              Newline());
        break;
      }

      default:
        WABT_UNREACHABLE;
    }
  }
  Write(CloseBrace(), Newline());
}

void CWriter::WriteImportProperties(CWriterPhase kind) {
  if (import_module_set_.empty())
    return;

  Write(Newline());

  auto write_import_prop = [&](const Import* import, std::string prop,
                               std::string type, uint64_t value) {
    if (kind == CWriterPhase::Declarations) {
      Write("extern ");
    }
    Write("const ", type, " ", kAdminSymbolPrefix, module_prefix_, "_", prop,
          "_", MangleModuleName(import->module_name), "_",
          MangleName(import->field_name));
    if (kind == CWriterPhase::Definitions) {
      Write(" = ", value);
    }

    Write(";", Newline());
  };

  for (const Import* import : unique_imports_) {
    if (import->kind() == ExternalKind::Memory) {
      const Limits* limits = &(cast<MemoryImport>(import)->memory.page_limits);
      // We use u64 so we can handle both 32-bit and 64-bit memories
      const uint64_t default_max = limits->is_64
                                       ? (static_cast<uint64_t>(1) << 48)
                                       : (static_cast<uint64_t>(1) << 16);
      write_import_prop(import, "min", "u64", limits->initial);
      write_import_prop(import, "max", "u64",
                        limits->has_max ? limits->max : default_max);
      write_import_prop(import, "is64", "u8", limits->is_64);
      write_import_prop(import, "pagesize", "u32",
                        cast<MemoryImport>(import)->memory.page_size);
    } else if (import->kind() == ExternalKind::Table) {
      const Limits* limits = &(cast<TableImport>(import)->table.elem_limits);
      const uint64_t default_max = std::numeric_limits<uint32_t>::max();
      write_import_prop(import, "min", "u32", limits->initial);
      write_import_prop(import, "max", "u32",
                        limits->has_max ? limits->max : default_max);
    } else {
      continue;
    }
  }
}

void CWriter::WriteFree() {
  Write(Newline(), "void ", kAdminSymbolPrefix, module_prefix_, "_free(",
        ModuleInstanceTypeName(), "* instance) ", OpenBrace());

  {
    Index table_index = 0;
    for (const Table* table : module_->tables) {
      bool is_import = table_index < module_->num_table_imports;
      if (!is_import) {
        Write("wasm_rt_free_", GetReferenceTypeName(table->elem_type),
              "_table(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ");",
              Newline());
      }
      ++table_index;
    }
  }

  {
    Index memory_index = 0;
    for (const Memory* memory : module_->memories) {
      bool is_import = memory_index < module_->num_memory_imports;
      if (!is_import) {
        std::string func = GetMemoryAPIString(*memory, "wasm_rt_free_memory");
        Write(func, "(",
              ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ");",
              Newline());
      }
      ++memory_index;
    }
  }

  Write(CloseBrace(), Newline());
}

void CWriter::WriteFuncs() {
  std::vector<size_t> c_stream_assignment =
      name_to_output_file_index_(module_->funcs.begin(), module_->funcs.end(),
                                 module_->num_func_imports, c_streams_.size());
  Index func_index = 0;
  for (const Func* func : module_->funcs) {
    bool is_import = func_index < module_->num_func_imports;
    if (!is_import) {
      stream_ = c_streams_.at(c_stream_assignment.at(func_index));
      Write(*func);
      if (func->features_used.tailcall) {
        WriteTailCallee(*func);
      }
    }
    ++func_index;
  }
}

void CWriter::PushFuncSection(std::string_view include_condition) {
  func_sections_.emplace_back(include_condition, MemoryStream{});
  stream_ = &func_sections_.back().second;
}

bool CWriter::IsImport(const std::string& name) const {
  return import_module_sym_map_.count(name);
}

template <typename sources>
void CWriter::Spill(const TypeVector& types, const sources& src) {
  assert(types.size() > 0);

  if (types.size() == 1) {
    Write("tmp = ", src(0), ";", Newline());
    return;
  }

  for (Index i = 0; i < types.size(); ++i) {
    Writef("tmp.%c%d = ", MangleType(types.at(i)), i);
    Write(src(i), ";", Newline());
  }
}

void CWriter::Spill(const TypeVector& types) {
  Spill(types, [&](auto i) { return StackVar(types.size() - i - 1); });
}

template <typename targets>
void CWriter::Unspill(const TypeVector& types, const targets& tgt) {
  assert(types.size() > 0);

  if (types.size() == 1) {
    Write(tgt(0), " = tmp;", Newline());
    return;
  }

  for (Index i = 0; i < types.size(); ++i) {
    Write(tgt(i));
    Writef(" = tmp.%c%d;", MangleType(types.at(i)), i);
    Write(Newline());
  }
}

void CWriter::Unspill(const TypeVector& types) {
  Unspill(types, [&](auto i) { return StackVar(types.size() - i - 1); });
}

void CWriter::WriteTailCallAsserts(const FuncSignature& sig) {
  if (sig.param_types.size()) {
    Write("static_assert(sizeof(", sig.param_types, ") <= ", kTailCallStackSize,
          ");", Newline());
  }
  if (sig.result_types.size() && sig.result_types != sig.param_types) {
    Write("static_assert(sizeof(", sig.result_types,
          ") <= ", kTailCallStackSize, ");", Newline());
  }
}

void CWriter::WriteTailCallStack() {
  Write("void *instance_ptr_storage;", Newline());
  Write("void **instance_ptr = &instance_ptr_storage;", Newline());
  Write("char tail_call_stack[", std::to_string(kTailCallStackSize), "];",
        Newline());
  Write("wasm_rt_tailcallee_t next_storage;", Newline());
  Write("wasm_rt_tailcallee_t *next = &next_storage;", Newline());
}

void CWriter::WriteUnwindTryCatchStack(const Label* label) {
  assert(try_catch_stack_.size() >= label->try_catch_stack_size);

  if (try_catch_stack_.size() != label->try_catch_stack_size) {
    const std::string& name =
        try_catch_stack_.at(label->try_catch_stack_size).name;

    Write("wasm_rt_set_unwind_target(", name, "_outer_target);", Newline());
  }
}

void CWriter::FinishReturnCall() {
  if (in_tail_callee_) {
    Write(CloseBrace(), Newline(), "return;", Newline());
    return;
  }

  Write("while (next->fn) { next->fn(instance_ptr, tail_call_stack, next); }",
        Newline());
  PushTypes(func_->decl.sig.result_types);
  if (!func_->decl.sig.result_types.empty()) {
    Write(OpenBrace(), func_->decl.sig.result_types, " tmp;", Newline(),
          "wasm_rt_memcpy(&tmp, tail_call_stack, sizeof(tmp));", Newline());
    Unspill(func_->decl.sig.result_types);
    Write(CloseBrace(), Newline());
  }

  Write(CloseBrace(), Newline(), "goto ", LabelName(kImplicitFuncLabel), ";",
        Newline());
}

void CWriter::BeginFunction(const Func& func) {
  func_ = &func;
  in_tail_callee_ = false;
  local_syms_.clear();
  local_sym_map_.clear();
  stack_var_sym_map_.clear();
  func_sections_.clear();
  func_includes_.clear();

  /*
   * If offset of stream_ is 0, this is the first time some function is written
   * to this stream, then write multi c top.
   */
  if (stream_->offset() == 0) {
    WriteMultiCTop();
  }
  Write(Newline());
}

void CWriter::FinishFunction(size_t stack_var_section) {
  for (size_t i = 0; i < func_sections_.size(); ++i) {
    auto& [condition, stream] = func_sections_.at(i);
    std::unique_ptr<OutputBuffer> buf = stream.ReleaseOutputBuffer();
    if (condition.empty() || func_includes_.count(condition)) {
      stream_->WriteData(buf->data.data(), buf->data.size());
    }

    if (i == stack_var_section) {
      WriteStackVarDeclarations();  // these come immediately after section #0
                                    // (return type/name/params/locals)
    }
  }

  Write(CloseBrace(), Newline());

  func_ = nullptr;
}

void CWriter::Write(const Func& func) {
  Stream* prev_stream = stream_;
  BeginFunction(func);
  PushFuncSection();
  Write(func.decl.sig.result_types, " ",
        GlobalName(ModuleFieldType::Func, func.name), "(");
  WriteParamsAndLocals();
  Write("FUNC_PROLOGUE;", Newline());

  size_t stack_vars_section = func_sections_.size() - 1;
  PushFuncSection();

  std::string label = DefineLabelName(kImplicitFuncLabel);
  ResetTypeStack(0);
  std::string empty;  // Must not be temporary, since address is taken by Label.
  PushLabel(LabelType::Func, empty, func.decl.sig);
  Write(func.exprs, LabelDecl(label));
  PopLabel();
  ResetTypeStack(0);
  PushTypes(func.decl.sig.result_types);
  Write("FUNC_EPILOGUE;", Newline());

  // Return the top of the stack implicitly.
  Index num_results = func.GetNumResults();
  if (num_results == 1) {
    Write("return ", StackVar(0), ";", Newline());
  } else if (num_results >= 2) {
    Write(OpenBrace(), func.decl.sig.result_types, " tmp;", Newline());
    Spill(func.decl.sig.result_types);
    Write("return tmp;", Newline(), CloseBrace(), Newline());
  }

  stream_ = prev_stream;
  FinishFunction(stack_vars_section);
}

template <typename Vars, typename TypeOf, typename ToDo>
void CWriter::WriteVarsByType(const Vars& vars,
                              const TypeOf& typeoffunc,
                              const ToDo& todo,
                              bool setjmp_safe) {
  for (Type type : {Type::I32, Type::I64, Type::F32, Type::F64, Type::V128,
                    Type::FuncRef, Type::ExternRef, Type::ExnRef}) {
    Index var_index = 0;
    size_t count = 0;
    for (const auto& var : vars) {
      if (typeoffunc(var) == type) {
        if (count == 0) {
          Write(type, " ");
          if (setjmp_safe) {
            PushFuncSection("exceptions");
            Write("volatile ");
            PushFuncSection();
          }
          Indent(4);
        } else {
          Write(", ");
          if ((count % 8) == 0)
            Write(Newline());
        }

        todo(var_index, var);
        ++count;
      }
      ++var_index;
    }
    if (count != 0) {
      Dedent(4);
      Write(";", Newline());
    }
  }
}

void CWriter::WriteTailCallee(const Func& func) {
  Stream* prev_stream = stream_;
  BeginFunction(func);
  in_tail_callee_ = true;
  PushFuncSection();
  WriteTailCallFuncDeclaration(GetTailCallRef(func.name));
  Write(" ", OpenBrace());
  WriteTailCallAsserts(func.decl.sig);
  Write(ModuleInstanceTypeName(), "* instance = *instance_ptr;", Newline());

  std::vector<std::string> index_to_name;
  MakeTypeBindingReverseMapping(func.GetNumParamsAndLocals(), func.bindings,
                                &index_to_name);
  if (func.GetNumParams()) {
    WriteVarsByType(
        func.decl.sig.param_types, [](auto x) { return x; },
        [&](Index i, Type) { Write(DefineParamName(index_to_name[i])); }, true);
    Write(OpenBrace(), func.decl.sig.param_types, " tmp;", Newline(),
          "wasm_rt_memcpy(&tmp, tail_call_stack, sizeof(tmp));", Newline());
    Unspill(func.decl.sig.param_types,
            [&](auto i) { return ParamName(index_to_name[i]); });
    Write(CloseBrace(), Newline());
  }

  size_t stack_vars_section = func_sections_.size() - 1;
  WriteLocals(index_to_name);

  PushFuncSection();

  std::string label = DefineLabelName(kImplicitFuncLabel);
  ResetTypeStack(0);
  std::string empty;  // Must not be temporary, since address is taken by Label.
  PushLabel(LabelType::Func, empty, func.decl.sig);
  Write(func.exprs, LabelDecl(label));
  PopLabel();
  ResetTypeStack(0);
  PushTypes(func.decl.sig.result_types);

  // Return the top of the stack implicitly.
  if (func.GetNumResults()) {
    Write(OpenBrace(), func.decl.sig.result_types, " tmp;", Newline());
    Spill(func.decl.sig.result_types);
    Write("wasm_rt_memcpy(tail_call_stack, &tmp, sizeof(tmp));", Newline());
    Write(CloseBrace(), Newline());
  }
  Write("next->fn = NULL;", Newline());

  stream_ = prev_stream;
  FinishFunction(stack_vars_section);
}

void CWriter::WriteParamsAndLocals() {
  std::vector<std::string> index_to_name;
  MakeTypeBindingReverseMapping(func_->GetNumParamsAndLocals(), func_->bindings,
                                &index_to_name);
  WriteParams(index_to_name, true);
  Write(" ", OpenBrace());
  WriteLocals(index_to_name);
}

void CWriter::WriteParams(const std::vector<std::string>& index_to_name,
                          bool setjmp_safe) {
  Write(ModuleInstanceTypeName(), "* instance");
  if (func_->GetNumParams() != 0) {
    Indent(4);
    for (Index i = 0; i < func_->GetNumParams(); ++i) {
      Write(", ");
      if (i != 0 && (i % 8) == 0) {
        Write(Newline());
      }
      Write(func_->GetParamType(i), " ");
      if (setjmp_safe) {
        PushFuncSection("exceptions");
        Write("volatile ");
        PushFuncSection();
      }
      Write(DefineParamName(index_to_name[i]));
    }
    Dedent(4);
  }
  Write(")");
}

void CWriter::WriteParamSymbols(const std::vector<std::string>& index_to_name) {
  if (func_->GetNumParams() != 0) {
    Indent(4);
    for (Index i = 0; i < func_->GetNumParams(); ++i) {
      Write(", ");
      if (i != 0 && (i % 8) == 0) {
        Write(Newline());
      }
      Write(ParamName(index_to_name[i]));
    }
    Dedent(4);
  }
  Write(");", Newline());
}

void CWriter::WriteParamTypes(const FuncDeclaration& decl) {
  if (decl.GetNumParams() != 0) {
    for (Index i = 0; i < decl.GetNumParams(); ++i) {
      Write(", ");
      Write(decl.GetParamType(i));
    }
  }
}

void CWriter::WriteLocals(const std::vector<std::string>& index_to_name) {
  Index num_params = func_->GetNumParams();
  WriteVarsByType(
      func_->local_types, [](auto x) { return x; },
      [&](Index local_index, Type local_type) {
        Write(DefineParamName(index_to_name[num_params + local_index]), " = ");
        if (local_type.IsRef()) {
          Write(GetReferenceNullValue(local_type));
        } else if (local_type == Type::V128) {
          Write("simde_wasm_i64x2_make(0, 0)");
        } else {
          Write("0");
        }
      },
      true);
}

void CWriter::WriteStackVarDeclarations() {
  // NOTE: setjmp_safe must be false, can't push func sections in here because
  // this is called from FinishFunction.
  // (luckily, we don't need setjmp_safe for these.)
  WriteVarsByType(
      stack_var_sym_map_, [](auto stp_name) { return stp_name.first.second; },
      [&](Index, auto& stp_name) { Write(stp_name.second); }, false);
}

void CWriter::Write(const Block& block) {
  std::string label = DefineLabelName(block.label);
  DropTypes(block.decl.GetNumParams());
  size_t mark = MarkTypeStack();
  PushLabel(LabelType::Block, block.label, block.decl.sig);
  PushTypes(block.decl.sig.param_types);
  Write(block.exprs, LabelDecl(label));
  ResetTypeStack(mark);
  PopLabel();
  PushTypes(block.decl.sig.result_types);
}

size_t CWriter::BeginTry(const Block& block) {
  func_includes_.insert("exceptions");
  Write(OpenBrace()); /* beginning of try-catch or try_table */
  const std::string tlabel = DefineLabelName(block.label);
  Write("WASM_RT_UNWIND_TARGET *", tlabel,
        "_outer_target = wasm_rt_get_unwind_target();", Newline());
  Write("WASM_RT_UNWIND_TARGET ", tlabel, "_unwind_target;", Newline());
  Write("if (!wasm_rt_try(", tlabel, "_unwind_target)) ");
  Write(OpenBrace()); /* beginning of try or try_table block */
  DropTypes(block.decl.GetNumParams());
  const size_t mark = MarkTypeStack();
  PushLabel(LabelType::Try, block.label, block.decl.sig);
  PushTypes(block.decl.sig.param_types);
  Write("wasm_rt_set_unwind_target(&", tlabel, "_unwind_target);", Newline());
  PushTryCatch(tlabel);
  Write(block.exprs);
  ResetTypeStack(mark);
  Write("wasm_rt_set_unwind_target(", tlabel, "_outer_target);", Newline());
  Write(CloseBrace());          /* end of try or try_table block */
  Write(" else ", OpenBrace()); /* beginning of catch blocks or delegate */
  assert(label_stack_.back().name == block.label);
  assert(label_stack_.back().label_type == LabelType::Try);
  label_stack_.back().label_type = LabelType::Catch;
  if (try_catch_stack_.back().used) {
    Write(tlabel, "_catch:;", Newline());
  }

  return mark;
}

void CWriter::WriteTryCatch(const TryExpr& tryexpr) {
  const size_t mark = BeginTry(tryexpr.block);

  /* exception has been thrown -- do we catch it? */

  const LabelName tlabel = LabelName(tryexpr.block.label);

  Write("wasm_rt_set_unwind_target(", tlabel, "_outer_target);", Newline());
  PopTryCatch();

  /* save the thrown exception to the stack if it might be rethrown later */
  PushFuncSection(tryexpr.block.label);
  Write("/* save exception ", tlabel, " for rethrow */", Newline());
  Write("const wasm_rt_tag_t ", tlabel, "_tag = wasm_rt_exception_tag();",
        Newline());
  Write("uint32_t ", tlabel, "_size = wasm_rt_exception_size();", Newline());
  Write("void *", tlabel, " = alloca(", tlabel, "_size);", Newline());
  Write("wasm_rt_memcpy(", tlabel, ", wasm_rt_exception(), ", tlabel, "_size);",
        Newline());
  PushFuncSection();

  assert(!tryexpr.catches.empty());
  bool has_catch_all{};
  for (auto it = tryexpr.catches.cbegin(); it != tryexpr.catches.cend(); ++it) {
    if (it == tryexpr.catches.cbegin()) {
      Write(Newline());
    } else {
      Write(" else ");
    }
    ResetTypeStack(mark);
    Write(*it);
    if (it->IsCatchAll()) {
      has_catch_all = true;
      break;
    }
  }
  if (!has_catch_all) {
    /* if not caught, rethrow */
    Write(" else ", OpenBrace());
    WriteThrow();
    Write(CloseBrace(), Newline());
  }
  Write(CloseBrace(), Newline()); /* end of catch blocks */
  Write(CloseBrace(), Newline()); /* end of try-catch */

  ResetTypeStack(mark);
  assert(!label_stack_.empty());
  assert(label_stack_.back().name == tryexpr.block.label);
  Write(LabelDecl(GetLocalName(tryexpr.block.label, true)));
  PopLabel();
  PushTypes(tryexpr.block.decl.sig.result_types);
}

void CWriter::Write(const Catch& c) {
  if (c.IsCatchAll()) {
    Write(c.exprs);
    return;
  }

  Write("if (wasm_rt_exception_tag() == ",
        TagSymbol(module_->GetTag(c.var)->name), ") ", OpenBrace());

  const Tag* tag = module_->GetTag(c.var);
  const FuncDeclaration& tag_type = tag->decl;
  const Index num_params = tag_type.GetNumParams();
  PushTypes(tag_type.sig.param_types);
  if (num_params == 1) {
    Write("wasm_rt_memcpy(&", StackVar(0), ", wasm_rt_exception(), sizeof(",
          tag_type.GetParamType(0), "));", Newline());
  } else if (num_params > 1) {
    Write(OpenBrace(), tag_type.sig.param_types, " tmp;", Newline());
    Write("wasm_rt_memcpy(&tmp, wasm_rt_exception(), sizeof(tmp));", Newline());
    Unspill(tag_type.sig.param_types);
    Write(CloseBrace(), Newline());
  }

  Write(c.exprs);
  Write(CloseBrace());
}

void CWriter::WriteThrow() {
  if (try_catch_stack_.empty()) {
    Write("wasm_rt_throw();", Newline());
  } else {
    Write("goto ", try_catch_stack_.back().name, "_catch;", Newline());
    try_catch_stack_.back().used = true;
  }
}

void CWriter::PushTryCatch(const std::string& name) {
  try_catch_stack_.emplace_back(name, try_catch_stack_.size());
}

void CWriter::PopTryCatch() {
  assert(!try_catch_stack_.empty());
  try_catch_stack_.pop_back();
}

void CWriter::WriteTryDelegate(const TryExpr& tryexpr) {
  const size_t mark = BeginTry(tryexpr.block);

  /* exception has been thrown -- where do we delegate it? */

  if (tryexpr.delegate_target.is_index()) {
    /* must be the implicit function label */
    assert(!try_catch_stack_.empty());
    const std::string& unwind_name = try_catch_stack_.at(0).name;
    Write("wasm_rt_set_unwind_target(", unwind_name, "_outer_target);",
          Newline());

    Write("wasm_rt_throw();", Newline());
  } else {
    const Label* label = FindLabel(tryexpr.delegate_target, false);

    assert(try_catch_stack_.size() >= label->try_catch_stack_size);

    if (label->label_type == LabelType::Try) {
      Write("goto ", LabelName(label->name), "_catch;", Newline());
      try_catch_stack_.at(label->try_catch_stack_size).used = true;
    } else if (label->try_catch_stack_size == 0) {
      assert(!try_catch_stack_.empty());
      const std::string& unwind_name = try_catch_stack_.at(0).name;
      Write("wasm_rt_set_unwind_target(", unwind_name, "_outer_target);",
            Newline());

      Write("wasm_rt_throw();", Newline());
    } else {
      const std::string label_target =
          try_catch_stack_.at(label->try_catch_stack_size - 1).name + "_catch";
      Write("goto ", label_target, ";", Newline());
      try_catch_stack_.at(label->try_catch_stack_size - 1).used = true;
    }
  }

  Write(CloseBrace(), Newline());
  Write(CloseBrace(), Newline());

  PopTryCatch();
  ResetTypeStack(mark);
  assert(!label_stack_.empty());
  assert(label_stack_.back().name == tryexpr.block.label);
  Write(LabelDecl(GetLocalName(tryexpr.block.label, true)));
  PopLabel();
  PushTypes(tryexpr.block.decl.sig.result_types);
}

void CWriter::Write(const TryTableExpr& try_table_expr) {
  const size_t mark = BeginTry(try_table_expr.block);

  /* exception has been thrown -- do we catch it? */

  const LabelName tlabel = LabelName(try_table_expr.block.label);

  Write("wasm_rt_set_unwind_target(", tlabel, "_outer_target);", Newline());
  PopTryCatch();

  ResetTypeStack(mark);
  assert(!label_stack_.empty());
  assert(label_stack_.back().name == try_table_expr.block.label);
  Write(LabelDecl(GetLocalName(try_table_expr.block.label, true)));
  PopLabel();

  assert(!try_table_expr.catches.empty());
  bool has_catch_all{};
  for (auto it = try_table_expr.catches.cbegin();
       it != try_table_expr.catches.cend(); ++it) {
    if (it == try_table_expr.catches.cbegin()) {
      Write(Newline());
    } else {
      Write(" else ");
    }
    ResetTypeStack(mark);
    Write(*it);
    if (it->IsCatchAll()) {
      has_catch_all = true;
      break;
    }
  }
  if (!has_catch_all) {
    /* if not caught, rethrow */
    Write(" else ", OpenBrace());
    WriteThrow();
    Write(CloseBrace(), Newline());
  }
  Write(CloseBrace(), Newline()); /* end of catch blocks */
  Write(CloseBrace(), Newline()); /* end of try-catch */

  ResetTypeStack(mark);
  PushTypes(try_table_expr.block.decl.sig.result_types);
}

void CWriter::Write(const TableCatch& c) {
  if (!c.IsCatchAll()) {
    Write("if (wasm_rt_exception_tag() == ",
          TagSymbol(module_->GetTag(c.tag)->name), ") ", OpenBrace());

    const Tag* tag = module_->GetTag(c.tag);
    const FuncDeclaration& tag_type = tag->decl;
    const Index num_params = tag_type.GetNumParams();
    PushTypes(tag_type.sig.param_types);
    if (num_params == 1) {
      Write("wasm_rt_memcpy(&", StackVar(0), ", wasm_rt_exception(), sizeof(",
            tag_type.GetParamType(0), "));", Newline());
    } else if (num_params > 1) {
      Write(OpenBrace(), tag_type.sig.param_types, " tmp;", Newline());
      Write("wasm_rt_memcpy(&tmp, wasm_rt_exception(), sizeof(tmp));",
            Newline());
      Unspill(tag_type.sig.param_types);
      Write(CloseBrace(), Newline());
    }
  }
  if (c.IsRef()) {
    PushType(Type::ExnRef);
    Write(StackVar(0), ".tag = wasm_rt_exception_tag();", Newline());
    Write(StackVar(0), ".size = wasm_rt_exception_size();", Newline());
    Write("wasm_rt_memcpy(&", StackVar(0),
          ".data, wasm_rt_exception(), wasm_rt_exception_size());", Newline());
  }

  Write(GotoLabel(c.target), Newline());
  if (!c.IsCatchAll()) {
    Write(CloseBrace());
  }
}

void CWriter::Write(const ExprList& exprs) {
  for (const Expr& expr : exprs) {
    switch (expr.type()) {
      case ExprType::Binary:
        Write(*cast<BinaryExpr>(&expr));
        break;

      case ExprType::Block:
        Write(cast<BlockExpr>(&expr)->block);
        break;

      case ExprType::Br:
        Write(GotoLabel(cast<BrExpr>(&expr)->var), Newline());
        // Stop processing this ExprList, since the following are unreachable.
        return;

      case ExprType::BrIf:
        Write("if (", StackVar(0), ") {");
        DropTypes(1);
        Write(GotoLabel(cast<BrIfExpr>(&expr)->var), "}", Newline());
        break;

      case ExprType::BrTable: {
        const auto* bt_expr = cast<BrTableExpr>(&expr);
        Write("switch (", StackVar(0), ") ", OpenBrace());
        DropTypes(1);
        Index i = 0;
        for (const Var& var : bt_expr->targets) {
          Write("case ", i++, ": ", GotoLabel(var), Newline());
        }
        Write("default: ");
        Write(GotoLabel(bt_expr->default_target), Newline(), CloseBrace(),
              Newline());
        // Stop processing this ExprList, since the following are unreachable.
        return;
      }

      case ExprType::Call: {
        const Var& var = cast<CallExpr>(&expr)->var;
        const Func& func = *module_->GetFunc(var);
        Index num_params = func.GetNumParams();
        Index num_results = func.GetNumResults();
        assert(type_stack_.size() >= num_params);
        if (num_results > 1) {
          Write(OpenBrace(), func.decl.sig.result_types, " tmp = ");
        } else if (num_results == 1) {
          Write(StackVar(num_params - 1, func.GetResultType(0)), " = ");
        }

        assert(var.is_name());
        Write(ExternalRef(ModuleFieldType::Func, var.name()), "(");
        if (IsImport(func.name)) {
          Write("instance->", GlobalName(ModuleFieldType::Import,
                                         import_module_sym_map_[func.name]));
        } else {
          Write("instance");
        }
        for (Index i = 0; i < num_params; ++i) {
          Write(", ");
          Write(StackVar(num_params - i - 1));
        }
        Write(");", Newline());
        DropTypes(num_params);
        PushTypes(func.decl.sig.result_types);
        if (num_results > 1) {
          Unspill(func.decl.sig.result_types);
          Write(CloseBrace(), Newline());
        }
        break;
      }

      case ExprType::CallIndirect: {
        const FuncDeclaration& decl = cast<CallIndirectExpr>(&expr)->decl;
        Index num_params = decl.GetNumParams();
        Index num_results = decl.GetNumResults();
        assert(type_stack_.size() > num_params);
        if (num_results > 1) {
          Write(OpenBrace(), decl.sig.result_types, " tmp = ");
        } else if (num_results == 1) {
          Write(StackVar(num_params, decl.GetResultType(0)), " = ");
        }

        const Table* table =
            module_->GetTable(cast<CallIndirectExpr>(&expr)->table);

        assert(decl.has_func_type);
        const FuncType* func_type = module_->GetFuncType(decl.type_var);

        Write("CALL_INDIRECT(",
              ExternalInstanceRef(ModuleFieldType::Table, table->name), ", ");
        WriteCallIndirectFuncDeclaration(decl, "(*)");
        Write(", ", FuncTypeExpr(func_type), ", ", StackVar(0));
        Write(", ", ExternalInstanceRef(ModuleFieldType::Table, table->name),
              ".data[", StackVar(0), "].module_instance");
        for (Index i = 0; i < num_params; ++i) {
          Write(", ", StackVar(num_params - i));
        }
        Write(");", Newline());
        if (IsSingleUnsharedMemory()) {
          InstallSegueBase(module_->memories[0], false /* save_old_value */);
        }
        DropTypes(num_params + 1);
        PushTypes(decl.sig.result_types);
        if (num_results > 1) {
          Unspill(decl.sig.result_types);
          Write(CloseBrace(), Newline());
        }
        break;
      }

      case ExprType::CodeMetadata:
        break;

      case ExprType::Compare:
        Write(*cast<CompareExpr>(&expr));
        break;

      case ExprType::Const: {
        const Const& const_ = cast<ConstExpr>(&expr)->const_;
        PushType(const_.type());
        Write(StackVar(0), " = ", const_, ";", Newline());
        break;
      }

      case ExprType::Convert:
        Write(*cast<ConvertExpr>(&expr));
        break;

      case ExprType::Drop:
        DropTypes(1);
        break;

      case ExprType::GlobalGet: {
        const Var& var = cast<GlobalGetExpr>(&expr)->var;
        PushType(module_->GetGlobal(var)->type);
        Write(StackVar(0), " = ", GlobalInstanceVar(var), ";", Newline());
        break;
      }

      case ExprType::GlobalSet: {
        const Var& var = cast<GlobalSetExpr>(&expr)->var;
        Write(GlobalInstanceVar(var), " = ", StackVar(0), ";", Newline());
        DropTypes(1);
        break;
      }

      case ExprType::If: {
        const IfExpr& if_ = *cast<IfExpr>(&expr);
        Write("if (", StackVar(0), ") ", OpenBrace());
        DropTypes(1);
        std::string label = DefineLabelName(if_.true_.label);
        DropTypes(if_.true_.decl.GetNumParams());
        size_t mark = MarkTypeStack();
        PushLabel(LabelType::If, if_.true_.label, if_.true_.decl.sig);
        PushTypes(if_.true_.decl.sig.param_types);
        Write(if_.true_.exprs, CloseBrace());
        if (!if_.false_.empty()) {
          ResetTypeStack(mark);
          PushTypes(if_.true_.decl.sig.param_types);
          Write(" else ", OpenBrace(), if_.false_, CloseBrace());
        }
        ResetTypeStack(mark);
        Write(Newline(), LabelDecl(label));
        PopLabel();
        PushTypes(if_.true_.decl.sig.result_types);
        break;
      }

      case ExprType::Load:
        Write(*cast<LoadExpr>(&expr));
        break;

      case ExprType::LocalGet: {
        const Var& var = cast<LocalGetExpr>(&expr)->var;
        PushType(func_->GetLocalType(var));
        Write(StackVar(0), " = ", var, ";", Newline());
        break;
      }

      case ExprType::LocalSet: {
        const Var& var = cast<LocalSetExpr>(&expr)->var;
        Write(var, " = ", StackVar(0), ";", Newline());
        DropTypes(1);
        break;
      }

      case ExprType::LocalTee: {
        const Var& var = cast<LocalTeeExpr>(&expr)->var;
        Write(var, " = ", StackVar(0), ";", Newline());
        break;
      }

      case ExprType::Loop: {
        const Block& block = cast<LoopExpr>(&expr)->block;
        if (!block.exprs.empty()) {
          Write(DefineLabelName(block.label), ": ");
          Indent();
          DropTypes(block.decl.GetNumParams());
          size_t mark = MarkTypeStack();
          PushLabel(LabelType::Loop, block.label, block.decl.sig);
          PushTypes(block.decl.sig.param_types);
          Write(Newline(), block.exprs);
          ResetTypeStack(mark);
          PopLabel();
          PushTypes(block.decl.sig.result_types);
          Dedent();
        }
        break;
      }

      case ExprType::MemoryFill: {
        const auto inst = cast<MemoryFillExpr>(&expr);
        Memory* memory =
            module_->memories[module_->GetMemoryIndex(inst->memidx)];
        Write("memory_fill(",
              ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", ",
              StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::MemoryCopy: {
        const auto inst = cast<MemoryCopyExpr>(&expr);
        Memory* dest_memory =
            module_->memories[module_->GetMemoryIndex(inst->destmemidx)];
        const Memory* src_memory = module_->GetMemory(inst->srcmemidx);
        Write("memory_copy(",
              ExternalInstancePtr(ModuleFieldType::Memory, dest_memory->name),
              ", ",
              ExternalInstancePtr(ModuleFieldType::Memory, src_memory->name),
              ", ", StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::MemoryInit: {
        const auto inst = cast<MemoryInitExpr>(&expr);
        Memory* dest_memory =
            module_->memories[module_->GetMemoryIndex(inst->memidx)];
        const DataSegment* src_data = module_->GetDataSegment(inst->var);
        Write("memory_init(",
              ExternalInstancePtr(ModuleFieldType::Memory, dest_memory->name),
              ", ");
        if (src_data->data.empty()) {
          Write("NULL, 0");
        } else {
          Write("data_segment_data_",
                GlobalName(ModuleFieldType::DataSegment, src_data->name), ", ");
          if (is_droppable(src_data)) {
            Write("(", "instance->data_segment_dropped_",
                  GlobalName(ModuleFieldType::DataSegment, src_data->name),
                  " ? 0 : ", src_data->data.size(), ")");
          } else {
            Write("0");
          }
        }

        Write(", ", StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::TableInit: {
        const auto inst = cast<TableInitExpr>(&expr);
        Table* dest_table =
            module_->tables[module_->GetTableIndex(inst->table_index)];
        const ElemSegment* src_segment =
            module_->GetElemSegment(inst->segment_index);

        WriteElemTableInit(false, src_segment, dest_table);
        DropTypes(3);
      } break;

      case ExprType::DataDrop: {
        const auto inst = cast<DataDropExpr>(&expr);
        const DataSegment* data = module_->GetDataSegment(inst->var);
        if (is_droppable(data)) {
          Write("instance->data_segment_dropped_",
                GlobalName(ModuleFieldType::DataSegment, data->name),
                " = true;", Newline());
        }
      } break;

      case ExprType::ElemDrop: {
        const auto inst = cast<ElemDropExpr>(&expr);
        const ElemSegment* seg = module_->GetElemSegment(inst->var);
        if (is_droppable(seg)) {
          Write("instance->elem_segment_dropped_",
                GlobalName(ModuleFieldType::ElemSegment, seg->name), " = true;",
                Newline());
        }
      } break;

      case ExprType::TableCopy: {
        const auto inst = cast<TableCopyExpr>(&expr);
        Table* dest_table =
            module_->tables[module_->GetTableIndex(inst->dst_table)];
        const Table* src_table = module_->GetTable(inst->src_table);
        if (dest_table->elem_type != src_table->elem_type) {
          WABT_UNREACHABLE;
        }

        Write(
            GetReferenceTypeName(dest_table->elem_type), "_table_copy(",
            ExternalInstancePtr(ModuleFieldType::Table, dest_table->name), ", ",
            ExternalInstancePtr(ModuleFieldType::Table, src_table->name), ", ",
            StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");", Newline());
        DropTypes(3);
      } break;

      case ExprType::TableGet: {
        const Table* table = module_->GetTable(cast<TableGetExpr>(&expr)->var);
        Write(StackVar(0, table->elem_type), " = ",
              GetReferenceTypeName(table->elem_type), "_table_get(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(0), ");", Newline());
        DropTypes(1);
        PushType(table->elem_type);
      } break;

      case ExprType::TableSet: {
        const Table* table = module_->GetTable(cast<TableSetExpr>(&expr)->var);
        Write(GetReferenceTypeName(table->elem_type), "_table_set(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(1), ", ", StackVar(0), ");", Newline());
        DropTypes(2);
      } break;

      case ExprType::TableGrow: {
        const Table* table = module_->GetTable(cast<TableGrowExpr>(&expr)->var);
        Write(StackVar(1, table->elem_limits.IndexType()), " = wasm_rt_grow_",
              GetReferenceTypeName(table->elem_type), "_table(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(0), ", ", StackVar(1), ");", Newline());
        DropTypes(2);
        PushType(table->elem_limits.IndexType());
      } break;

      case ExprType::TableSize: {
        const Table* table = module_->GetTable(cast<TableSizeExpr>(&expr)->var);

        PushType(table->elem_limits.IndexType());
        Write(StackVar(0), " = ",
              ExternalInstanceRef(ModuleFieldType::Table, table->name),
              ".size;", Newline());
      } break;

      case ExprType::TableFill: {
        const Table* table = module_->GetTable(cast<TableFillExpr>(&expr)->var);
        Write(GetReferenceTypeName(table->elem_type), "_table_fill(",
              ExternalInstancePtr(ModuleFieldType::Table, table->name), ", ",
              StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");",
              Newline());
        DropTypes(3);
      } break;

      case ExprType::RefFunc: {
        const Func* func = module_->GetFunc(cast<RefFuncExpr>(&expr)->var);
        PushType(Type::FuncRef);
        const FuncDeclaration& decl = func->decl;

        assert(decl.has_func_type);
        const FuncType* func_type = module_->GetFuncType(decl.type_var);

        Write(StackVar(0), " = (wasm_rt_funcref_t){", FuncTypeExpr(func_type),
              ", (wasm_rt_function_ptr_t)", WrapperRef(func->name), ", {");
        if (options_.features.tail_call_enabled() &&
            (IsImport(func->name) || func->features_used.tailcall)) {
          Write(TailCallRef(func->name));
        } else {
          Write("NULL");
        }
        Write("}, ");

        if (IsImport(func->name)) {
          Write("instance->", GlobalName(ModuleFieldType::Import,
                                         import_module_sym_map_[func->name]));
        } else {
          Write("instance");
        }

        Write("};", Newline());
      } break;

      case ExprType::RefNull:
        PushType(cast<RefNullExpr>(&expr)->type);
        Write(StackVar(0), " = ",
              GetReferenceNullValue(cast<RefNullExpr>(&expr)->type), ";",
              Newline());
        break;

      case ExprType::RefIsNull:
        switch (StackType(0)) {
          case Type::FuncRef:
            Write(StackVar(0, Type::I32), " = (", StackVar(0), ".func == NULL",
                  ");", Newline());
            break;
          case Type::ExternRef:
            Write(StackVar(0, Type::I32), " = (", StackVar(0),
                  " == ", GetReferenceNullValue(Type::ExternRef), ");",
                  Newline());
            break;
          case Type::ExnRef:
            Write(StackVar(0, Type::I32), " = (", StackVar(0), ".tag == NULL",
                  ");", Newline());
            break;
          default:
            WABT_UNREACHABLE;
        }

        DropTypes(1);
        PushType(Type::I32);
        break;

      case ExprType::MemoryGrow: {
        Memory* memory = module_->memories[module_->GetMemoryIndex(
            cast<MemoryGrowExpr>(&expr)->memidx)];

        std::string func = GetMemoryAPIString(*memory, "wasm_rt_grow_memory");
        Write(StackVar(0), " = ", func, "(",
              ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", ",
              StackVar(0), ");", Newline());
        if (IsSingleUnsharedMemory()) {
          InstallSegueBase(module_->memories[0], false /* save_old_value */);
        }
        break;
      }

      case ExprType::MemorySize: {
        Memory* memory = module_->memories[module_->GetMemoryIndex(
            cast<MemorySizeExpr>(&expr)->memidx)];

        PushType(memory->page_limits.IndexType());
        Write(StackVar(0), " = ",
              ExternalInstanceRef(ModuleFieldType::Memory, memory->name),
              ".pages;", Newline());
        break;
      }

      case ExprType::Nop:
        break;

      case ExprType::Return:
        // Goto the function label instead; this way we can do shared function
        // cleanup code in one place.
        Write(GotoLabel(Var(label_stack_.size() - 1, {})), Newline());
        // Stop processing this ExprList, since the following are unreachable.
        return;

      case ExprType::Select: {
        Type type = StackType(1);
        Write(StackVar(2), " = ", StackVar(0), " ? ", StackVar(2), " : ",
              StackVar(1), ";", Newline());
        DropTypes(3);
        PushType(type);
        break;
      }

      case ExprType::Store:
        Write(*cast<StoreExpr>(&expr));
        break;

      case ExprType::Unary:
        Write(*cast<UnaryExpr>(&expr));
        break;

      case ExprType::Ternary:
        Write(*cast<TernaryExpr>(&expr));
        break;

      case ExprType::SimdLaneOp: {
        Write(*cast<SimdLaneOpExpr>(&expr));
        break;
      }

      case ExprType::SimdLoadLane: {
        Write(*cast<SimdLoadLaneExpr>(&expr));
        break;
      }

      case ExprType::SimdStoreLane: {
        Write(*cast<SimdStoreLaneExpr>(&expr));
        break;
      }

      case ExprType::SimdShuffleOp: {
        Write(*cast<SimdShuffleOpExpr>(&expr));
        break;
      }

      case ExprType::LoadSplat:
        Write(*cast<LoadSplatExpr>(&expr));
        break;

      case ExprType::LoadZero:
        Write(*cast<LoadZeroExpr>(&expr));
        break;

      case ExprType::Unreachable:
        Write("UNREACHABLE;", Newline());
        return;

      case ExprType::Throw: {
        const Var& var = cast<ThrowExpr>(&expr)->var;
        const Tag* tag = module_->GetTag(var);

        Index num_params = tag->decl.GetNumParams();
        if (num_params == 0) {
          Write("wasm_rt_load_exception(", TagSymbol(tag->name), ", 0, NULL);",
                Newline());
        } else if (num_params == 1) {
          Write("wasm_rt_load_exception(", TagSymbol(tag->name), ", sizeof(",
                tag->decl.GetParamType(0), "), &", StackVar(0), ");",
                Newline());
        } else {
          Write(OpenBrace(), tag->decl.sig.param_types, " tmp;", Newline());
          Spill(tag->decl.sig.param_types);
          Write("wasm_rt_load_exception(", TagSymbol(tag->name),
                ", sizeof(tmp), &tmp);", Newline(), CloseBrace(), Newline());
        }

        WriteThrow();
        // Stop processing this ExprList, since the following are unreachable.
        return;
      }

      case ExprType::ThrowRef: {
        Write("if (", StackVar(0), ".tag == NULL) { TRAP(NULL_REF); }");
        Write("wasm_rt_load_exception(", StackVar(0), ".tag, ", StackVar(0),
              ".size, ", StackVar(0), ".data);", Newline());
        DropTypes(1);
        WriteThrow();
        // Stop processing this ExprList, since the following are unreachable.
        return;
      }

      case ExprType::Rethrow: {
        const RethrowExpr* rethrow = cast<RethrowExpr>(&expr);
        assert(rethrow->var.is_name());
        const LabelName ex{rethrow->var.name()};
        func_includes_.insert(rethrow->var.name());
        Write("wasm_rt_load_exception(", ex, "_tag, ", ex, "_size, ", ex, ");",
              Newline());
        WriteThrow();
      } break;

      case ExprType::Try: {
        const TryExpr& tryexpr = *cast<TryExpr>(&expr);
        switch (tryexpr.kind) {
          case TryKind::Plain:
            Write(tryexpr.block);
            break;
          case TryKind::Catch:
            WriteTryCatch(tryexpr);
            break;
          case TryKind::Delegate:
            WriteTryDelegate(tryexpr);
            break;
        }
      } break;

      case ExprType::TryTable: {
        const TryTableExpr& try_table = *cast<TryTableExpr>(&expr);
        if (try_table.catches.empty()) {
          Write(try_table.block);
        } else {
          Write(try_table);
        }
      } break;

      case ExprType::AtomicLoad: {
        Write(*cast<AtomicLoadExpr>(&expr));
        break;
      }

      case ExprType::AtomicStore: {
        Write(*cast<AtomicStoreExpr>(&expr));
        break;
      }

      case ExprType::AtomicRmw: {
        Write(*cast<AtomicRmwExpr>(&expr));
        break;
      }

      case ExprType::AtomicRmwCmpxchg: {
        Write(*cast<AtomicRmwCmpxchgExpr>(&expr));
        break;
      }

      case ExprType::AtomicFence: {
        Write("atomic_fence();", Newline());
        break;
      }

      case ExprType::ReturnCall: {
        const auto inst = cast<ReturnCallExpr>(&expr);
        const Func& func = *module_->GetFunc(inst->var);

        const FuncDeclaration& decl = func.decl;
        assert(decl.sig.result_types == func_->decl.sig.result_types);
        WriteUnwindTryCatchStack(FindLabel(Var(label_stack_.size() - 1, {})));

        if (!IsImport(func.name) && !func.features_used.tailcall) {
          // make normal call, then return
          Write(ExprList{std::make_unique<CallExpr>(inst->var, inst->loc)});
          Write("goto ", LabelName(kImplicitFuncLabel), ";", Newline());
          return;
        }

        WriteTailCallAsserts(decl.sig);
        Write(OpenBrace());
        if (!in_tail_callee_) {
          WriteTailCallStack();
        }

        const Index num_params = decl.GetNumParams();
        if (decl.GetNumParams()) {
          Write(OpenBrace(), decl.sig.param_types, " tmp;", Newline());
          Spill(decl.sig.param_types);
          Write("wasm_rt_memcpy(tail_call_stack, &tmp, sizeof(tmp));",
                Newline());
          Write(CloseBrace(), Newline());
        }

        Write("next->fn = ", TailCallRef(func.name), ";", Newline());
        if (IsImport(func.name)) {
          Write("*instance_ptr = ",
                GlobalName(ModuleFieldType::Import,
                           import_module_sym_map_.at(func.name)),
                ";", Newline());
        }
        DropTypes(num_params);
        FinishReturnCall();
        return;
      }

      case ExprType::ReturnCallIndirect: {
        const auto inst = cast<ReturnCallIndirectExpr>(&expr);
        const FuncDeclaration& decl = inst->decl;
        assert(decl.sig.result_types == func_->decl.sig.result_types);
        assert(decl.has_func_type);
        const Index num_params = decl.GetNumParams();
        WriteTailCallAsserts(decl.sig);
        WriteUnwindTryCatchStack(FindLabel(Var(label_stack_.size() - 1, {})));
        const Table* table = module_->GetTable(inst->table);
        Write("CHECK_CALL_INDIRECT(",
              ExternalInstanceRef(ModuleFieldType::Table, table->name), ", ",
              FuncTypeExpr(module_->GetFuncType(decl.type_var)), ", ",
              StackVar(0), ");", Newline());

        Write("if (!", ExternalInstanceRef(ModuleFieldType::Table, table->name),
              ".data[", StackVar(0), "].func_tailcallee.fn) ", OpenBrace());
        auto ci = std::make_unique<CallIndirectExpr>(inst->loc);
        std::tie(ci->decl, ci->table) = std::make_pair(inst->decl, inst->table);
        Write(ExprList{std::move(ci)});
        if (in_tail_callee_) {
          Write("next->fn = NULL;", Newline());
        }
        Write(CloseBrace(), " else ", OpenBrace());

        DropTypes(decl.GetNumResults());
        PushTypes(decl.sig.param_types);
        PushType(Type::I32);

        if (!in_tail_callee_) {
          WriteTailCallStack();
        }

        if (num_params) {
          Write(OpenBrace(), decl.sig.param_types, " tmp;", Newline());
          Spill(decl.sig.param_types,
                [&](auto i) { return StackVar(num_params - i); });
          Write("wasm_rt_memcpy(tail_call_stack, &tmp, sizeof(tmp));",
                Newline());
          Write(CloseBrace(), Newline());
        }

        assert(decl.has_func_type);
        Write("next->fn = ",
              ExternalInstanceRef(ModuleFieldType::Table, table->name),
              ".data[", StackVar(0), "].func_tailcallee.fn;", Newline());
        Write("*instance_ptr = ",
              ExternalInstanceRef(ModuleFieldType::Table, table->name),
              ".data[", StackVar(0), "].module_instance;", Newline());

        DropTypes(num_params + 1);
        FinishReturnCall();
        return;
      }

      case ExprType::AtomicWait:
      case ExprType::AtomicNotify:
      case ExprType::CallRef:
        UNIMPLEMENTED("...");
        break;
    }
  }
}

void CWriter::WriteSimpleUnaryExpr(Opcode opcode, const char* op) {
  Type result_type = opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", op, "(", StackVar(0), ");", Newline());
  DropTypes(1);
  PushType(opcode.GetResultType());
}

void CWriter::WriteInfixBinaryExpr(Opcode opcode,
                                   const char* op,
                                   AssignOp assign_op) {
  Type result_type = opcode.GetResultType();
  Write(StackVar(1, result_type));
  if (assign_op == AssignOp::Allowed) {
    Write(" ", op, "= ", StackVar(0));
  } else {
    Write(" = ", StackVar(1), " ", op, " ", StackVar(0));
  }
  Write(";", Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::WritePrefixBinaryExpr(Opcode opcode, const char* op) {
  Type result_type = opcode.GetResultType();
  Write(StackVar(1, result_type), " = ", op, "(", StackVar(1), ", ",
        StackVar(0), ");", Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::WriteSignedBinaryExpr(Opcode opcode, const char* op) {
  Type result_type = opcode.GetResultType();
  Type type = opcode.GetParamType1();
  assert(opcode.GetParamType2() == type);
  Write(StackVar(1, result_type), " = (", type, ")((", SignedType(type), ")",
        StackVar(1), " ", op, " (", SignedType(type), ")", StackVar(0), ");",
        Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::Write(const BinaryExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Add:
    case Opcode::I64Add:
    case Opcode::F32Add:
    case Opcode::F64Add:
      WriteInfixBinaryExpr(expr.opcode, "+");
      break;

    case Opcode::I32Sub:
    case Opcode::I64Sub:
    case Opcode::F32Sub:
    case Opcode::F64Sub:
      WriteInfixBinaryExpr(expr.opcode, "-");
      break;

    case Opcode::I32Mul:
    case Opcode::I64Mul:
    case Opcode::F32Mul:
    case Opcode::F64Mul:
      WriteInfixBinaryExpr(expr.opcode, "*");
      break;

    case Opcode::I32DivS:
      WritePrefixBinaryExpr(expr.opcode, "I32_DIV_S");
      break;

    case Opcode::I64DivS:
      WritePrefixBinaryExpr(expr.opcode, "I64_DIV_S");
      break;

    case Opcode::I32DivU:
    case Opcode::I64DivU:
      WritePrefixBinaryExpr(expr.opcode, "DIV_U");
      break;

    case Opcode::F32Div:
    case Opcode::F64Div:
      WriteInfixBinaryExpr(expr.opcode, "/");
      break;

    case Opcode::I32RemS:
      WritePrefixBinaryExpr(expr.opcode, "I32_REM_S");
      break;

    case Opcode::I64RemS:
      WritePrefixBinaryExpr(expr.opcode, "I64_REM_S");
      break;

    case Opcode::I32RemU:
    case Opcode::I64RemU:
      WritePrefixBinaryExpr(expr.opcode, "REM_U");
      break;

    case Opcode::I32And:
    case Opcode::I64And:
      WriteInfixBinaryExpr(expr.opcode, "&");
      break;

    case Opcode::I32Or:
    case Opcode::I64Or:
      WriteInfixBinaryExpr(expr.opcode, "|");
      break;

    case Opcode::I32Xor:
    case Opcode::I64Xor:
      WriteInfixBinaryExpr(expr.opcode, "^");
      break;

    case Opcode::I32Shl:
    case Opcode::I64Shl:
      Write(StackVar(1), " <<= (", StackVar(0), " & ",
            GetShiftMask(expr.opcode.GetResultType()), ");", Newline());
      DropTypes(1);
      break;

    case Opcode::I32ShrS:
    case Opcode::I64ShrS: {
      Type type = expr.opcode.GetResultType();
      Write(StackVar(1), " = (", type, ")((", SignedType(type), ")",
            StackVar(1), " >> (", StackVar(0), " & ", GetShiftMask(type), "));",
            Newline());
      DropTypes(1);
      break;
    }

    case Opcode::I32ShrU:
    case Opcode::I64ShrU:
      Write(StackVar(1), " >>= (", StackVar(0), " & ",
            GetShiftMask(expr.opcode.GetResultType()), ");", Newline());
      DropTypes(1);
      break;

    case Opcode::I32Rotl:
      WritePrefixBinaryExpr(expr.opcode, "I32_ROTL");
      break;

    case Opcode::I64Rotl:
      WritePrefixBinaryExpr(expr.opcode, "I64_ROTL");
      break;

    case Opcode::I32Rotr:
      WritePrefixBinaryExpr(expr.opcode, "I32_ROTR");
      break;

    case Opcode::I64Rotr:
      WritePrefixBinaryExpr(expr.opcode, "I64_ROTR");
      break;

    case Opcode::F32Min:
    case Opcode::F64Min:
      WritePrefixBinaryExpr(expr.opcode, "FMIN");
      break;

    case Opcode::F32Max:
    case Opcode::F64Max:
      WritePrefixBinaryExpr(expr.opcode, "FMAX");
      break;

    case Opcode::F32Copysign:
      WritePrefixBinaryExpr(expr.opcode, "copysignf");
      break;

    case Opcode::F64Copysign:
      WritePrefixBinaryExpr(expr.opcode, "copysign");
      break;

    case Opcode::I8X16Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_add");
      break;

    case Opcode::I8X16AddSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_add_sat");
      break;

    case Opcode::I8X16AddSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_add_sat");
      break;

    case Opcode::I8X16AvgrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_avgr");
      break;

    case Opcode::I8X16MaxS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_max");
      break;

    case Opcode::I8X16MaxU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_max");
      break;

    case Opcode::I8X16MinS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_min");
      break;

    case Opcode::I8X16MinU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_min");
      break;

    case Opcode::I8X16NarrowI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i8x16_narrow_i16x8");
      break;

    case Opcode::I8X16NarrowI16X8U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u8x16_narrow_i16x8");
      break;

    case Opcode::I8X16Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_shl");
      break;

    case Opcode::I8X16ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_shr");
      break;

    case Opcode::I8X16ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_shr");
      break;

    case Opcode::I8X16Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_sub");
      break;

    case Opcode::I8X16SubSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_sub_sat");
      break;

    case Opcode::I8X16SubSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_sub_sat");
      break;

    case Opcode::I8X16Swizzle:
      WritePrefixBinaryExpr(expr.opcode, "v128_i8x16_swizzle");
      break;

    case Opcode::I16X8Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_add");
      break;

    case Opcode::I16X8AvgrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_avgr");
      break;

    case Opcode::I16X8AddSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_add_sat");
      break;

    case Opcode::I16X8AddSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_add_sat");
      break;

    case Opcode::I16X8ExtmulHighI8X16S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i16x8_extmul_high_i8x16");
      break;

    case Opcode::I16X8ExtmulHighI8X16U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u16x8_extmul_high_u8x16");
      break;

    case Opcode::I16X8ExtmulLowI8X16S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i16x8_extmul_low_i8x16");
      break;

    case Opcode::I16X8ExtmulLowI8X16U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u16x8_extmul_low_u8x16");
      break;

    case Opcode::I16X8MaxS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_max");
      break;

    case Opcode::I16X8MaxU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_max");
      break;

    case Opcode::I16X8MinS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_min");
      break;

    case Opcode::I16X8MinU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_min");
      break;

    case Opcode::I16X8Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_mul");
      break;

    case Opcode::I16X8NarrowI32X4S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i16x8_narrow_i32x4");
      break;

    case Opcode::I16X8NarrowI32X4U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u16x8_narrow_i32x4");
      break;

    case Opcode::I16X8Q15mulrSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_q15mulr_sat");
      break;

    case Opcode::I16X8Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_shl");
      break;

    case Opcode::I16X8ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_shr");
      break;

    case Opcode::I16X8ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_shr");
      break;

    case Opcode::I16X8Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_sub");
      break;

    case Opcode::I16X8SubSatS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_sub_sat");
      break;

    case Opcode::I16X8SubSatU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_sub_sat");
      break;

    case Opcode::I32X4Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_add");
      break;

    case Opcode::I32X4DotI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_dot_i16x8");
      break;

    case Opcode::I32X4ExtmulHighI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i32x4_extmul_high_i16x8");
      break;

    case Opcode::I32X4ExtmulHighI16X8U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u32x4_extmul_high_u16x8");
      break;

    case Opcode::I32X4ExtmulLowI16X8S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i32x4_extmul_low_i16x8");
      break;

    case Opcode::I32X4ExtmulLowI16X8U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u32x4_extmul_low_u16x8");
      break;

    case Opcode::I32X4MaxS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_max");
      break;

    case Opcode::I32X4MaxU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_max");
      break;

    case Opcode::I32X4MinS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_min");
      break;

    case Opcode::I32X4MinU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_min");
      break;

    case Opcode::I32X4Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_mul");
      break;

    case Opcode::I32X4Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_shl");
      break;

    case Opcode::I32X4ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_shr");
      break;

    case Opcode::I32X4ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_shr");
      break;

    case Opcode::I32X4Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_sub");
      break;

    case Opcode::I64X2Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_add");
      break;

    case Opcode::I64X2ExtmulHighI32X4S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i64x2_extmul_high_i32x4");
      break;

    case Opcode::I64X2ExtmulHighI32X4U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u64x2_extmul_high_u32x4");
      break;

    case Opcode::I64X2ExtmulLowI32X4S:
      WritePrefixBinaryExpr(expr.opcode, "v128_i64x2_extmul_low_i32x4");
      break;

    case Opcode::I64X2ExtmulLowI32X4U:
      WritePrefixBinaryExpr(expr.opcode, "v128_u64x2_extmul_low_u32x4");
      break;

    case Opcode::I64X2Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_mul");
      break;

    case Opcode::I64X2Shl:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_shl");
      break;

    case Opcode::I64X2ShrS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_shr");
      break;

    case Opcode::I64X2ShrU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u64x2_shr");
      break;

    case Opcode::I64X2Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_sub");
      break;

    case Opcode::F32X4Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_add");
      break;

    case Opcode::F32X4Div:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_div");
      break;

    case Opcode::F32X4Max:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_max");
      break;

    case Opcode::F32X4Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_mul");
      break;

    case Opcode::F32X4Min:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_min");
      break;

    case Opcode::F32X4PMax:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_pmax");
      break;

    case Opcode::F32X4PMin:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_pmin");
      break;

    case Opcode::F32X4Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_sub");
      break;

    case Opcode::F64X2Add:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_add");
      break;

    case Opcode::F64X2Div:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_div");
      break;

    case Opcode::F64X2Max:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_max");
      break;

    case Opcode::F64X2Mul:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_mul");
      break;

    case Opcode::F64X2Min:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_min");
      break;

    case Opcode::F64X2PMax:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_pmax");
      break;

    case Opcode::F64X2PMin:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_pmin");
      break;

    case Opcode::F64X2Sub:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_sub");
      break;

    case Opcode::V128And:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_and");
      break;

    case Opcode::V128Andnot:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_andnot");
      break;

    case Opcode::V128Or:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_or");
      break;

    case Opcode::V128Xor:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_v128_xor");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const CompareExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Eq:
    case Opcode::I64Eq:
    case Opcode::F32Eq:
    case Opcode::F64Eq:
      WriteInfixBinaryExpr(expr.opcode, "==", AssignOp::Disallowed);
      break;

    case Opcode::I32Ne:
    case Opcode::I64Ne:
    case Opcode::F32Ne:
    case Opcode::F64Ne:
      WriteInfixBinaryExpr(expr.opcode, "!=", AssignOp::Disallowed);
      break;

    case Opcode::I32LtS:
    case Opcode::I64LtS:
      WriteSignedBinaryExpr(expr.opcode, "<");
      break;

    case Opcode::I32LtU:
    case Opcode::I64LtU:
    case Opcode::F32Lt:
    case Opcode::F64Lt:
      WriteInfixBinaryExpr(expr.opcode, "<", AssignOp::Disallowed);
      break;

    case Opcode::I32LeS:
    case Opcode::I64LeS:
      WriteSignedBinaryExpr(expr.opcode, "<=");
      break;

    case Opcode::I32LeU:
    case Opcode::I64LeU:
    case Opcode::F32Le:
    case Opcode::F64Le:
      WriteInfixBinaryExpr(expr.opcode, "<=", AssignOp::Disallowed);
      break;

    case Opcode::I32GtS:
    case Opcode::I64GtS:
      WriteSignedBinaryExpr(expr.opcode, ">");
      break;

    case Opcode::I32GtU:
    case Opcode::I64GtU:
    case Opcode::F32Gt:
    case Opcode::F64Gt:
      WriteInfixBinaryExpr(expr.opcode, ">", AssignOp::Disallowed);
      break;

    case Opcode::I32GeS:
    case Opcode::I64GeS:
      WriteSignedBinaryExpr(expr.opcode, ">=");
      break;

    case Opcode::I32GeU:
    case Opcode::I64GeU:
    case Opcode::F32Ge:
    case Opcode::F64Ge:
      WriteInfixBinaryExpr(expr.opcode, ">=", AssignOp::Disallowed);
      break;

    case Opcode::I8X16Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_eq");
      break;

    case Opcode::I8X16GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_ge");
      break;

    case Opcode::I8X16GeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_ge");
      break;

    case Opcode::I8X16GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_gt");
      break;

    case Opcode::I8X16GtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_gt");
      break;

    case Opcode::I8X16LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_le");
      break;

    case Opcode::I8X16LeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_le");
      break;

    case Opcode::I8X16LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_lt");
      break;

    case Opcode::I8X16LtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u8x16_lt");
      break;

    case Opcode::I8X16Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i8x16_ne");
      break;

    case Opcode::I16X8Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_eq");
      break;

    case Opcode::I16X8GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_ge");
      break;

    case Opcode::I16X8GeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_ge");
      break;

    case Opcode::I16X8GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_gt");
      break;
    case Opcode::I16X8GtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_gt");
      break;

    case Opcode::I16X8LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_le");
      break;

    case Opcode::I16X8LeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_le");
      break;

    case Opcode::I16X8LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_lt");
      break;

    case Opcode::I16X8LtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u16x8_lt");
      break;

    case Opcode::I16X8Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i16x8_ne");
      break;

    case Opcode::I32X4Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_eq");
      break;

    case Opcode::I32X4GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_ge");
      break;

    case Opcode::I32X4GeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_ge");
      break;

    case Opcode::I32X4GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_gt");
      break;

    case Opcode::I32X4GtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_gt");
      break;

    case Opcode::I32X4LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_le");
      break;

    case Opcode::I32X4LeU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_le");
      break;

    case Opcode::I32X4LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_lt");
      break;

    case Opcode::I32X4LtU:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_u32x4_lt");
      break;

    case Opcode::I32X4Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i32x4_ne");
      break;

    case Opcode::I64X2Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_eq");
      break;

    case Opcode::I64X2GeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_ge");
      break;

    case Opcode::I64X2GtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_gt");
      break;

    case Opcode::I64X2LeS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_le");
      break;

    case Opcode::I64X2LtS:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_lt");
      break;

    case Opcode::I64X2Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_i64x2_ne");
      break;

    case Opcode::F32X4Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_eq");
      break;

    case Opcode::F32X4Ge:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_ge");
      break;

    case Opcode::F32X4Gt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_gt");
      break;

    case Opcode::F32X4Le:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_le");
      break;

    case Opcode::F32X4Lt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_lt");
      break;

    case Opcode::F32X4Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f32x4_ne");
      break;

    case Opcode::F64X2Eq:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_eq");
      break;

    case Opcode::F64X2Ge:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_ge");
      break;

    case Opcode::F64X2Gt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_gt");
      break;

    case Opcode::F64X2Le:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_le");
      break;

    case Opcode::F64X2Lt:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_lt");
      break;

    case Opcode::F64X2Ne:
      WritePrefixBinaryExpr(expr.opcode, "simde_wasm_f64x2_ne");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const ConvertExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Eqz:
    case Opcode::I64Eqz:
      WriteSimpleUnaryExpr(expr.opcode, "!");
      break;

    case Opcode::I64ExtendI32S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s32)");
      break;

    case Opcode::I64ExtendI32U:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)");
      break;

    case Opcode::I32WrapI64:
      WriteSimpleUnaryExpr(expr.opcode, "(u32)");
      break;

    case Opcode::I32TruncF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_S_F32");
      break;

    case Opcode::I64TruncF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_S_F32");
      break;

    case Opcode::I32TruncF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_S_F64");
      break;

    case Opcode::I64TruncF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_S_F64");
      break;

    case Opcode::I32TruncF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_U_F32");
      break;

    case Opcode::I64TruncF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_U_F32");
      break;

    case Opcode::I32TruncF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_U_F64");
      break;

    case Opcode::I64TruncF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_U_F64");
      break;

    case Opcode::I32TruncSatF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_S_F32");
      break;

    case Opcode::I64TruncSatF32S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_S_F32");
      break;

    case Opcode::I32TruncSatF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_S_F64");
      break;

    case Opcode::I64TruncSatF64S:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_S_F64");
      break;

    case Opcode::I32TruncSatF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_U_F32");
      break;

    case Opcode::I64TruncSatF32U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_U_F32");
      break;

    case Opcode::I32TruncSatF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I32_TRUNC_SAT_U_F64");
      break;

    case Opcode::I64TruncSatF64U:
      WriteSimpleUnaryExpr(expr.opcode, "I64_TRUNC_SAT_U_F64");
      break;

    case Opcode::F32ConvertI32S:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)(s32)");
      break;

    case Opcode::F32ConvertI64S:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)(s64)");
      break;

    case Opcode::F32ConvertI32U:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)");
      break;

    case Opcode::F32DemoteF64:
      WriteSimpleUnaryExpr(expr.opcode, "(f32)wasm_quiet");
      break;

    case Opcode::F32ConvertI64U:
      // TODO(binji): This needs to be handled specially (see
      // wabt_convert_uint64_to_float).
      WriteSimpleUnaryExpr(expr.opcode, "(f32)");
      break;

    case Opcode::F64ConvertI32S:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)(s32)");
      break;

    case Opcode::F64ConvertI64S:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)(s64)");
      break;

    case Opcode::F64ConvertI32U:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)");
      break;

    case Opcode::F64PromoteF32:
      WriteSimpleUnaryExpr(expr.opcode, "(f64)wasm_quietf");
      break;

    case Opcode::F64ConvertI64U:
      // TODO(binji): This needs to be handled specially (see
      // wabt_convert_uint64_to_double).
      WriteSimpleUnaryExpr(expr.opcode, "(f64)");
      break;

    case Opcode::F32ReinterpretI32:
      WriteSimpleUnaryExpr(expr.opcode, "f32_reinterpret_i32");
      break;

    case Opcode::I32ReinterpretF32:
      WriteSimpleUnaryExpr(expr.opcode, "i32_reinterpret_f32");
      break;

    case Opcode::F64ReinterpretI64:
      WriteSimpleUnaryExpr(expr.opcode, "f64_reinterpret_i64");
      break;

    case Opcode::I64ReinterpretF64:
      WriteSimpleUnaryExpr(expr.opcode, "i64_reinterpret_f64");
      break;

    case Opcode::I32X4TruncSatF32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_trunc_sat_f32x4");
      break;

    case Opcode::I32X4TruncSatF32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_u32x4_trunc_sat_f32x4");
      break;

    case Opcode::I32X4TruncSatF64X2SZero:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i32x4_trunc_sat_f64x2_zero");
      break;

    case Opcode::I32X4TruncSatF64X2UZero:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u32x4_trunc_sat_f64x2_zero");
      break;

    case Opcode::F32X4ConvertI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_convert_i32x4");
      break;

    case Opcode::F32X4ConvertI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_convert_u32x4");
      break;

    case Opcode::F32X4DemoteF64X2Zero:
      WriteSimpleUnaryExpr(expr.opcode, "v128_f32x4_demote_f64x2_zero");
      break;

    case Opcode::F64X2ConvertLowI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_f64x2_convert_low_i32x4");
      break;

    case Opcode::F64X2ConvertLowI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_f64x2_convert_low_u32x4");
      break;

    case Opcode::F64X2PromoteLowF32X4:
      WriteSimpleUnaryExpr(expr.opcode, "v128_f64x2_promote_low_f32x4");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const LoadExpr& expr) {
  std::string func;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32Load: func = "i32_load"; break;
    case Opcode::I64Load: func = "i64_load"; break;
    case Opcode::F32Load: func = "f32_load"; break;
    case Opcode::F64Load: func = "f64_load"; break;
    case Opcode::I32Load8S: func = "i32_load8_s"; break;
    case Opcode::I64Load8S: func = "i64_load8_s"; break;
    case Opcode::I32Load8U: func = "i32_load8_u"; break;
    case Opcode::I64Load8U: func = "i64_load8_u"; break;
    case Opcode::I32Load16S: func = "i32_load16_s"; break;
    case Opcode::I64Load16S: func = "i64_load16_s"; break;
    case Opcode::I32Load16U: func = "i32_load16_u"; break;
    case Opcode::I64Load16U: func = "i64_load16_u"; break;
    case Opcode::I64Load32S: func = "i64_load32_s"; break;
    case Opcode::I64Load32U: func = "i64_load32_u"; break;
    case Opcode::V128Load: func = "v128_load"; break;
    case Opcode::V128Load8X8S: func = "i16x8_load8x8"; break;
    case Opcode::V128Load8X8U: func = "u16x8_load8x8"; break;
    case Opcode::V128Load16X4S: func = "i32x4_load16x4"; break;
    case Opcode::V128Load16X4U: func = "u32x4_load16x4"; break;
    case Opcode::V128Load32X2S: func = "i64x2_load32x2"; break;
    case Opcode::V128Load32X2U: func = "u64x2_load32x2"; break;

    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  func = GetMemoryAPIString(*memory, func);

  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(0), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset, "u");
  Write(");", Newline());
  DropTypes(1);
  PushType(result_type);
}

void CWriter::Write(const StoreExpr& expr) {
  std::string func;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32Store: func = "i32_store"; break;
    case Opcode::I64Store: func = "i64_store"; break;
    case Opcode::F32Store: func = "f32_store"; break;
    case Opcode::F64Store: func = "f64_store"; break;
    case Opcode::I32Store8: func = "i32_store8"; break;
    case Opcode::I64Store8: func = "i64_store8"; break;
    case Opcode::I32Store16: func = "i32_store16"; break;
    case Opcode::I64Store16: func = "i64_store16"; break;
    case Opcode::I64Store32: func = "i64_store32"; break;
    case Opcode::V128Store: func = "v128_store"; break;

    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  func = GetMemoryAPIString(*memory, func);

  Write(func, "(", ExternalInstancePtr(ModuleFieldType::Memory, memory->name),
        ", (u64)(", StackVar(1), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset);
  Write(", ", StackVar(0), ");", Newline());
  DropTypes(2);
}

void CWriter::Write(const UnaryExpr& expr) {
  switch (expr.opcode) {
    case Opcode::I32Clz:
      WriteSimpleUnaryExpr(expr.opcode, "I32_CLZ");
      break;

    case Opcode::I64Clz:
      WriteSimpleUnaryExpr(expr.opcode, "I64_CLZ");
      break;

    case Opcode::I32Ctz:
      WriteSimpleUnaryExpr(expr.opcode, "I32_CTZ");
      break;

    case Opcode::I64Ctz:
      WriteSimpleUnaryExpr(expr.opcode, "I64_CTZ");
      break;

    case Opcode::I32Popcnt:
      WriteSimpleUnaryExpr(expr.opcode, "I32_POPCNT");
      break;

    case Opcode::I64Popcnt:
      WriteSimpleUnaryExpr(expr.opcode, "I64_POPCNT");
      break;

    case Opcode::F32Neg:
    case Opcode::F64Neg:
      WriteSimpleUnaryExpr(expr.opcode, "-");
      break;

    case Opcode::F32Abs:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_fabsf");
      break;

    case Opcode::F64Abs:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_fabs");
      break;

    case Opcode::F32Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_sqrtf");
      break;

    case Opcode::F64Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_sqrt");
      break;

    case Opcode::F32Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_ceilf");
      break;

    case Opcode::F64Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_ceil");
      break;

    case Opcode::F32Floor:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_floorf");
      break;

    case Opcode::F64Floor:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_floor");
      break;

    case Opcode::F32Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_truncf");
      break;

    case Opcode::F64Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_trunc");
      break;

    case Opcode::F32Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_nearbyintf");
      break;

    case Opcode::F64Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "wasm_nearbyint");
      break;

    case Opcode::I32Extend8S:
      WriteSimpleUnaryExpr(expr.opcode, "(u32)(s32)(s8)(u8)");
      break;

    case Opcode::I32Extend16S:
      WriteSimpleUnaryExpr(expr.opcode, "(u32)(s32)(s16)(u16)");
      break;

    case Opcode::I64Extend8S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s8)(u8)");
      break;

    case Opcode::I64Extend16S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s16)(u16)");
      break;

    case Opcode::I64Extend32S:
      WriteSimpleUnaryExpr(expr.opcode, "(u64)(s64)(s32)(u32)");
      break;

    case Opcode::I8X16Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_abs");
      break;

    case Opcode::I8X16AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_all_true");
      break;

    case Opcode::I8X16Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i8x16_bitmask");
      break;

    case Opcode::I8X16Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_neg");
      break;

    case Opcode::I8X16Popcnt:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_popcnt");
      break;

    case Opcode::I8X16Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i8x16_splat");
      break;

    case Opcode::I16X8Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_abs");
      break;

    case Opcode::I16X8AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_all_true");
      break;

    case Opcode::I16X8Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i16x8_bitmask");
      break;

    case Opcode::I16X8ExtaddPairwiseI8X16S:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_i16x8_extadd_pairwise_i8x16");
      break;

    case Opcode::I16X8ExtaddPairwiseI8X16U:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_u16x8_extadd_pairwise_u8x16");
      break;

    case Opcode::I16X8ExtendHighI8X16S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i16x8_extend_high_i8x16");
      break;

    case Opcode::I16X8ExtendHighI8X16U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u16x8_extend_high_u8x16");
      break;

    case Opcode::I16X8ExtendLowI8X16S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i16x8_extend_low_i8x16");
      break;

    case Opcode::I16X8ExtendLowI8X16U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u16x8_extend_low_u8x16");
      break;

    case Opcode::I16X8Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_neg");
      break;

    case Opcode::I16X8Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i16x8_splat");
      break;

    case Opcode::I32X4Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_abs");
      break;

    case Opcode::I32X4AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_all_true");
      break;

    case Opcode::I32X4Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i32x4_bitmask");
      break;

    case Opcode::I32X4ExtaddPairwiseI16X8S:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_i32x4_extadd_pairwise_i16x8");
      break;

    case Opcode::I32X4ExtaddPairwiseI16X8U:
      WriteSimpleUnaryExpr(expr.opcode,
                           "simde_wasm_u32x4_extadd_pairwise_u16x8");
      break;

    case Opcode::I32X4ExtendHighI16X8S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i32x4_extend_high_i16x8");
      break;

    case Opcode::I32X4ExtendHighI16X8U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u32x4_extend_high_u16x8");
      break;

    case Opcode::I32X4ExtendLowI16X8S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i32x4_extend_low_i16x8");
      break;

    case Opcode::I32X4ExtendLowI16X8U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u32x4_extend_low_u16x8");
      break;

    case Opcode::I32X4Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_neg");
      break;

    case Opcode::I32X4Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i32x4_splat");
      break;

    case Opcode::I64X2Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_abs");
      break;

    case Opcode::I64X2AllTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_all_true");
      break;

    case Opcode::I64X2Bitmask:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i64x2_bitmask");
      break;

    case Opcode::I64X2ExtendHighI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i64x2_extend_high_i32x4");
      break;

    case Opcode::I64X2ExtendHighI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u64x2_extend_high_u32x4");
      break;

    case Opcode::I64X2ExtendLowI32X4S:
      WriteSimpleUnaryExpr(expr.opcode, "v128_i64x2_extend_low_i32x4");
      break;

    case Opcode::I64X2ExtendLowI32X4U:
      WriteSimpleUnaryExpr(expr.opcode, "v128_u64x2_extend_low_u32x4");
      break;

    case Opcode::I64X2Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_neg");
      break;

    case Opcode::I64X2Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_i64x2_splat");
      break;

    case Opcode::F32X4Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_abs");
      break;

    case Opcode::F32X4Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_ceil");
      break;

    case Opcode::F32X4Floor:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_floor");
      break;

    case Opcode::F32X4Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_nearest");
      break;

    case Opcode::F32X4Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_neg");
      break;

    case Opcode::F32X4Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_splat");
      break;

    case Opcode::F32X4Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_sqrt");
      break;

    case Opcode::F32X4Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f32x4_trunc");
      break;

    case Opcode::F64X2Abs:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_abs");
      break;

    case Opcode::F64X2Ceil:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_ceil");
      break;

    case Opcode::F64X2Floor:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_floor");
      break;

    case Opcode::F64X2Nearest:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_nearest");
      break;

    case Opcode::F64X2Neg:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_neg");
      break;

    case Opcode::F64X2Splat:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_splat");
      break;

    case Opcode::F64X2Sqrt:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_sqrt");
      break;

    case Opcode::F64X2Trunc:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_f64x2_trunc");
      break;

    case Opcode::V128AnyTrue:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_v128_any_true");
      break;

    case Opcode::V128Not:
      WriteSimpleUnaryExpr(expr.opcode, "simde_wasm_v128_not");
      break;

    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const TernaryExpr& expr) {
  switch (expr.opcode) {
    case Opcode::V128BitSelect: {
      Type result_type = expr.opcode.GetResultType();
      Write(StackVar(2, result_type), " = ", "simde_wasm_v128_bitselect", "(",
            StackVar(2), ", ", StackVar(1), ", ", StackVar(0), ");", Newline());
      DropTypes(3);
      PushType(result_type);
      break;
    }
    default:
      WABT_UNREACHABLE;
  }
}

void CWriter::Write(const SimdLaneOpExpr& expr) {
  Type result_type = expr.opcode.GetResultType();

  switch (expr.opcode) {
    case Opcode::I8X16ExtractLaneS: {
      Write(StackVar(0, result_type), " = v128_i8x16_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I8X16ExtractLaneU: {
      Write(StackVar(0, result_type), " = v128_u8x16_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I16X8ExtractLaneS: {
      Write(StackVar(0, result_type), " = v128_i16x8_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I16X8ExtractLaneU: {
      Write(StackVar(0, result_type), " = v128_u16x8_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I32X4ExtractLane: {
      Write(StackVar(0, result_type), " = v128_i32x4_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I64X2ExtractLane: {
      Write(StackVar(0, result_type), " = v128_i64x2_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::F32X4ExtractLane: {
      Write(StackVar(0, result_type), " = v128_f32x4_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::F64X2ExtractLane: {
      Write(StackVar(0, result_type), " = v128_f64x2_extract_lane(",
            StackVar(0), ", ", expr.val, ");", Newline());
      DropTypes(1);
      break;
    }
    case Opcode::I8X16ReplaceLane: {
      Write(StackVar(1, result_type), " = v128_i8x16_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::I16X8ReplaceLane: {
      Write(StackVar(1, result_type), " = v128_i16x8_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::I32X4ReplaceLane: {
      Write(StackVar(1, result_type), " = v128_i32x4_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::I64X2ReplaceLane: {
      Write(StackVar(1, result_type), " = v128_i64x2_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::F32X4ReplaceLane: {
      Write(StackVar(1, result_type), " = v128_f32x4_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    case Opcode::F64X2ReplaceLane: {
      Write(StackVar(1, result_type), " = v128_f64x2_replace_lane(",
            StackVar(1), ", ", expr.val, ", ", StackVar(0), ");", Newline());
      DropTypes(2);
      break;
    }
    default:
      WABT_UNREACHABLE;
  }

  PushType(result_type);
}

void CWriter::Write(const SimdLoadLaneExpr& expr) {
  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Load8Lane: func = "v128_load8_lane"; break;
    case Opcode::V128Load16Lane: func = "v128_load16_lane"; break;
    case Opcode::V128Load32Lane: func = "v128_load32_lane"; break;
    case Opcode::V128Load64Lane: func = "v128_load64_lane"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(1, result_type), " = ", func, expr.val, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(1), ")");

  if (expr.offset != 0)
    Write(" + ", expr.offset, "u");
  Write(", ", StackVar(0));
  Write(");", Newline());

  DropTypes(2);
  PushType(result_type);
}

void CWriter::Write(const SimdStoreLaneExpr& expr) {
  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Store8Lane: func = "v128_store8_lane"; break;
    case Opcode::V128Store16Lane: func = "v128_store16_lane"; break;
    case Opcode::V128Store32Lane: func = "v128_store32_lane"; break;
    case Opcode::V128Store64Lane: func = "v128_store64_lane"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  Write(func, expr.val, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(1), ")");

  if (expr.offset != 0)
    Write(" + ", expr.offset, "u");
  Write(", ", StackVar(0));
  Write(");", Newline());

  DropTypes(2);
}

void CWriter::Write(const SimdShuffleOpExpr& expr) {
  Type result_type = expr.opcode.GetResultType();
  switch (expr.opcode) {
    case Opcode::I8X16Shuffle: {
      Write(StackVar(1, result_type), " = v128_i8x16_shuffle(", StackVar(1),
            ", ", StackVar(0));
      for (int i = 0; i < 16; i++) {
        Write(", ", expr.val.u8(i));
      }
      Write(");", Newline());
      DropTypes(2);
      break;
    }
    default:
      WABT_UNREACHABLE;
  }
  PushType(result_type);
}

void CWriter::Write(const LoadSplatExpr& expr) {
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Load8Splat: func = "v128_load8_splat"; break;
    case Opcode::V128Load16Splat: func = "v128_load16_splat"; break;
    case Opcode::V128Load32Splat: func = "v128_load32_splat"; break;
    case Opcode::V128Load64Splat: func = "v128_load64_splat"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on
  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(0), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset);
  Write(");", Newline());

  DropTypes(1);
  PushType(result_type);
}

void CWriter::Write(const LoadZeroExpr& expr) {
  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];

  const char* func = nullptr;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::V128Load32Zero: func = "v128_load32_zero"; break;
    case Opcode::V128Load64Zero: func = "v128_load64_zero"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(0), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset);
  Write(");", Newline());

  DropTypes(1);
  PushType(result_type);
}

void CWriter::Write(const AtomicLoadExpr& expr) {
  std::string func;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32AtomicLoad: func = "i32_atomic_load"; break;
    case Opcode::I64AtomicLoad: func = "i64_atomic_load"; break;
    case Opcode::I32AtomicLoad8U: func = "i32_atomic_load8_u"; break;
    case Opcode::I64AtomicLoad8U: func = "i64_atomic_load8_u"; break;
    case Opcode::I32AtomicLoad16U: func = "i32_atomic_load16_u"; break;
    case Opcode::I64AtomicLoad16U: func = "i64_atomic_load16_u"; break;
    case Opcode::I64AtomicLoad32U: func = "i64_atomic_load32_u"; break;

    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  func = GetMemoryAPIString(*memory, func);

  Type result_type = expr.opcode.GetResultType();
  Write(StackVar(0, result_type), " = ", func, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(0), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset, "u");
  Write(");", Newline());
  DropTypes(1);
  PushType(result_type);
}

void CWriter::Write(const AtomicStoreExpr& expr) {
  std::string func;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32AtomicStore: func = "i32_atomic_store"; break;
    case Opcode::I64AtomicStore: func = "i64_atomic_store"; break;
    case Opcode::I32AtomicStore8: func = "i32_atomic_store8"; break;
    case Opcode::I64AtomicStore8: func = "i64_atomic_store8"; break;
    case Opcode::I32AtomicStore16: func = "i32_atomic_store16"; break;
    case Opcode::I64AtomicStore16: func = "i64_atomic_store16"; break;
    case Opcode::I64AtomicStore32: func = "i64_atomic_store32"; break;

    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  func = GetMemoryAPIString(*memory, func);

  Write(func, "(", ExternalInstancePtr(ModuleFieldType::Memory, memory->name),
        ", (u64)(", StackVar(1), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset);
  Write(", ", StackVar(0), ");", Newline());
  DropTypes(2);
}

void CWriter::Write(const AtomicRmwExpr& expr) {
  std::string func;
  // clang-format off
  switch (expr.opcode) {
    case Opcode::I32AtomicRmwAdd: func = "i32_atomic_rmw_add"; break;
    case Opcode::I64AtomicRmwAdd: func = "i64_atomic_rmw_add"; break;
    case Opcode::I32AtomicRmw8AddU: func = "i32_atomic_rmw8_add_u"; break;
    case Opcode::I32AtomicRmw16AddU: func = "i32_atomic_rmw16_add_u"; break;
    case Opcode::I64AtomicRmw8AddU: func = "i64_atomic_rmw8_add_u"; break;
    case Opcode::I64AtomicRmw16AddU: func = "i64_atomic_rmw16_add_u"; break;
    case Opcode::I64AtomicRmw32AddU: func = "i64_atomic_rmw32_add_u"; break;
    case Opcode::I32AtomicRmwSub: func = "i32_atomic_rmw_sub"; break;
    case Opcode::I64AtomicRmwSub: func = "i64_atomic_rmw_sub"; break;
    case Opcode::I32AtomicRmw8SubU: func = "i32_atomic_rmw8_sub_u"; break;
    case Opcode::I32AtomicRmw16SubU: func = "i32_atomic_rmw16_sub_u"; break;
    case Opcode::I64AtomicRmw8SubU: func = "i64_atomic_rmw8_sub_u"; break;
    case Opcode::I64AtomicRmw16SubU: func = "i64_atomic_rmw16_sub_u"; break;
    case Opcode::I64AtomicRmw32SubU: func = "i64_atomic_rmw32_sub_u"; break;
    case Opcode::I32AtomicRmwAnd: func = "i32_atomic_rmw_and"; break;
    case Opcode::I64AtomicRmwAnd: func = "i64_atomic_rmw_and"; break;
    case Opcode::I32AtomicRmw8AndU: func = "i32_atomic_rmw8_and_u"; break;
    case Opcode::I32AtomicRmw16AndU: func = "i32_atomic_rmw16_and_u"; break;
    case Opcode::I64AtomicRmw8AndU: func = "i64_atomic_rmw8_and_u"; break;
    case Opcode::I64AtomicRmw16AndU: func = "i64_atomic_rmw16_and_u"; break;
    case Opcode::I64AtomicRmw32AndU: func = "i64_atomic_rmw32_and_u"; break;
    case Opcode::I32AtomicRmwOr: func = "i32_atomic_rmw_or"; break;
    case Opcode::I64AtomicRmwOr: func = "i64_atomic_rmw_or"; break;
    case Opcode::I32AtomicRmw8OrU: func = "i32_atomic_rmw8_or_u"; break;
    case Opcode::I32AtomicRmw16OrU: func = "i32_atomic_rmw16_or_u"; break;
    case Opcode::I64AtomicRmw8OrU: func = "i64_atomic_rmw8_or_u"; break;
    case Opcode::I64AtomicRmw16OrU: func = "i64_atomic_rmw16_or_u"; break;
    case Opcode::I64AtomicRmw32OrU: func = "i64_atomic_rmw32_or_u"; break;
    case Opcode::I32AtomicRmwXor: func = "i32_atomic_rmw_xor"; break;
    case Opcode::I64AtomicRmwXor: func = "i64_atomic_rmw_xor"; break;
    case Opcode::I32AtomicRmw8XorU: func = "i32_atomic_rmw8_xor_u"; break;
    case Opcode::I32AtomicRmw16XorU: func = "i32_atomic_rmw16_xor_u"; break;
    case Opcode::I64AtomicRmw8XorU: func = "i64_atomic_rmw8_xor_u"; break;
    case Opcode::I64AtomicRmw16XorU: func = "i64_atomic_rmw16_xor_u"; break;
    case Opcode::I64AtomicRmw32XorU: func = "i64_atomic_rmw32_xor_u"; break;
    case Opcode::I32AtomicRmwXchg: func = "i32_atomic_rmw_xchg"; break;
    case Opcode::I64AtomicRmwXchg: func = "i64_atomic_rmw_xchg"; break;
    case Opcode::I32AtomicRmw8XchgU: func = "i32_atomic_rmw8_xchg_u"; break;
    case Opcode::I32AtomicRmw16XchgU: func = "i32_atomic_rmw16_xchg_u"; break;
    case Opcode::I64AtomicRmw8XchgU: func = "i64_atomic_rmw8_xchg_u"; break;
    case Opcode::I64AtomicRmw16XchgU: func = "i64_atomic_rmw16_xchg_u"; break;
    case Opcode::I64AtomicRmw32XchgU: func = "i64_atomic_rmw32_xchg_u"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  func = GetMemoryAPIString(*memory, func);

  Type result_type = expr.opcode.GetResultType();

  Write(StackVar(1, result_type), " = ", func, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(1), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset);
  Write(", ", StackVar(0), ");", Newline());
  DropTypes(2);
  PushType(result_type);
}

void CWriter::Write(const AtomicRmwCmpxchgExpr& expr) {
  std::string func;
  // clang-format off
  switch(expr.opcode) {
    case Opcode::I32AtomicRmwCmpxchg: func = "i32_atomic_rmw_cmpxchg"; break;
    case Opcode::I64AtomicRmwCmpxchg: func = "i64_atomic_rmw_cmpxchg"; break;
    case Opcode::I32AtomicRmw8CmpxchgU: func = "i32_atomic_rmw8_cmpxchg_u"; break;
    case Opcode::I32AtomicRmw16CmpxchgU: func = "i32_atomic_rmw16_cmpxchg_u"; break;
    case Opcode::I64AtomicRmw8CmpxchgU: func = "i64_atomic_rmw8_cmpxchg_u"; break;
    case Opcode::I64AtomicRmw16CmpxchgU: func = "i64_atomic_rmw16_cmpxchg_u"; break;
    case Opcode::I64AtomicRmw32CmpxchgU: func = "i64_atomic_rmw32_cmpxchg_u"; break;
    default:
      WABT_UNREACHABLE;
  }
  // clang-format on

  Memory* memory = module_->memories[module_->GetMemoryIndex(expr.memidx)];
  func = GetMemoryAPIString(*memory, func);

  Type result_type = expr.opcode.GetResultType();

  Write(StackVar(2, result_type), " = ", func, "(",
        ExternalInstancePtr(ModuleFieldType::Memory, memory->name), ", (u64)(",
        StackVar(2), ")");
  if (expr.offset != 0)
    Write(" + ", expr.offset);
  Write(", ", StackVar(1), ", ", StackVar(0), ");", Newline());
  DropTypes(3);
  PushType(result_type);
}

void CWriter::ReserveExportNames() {
  for (const Export* export_ : module_->exports) {
    ReserveExportName(export_->name);
  }
}

void CWriter::WriteCHeader() {
  ReserveExportNames();

  stream_ = h_stream_;
  std::string guard = GenerateHeaderGuard();
  Write("/* Automatically generated by wasm2c */", Newline());
  Write("#ifndef ", guard, Newline());
  Write("#define ", guard, Newline());
  Write(Newline());
  ComputeSimdScope();
  WriteHeaderIncludes();
  Write(s_header_top);
  Write(Newline());
  WriteModuleInstance();
  WriteInitDecl();
  WriteFreeDecl();
  WriteGetFuncTypeDecl();
  WriteMultivalueResultTypes();
  WriteImports();
  WriteImportProperties(CWriterPhase::Declarations);
  WriteExports(CWriterPhase::Declarations);
  if (options_.features.tail_call_enabled()) {
    WriteTailCallExports(CWriterPhase::Declarations);
  }
  Write(Newline());
  Write(s_header_bottom);
  Write(Newline(), "#endif  /* ", guard, " */", Newline());
}

void CWriter::WriteCSource() {
  /* Write the "top" to h_impl stream */
  stream_ = h_impl_stream_;
  Write("/* Automatically generated by wasm2c */", Newline());
  WriteSourceTop();

  /* Write module-wide declarations to impl header */
  WriteFuncTypeDecls();
  WriteTagTypes();
  WriteTagDecls();
  WriteFuncDeclarations();
  WriteDataInitializerDecls();
  WriteElemInitializerDecls();
  if (options_.features.tail_call_enabled()) {
    WriteTailcalleeParamTypes();
  }

  /* Write the module-wide material to the first output stream */
  stream_ = c_streams_.front();
  WriteMultiCTop();
  WriteFuncTypes();
  WriteTags();
  WriteFuncRefWrappers();
  WriteGlobalInitializers();
  WriteDataInitializers();
  WriteElemInitializers();
  WriteExports(CWriterPhase::Definitions);
  if (options_.features.tail_call_enabled()) {
    WriteTailCallExports(CWriterPhase::Definitions);
    WriteTailCallWeakImports();
  }
  WriteInitInstanceImport();
  WriteImportProperties(CWriterPhase::Definitions);
  WriteInit();
  WriteFree();
  WriteGetFuncType();

  /* Write function bodies across the different output streams */
  WriteFuncs();

  /* For any empty .c output, write a dummy typedef to avoid gcc warning */
  WriteMultiCTopEmpty();
}

Result CWriter::WriteModule(const Module& module) {
  WABT_USE(options_);
  module_ = &module;
  WriteCHeader();
  WriteCSource();
  return result_;
}

// static
const char* CWriter::GetReferenceTypeName(const Type& type) {
  switch (type) {
    case Type::FuncRef:
      return "funcref";
    case Type::ExternRef:
      return "externref";
    case Type::ExnRef:
      return "exnref";
    default:
      WABT_UNREACHABLE;
  }
}

// static
const char* CWriter::GetReferenceNullValue(const Type& type) {
  switch (type) {
    case Type::FuncRef:
      return "wasm_rt_funcref_null_value";
    case Type::ExternRef:
      return "wasm_rt_externref_null_value";
    case Type::ExnRef:
      return "wasm_rt_exnref_null_value";
    default:
      WABT_UNREACHABLE;
  }
}

const char* CWriter::InternalSymbolScope() const {
  if (c_streams_.size() == 1) {
    return "static ";
  } else {
    return "";
  }
}

}  // end anonymous namespace

Result WriteC(std::vector<Stream*>&& c_streams,
              Stream* h_stream,
              Stream* h_impl_stream,
              const char* header_name,
              const char* header_impl_name,
              const Module* module,
              const WriteCOptions& options) {
  CWriter c_writer(std::move(c_streams), h_stream, h_impl_stream, header_name,
                   header_impl_name, options);
  return c_writer.WriteModule(*module);
}

}  // namespace wabt
