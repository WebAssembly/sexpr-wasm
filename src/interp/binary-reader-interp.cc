/*
 * Copyright 2016 WebAssembly Community Group participants
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

#include "wabt/interp/binary-reader-interp.h"

#include <map>
#include <set>

#include "wabt/binary-reader-nop.h"
#include "wabt/feature.h"
#include "wabt/interp/interp.h"
#include "wabt/shared-validator.h"
#include "wabt/stream.h"

namespace wabt {
namespace interp {

namespace {

ValueTypes ToInterp(Index count, Type* types) {
  return ValueTypes(&types[0], &types[count]);
}

Mutability ToMutability(bool mut) {
  return mut ? Mutability::Var : Mutability::Const;
}

SegmentMode ToSegmentMode(uint8_t flags) {
  if ((flags & SegDeclared) == SegDeclared) {
    return SegmentMode::Declared;
  } else if ((flags & SegPassive) == SegPassive) {
    return SegmentMode::Passive;
  } else {
    return SegmentMode::Active;
  }
}

// This is only used to distinguish try blocks and all other blocks,
// so there are only two kinds.
enum class LabelKind { Block, Try };

struct Label {
  LabelKind kind;
  Istream::Offset offset;
  Istream::Offset fixup_offset;
  // Only needs to be set for try blocks.
  u32 handler_desc_index;
};

struct FixupMap {
  using Offset = Istream::Offset;
  using Fixups = std::vector<Offset>;

  void Clear();
  void Append(Index, Offset);
  void Resolve(Istream&, Index);

  std::map<Index, Fixups> map;
};

class BinaryReaderInterp : public BinaryReaderNop {
 public:
  BinaryReaderInterp(ModuleDesc* module,
                     std::string_view filename,
                     Errors* errors,
                     const Features& features);

  // Implement BinaryReader.
  bool OnError(const Error&) override;

  Result EndModule() override;

  Result OnTypeCount(Index count) override;
  Result OnFuncType(Index index,
                    Index param_count,
                    Type* param_types,
                    Index result_count,
                    Type* result_types) override;

  Result OnImportFunc(Index import_index,
                      std::string_view module_name,
                      std::string_view field_name,
                      Index func_index,
                      Index sig_index) override;
  Result OnImportTable(Index import_index,
                       std::string_view module_name,
                       std::string_view field_name,
                       Index table_index,
                       Type elem_type,
                       const Limits* elem_limits) override;
  Result OnImportMemory(Index import_index,
                        std::string_view module_name,
                        std::string_view field_name,
                        Index memory_index,
                        const Limits* page_limits,
                        uint32_t page_size) override;
  Result OnImportGlobal(Index import_index,
                        std::string_view module_name,
                        std::string_view field_name,
                        Index global_index,
                        Type type,
                        bool mutable_) override;
  Result OnImportTag(Index import_index,
                     std::string_view module_name,
                     std::string_view field_name,
                     Index tag_index,
                     Index sig_index) override;

  Result OnFunctionCount(Index count) override;
  Result OnFunction(Index index, Index sig_index) override;

  Result OnTableCount(Index count) override;
  Result OnTable(Index index,
                 Type elem_type,
                 const Limits* elem_limits) override;

  Result OnMemoryCount(Index count) override;
  Result OnMemory(Index index,
                  const Limits* limits,
                  uint32_t page_size) override;

  Result OnGlobalCount(Index count) override;
  Result BeginGlobal(Index index, Type type, bool mutable_) override;
  Result BeginGlobalInitExpr(Index index) override;
  Result EndGlobalInitExpr(Index index) override;

  Result OnTagCount(Index count) override;
  Result OnTagType(Index index, Index sig_index) override;

  Result OnExport(Index index,
                  ExternalKind kind,
                  Index item_index,
                  std::string_view name) override;

  Result OnStartFunction(Index func_index) override;

  Result BeginFunctionBody(Index index, Offset size) override;
  Result OnLocalDeclCount(Index count) override;
  Result OnLocalDecl(Index decl_index, Index count, Type type) override;
  Result EndLocalDecls() override;

  Result OnOpcode(Opcode Opcode) override;
  Result OnAtomicLoadExpr(Opcode opcode,
                          Index memidx,
                          Address alignment_log2,
                          Address offset) override;
  Result OnAtomicStoreExpr(Opcode opcode,
                           Index memidx,
                           Address alignment_log2,
                           Address offset) override;
  Result OnAtomicRmwExpr(Opcode opcode,
                         Index memidx,
                         Address alignment_log2,
                         Address offset) override;
  Result OnAtomicRmwCmpxchgExpr(Opcode opcode,
                                Index memidx,
                                Address alignment_log2,
                                Address offset) override;
  Result OnAtomicWaitExpr(Opcode opcode,
                          Index memidx,
                          Address alignment_log2,
                          Address offset) override;
  Result OnAtomicFenceExpr(uint32_t consistency_model) override;
  Result OnAtomicNotifyExpr(Opcode opcode,
                            Index memidx,
                            Address alignment_log2,
                            Address offset) override;
  Result OnBinaryExpr(Opcode opcode) override;
  Result OnBlockExpr(Type sig_type) override;
  Result OnBrExpr(Index depth) override;
  Result OnBrIfExpr(Index depth) override;
  Result OnBrTableExpr(Index num_targets,
                       Index* target_depths,
                       Index default_target_depth) override;
  Result OnCallExpr(Index func_index) override;
  Result OnCallIndirectExpr(Index sig_index, Index table_index) override;
  Result OnCatchExpr(Index tag_index) override;
  Result OnCatchAllExpr() override;
  Result OnDelegateExpr(Index depth) override;
  Result OnReturnCallExpr(Index func_index) override;
  Result OnReturnCallIndirectExpr(Index sig_index, Index table_index) override;
  Result OnCompareExpr(Opcode opcode) override;
  Result OnConvertExpr(Opcode opcode) override;
  Result OnDropExpr() override;
  Result OnElseExpr() override;
  Result OnEndExpr() override;
  Result OnSkipFunctionBodyExpr(std::vector<uint8_t>& opcode_buffer) override;
  Result OnF32ConstExpr(uint32_t value_bits) override;
  Result OnF64ConstExpr(uint64_t value_bits) override;
  Result OnV128ConstExpr(v128 value_bits) override;
  Result OnGlobalGetExpr(Index global_index) override;
  Result OnGlobalSetExpr(Index global_index) override;
  Result OnI32ConstExpr(uint32_t value) override;
  Result OnI64ConstExpr(uint64_t value) override;
  Result OnIfExpr(Type sig_type) override;
  Result OnLoadExpr(Opcode opcode,
                    Index memidx,
                    Address alignment_log2,
                    Address offset) override;
  Result OnLocalGetExpr(Index local_index) override;
  Result OnLocalSetExpr(Index local_index) override;
  Result OnLocalTeeExpr(Index local_index) override;
  Result OnLoopExpr(Type sig_type) override;
  Result OnMemoryCopyExpr(Index destmemidx, Index srcmemidx) override;
  Result OnDataDropExpr(Index segment_index) override;
  Result OnMemoryGrowExpr(Index memidx) override;
  Result OnMemoryFillExpr(Index memidx) override;
  Result OnMemoryInitExpr(Index segment_index, Index memidx) override;
  Result OnMemorySizeExpr(Index memidx) override;
  Result OnRefFuncExpr(Index func_index) override;
  Result OnRefNullExpr(Type type) override;
  Result OnRefIsNullExpr() override;
  Result OnNopExpr() override;
  Result OnRethrowExpr(Index depth) override;
  Result OnReturnExpr() override;
  Result OnSelectExpr(Index result_count, Type* result_types) override;
  Result OnStoreExpr(Opcode opcode,
                     Index memidx,
                     Address alignment_log2,
                     Address offset) override;
  Result OnUnaryExpr(Opcode opcode) override;
  Result OnTableCopyExpr(Index dst_index, Index src_index) override;
  Result OnTableGetExpr(Index table_index) override;
  Result OnTableSetExpr(Index table_index) override;
  Result OnTableGrowExpr(Index table_index) override;
  Result OnTableSizeExpr(Index table_index) override;
  Result OnTableFillExpr(Index table_index) override;
  Result OnElemDropExpr(Index segment_index) override;
  Result OnTableInitExpr(Index segment_index, Index table_index) override;
  Result OnTernaryExpr(Opcode opcode) override;
  Result OnThrowExpr(Index tag_index) override;
  Result OnThrowRefExpr() override;
  Result OnTryExpr(Type sig_type) override;
  Result OnTryTableExpr(Type sig_type,
                        const CatchClauseVector& catches) override;
  Result OnUnreachableExpr() override;
  Result EndFunctionBody(Index index) override;
  Result OnSimdLaneOpExpr(Opcode opcode, uint64_t value) override;
  Result OnSimdLoadLaneExpr(Opcode opcode,
                            Index memidx,
                            Address alignment_log2,
                            Address offset,
                            uint64_t value) override;
  Result OnSimdStoreLaneExpr(Opcode opcode,
                             Index memidx,
                             Address alignment_log2,
                             Address offset,
                             uint64_t value) override;
  Result OnSimdShuffleOpExpr(Opcode opcode, v128 value) override;
  Result OnLoadSplatExpr(Opcode opcode,
                         Index memidx,
                         Address alignment_log2,
                         Address offset) override;
  Result OnLoadZeroExpr(Opcode opcode,
                        Index memidx,
                        Address alignment_log2,
                        Address offset) override;

  Result OnElemSegmentCount(Index count) override;
  Result BeginElemSegment(Index index,
                          Index table_index,
                          uint8_t flags) override;
  Result BeginElemSegmentInitExpr(Index index) override;
  Result EndElemSegmentInitExpr(Index index) override;
  Result OnElemSegmentElemType(Index index, Type elem_type) override;
  Result OnElemSegmentElemExprCount(Index index, Index count) override;
  Result BeginElemExpr(Index elem_index, Index expr_index) override;
  Result EndElemExpr(Index elem_index, Index expr_index) override;

  Result OnDataCount(Index count) override;
  Result BeginDataSegmentInitExpr(Index index) override;
  Result EndDataSegmentInitExpr(Index index) override;
  Result BeginDataSegment(Index index,
                          Index memory_index,
                          uint8_t flags) override;
  Result OnDataSegmentData(Index index,
                           const void* data,
                           Address size) override;

 private:
  Location GetLocation() const;
  Label* GetLabel(Index depth);
  Label* GetNearestTryLabel(Index depth);
  Label* TopLabel();
  void PushLabel(LabelKind label = LabelKind::Block,
                 Istream::Offset offset = Istream::kInvalidOffset,
                 Istream::Offset fixup_offset = Istream::kInvalidOffset,
                 u32 handler_desc_index = kInvalidIndex);
  void PopLabel();

  void PrintError(const char* format, ...);

  Result GetDropCount(Index keep_count,
                      size_t type_stack_limit,
                      Index* out_drop_count);
  Result GetBrDropKeepCount(Index depth,
                            Index* out_drop_count,
                            Index* out_keep_count);
  Result GetReturnDropKeepCount(Index* out_drop_count, Index* out_keep_count);
  Result GetReturnCallDropKeepCount(const FuncType&,
                                    Index keep_extra,
                                    Index* out_drop_count,
                                    Index* out_keep_count);
  Result BeginInitExpr(FuncDesc* init_func);
  Result EndInitExpr();

  void EmitBr(Index depth,
              Index drop_count,
              Index keep_count,
              Index catch_drop_count);
  void FixupTopLabel();
  u32 GetFuncOffset(Index func_index);

  Index TranslateLocalIndex(Index local_index);

  Index num_func_imports() const;

  Errors* errors_ = nullptr;
  ModuleDesc& module_;
  Istream& istream_;

  SharedValidator validator_;

  FuncDesc* func_;
  std::vector<Label> label_stack_;
  FixupMap depth_fixups_;
  FixupMap func_fixups_;

  u32 local_decl_count_;
  u32 local_count_;

  std::vector<FuncType> func_types_;      // Includes imported and defined.
  std::vector<TableType> table_types_;    // Includes imported and defined.
  std::vector<MemoryType> memory_types_;  // Includes imported and defined.
  std::vector<GlobalType> global_types_;  // Includes imported and defined.
  std::vector<TagType> tag_types_;        // Includes imported and defined.

  std::string_view filename_;
};

Location BinaryReaderInterp::GetLocation() const {
  Location loc;
  loc.filename = filename_;
  loc.offset = state->offset;
  return loc;
}

void FixupMap::Clear() {
  map.clear();
}

void FixupMap::Append(Index index, Offset offset) {
  map[index].push_back(offset);
}

void FixupMap::Resolve(Istream& istream, Index index) {
  auto iter = map.find(index);
  if (iter == map.end()) {
    return;
  }
  for (Offset offset : iter->second) {
    istream.ResolveFixupU32(offset);
  }
  map.erase(iter);
}

BinaryReaderInterp::BinaryReaderInterp(ModuleDesc* module,
                                       std::string_view filename,
                                       Errors* errors,
                                       const Features& features)
    : errors_(errors),
      module_(*module),
      istream_(module->istream),
      validator_(errors, ValidateOptions(features)),
      filename_(filename) {}

Label* BinaryReaderInterp::GetLabel(Index depth) {
  assert(depth < label_stack_.size());
  return &label_stack_[label_stack_.size() - depth - 1];
}

Label* BinaryReaderInterp::GetNearestTryLabel(Index depth) {
  for (size_t i = depth; i < label_stack_.size(); i++) {
    Label* label = &label_stack_[label_stack_.size() - i - 1];
    if (label->kind == LabelKind::Try) {
      return label;
    }
  }
  return nullptr;
}

Label* BinaryReaderInterp::TopLabel() {
  return GetLabel(0);
}

void WABT_PRINTF_FORMAT(2, 3) BinaryReaderInterp::PrintError(const char* format,
                                                             ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  errors_->emplace_back(ErrorLevel::Error, Location(kInvalidOffset), buffer);
}

Result BinaryReaderInterp::GetDropCount(Index keep_count,
                                        size_t type_stack_limit,
                                        Index* out_drop_count) {
  assert(validator_.type_stack_size() >= type_stack_limit);
  Index type_stack_count = validator_.type_stack_size() - type_stack_limit;
  // The keep_count may be larger than the type_stack_count if the typechecker
  // is currently unreachable. In that case, it doesn't matter what value we
  // drop, but 0 is a reasonable choice.
  *out_drop_count =
      type_stack_count >= keep_count ? type_stack_count - keep_count : 0;
  return Result::Ok;
}

Result BinaryReaderInterp::GetBrDropKeepCount(Index depth,
                                              Index* out_drop_count,
                                              Index* out_keep_count) {
  SharedValidator::Label* label;
  CHECK_RESULT(validator_.GetLabel(depth, &label));
  Index keep_count = label->br_types().size();
  CHECK_RESULT(
      GetDropCount(keep_count, label->type_stack_limit, out_drop_count));
  *out_keep_count = keep_count;
  return Result::Ok;
}

Result BinaryReaderInterp::GetReturnDropKeepCount(Index* out_drop_count,
                                                  Index* out_keep_count) {
  CHECK_RESULT(GetBrDropKeepCount(label_stack_.size() - 1, out_drop_count,
                                  out_keep_count));
  *out_drop_count += validator_.GetLocalCount();
  return Result::Ok;
}

Result BinaryReaderInterp::GetReturnCallDropKeepCount(const FuncType& func_type,
                                                      Index keep_extra,
                                                      Index* out_drop_count,
                                                      Index* out_keep_count) {
  Index keep_count = static_cast<Index>(func_type.params.size()) + keep_extra;
  CHECK_RESULT(GetDropCount(keep_count, 0, out_drop_count));
  *out_drop_count += validator_.GetLocalCount();
  *out_keep_count = keep_count;
  return Result::Ok;
}

void BinaryReaderInterp::EmitBr(Index depth,
                                Index drop_count,
                                Index keep_count,
                                Index catch_drop_count) {
  istream_.EmitDropKeep(drop_count, keep_count);
  istream_.EmitCatchDrop(catch_drop_count);
  Istream::Offset offset = GetLabel(depth)->offset;
  istream_.Emit(Opcode::Br);
  if (offset == Istream::kInvalidOffset) {
    // depth_fixups_ stores the depth counting up from zero, where zero is the
    // top-level function scope.
    depth_fixups_.Append(label_stack_.size() - 1 - depth, istream_.end());
  }
  istream_.Emit(offset);
}

void BinaryReaderInterp::FixupTopLabel() {
  depth_fixups_.Resolve(istream_, label_stack_.size() - 1);
}

u32 BinaryReaderInterp::GetFuncOffset(Index func_index) {
  assert(func_index >= num_func_imports());
  FuncDesc& func = module_.funcs[func_index - num_func_imports()];
  if (func.code_offset == Istream::kInvalidOffset) {
    func_fixups_.Append(func_index, istream_.end());
  }
  return func.code_offset;
}

bool BinaryReaderInterp::OnError(const Error& error) {
  errors_->push_back(error);
  return true;
}

Result BinaryReaderInterp::EndModule() {
  CHECK_RESULT(validator_.EndModule());
  return Result::Ok;
}

Result BinaryReaderInterp::OnTypeCount(Index count) {
  module_.func_types.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::OnFuncType(Index index,
                                      Index param_count,
                                      Type* param_types,
                                      Index result_count,
                                      Type* result_types) {
  CHECK_RESULT(validator_.OnFuncType(GetLocation(), param_count, param_types,
                                     result_count, result_types, index));
  module_.func_types.push_back(FuncType(ToInterp(param_count, param_types),
                                        ToInterp(result_count, result_types)));
  return Result::Ok;
}

Result BinaryReaderInterp::OnImportFunc(Index import_index,
                                        std::string_view module_name,
                                        std::string_view field_name,
                                        Index func_index,
                                        Index sig_index) {
  CHECK_RESULT(
      validator_.OnFunction(GetLocation(), Var(sig_index, GetLocation())));
  FuncType& func_type = module_.func_types[sig_index];
  module_.imports.push_back(ImportDesc{ImportType(
      std::string(module_name), std::string(field_name), func_type.Clone())});
  func_types_.push_back(func_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnImportTable(Index import_index,
                                         std::string_view module_name,
                                         std::string_view field_name,
                                         Index table_index,
                                         Type elem_type,
                                         const Limits* elem_limits) {
  CHECK_RESULT(validator_.OnTable(GetLocation(), elem_type, *elem_limits));
  TableType table_type{elem_type, *elem_limits};
  module_.imports.push_back(ImportDesc{ImportType(
      std::string(module_name), std::string(field_name), table_type.Clone())});
  table_types_.push_back(table_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnImportMemory(Index import_index,
                                          std::string_view module_name,
                                          std::string_view field_name,
                                          Index memory_index,
                                          const Limits* page_limits,
                                          uint32_t page_size) {
  CHECK_RESULT(validator_.OnMemory(GetLocation(), *page_limits, page_size));
  MemoryType memory_type{*page_limits, page_size};
  module_.imports.push_back(ImportDesc{ImportType(
      std::string(module_name), std::string(field_name), memory_type.Clone())});
  memory_types_.push_back(memory_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnImportGlobal(Index import_index,
                                          std::string_view module_name,
                                          std::string_view field_name,
                                          Index global_index,
                                          Type type,
                                          bool mutable_) {
  CHECK_RESULT(validator_.OnGlobalImport(GetLocation(), type, mutable_));
  GlobalType global_type{type, ToMutability(mutable_)};
  module_.imports.push_back(ImportDesc{ImportType(
      std::string(module_name), std::string(field_name), global_type.Clone())});
  global_types_.push_back(global_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnImportTag(Index import_index,
                                       std::string_view module_name,
                                       std::string_view field_name,
                                       Index tag_index,
                                       Index sig_index) {
  CHECK_RESULT(validator_.OnTag(GetLocation(), Var(sig_index, GetLocation())));
  FuncType& func_type = module_.func_types[sig_index];
  TagType tag_type{TagAttr::Exception, func_type.params};
  module_.imports.push_back(ImportDesc{ImportType(
      std::string(module_name), std::string(field_name), tag_type.Clone())});
  tag_types_.push_back(tag_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnFunctionCount(Index count) {
  module_.funcs.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::OnFunction(Index index, Index sig_index) {
  CHECK_RESULT(
      validator_.OnFunction(GetLocation(), Var(sig_index, GetLocation())));
  FuncType& func_type = module_.func_types[sig_index];
  module_.funcs.push_back(FuncDesc{func_type, {}, Istream::kInvalidOffset, {}});
  func_types_.push_back(func_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableCount(Index count) {
  module_.tables.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTable(Index index,
                                   Type elem_type,
                                   const Limits* elem_limits) {
  CHECK_RESULT(validator_.OnTable(GetLocation(), elem_type, *elem_limits));
  TableType table_type{elem_type, *elem_limits};
  module_.tables.push_back(TableDesc{table_type});
  table_types_.push_back(table_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemoryCount(Index count) {
  module_.memories.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemory(Index index,
                                    const Limits* limits,
                                    uint32_t page_size) {
  CHECK_RESULT(validator_.OnMemory(GetLocation(), *limits, page_size));
  MemoryType memory_type{*limits, page_size};
  module_.memories.push_back(MemoryDesc{memory_type});
  memory_types_.push_back(memory_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnGlobalCount(Index count) {
  module_.globals.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::BeginGlobal(Index index, Type type, bool mutable_) {
  CHECK_RESULT(validator_.OnGlobal(GetLocation(), type, mutable_));
  GlobalType global_type{type, ToMutability(mutable_)};
  FuncDesc init_func{FuncType{{}, {type}}, {}, Istream::kInvalidOffset, {}};
  module_.globals.push_back(GlobalDesc{global_type, init_func});
  global_types_.push_back(global_type);
  return Result::Ok;
}

Result BinaryReaderInterp::BeginGlobalInitExpr(Index index) {
  GlobalDesc& global = module_.globals.back();
  return BeginInitExpr(&global.init_func);
}

Result BinaryReaderInterp::EndInitExpr() {
  FixupTopLabel();
  CHECK_RESULT(validator_.EndInitExpr());
  istream_.Emit(Opcode::Return);
  PopLabel();
  return Result::Ok;
}

Result BinaryReaderInterp::BeginInitExpr(FuncDesc* func) {
  label_stack_.clear();
  func_ = func;
  func_->code_offset = istream_.end();
  Type type = func->type.results[0];
  CHECK_RESULT(validator_.BeginInitExpr(GetLocation(), type));
  // Push implicit init func label (equivalent to return).
  PushLabel(LabelKind::Try, Istream::kInvalidOffset, Istream::kInvalidOffset);
  return Result::Ok;
}

Result BinaryReaderInterp::EndGlobalInitExpr(Index index) {
  return EndInitExpr();
}

Result BinaryReaderInterp::OnTagCount(Index count) {
  module_.tags.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTagType(Index index, Index sig_index) {
  CHECK_RESULT(validator_.OnTag(GetLocation(), Var(sig_index, GetLocation())));
  FuncType& func_type = module_.func_types[sig_index];
  TagType tag_type{TagAttr::Exception, func_type.params};
  module_.tags.push_back(TagDesc{tag_type});
  tag_types_.push_back(tag_type);
  return Result::Ok;
}

Result BinaryReaderInterp::OnExport(Index index,
                                    ExternalKind kind,
                                    Index item_index,
                                    std::string_view name) {
  CHECK_RESULT(validator_.OnExport(GetLocation(), kind,
                                   Var(item_index, GetLocation()), name));

  std::unique_ptr<ExternType> type;
  switch (kind) {
    case ExternalKind::Func:   type = func_types_[item_index].Clone(); break;
    case ExternalKind::Table:  type = table_types_[item_index].Clone(); break;
    case ExternalKind::Memory: type = memory_types_[item_index].Clone(); break;
    case ExternalKind::Global: type = global_types_[item_index].Clone(); break;
    case ExternalKind::Tag:    type = tag_types_[item_index].Clone(); break;
  }
  module_.exports.push_back(
      ExportDesc{ExportType(std::string(name), std::move(type)), item_index});
  return Result::Ok;
}

Result BinaryReaderInterp::OnStartFunction(Index func_index) {
  CHECK_RESULT(
      validator_.OnStart(GetLocation(), Var(func_index, GetLocation())));
  module_.starts.push_back(StartDesc{func_index});
  return Result::Ok;
}

Result BinaryReaderInterp::OnElemSegmentCount(Index count) {
  module_.elems.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::BeginElemSegment(Index index,
                                            Index table_index,
                                            uint8_t flags) {
  auto mode = ToSegmentMode(flags);
  CHECK_RESULT(validator_.OnElemSegment(GetLocation(),
                                        Var(table_index, GetLocation()), mode));

  ValueType offset_type = ValueType::I32;
  if (table_index < table_types_.size() &&
      table_types_[table_index].limits.is_64) {
    offset_type = ValueType::I64;
  }
  FuncDesc init_func{
      FuncType{{}, {offset_type}}, {}, Istream::kInvalidOffset, {}};
  ElemDesc desc{{}, ValueType::Void, mode, table_index, init_func};
  module_.elems.push_back(desc);
  return Result::Ok;
}

Result BinaryReaderInterp::BeginElemSegmentInitExpr(Index index) {
  ElemDesc& elem = module_.elems.back();
  return BeginInitExpr(&elem.init_func);
}

Result BinaryReaderInterp::EndElemSegmentInitExpr(Index index) {
  return EndInitExpr();
}

Result BinaryReaderInterp::OnElemSegmentElemType(Index index, Type elem_type) {
  ElemDesc& elem = module_.elems.back();
  elem.type = elem_type;
  return validator_.OnElemSegmentElemType(GetLocation(), elem_type);
}

Result BinaryReaderInterp::OnElemSegmentElemExprCount(Index index,
                                                      Index count) {
  ElemDesc& elem = module_.elems.back();
  elem.elements.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::BeginElemExpr(Index elem_index, Index expr_index) {
  assert(elem_index == module_.elems.size() - 1);
  ElemDesc& elem = module_.elems.back();
  elem.elements.push_back(
      {FuncType{{}, {elem.type}}, {}, Istream::kInvalidOffset, {}});
  assert(expr_index == elem.elements.size() - 1);
  return BeginInitExpr(&elem.elements.back());
}

Result BinaryReaderInterp::EndElemExpr(Index elem_index, Index expr_index) {
  return EndInitExpr();
}

Result BinaryReaderInterp::OnDataCount(Index count) {
  validator_.OnDataCount(count);
  module_.datas.reserve(count);
  return Result::Ok;
}

Result BinaryReaderInterp::BeginDataSegmentInitExpr(Index index) {
  DataDesc& data = module_.datas.back();
  return BeginInitExpr(&data.init_func);
}

Result BinaryReaderInterp::EndDataSegmentInitExpr(Index index) {
  return EndInitExpr();
}

Result BinaryReaderInterp::BeginDataSegment(Index index,
                                            Index memory_index,
                                            uint8_t flags) {
  auto mode = ToSegmentMode(flags);
  CHECK_RESULT(validator_.OnDataSegment(
      GetLocation(), Var(memory_index, GetLocation()), mode));

  ValueType offset_type = ValueType::I32;
  if (memory_index < memory_types_.size() &&
      memory_types_[memory_index].limits.is_64) {
    offset_type = ValueType::I64;
  }
  FuncDesc init_func{
      FuncType{{}, {offset_type}}, {}, Istream::kInvalidOffset, {}};
  DataDesc desc{{}, mode, memory_index, init_func};
  module_.datas.push_back(desc);
  return Result::Ok;
}

Result BinaryReaderInterp::OnDataSegmentData(Index index,
                                             const void* src_data,
                                             Address size) {
  DataDesc& dst_data = module_.datas.back();
  if (size > 0) {
    dst_data.data.resize(size);
    memcpy(dst_data.data.data(), src_data, size);
  }
  return Result::Ok;
}

void BinaryReaderInterp::PushLabel(LabelKind kind,
                                   Istream::Offset offset,
                                   Istream::Offset fixup_offset,
                                   u32 handler_desc_index) {
  label_stack_.push_back(Label{kind, offset, fixup_offset, handler_desc_index});
}

void BinaryReaderInterp::PopLabel() {
  label_stack_.pop_back();
}

Result BinaryReaderInterp::BeginFunctionBody(Index index, Offset size) {
  Index defined_index = index - num_func_imports();
  func_ = &module_.funcs[defined_index];
  func_->code_offset = istream_.end();

  depth_fixups_.Clear();
  label_stack_.clear();

  func_fixups_.Resolve(istream_, defined_index);

  CHECK_RESULT(validator_.BeginFunctionBody(GetLocation(), index));

  // Push implicit func label (equivalent to return).
  // With exception handling it acts as a catch-less try block, which is
  // needed to support delegating to the caller of a function using the
  // try-delegate instruction.
  PushLabel(LabelKind::Try, Istream::kInvalidOffset, Istream::kInvalidOffset,
            func_->handlers.size());
  return Result::Ok;
}

Result BinaryReaderInterp::EndFunctionBody(Index index) {
  FixupTopLabel();
  Index drop_count, keep_count;
  CHECK_RESULT(GetReturnDropKeepCount(&drop_count, &keep_count));
  CHECK_RESULT(validator_.EndFunctionBody(GetLocation()));
  istream_.EmitDropKeep(drop_count, keep_count);
  istream_.Emit(Opcode::Return);
  PopLabel();
  func_ = nullptr;
  return Result::Ok;
}

Result BinaryReaderInterp::OnLocalDeclCount(Index count) {
  local_decl_count_ = count;
  local_count_ = 0;
  return Result::Ok;
}

Result BinaryReaderInterp::OnLocalDecl(Index decl_index,
                                       Index count,
                                       Type type) {
  CHECK_RESULT(validator_.OnLocalDecl(GetLocation(), count, type));

  local_count_ += count;
  func_->locals.push_back(LocalDesc{type, count, local_count_});
  return Result::Ok;
}

Result BinaryReaderInterp::EndLocalDecls() {
  if (local_count_ != 0) {
    istream_.Emit(Opcode::InterpAlloca, local_count_);
  }
  // Continuation of the implicit func label, used for exception handling. (See
  // BeginFunctionBody.)
  // We need the local count for this, which is only available after processing
  // all local decls.
  // NOTE: we don't count the parameters, as they're not part of the frame.
  func_->handlers.push_back(HandlerDesc{HandlerKind::Catch,
                                        istream_.end(),
                                        Istream::kInvalidOffset,
                                        {},
                                        {Istream::kInvalidOffset},
                                        static_cast<u32>(local_count_),
                                        0});

  return Result::Ok;
}

Index BinaryReaderInterp::num_func_imports() const {
  return func_types_.size() - module_.funcs.size();
}

Result BinaryReaderInterp::OnOpcode(Opcode opcode) {
  if (func_ == nullptr || label_stack_.empty()) {
    PrintError("Unexpected instruction after end of function");
    return Result::Error;
  }
  return Result::Ok;
}

Result BinaryReaderInterp::OnUnaryExpr(Opcode opcode) {
  CHECK_RESULT(validator_.OnUnary(GetLocation(), opcode));
  istream_.Emit(opcode);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTernaryExpr(Opcode opcode) {
  CHECK_RESULT(validator_.OnTernary(GetLocation(), opcode));
  istream_.Emit(opcode);
  return Result::Ok;
}

Result BinaryReaderInterp::OnSimdLaneOpExpr(Opcode opcode, uint64_t value) {
  CHECK_RESULT(validator_.OnSimdLaneOp(GetLocation(), opcode, value));
  istream_.Emit(opcode, static_cast<u8>(value));
  return Result::Ok;
}

uint32_t GetAlignment(Address alignment_log2) {
  return alignment_log2 < 32 ? 1 << alignment_log2 : ~0u;
}

Result BinaryReaderInterp::OnSimdLoadLaneExpr(Opcode opcode,
                                              Index memidx,
                                              Address alignment_log2,
                                              Address offset,
                                              uint64_t value) {
  CHECK_RESULT(validator_.OnSimdLoadLane(
      GetLocation(), opcode, Var(memidx, GetLocation()),
      GetAlignment(alignment_log2), offset, value));
  istream_.Emit(opcode, memidx, offset, static_cast<u8>(value));
  return Result::Ok;
}

Result BinaryReaderInterp::OnSimdStoreLaneExpr(Opcode opcode,
                                               Index memidx,
                                               Address alignment_log2,
                                               Address offset,
                                               uint64_t value) {
  CHECK_RESULT(validator_.OnSimdStoreLane(
      GetLocation(), opcode, Var(memidx, GetLocation()),
      GetAlignment(alignment_log2), offset, value));
  istream_.Emit(opcode, memidx, offset, static_cast<u8>(value));
  return Result::Ok;
}

Result BinaryReaderInterp::OnSimdShuffleOpExpr(Opcode opcode, v128 value) {
  CHECK_RESULT(validator_.OnSimdShuffleOp(GetLocation(), opcode, value));
  istream_.Emit(opcode, value);
  return Result::Ok;
}

Result BinaryReaderInterp::OnLoadSplatExpr(Opcode opcode,
                                           Index memidx,
                                           Address align_log2,
                                           Address offset) {
  CHECK_RESULT(validator_.OnLoadSplat(GetLocation(), opcode,
                                      Var(memidx, GetLocation()),
                                      GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnLoadZeroExpr(Opcode opcode,
                                          Index memidx,
                                          Address align_log2,
                                          Address offset) {
  CHECK_RESULT(validator_.OnLoadZero(GetLocation(), opcode,
                                     Var(memidx, GetLocation()),
                                     GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicLoadExpr(Opcode opcode,
                                            Index memidx,
                                            Address align_log2,
                                            Address offset) {
  CHECK_RESULT(validator_.OnAtomicLoad(GetLocation(), opcode,
                                       Var(memidx, GetLocation()),
                                       GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicStoreExpr(Opcode opcode,
                                             Index memidx,
                                             Address align_log2,
                                             Address offset) {
  CHECK_RESULT(validator_.OnAtomicStore(GetLocation(), opcode,
                                        Var(memidx, GetLocation()),
                                        GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicRmwExpr(Opcode opcode,
                                           Index memidx,
                                           Address align_log2,
                                           Address offset) {
  CHECK_RESULT(validator_.OnAtomicRmw(GetLocation(), opcode,
                                      Var(memidx, GetLocation()),
                                      GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicRmwCmpxchgExpr(Opcode opcode,
                                                  Index memidx,
                                                  Address align_log2,
                                                  Address offset) {
  CHECK_RESULT(validator_.OnAtomicRmwCmpxchg(GetLocation(), opcode,
                                             Var(memidx, GetLocation()),
                                             GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnBinaryExpr(Opcode opcode) {
  CHECK_RESULT(validator_.OnBinary(GetLocation(), opcode));
  istream_.Emit(opcode);
  return Result::Ok;
}

Result BinaryReaderInterp::OnBlockExpr(Type sig_type) {
  CHECK_RESULT(validator_.OnBlock(GetLocation(), sig_type));
  PushLabel();
  return Result::Ok;
}

Result BinaryReaderInterp::OnLoopExpr(Type sig_type) {
  CHECK_RESULT(validator_.OnLoop(GetLocation(), sig_type));
  PushLabel(LabelKind::Block, istream_.end());
  return Result::Ok;
}

Result BinaryReaderInterp::OnIfExpr(Type sig_type) {
  CHECK_RESULT(validator_.OnIf(GetLocation(), sig_type));
  istream_.Emit(Opcode::InterpBrUnless);
  auto fixup = istream_.EmitFixupU32();
  PushLabel(LabelKind::Block, Istream::kInvalidOffset, fixup);
  return Result::Ok;
}

Result BinaryReaderInterp::OnElseExpr() {
  CHECK_RESULT(validator_.OnElse(GetLocation()));
  Label* label = TopLabel();
  Istream::Offset fixup_cond_offset = label->fixup_offset;
  istream_.Emit(Opcode::Br);
  label->fixup_offset = istream_.EmitFixupU32();
  istream_.ResolveFixupU32(fixup_cond_offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnEndExpr() {
  if (label_stack_.size() == 1) {
    return Result::Ok;
  }
  SharedValidator::Label* label;
  CHECK_RESULT(validator_.GetLabel(0, &label));
  LabelType label_type = label->label_type;
  CHECK_RESULT(validator_.OnEnd(GetLocation()));
  if (label_type == LabelType::If || label_type == LabelType::Else) {
    istream_.ResolveFixupU32(TopLabel()->fixup_offset);
  } else if (label_type == LabelType::Try) {
    // Catch-less try blocks need to fill in the handler description
    // so that it can trigger an exception rethrow when it's reached.
    Label* local_label = TopLabel();
    HandlerDesc& desc = func_->handlers[local_label->handler_desc_index];
    desc.try_end_offset = istream_.end();
    assert(desc.catches.size() == 0);
  } else if (label_type == LabelType::TryTable) {
    // TryTable blocks need a try_end_offset
    Label* local_label = TopLabel();
    HandlerDesc& desc = func_->handlers[local_label->handler_desc_index];
    desc.try_end_offset = istream_.end();
  } else if (label_type == LabelType::Catch) {
    istream_.EmitCatchDrop(1);
  }
  FixupTopLabel();
  PopLabel();
  return Result::Ok;
}

Result BinaryReaderInterp::OnSkipFunctionBodyExpr(
    std::vector<uint8_t>& opcode_buffer) {
  PopLabel();
  return Result::Ok;
}

Result BinaryReaderInterp::OnBrExpr(Index depth) {
  Index drop_count, keep_count, catch_drop_count;
  CHECK_RESULT(GetBrDropKeepCount(depth, &drop_count, &keep_count));
  CHECK_RESULT(validator_.GetCatchCount(depth, &catch_drop_count));
  CHECK_RESULT(validator_.OnBr(GetLocation(), Var(depth, GetLocation())));
  EmitBr(depth, drop_count, keep_count, catch_drop_count);
  return Result::Ok;
}

Result BinaryReaderInterp::OnBrIfExpr(Index depth) {
  Index drop_count, keep_count, catch_drop_count;
  CHECK_RESULT(validator_.OnBrIf(GetLocation(), Var(depth, GetLocation())));
  CHECK_RESULT(GetBrDropKeepCount(depth, &drop_count, &keep_count));
  CHECK_RESULT(validator_.GetCatchCount(depth, &catch_drop_count));
  // Flip the br_if so if <cond> is true it can drop values from the stack.
  istream_.Emit(Opcode::InterpBrUnless);
  auto fixup = istream_.EmitFixupU32();
  EmitBr(depth, drop_count, keep_count, catch_drop_count);
  istream_.ResolveFixupU32(fixup);
  return Result::Ok;
}

Result BinaryReaderInterp::OnBrTableExpr(Index num_targets,
                                         Index* target_depths,
                                         Index default_target_depth) {
  CHECK_RESULT(validator_.BeginBrTable(GetLocation()));
  Index drop_count, keep_count, catch_drop_count;
  istream_.Emit(Opcode::BrTable, num_targets);

  for (Index i = 0; i < num_targets; ++i) {
    Index depth = target_depths[i];
    CHECK_RESULT(
        validator_.OnBrTableTarget(GetLocation(), Var(depth, GetLocation())));
    CHECK_RESULT(GetBrDropKeepCount(depth, &drop_count, &keep_count));
    CHECK_RESULT(validator_.GetCatchCount(depth, &catch_drop_count));
    // Emit DropKeep directly (instead of using EmitDropKeep) so the
    // instruction has a fixed size. Same for CatchDrop as well.
    istream_.Emit(Opcode::InterpDropKeep, drop_count, keep_count);
    istream_.Emit(Opcode::InterpCatchDrop, catch_drop_count);
    EmitBr(depth, 0, 0, 0);
  }
  CHECK_RESULT(validator_.OnBrTableTarget(
      GetLocation(), Var(default_target_depth, GetLocation())));
  CHECK_RESULT(
      GetBrDropKeepCount(default_target_depth, &drop_count, &keep_count));
  CHECK_RESULT(
      validator_.GetCatchCount(default_target_depth, &catch_drop_count));
  // The default case doesn't need a fixed size, since it is never jumped over.
  istream_.EmitDropKeep(drop_count, keep_count);
  istream_.Emit(Opcode::InterpCatchDrop, catch_drop_count);
  EmitBr(default_target_depth, 0, 0, 0);

  CHECK_RESULT(validator_.EndBrTable(GetLocation()));
  return Result::Ok;
}

Result BinaryReaderInterp::OnCallExpr(Index func_index) {
  CHECK_RESULT(
      validator_.OnCall(GetLocation(), Var(func_index, GetLocation())));

  if (func_index >= num_func_imports()) {
    istream_.Emit(Opcode::Call, func_index);
  } else {
    istream_.Emit(Opcode::InterpCallImport, func_index);
  }

  return Result::Ok;
}

Result BinaryReaderInterp::OnCallIndirectExpr(Index sig_index,
                                              Index table_index) {
  CHECK_RESULT(validator_.OnCallIndirect(GetLocation(),
                                         Var(sig_index, GetLocation()),
                                         Var(table_index, GetLocation())));
  istream_.Emit(Opcode::CallIndirect, table_index, sig_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnReturnCallExpr(Index func_index) {
  CHECK_RESULT(
      validator_.OnReturnCall(GetLocation(), Var(func_index, GetLocation())));

  FuncType& func_type = func_types_[func_index];

  Index drop_count, keep_count, catch_drop_count;
  CHECK_RESULT(
      GetReturnCallDropKeepCount(func_type, 0, &drop_count, &keep_count));
  CHECK_RESULT(
      validator_.GetCatchCount(label_stack_.size() - 1, &catch_drop_count));
  // The validator must be run after we get the drop/keep counts, since it
  // will change the type stack.
  CHECK_RESULT(
      validator_.OnReturnCall(GetLocation(), Var(func_index, GetLocation())));
  istream_.EmitDropKeep(drop_count, keep_count);
  istream_.EmitCatchDrop(catch_drop_count);

  if (func_index >= num_func_imports()) {
    istream_.Emit(Opcode::InterpAdjustFrameForReturnCall, func_index);
    istream_.Emit(Opcode::Br);
    // We emit this separately to ensure that the fixup generated by
    // GetFuncOffset comes after the Br opcode.
    istream_.Emit(GetFuncOffset(func_index));
  } else {
    istream_.Emit(Opcode::InterpCallImport, func_index);
    istream_.Emit(Opcode::Return);
  }

  return Result::Ok;
}

Result BinaryReaderInterp::OnReturnCallIndirectExpr(Index sig_index,
                                                    Index table_index) {
  CHECK_RESULT(validator_.OnReturnCallIndirect(
      GetLocation(), Var(sig_index, GetLocation()),
      Var(table_index, GetLocation())));

  FuncType& func_type = module_.func_types[sig_index];

  Index drop_count, keep_count, catch_drop_count;
  // +1 to include the index of the function.
  CHECK_RESULT(
      GetReturnCallDropKeepCount(func_type, +1, &drop_count, &keep_count));
  CHECK_RESULT(
      validator_.GetCatchCount(label_stack_.size() - 1, &catch_drop_count));
  // The validator must be run after we get the drop/keep counts, since it
  // changes the type stack.
  CHECK_RESULT(validator_.OnReturnCallIndirect(
      GetLocation(), Var(sig_index, GetLocation()),
      Var(table_index, GetLocation())));
  istream_.EmitDropKeep(drop_count, keep_count);
  istream_.EmitCatchDrop(catch_drop_count);
  istream_.Emit(Opcode::ReturnCallIndirect, table_index, sig_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnCompareExpr(Opcode opcode) {
  CHECK_RESULT(validator_.OnCompare(GetLocation(), opcode));
  istream_.Emit(opcode);
  return Result::Ok;
}

Result BinaryReaderInterp::OnConvertExpr(Opcode opcode) {
  CHECK_RESULT(validator_.OnConvert(GetLocation(), opcode));
  istream_.Emit(opcode);
  return Result::Ok;
}

Result BinaryReaderInterp::OnDropExpr() {
  CHECK_RESULT(validator_.OnDrop(GetLocation()));
  istream_.Emit(Opcode::Drop);
  return Result::Ok;
}

Result BinaryReaderInterp::OnI32ConstExpr(uint32_t value) {
  CHECK_RESULT(validator_.OnConst(GetLocation(), Type::I32));
  istream_.Emit(Opcode::I32Const, value);
  return Result::Ok;
}

Result BinaryReaderInterp::OnI64ConstExpr(uint64_t value) {
  CHECK_RESULT(validator_.OnConst(GetLocation(), Type::I64));
  istream_.Emit(Opcode::I64Const, value);
  return Result::Ok;
}

Result BinaryReaderInterp::OnF32ConstExpr(uint32_t value_bits) {
  CHECK_RESULT(validator_.OnConst(GetLocation(), Type::F32));
  istream_.Emit(Opcode::F32Const, value_bits);
  return Result::Ok;
}

Result BinaryReaderInterp::OnF64ConstExpr(uint64_t value_bits) {
  CHECK_RESULT(validator_.OnConst(GetLocation(), Type::F64));
  istream_.Emit(Opcode::F64Const, value_bits);
  return Result::Ok;
}

Result BinaryReaderInterp::OnV128ConstExpr(v128 value_bits) {
  CHECK_RESULT(validator_.OnConst(GetLocation(), Type::V128));
  istream_.Emit(Opcode::V128Const, value_bits);
  return Result::Ok;
}

Result BinaryReaderInterp::OnGlobalGetExpr(Index global_index) {
  CHECK_RESULT(
      validator_.OnGlobalGet(GetLocation(), Var(global_index, GetLocation())));
  istream_.Emit(Opcode::GlobalGet, global_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnGlobalSetExpr(Index global_index) {
  CHECK_RESULT(
      validator_.OnGlobalSet(GetLocation(), Var(global_index, GetLocation())));
  istream_.Emit(Opcode::GlobalSet, global_index);
  return Result::Ok;
}

Index BinaryReaderInterp::TranslateLocalIndex(Index local_index) {
  return validator_.type_stack_size() + validator_.GetLocalCount() -
         local_index;
}

Result BinaryReaderInterp::OnLocalGetExpr(Index local_index) {
  // Get the translated index before calling validator_.OnLocalGet because it
  // will update the type stack size. We need the index to be relative to the
  // old stack size.
  Index translated_local_index = TranslateLocalIndex(local_index);
  CHECK_RESULT(
      validator_.OnLocalGet(GetLocation(), Var(local_index, GetLocation())));
  istream_.Emit(Opcode::LocalGet, translated_local_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnLocalSetExpr(Index local_index) {
  // See comment in OnLocalGetExpr above.
  Index translated_local_index = TranslateLocalIndex(local_index);
  CHECK_RESULT(
      validator_.OnLocalSet(GetLocation(), Var(local_index, GetLocation())));
  istream_.Emit(Opcode::LocalSet, translated_local_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnLocalTeeExpr(Index local_index) {
  CHECK_RESULT(
      validator_.OnLocalTee(GetLocation(), Var(local_index, GetLocation())));
  istream_.Emit(Opcode::LocalTee, TranslateLocalIndex(local_index));
  return Result::Ok;
}

Result BinaryReaderInterp::OnLoadExpr(Opcode opcode,
                                      Index memidx,
                                      Address align_log2,
                                      Address offset) {
  CHECK_RESULT(validator_.OnLoad(GetLocation(), opcode,
                                 Var(memidx, GetLocation()),
                                 GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnStoreExpr(Opcode opcode,
                                       Index memidx,
                                       Address align_log2,
                                       Address offset) {
  CHECK_RESULT(validator_.OnStore(GetLocation(), opcode,
                                  Var(memidx, GetLocation()),
                                  GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemoryGrowExpr(Index memidx) {
  CHECK_RESULT(
      validator_.OnMemoryGrow(GetLocation(), Var(memidx, GetLocation())));
  istream_.Emit(Opcode::MemoryGrow, memidx);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemorySizeExpr(Index memidx) {
  CHECK_RESULT(
      validator_.OnMemorySize(GetLocation(), Var(memidx, GetLocation())));
  istream_.Emit(Opcode::MemorySize, memidx);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableGrowExpr(Index table_index) {
  CHECK_RESULT(
      validator_.OnTableGrow(GetLocation(), Var(table_index, GetLocation())));
  istream_.Emit(Opcode::TableGrow, table_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableSizeExpr(Index table_index) {
  CHECK_RESULT(
      validator_.OnTableSize(GetLocation(), Var(table_index, GetLocation())));
  istream_.Emit(Opcode::TableSize, table_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableFillExpr(Index table_index) {
  CHECK_RESULT(
      validator_.OnTableFill(GetLocation(), Var(table_index, GetLocation())));
  istream_.Emit(Opcode::TableFill, table_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnRefFuncExpr(Index func_index) {
  CHECK_RESULT(
      validator_.OnRefFunc(GetLocation(), Var(func_index, GetLocation())));
  istream_.Emit(Opcode::RefFunc, func_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnRefNullExpr(Type type) {
  CHECK_RESULT(validator_.OnRefNull(GetLocation(), type));
  istream_.Emit(Opcode::RefNull);
  return Result::Ok;
}

Result BinaryReaderInterp::OnRefIsNullExpr() {
  CHECK_RESULT(validator_.OnRefIsNull(GetLocation()));
  istream_.Emit(Opcode::RefIsNull);
  return Result::Ok;
}

Result BinaryReaderInterp::OnNopExpr() {
  CHECK_RESULT(validator_.OnNop(GetLocation()));
  return Result::Ok;
}

Result BinaryReaderInterp::OnReturnExpr() {
  Index drop_count, keep_count, catch_drop_count;
  CHECK_RESULT(GetReturnDropKeepCount(&drop_count, &keep_count));
  CHECK_RESULT(
      validator_.GetCatchCount(label_stack_.size() - 1, &catch_drop_count));
  CHECK_RESULT(validator_.OnReturn(GetLocation()));
  istream_.EmitDropKeep(drop_count, keep_count);
  istream_.EmitCatchDrop(catch_drop_count);
  istream_.Emit(Opcode::Return);
  return Result::Ok;
}

Result BinaryReaderInterp::OnSelectExpr(Index result_count,
                                        Type* result_types) {
  CHECK_RESULT(validator_.OnSelect(GetLocation(), result_count, result_types));
  istream_.Emit(Opcode::Select);
  return Result::Ok;
}

Result BinaryReaderInterp::OnUnreachableExpr() {
  CHECK_RESULT(validator_.OnUnreachable(GetLocation()));
  istream_.Emit(Opcode::Unreachable);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicWaitExpr(Opcode opcode,
                                            Index memidx,
                                            Address align_log2,
                                            Address offset) {
  CHECK_RESULT(validator_.OnAtomicWait(GetLocation(), opcode,
                                       Var(memidx, GetLocation()),
                                       GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicFenceExpr(uint32_t consistency_model) {
  CHECK_RESULT(validator_.OnAtomicFence(GetLocation(), consistency_model));
  istream_.Emit(Opcode::AtomicFence, consistency_model);
  return Result::Ok;
}

Result BinaryReaderInterp::OnAtomicNotifyExpr(Opcode opcode,
                                              Index memidx,
                                              Address align_log2,
                                              Address offset) {
  CHECK_RESULT(validator_.OnAtomicNotify(GetLocation(), opcode,
                                         Var(memidx, GetLocation()),
                                         GetAlignment(align_log2), offset));
  istream_.Emit(opcode, memidx, offset);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemoryCopyExpr(Index destmemidx, Index srcmemidx) {
  CHECK_RESULT(validator_.OnMemoryCopy(GetLocation(),
                                       Var(destmemidx, GetLocation()),
                                       Var(srcmemidx, GetLocation())));
  istream_.Emit(Opcode::MemoryCopy, destmemidx, srcmemidx);
  return Result::Ok;
}

Result BinaryReaderInterp::OnDataDropExpr(Index segment_index) {
  CHECK_RESULT(
      validator_.OnDataDrop(GetLocation(), Var(segment_index, GetLocation())));
  istream_.Emit(Opcode::DataDrop, segment_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemoryFillExpr(Index memidx) {
  CHECK_RESULT(
      validator_.OnMemoryFill(GetLocation(), Var(memidx, GetLocation())));
  istream_.Emit(Opcode::MemoryFill, memidx);
  return Result::Ok;
}

Result BinaryReaderInterp::OnMemoryInitExpr(Index segment_index, Index memidx) {
  CHECK_RESULT(validator_.OnMemoryInit(GetLocation(),
                                       Var(segment_index, GetLocation()),
                                       Var(memidx, GetLocation())));
  istream_.Emit(Opcode::MemoryInit, memidx, segment_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableGetExpr(Index table_index) {
  CHECK_RESULT(
      validator_.OnTableGet(GetLocation(), Var(table_index, GetLocation())));
  istream_.Emit(Opcode::TableGet, table_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableSetExpr(Index table_index) {
  CHECK_RESULT(
      validator_.OnTableSet(GetLocation(), Var(table_index, GetLocation())));
  istream_.Emit(Opcode::TableSet, table_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableCopyExpr(Index dst_index, Index src_index) {
  CHECK_RESULT(validator_.OnTableCopy(GetLocation(),
                                      Var(dst_index, GetLocation()),
                                      Var(src_index, GetLocation())));
  istream_.Emit(Opcode::TableCopy, dst_index, src_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnElemDropExpr(Index segment_index) {
  CHECK_RESULT(
      validator_.OnElemDrop(GetLocation(), Var(segment_index, GetLocation())));
  istream_.Emit(Opcode::ElemDrop, segment_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTableInitExpr(Index segment_index,
                                           Index table_index) {
  CHECK_RESULT(validator_.OnTableInit(GetLocation(),
                                      Var(segment_index, GetLocation()),
                                      Var(table_index, GetLocation())));
  istream_.Emit(Opcode::TableInit, table_index, segment_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnThrowExpr(Index tag_index) {
  CHECK_RESULT(
      validator_.OnThrow(GetLocation(), Var(tag_index, GetLocation())));
  istream_.Emit(Opcode::Throw, tag_index);
  return Result::Ok;
}

Result BinaryReaderInterp::OnThrowRefExpr() {
  CHECK_RESULT(validator_.OnThrowRef(GetLocation()));
  istream_.Emit(Opcode::ThrowRef);
  return Result::Ok;
}

Result BinaryReaderInterp::OnRethrowExpr(Index depth) {
  Index catch_depth;
  CHECK_RESULT(validator_.OnRethrow(GetLocation(), Var(depth, GetLocation())));
  CHECK_RESULT(validator_.GetCatchCount(depth, &catch_depth));
  // The rethrow opcode takes an index into the exception stack rather than
  // the number of catch nestings, so we subtract one here.
  istream_.Emit(Opcode::Rethrow, catch_depth - 1);
  return Result::Ok;
}

Result BinaryReaderInterp::OnTryExpr(Type sig_type) {
  u32 exn_stack_height;
  CHECK_RESULT(
      validator_.GetCatchCount(label_stack_.size() - 1, &exn_stack_height));
  // NOTE: *NOT* GetLocalCount. we don't count the parameters, as they're not
  // part of the frame.
  u32 value_stack_height = validator_.type_stack_size() + local_count_;
  CHECK_RESULT(validator_.OnTry(GetLocation(), sig_type));
  // Push a label that tracks mapping of exn -> catch
  PushLabel(LabelKind::Try, Istream::kInvalidOffset, Istream::kInvalidOffset,
            func_->handlers.size());
  func_->handlers.push_back(HandlerDesc{HandlerKind::Catch,
                                        istream_.end(),
                                        Istream::kInvalidOffset,
                                        {},
                                        {Istream::kInvalidOffset},
                                        value_stack_height,
                                        exn_stack_height});
  return Result::Ok;
}

Result BinaryReaderInterp::OnTryTableExpr(Type sig_type,
                                          const CatchClauseVector& catches) {
  // we can just emit the catch handlers beforehand, so long as we skip over
  // them when entering the try.
  CHECK_RESULT(validator_.BeginTryTable(GetLocation(), sig_type));

  u32 exn_stack_height;
  CHECK_RESULT(
      validator_.GetCatchCount(label_stack_.size() - 1, &exn_stack_height));
  // NOTE: *NOT* GetLocalCount. we don't count the parameters, as they're not
  // part of the frame.
  u32 value_stack_height = validator_.type_stack_size() + local_count_;

  HandlerDesc desc =
      HandlerDesc{HandlerKind::Catch,        Istream::kInvalidOffset,
                  Istream::kInvalidOffset,   {},
                  {Istream::kInvalidOffset}, value_stack_height,
                  exn_stack_height};

  istream_.Emit(Opcode::Br);
  auto offset = istream_.EmitFixupU32();

  bool has_catch_all = false;
  for (const auto& raw_catch : catches) {
    TableCatch catch_;
    catch_.kind = raw_catch.kind;
    catch_.tag = Var(raw_catch.tag, GetLocation());
    catch_.target = Var(raw_catch.depth, GetLocation());
    CHECK_RESULT(validator_.OnTryTableCatch(GetLocation(), catch_));
    // stop emitting handlers after catch_all - but we must still validate the
    // handlers we don't emit
    if (has_catch_all) {
      continue;
    }
    if (catch_.IsCatchAll()) {
      has_catch_all = true;
      desc.catch_all_ref = catch_.IsRef();
      desc.catch_all_offset = istream_.end();
    } else {
      desc.catches.push_back(
          CatchDesc{raw_catch.tag, istream_.end(), catch_.IsRef()});
    }
    // we can't use GetBrDropKeepCount because we're not in a real block.
    SharedValidator::Label* vlabel;
    CHECK_RESULT(validator_.GetLabel(raw_catch.depth, &vlabel));
    // we keep the exception's results.
    // (this has already been validated, above)
    Index keep_count = vlabel->br_types().size();
    // we drop everything between the current block and the br target.
    // (we have already taken the TryTable block parameters into account, in
    // BeginTryTable)
    Index drop_count = validator_.type_stack_size() - vlabel->type_stack_limit;
    Index catch_drop_count;
    // we use the regular catch count
    CHECK_RESULT(validator_.GetCatchCount(raw_catch.depth, &catch_drop_count));
    // but increment, as we are semantically in a catch
    catch_drop_count++;
    EmitBr(raw_catch.depth, drop_count, keep_count, catch_drop_count);
  }

  CHECK_RESULT(validator_.EndTryTable(GetLocation(), sig_type));

  desc.try_start_offset = istream_.end();

  // as usual, the label is pushed after the catch handlers
  PushLabel(LabelKind::Try, Istream::kInvalidOffset, Istream::kInvalidOffset,
            func_->handlers.size());
  func_->handlers.push_back(std::move(desc));

  istream_.ResolveFixupU32(offset);

  return Result::Ok;
}

Result BinaryReaderInterp::OnCatchExpr(Index tag_index) {
  CHECK_RESULT(
      validator_.OnCatch(GetLocation(), Var(tag_index, GetLocation()), false));
  Label* label = TopLabel();
  HandlerDesc& desc = func_->handlers[label->handler_desc_index];
  desc.kind = HandlerKind::Catch;
  // Drop the previous block's exception if it was a catch.
  if (label->kind == LabelKind::Block) {
    istream_.EmitCatchDrop(1);
  }
  // Jump to the end of the block at the end of the previous try or catch.
  Istream::Offset offset = label->offset;
  istream_.Emit(Opcode::Br);
  assert(offset == Istream::kInvalidOffset);
  depth_fixups_.Append(label_stack_.size() - 1, istream_.end());
  istream_.Emit(offset);
  // The offset is only set after the first catch block, as the offset range
  // should only cover the try block itself.
  if (desc.try_end_offset == Istream::kInvalidOffset) {
    desc.try_end_offset = istream_.end();
  }
  // The label kind is switched to Block from Try in order to distinguish
  // catch blocks from try blocks. This is used to ensure that a try-delegate
  // inside this catch will not delegate to the catch, and instead find outer
  // try blocks to use as a delegate target.
  label->kind = LabelKind::Block;
  desc.catches.push_back(CatchDesc{tag_index, istream_.end()});
  return Result::Ok;
}

Result BinaryReaderInterp::OnCatchAllExpr() {
  CHECK_RESULT(validator_.OnCatch(GetLocation(), Var(), true));
  Label* label = TopLabel();
  HandlerDesc& desc = func_->handlers[label->handler_desc_index];
  desc.kind = HandlerKind::Catch;
  if (label->kind == LabelKind::Block) {
    istream_.EmitCatchDrop(1);
  }
  Istream::Offset offset = label->offset;
  istream_.Emit(Opcode::Br);
  assert(offset == Istream::kInvalidOffset);
  depth_fixups_.Append(label_stack_.size() - 1, istream_.end());
  istream_.Emit(offset);
  if (desc.try_end_offset == Istream::kInvalidOffset) {
    desc.try_end_offset = istream_.end();
  }
  label->kind = LabelKind::Block;
  desc.catch_all_offset = istream_.end();
  return Result::Ok;
}

Result BinaryReaderInterp::OnDelegateExpr(Index depth) {
  CHECK_RESULT(validator_.OnDelegate(GetLocation(), Var(depth, GetLocation())));
  Label* label = TopLabel();
  assert(label->kind == LabelKind::Try);
  HandlerDesc& desc = func_->handlers[label->handler_desc_index];
  desc.kind = HandlerKind::Delegate;
  Istream::Offset offset = label->offset;
  istream_.Emit(Opcode::Br);
  assert(offset == Istream::kInvalidOffset);
  depth_fixups_.Append(label_stack_.size() - 1, istream_.end());
  istream_.Emit(offset);
  desc.try_end_offset = istream_.end();
  Label* target_label = GetNearestTryLabel(depth + 1);
  assert(target_label);
  desc.delegate_handler_index = target_label->handler_desc_index;
  FixupTopLabel();
  PopLabel();
  return Result::Ok;
}

}  // namespace

Result ReadBinaryInterp(std::string_view filename,
                        const void* data,
                        size_t size,
                        const ReadBinaryOptions& options,
                        Errors* errors,
                        ModuleDesc* out_module) {
  BinaryReaderInterp reader(out_module, filename, errors, options.features);
  return ReadBinary(data, size, &reader, options);
}

}  // namespace interp
}  // namespace wabt
