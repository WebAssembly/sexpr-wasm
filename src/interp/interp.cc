/*
 * Copyright 2020 WebAssembly Community Group participants
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

#include "wabt/interp/interp.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>

#include "wabt/interp/interp-math.h"

namespace wabt {
namespace interp {

const char* GetName(Mutability mut) {
  static const char* kNames[] = {"immutable", "mutable"};
  return kNames[int(mut)];
}

const std::string GetName(ValueType type) {
  return type.GetName();
}

const char* GetName(ExternKind kind) {
  return GetKindName(kind);
}

const char* GetName(ObjectKind kind) {
  static const char* kNames[] = {
      "Null",  "Foreign", "Trap",   "Exception", "DefinedFunc", "HostFunc",
      "Table", "Memory",  "Global", "Tag",       "Module",      "Instance",
  };

  WABT_STATIC_ASSERT(WABT_ARRAY_SIZE(kNames) == kCommandTypeCount);

  return kNames[int(kind)];
}

//// Refs ////
// static
const Ref Ref::Null{0};

//// Limits ////
Result Match(const Limits& expected,
             const Limits& actual,
             std::string* out_msg) {
  if (actual.initial < expected.initial) {
    *out_msg = StringPrintf("actual size (%" PRIu64
                            ") smaller than declared (%" PRIu64 ")",
                            actual.initial, expected.initial);
    return Result::Error;
  }

  if (expected.has_max) {
    if (!actual.has_max) {
      *out_msg = StringPrintf(
          "max size (unspecified) larger than declared (%" PRIu64 ")",
          expected.max);
      return Result::Error;
    } else if (actual.max > expected.max) {
      *out_msg = StringPrintf("max size (%" PRIu64
                              ") larger than declared (%" PRIu64 ")",
                              actual.max, expected.max);
      return Result::Error;
    }
  }

  if (expected.is_64 && !actual.is_64) {
    *out_msg = StringPrintf("expected i64 memory, but i32 memory provided");
    return Result::Error;
  } else if (actual.is_64 && !expected.is_64) {
    *out_msg = StringPrintf("expected i32 memory, but i64 memory provided");
    return Result::Error;
  }

  return Result::Ok;
}

//// FuncType ////
std::unique_ptr<ExternType> FuncType::Clone() const {
  return std::make_unique<FuncType>(*this);
}

Result Match(const FuncType& expected,
             const FuncType& actual,
             std::string* out_msg) {
  if (expected.params != actual.params || expected.results != actual.results) {
    if (out_msg) {
      *out_msg = "import signature mismatch";
    }
    return Result::Error;
  }
  return Result::Ok;
}

//// TableType ////
std::unique_ptr<ExternType> TableType::Clone() const {
  return std::make_unique<TableType>(*this);
}

Result Match(const TableType& expected,
             const TableType& actual,
             std::string* out_msg) {
  if (expected.element != actual.element) {
    *out_msg = StringPrintf(
        "type mismatch in imported table, expected %s but got %s.",
        GetName(expected.element).c_str(), GetName(actual.element).c_str());
    return Result::Error;
  }

  if (Failed(Match(expected.limits, actual.limits, out_msg))) {
    return Result::Error;
  }

  return Result::Ok;
}

//// MemoryType ////
std::unique_ptr<ExternType> MemoryType::Clone() const {
  return std::make_unique<MemoryType>(*this);
}

Result Match(const MemoryType& expected,
             const MemoryType& actual,
             std::string* out_msg) {
  if (expected.page_size != actual.page_size) {
    *out_msg = StringPrintf(
        "page_size mismatch in imported memory, expected %u but got %u.",
        expected.page_size, actual.page_size);
    return Result::Error;
  }
  return Match(expected.limits, actual.limits, out_msg);
}

//// GlobalType ////
std::unique_ptr<ExternType> GlobalType::Clone() const {
  return std::make_unique<GlobalType>(*this);
}

Result Match(const GlobalType& expected,
             const GlobalType& actual,
             std::string* out_msg) {
  if (actual.mut != expected.mut) {
    *out_msg = StringPrintf(
        "mutability mismatch in imported global, expected %s but got %s.",
        GetName(actual.mut), GetName(expected.mut));
    return Result::Error;
  }

  if (actual.type != expected.type &&
      (expected.mut == Mutability::Var ||
       !TypesMatch(expected.type, actual.type))) {
    *out_msg = StringPrintf(
        "type mismatch in imported global, expected %s but got %s.",
        GetName(expected.type).c_str(), GetName(actual.type).c_str());
    return Result::Error;
  }

  return Result::Ok;
}

//// TagType ////
std::unique_ptr<ExternType> TagType::Clone() const {
  return std::make_unique<TagType>(*this);
}

Result Match(const TagType& expected,
             const TagType& actual,
             std::string* out_msg) {
  if (expected.signature != actual.signature) {
    if (out_msg) {
      *out_msg = "signature mismatch in imported tag";
    }
    return Result::Error;
  }
  return Result::Ok;
}

//// Limits ////
template <typename T>
bool CanGrow(const Limits& limits, T old_size, T delta, T* new_size) {
  if (limits.max >= delta && old_size <= limits.max - delta) {
    *new_size = old_size + delta;
    return true;
  }
  return false;
}

//// FuncDesc ////

ValueType FuncDesc::GetLocalType(Index index) const {
  if (index < type.params.size()) {
    return type.params[index];
  }
  index -= type.params.size();

  auto iter = std::lower_bound(
      locals.begin(), locals.end(), index + 1,
      [](const LocalDesc& lhs, Index rhs) { return lhs.end < rhs; });
  assert(iter != locals.end());
  return iter->type;
}

//// Store ////
Store::Store(const Features& features) : features_(features) {
  Ref ref{objects_.New(new Object(ObjectKind::Null))};
  assert(ref == Ref::Null);
  roots_.New(ref);
}

#ifndef NDEBUG
bool Store::HasValueType(Ref ref, ValueType type) const {
  // TODO opt?
  if (!IsValid(ref)) {
    return false;
  }
  if (type == ValueType::ExternRef) {
    return true;
  }
  if (ref == Ref::Null) {
    return true;
  }

  Object* obj = objects_.Get(ref.index);
  switch (type) {
    case ValueType::FuncRef:
      return obj->kind() == ObjectKind::DefinedFunc ||
             obj->kind() == ObjectKind::HostFunc;
    case ValueType::ExnRef:
      return obj->kind() == ObjectKind::Exception;
    default:
      return false;
  }
}
#endif

Store::RootList::Index Store::NewRoot(Ref ref) {
  return roots_.New(ref);
}

Store::RootList::Index Store::CopyRoot(RootList::Index index) {
  // roots_.Get() returns a reference to an element in an internal vector, and
  // roots_.New() might forward its arguments to emplace_back on the same
  // vector. This seems to "work" in most environments, but fails on Visual
  // Studio 2015 Win64. Copying it to a value fixes the issue.
  auto obj_index = roots_.Get(index);
  return roots_.New(obj_index);
}

void Store::DeleteRoot(RootList::Index index) {
  roots_.Delete(index);
}

void Store::Collect() {
  size_t object_count = objects_.size();

  assert(gc_context_.call_depth == 0);

  gc_context_.marks.resize(object_count);
  std::fill(gc_context_.marks.begin(), gc_context_.marks.end(), false);

  // First mark all roots.
  for (RootList::Index i = 0; i < roots_.size(); ++i) {
    if (roots_.IsUsed(i)) {
      Mark(roots_.Get(i));
    }
  }

  for (auto thread : threads_) {
    thread->Mark();
  }

  // This vector is often empty since the default maximum
  // recursion is usually enough to mark all objects.
  while (WABT_UNLIKELY(!gc_context_.untraced_objects.empty())) {
    size_t index = gc_context_.untraced_objects.back();

    assert(gc_context_.marks[index]);
    assert(gc_context_.call_depth == 0);

    gc_context_.untraced_objects.pop_back();
    objects_.Get(index)->Mark(*this);
  }

  assert(gc_context_.call_depth == 0);

  // Delete all unmarked objects.
  for (size_t i = 0; i < object_count; ++i) {
    if (objects_.IsUsed(i) && !gc_context_.marks[i]) {
      objects_.Delete(i);
    }
  }
}

void Store::Mark(Ref ref) {
  size_t index = ref.index;

  if (gc_context_.marks[index])
    return;

  gc_context_.marks[index] = true;

  if (WABT_UNLIKELY(gc_context_.call_depth >= max_call_depth)) {
    gc_context_.untraced_objects.push_back(index);
    return;
  }

  gc_context_.call_depth++;
  objects_.Get(index)->Mark(*this);
  gc_context_.call_depth--;
}

void Store::Mark(const RefVec& refs) {
  for (auto&& ref : refs) {
    Mark(ref);
  }
}

//// Object ////
Object::~Object() {
  if (finalizer_) {
    finalizer_(this);
  }
}

//// Foreign ////
Foreign::Foreign(Store&, void* ptr) : Object(skind), ptr_(ptr) {}

void Foreign::Mark(Store&) {}

//// Frame ////
void Frame::Mark(Store& store) {
  store.Mark(func);
}

//// Trap ////
Trap::Trap(Store& store,
           const std::string& msg,
           const std::vector<Frame>& trace)
    : Object(skind), message_(msg), trace_(trace) {}

void Trap::Mark(Store& store) {
  for (auto&& frame : trace_) {
    frame.Mark(store);
  }
}

//// Exception ////
Exception::Exception(Store& store, Ref tag, Values& args)
    : Object(skind), tag_(tag), args_(args) {}

void Exception::Mark(Store& store) {
  Tag::Ptr tag(store, tag_);
  store.Mark(tag_);
  ValueTypes params = tag->type().signature;
  for (size_t i = 0; i < params.size(); i++) {
    if (params[i].IsRef()) {
      store.Mark(args_[i].Get<Ref>());
    }
  }
}

//// Extern ////
template <typename T>
Result Extern::MatchImpl(Store& store,
                         const ImportType& import_type,
                         const T& actual,
                         Trap::Ptr* out_trap) {
  const T* extern_type = dyn_cast<T>(import_type.type.get());
  if (!extern_type) {
    *out_trap = Trap::New(
        store,
        StringPrintf("expected import \"%s.%s\" to have kind %s, not %s",
                     import_type.module.c_str(), import_type.name.c_str(),
                     GetName(import_type.type->kind), GetName(T::skind)));
    return Result::Error;
  }

  std::string msg;
  if (Failed(interp::Match(*extern_type, actual, &msg))) {
    *out_trap = Trap::New(store, msg);
    return Result::Error;
  }

  return Result::Ok;
}

//// Func ////
Func::Func(ObjectKind kind, FuncType type) : Extern(kind), type_(type) {}

Result Func::Call(Store& store,
                  const Values& params,
                  Values& results,
                  Trap::Ptr* out_trap,
                  Stream* trace_stream) {
  Thread thread(store, trace_stream);
  return DoCall(thread, params, results, out_trap);
}

Result Func::Call(Thread& thread,
                  const Values& params,
                  Values& results,
                  Trap::Ptr* out_trap) {
  return DoCall(thread, params, results, out_trap);
}

//// DefinedFunc ////
DefinedFunc::DefinedFunc(Store& store, Ref instance, FuncDesc desc)
    : Func(skind, desc.type), instance_(instance), desc_(desc) {}

void DefinedFunc::Mark(Store& store) {
  store.Mark(instance_);
}

Result DefinedFunc::Match(Store& store,
                          const ImportType& import_type,
                          Trap::Ptr* out_trap) {
  return MatchImpl(store, import_type, type_, out_trap);
}

Result DefinedFunc::DoCall(Thread& thread,
                           const Values& params,
                           Values& results,
                           Trap::Ptr* out_trap) {
  assert(params.size() == type_.params.size());
  thread.PushValues(type_.params, params);
  RunResult result = thread.PushCall(*this, out_trap);
  if (result == RunResult::Trap) {
    return Result::Error;
  }
  result = thread.Run(out_trap);
  if (result == RunResult::Trap) {
    return Result::Error;
  } else if (result == RunResult::Exception) {
    // While this is not actually a trap, it is a convenient way
    // to report an uncaught exception.
    *out_trap = Trap::New(thread.store(), "uncaught exception");
    return Result::Error;
  }
  thread.PopValues(type_.results, &results);
  return Result::Ok;
}

//// HostFunc ////
HostFunc::HostFunc(Store&, FuncType type, Callback callback)
    : Func(skind, type), callback_(callback) {}

void HostFunc::Mark(Store&) {}

Result HostFunc::Match(Store& store,
                       const ImportType& import_type,
                       Trap::Ptr* out_trap) {
  return MatchImpl(store, import_type, type_, out_trap);
}

Result HostFunc::DoCall(Thread& thread,
                        const Values& params,
                        Values& results,
                        Trap::Ptr* out_trap) {
  return callback_(thread, params, results, out_trap);
}

//// Table ////
Table::Table(Store&, TableType type) : Extern(skind), type_(type) {
  elements_.resize(type.limits.initial);
}

void Table::Mark(Store& store) {
  store.Mark(elements_);
}

Result Table::Match(Store& store,
                    const ImportType& import_type,
                    Trap::Ptr* out_trap) {
  return MatchImpl(store, import_type, type_, out_trap);
}

bool Table::IsValidRange(u32 offset, u32 size) const {
  size_t elem_size = elements_.size();
  return size <= elem_size && offset <= elem_size - size;
}

Result Table::Get(u32 offset, Ref* out) const {
  if (IsValidRange(offset, 1)) {
    *out = elements_[offset];
    return Result::Ok;
  }
  return Result::Error;
}

Ref Table::UnsafeGet(u32 offset) const {
  assert(IsValidRange(offset, 1));
  return elements_[offset];
}

Result Table::Set(Store& store, u32 offset, Ref ref) {
  assert(store.HasValueType(ref, type_.element));
  if (IsValidRange(offset, 1)) {
    elements_[offset] = ref;
    return Result::Ok;
  }
  return Result::Error;
}

Result Table::Grow(Store& store, u32 count, Ref ref) {
  size_t old_size = elements_.size();
  u32 new_size;
  assert(store.HasValueType(ref, type_.element));
  if (CanGrow<u32>(type_.limits, old_size, count, &new_size)) {
    // Grow the limits of the table too, so that if it is used as an
    // import to another module its new size is honored.
    type_.limits.initial += count;
    elements_.resize(new_size);
    Fill(store, old_size, ref, new_size - old_size);
    return Result::Ok;
  }
  return Result::Error;
}

Result Table::Fill(Store& store, u32 offset, Ref ref, u32 size) {
  assert(store.HasValueType(ref, type_.element));
  if (IsValidRange(offset, size)) {
    std::fill(elements_.begin() + offset, elements_.begin() + offset + size,
              ref);
    return Result::Ok;
  }
  return Result::Error;
}

Result Table::Init(Store& store,
                   u32 dst_offset,
                   const ElemSegment& src,
                   u32 src_offset,
                   u32 size) {
  if (IsValidRange(dst_offset, size) && src.IsValidRange(src_offset, size) &&
      TypesMatch(type_.element, src.desc().type)) {
    std::copy(src.elements().begin() + src_offset,
              src.elements().begin() + src_offset + size,
              elements_.begin() + dst_offset);
    return Result::Ok;
  }
  return Result::Error;
}

// static
Result Table::Copy(Store& store,
                   Table& dst,
                   u32 dst_offset,
                   const Table& src,
                   u32 src_offset,
                   u32 size) {
  if (dst.IsValidRange(dst_offset, size) &&
      src.IsValidRange(src_offset, size) &&
      TypesMatch(dst.type_.element, src.type_.element)) {
    auto src_begin = src.elements_.begin() + src_offset;
    auto src_end = src_begin + size;
    auto dst_begin = dst.elements_.begin() + dst_offset;
    auto dst_end = dst_begin + size;
    if (dst.self() == src.self() && src_begin < dst_begin) {
      std::move_backward(src_begin, src_end, dst_end);
    } else {
      std::move(src_begin, src_end, dst_begin);
    }
    return Result::Ok;
  }
  return Result::Error;
}

//// Memory ////
Memory::Memory(class Store&, MemoryType type)
    : Extern(skind), type_(type), pages_(type.limits.initial) {
  data_.resize(pages_ * type_.page_size);
}

void Memory::Mark(class Store&) {}

Result Memory::Match(class Store& store,
                     const ImportType& import_type,
                     Trap::Ptr* out_trap) {
  return MatchImpl(store, import_type, type_, out_trap);
}

Result Memory::Grow(u64 count) {
  u64 new_pages;
  if (CanGrow<u64>(type_.limits, pages_, count, &new_pages)) {
    // Grow the limits of the memory too, so that if it is used as an
    // import to another module its new size is honored.
    type_.limits.initial += count;
#if WABT_BIG_ENDIAN
    auto old_size = data_.size();
#endif
    pages_ = new_pages;
    data_.resize(new_pages * type_.page_size);
#if WABT_BIG_ENDIAN
    std::move_backward(data_.begin(), data_.begin() + old_size, data_.end());
    std::fill(data_.begin(), data_.end() - old_size, 0);
#endif
    return Result::Ok;
  }
  return Result::Error;
}

Result Memory::Fill(u64 offset, u8 value, u64 size) {
  if (IsValidAccess(offset, 0, size)) {
#if WABT_BIG_ENDIAN
    std::fill(data_.end() - offset - size, data_.end() - offset, value);
#else
    std::fill(data_.begin() + offset, data_.begin() + offset + size, value);
#endif
    return Result::Ok;
  }
  return Result::Error;
}

Result Memory::Init(u64 dst_offset,
                    const DataSegment& src,
                    u64 src_offset,
                    u64 size) {
  if (IsValidAccess(dst_offset, 0, size) &&
      src.IsValidRange(src_offset, size)) {
    std::copy(src.desc().data.begin() + src_offset,
              src.desc().data.begin() + src_offset + size,
#if WABT_BIG_ENDIAN
              data_.rbegin() + dst_offset);
#else
              data_.begin() + dst_offset);
#endif
    return Result::Ok;
  }
  return Result::Error;
}

// static
Result Memory::Copy(Memory& dst,
                    u64 dst_offset,
                    const Memory& src,
                    u64 src_offset,
                    u64 size) {
  if (dst.IsValidAccess(dst_offset, 0, size) &&
      src.IsValidAccess(src_offset, 0, size)) {
#if WABT_BIG_ENDIAN
    auto src_begin = src.data_.end() - src_offset - size;
    auto dst_begin = dst.data_.end() - dst_offset - size;
#else
    auto src_begin = src.data_.begin() + src_offset;
    auto dst_begin = dst.data_.begin() + dst_offset;
#endif
    auto src_end = src_begin + size;
    auto dst_end = dst_begin + size;
    if (src.self() == dst.self() && src_begin < dst_begin) {
      std::move_backward(src_begin, src_end, dst_end);
    } else {
      std::move(src_begin, src_end, dst_begin);
    }
    return Result::Ok;
  }
  return Result::Error;
}

Result Instance::CallInitFunc(Store& store,
                              const Ref func_ref,
                              Value* result,
                              Trap::Ptr* out_trap) {
  Values results;
  Func::Ptr func{store, func_ref};
  if (Failed(func->Call(store, {}, results, out_trap))) {
    return Result::Error;
  }
  assert(results.size() == 1);
  *result = results[0];
  return Result::Ok;
}

//// Global ////
Global::Global(Store& store, GlobalType type, Value value)
    : Extern(skind), type_(type), value_(value) {}

void Global::Mark(Store& store) {
  if (IsReference(type_.type)) {
    store.Mark(value_.Get<Ref>());
  }
}

Result Global::Match(Store& store,
                     const ImportType& import_type,
                     Trap::Ptr* out_trap) {
  return MatchImpl(store, import_type, type_, out_trap);
}

void Global::Set(Store& store, Ref ref) {
  assert(store.HasValueType(ref, type_.type));
  value_.Set(ref);
}

void Global::UnsafeSet(Value value) {
  value_ = value;
}

//// Tag ////
Tag::Tag(Store&, TagType type) : Extern(skind), type_(type) {}

void Tag::Mark(Store&) {}

Result Tag::Match(Store& store,
                  const ImportType& import_type,
                  Trap::Ptr* out_trap) {
  return MatchImpl(store, import_type, type_, out_trap);
}

//// ElemSegment ////
ElemSegment::ElemSegment(Store& store,
                         const ElemDesc* desc,
                         Instance::Ptr& inst)
    : desc_(desc) {
  Trap::Ptr out_trap;
  elements_.reserve(desc->elements.size());
  for (auto&& elem_expr : desc->elements) {
    Value value;
    Ref func_ref = DefinedFunc::New(store, inst.ref(), elem_expr).ref();
    if (Failed(inst->CallInitFunc(store, func_ref, &value, &out_trap))) {
      WABT_UNREACHABLE;  // valid const expression cannot trap
    }
    elements_.push_back(value.Get<Ref>());
  }
}

bool ElemSegment::IsValidRange(u32 offset, u32 size) const {
  u32 elem_size = this->size();
  return size <= elem_size && offset <= elem_size - size;
}

//// DataSegment ////
DataSegment::DataSegment(const DataDesc* desc)
    : desc_(desc), size_(desc->data.size()) {}

bool DataSegment::IsValidRange(u64 offset, u64 size) const {
  u64 data_size = size_;
  return size <= data_size && offset <= data_size - size;
}

//// Module ////
Module::Module(Store&, ModuleDesc desc)
    : Object(skind), desc_(std::move(desc)) {
  for (auto&& import : desc_.imports) {
    import_types_.emplace_back(import.type);
  }

  for (auto&& export_ : desc_.exports) {
    export_types_.emplace_back(export_.type);
  }
}

void Module::Mark(Store&) {}

//// ElemSegment ////
void ElemSegment::Mark(Store& store) {
  store.Mark(elements_);
}

//// Instance ////
Instance::Instance(Store& store, Ref module) : Object(skind), module_(module) {
  assert(store.Is<Module>(module));
}

// static
Instance::Ptr Instance::Instantiate(Store& store,
                                    Ref module,
                                    const RefVec& imports,
                                    Trap::Ptr* out_trap) {
  Module::Ptr mod{store, module};
  Instance::Ptr inst = store.Alloc<Instance>(store, module);

  size_t import_desc_count = mod->desc().imports.size();
  if (imports.size() < import_desc_count) {
    *out_trap = Trap::New(store, "not enough imports!");
    return {};
  }

  // Imports.
  for (size_t i = 0; i < import_desc_count; ++i) {
    auto&& import_desc = mod->desc().imports[i];
    Ref extern_ref = imports[i];
    if (extern_ref == Ref::Null) {
      *out_trap = Trap::New(store, StringPrintf("invalid import \"%s.%s\"",
                                                import_desc.type.module.c_str(),
                                                import_desc.type.name.c_str()));
      return {};
    }

    Extern::Ptr extern_{store, extern_ref};
    if (Failed(extern_->Match(store, import_desc.type, out_trap))) {
      return {};
    }

    inst->imports_.push_back(extern_ref);

    switch (import_desc.type.type->kind) {
      case ExternKind::Func:   inst->funcs_.push_back(extern_ref); break;
      case ExternKind::Table:  inst->tables_.push_back(extern_ref); break;
      case ExternKind::Memory: inst->memories_.push_back(extern_ref); break;
      case ExternKind::Global: inst->globals_.push_back(extern_ref); break;
      case ExternKind::Tag:    inst->tags_.push_back(extern_ref); break;
    }
  }

  // Funcs.
  for (auto&& desc : mod->desc().funcs) {
    inst->funcs_.push_back(DefinedFunc::New(store, inst.ref(), desc).ref());
  }

  // Tables.
  for (auto&& desc : mod->desc().tables) {
    inst->tables_.push_back(Table::New(store, desc.type).ref());
  }

  // Memories.
  for (auto&& desc : mod->desc().memories) {
    inst->memories_.push_back(Memory::New(store, desc.type).ref());
  }

  // Globals.
  for (auto&& desc : mod->desc().globals) {
    Value value;
    Ref func_ref = DefinedFunc::New(store, inst.ref(), desc.init_func).ref();
    if (Failed(inst->CallInitFunc(store, func_ref, &value, out_trap))) {
      return {};
    }
    inst->globals_.push_back(Global::New(store, desc.type, value).ref());
  }

  // Tags.
  for (auto&& desc : mod->desc().tags) {
    inst->tags_.push_back(Tag::New(store, desc.type).ref());
  }

  // Exports.
  for (auto&& desc : mod->desc().exports) {
    Ref ref;
    switch (desc.type.type->kind) {
      case ExternKind::Func:   ref = inst->funcs_[desc.index]; break;
      case ExternKind::Table:  ref = inst->tables_[desc.index]; break;
      case ExternKind::Memory: ref = inst->memories_[desc.index]; break;
      case ExternKind::Global: ref = inst->globals_[desc.index]; break;
      case ExternKind::Tag:    ref = inst->tags_[desc.index]; break;
    }
    inst->exports_.push_back(ref);
  }

  // Elems.
  for (auto&& desc : mod->desc().elems) {
    inst->elems_.emplace_back(store, &desc, inst);
  }

  // Datas.
  for (auto&& desc : mod->desc().datas) {
    inst->datas_.emplace_back(&desc);
  }

  // Initialization.
  // The MVP requires that all segments are bounds-checked before being copied
  // into the table or memory. The bulk memory proposal changes this behavior;
  // instead, each segment is copied in order. If any segment fails, then no
  // further segments are copied. Any data that was written persists.
  enum Pass { Check, Init };
  int pass = store.features().bulk_memory_enabled() ? Init : Check;
  for (; pass <= Init; ++pass) {
    // Elems.
    for (auto&& segment : inst->elems_) {
      auto&& desc = segment.desc();
      if (desc.mode == SegmentMode::Active) {
        Result result;
        Table::Ptr table{store, inst->tables_[desc.table_index]};
        Value value;
        Ref func_ref =
            DefinedFunc::New(store, inst.ref(), desc.init_func).ref();
        if (Failed(inst->CallInitFunc(store, func_ref, &value, out_trap))) {
          return {};
        }

        u64 offset;
        if (table->type().limits.is_64) {
          offset = value.Get<u64>();
        } else {
          offset = value.Get<u32>();
        }

        if (pass == Check) {
          result = table->IsValidRange(offset, segment.size()) ? Result::Ok
                                                               : Result::Error;
        } else {
          result = table->Init(store, offset, segment, 0, segment.size());
          if (Succeeded(result)) {
            segment.Drop();
          }
        }

        if (Failed(result)) {
          *out_trap = Trap::New(
              store,
              StringPrintf("out of bounds table access: elem segment is "
                           "out of bounds: [%" PRIu64 ", %" PRIu64
                           ") >= max value %u",
                           offset, offset + segment.size(), table->size()));
          return {};
        }
      } else if (desc.mode == SegmentMode::Declared) {
        segment.Drop();
      }
    }

    // Data.
    for (auto&& segment : inst->datas_) {
      auto&& desc = segment.desc();
      if (desc.mode == SegmentMode::Active) {
        Result result;
        Memory::Ptr memory{store, inst->memories_[desc.memory_index]};
        Value offset_op;
        Ref func_ref =
            DefinedFunc::New(store, inst.ref(), desc.init_func).ref();
        if (Failed(inst->CallInitFunc(store, func_ref, &offset_op, out_trap))) {
          return {};
        }
        u64 offset = memory->type().limits.is_64 ? offset_op.Get<u64>()
                                                 : offset_op.Get<u32>();
        if (pass == Check) {
          result = memory->IsValidAccess(offset, 0, segment.size())
                       ? Result::Ok
                       : Result::Error;
        } else {
          result = memory->Init(offset, segment, 0, segment.size());
          if (Succeeded(result)) {
            segment.Drop();
          }
        }

        if (Failed(result)) {
          *out_trap = Trap::New(
              store, StringPrintf(
                         "out of bounds memory access: data segment is "
                         "out of bounds: [%" PRIu64 ", %" PRIu64
                         ") >= max value %" PRIu64,
                         offset, offset + segment.size(), memory->ByteSize()));
          return {};
        }
      } else if (desc.mode == SegmentMode::Declared) {
        segment.Drop();
      }
    }
  }

  // Start.
  for (auto&& start : mod->desc().starts) {
    Func::Ptr func{store, inst->funcs_[start.func_index]};
    Values results;
    if (Failed(func->Call(store, {}, results, out_trap))) {
      return {};
    }
  }

  return inst;
}

void Instance::Mark(Store& store) {
  store.Mark(module_);
  store.Mark(imports_);
  store.Mark(funcs_);
  store.Mark(memories_);
  store.Mark(tables_);
  store.Mark(globals_);
  store.Mark(tags_);
  store.Mark(exports_);
  for (auto&& elem : elems_) {
    elem.Mark(store);
  }
}

//// Thread ////
Thread::Thread(Store& store, Stream* trace_stream)
    : store_(store), trace_stream_(trace_stream) {
  store.threads().insert(this);

  Thread::Options options;
  frames_.reserve(options.call_stack_size);
  values_.reserve(options.value_stack_size);
  if (trace_stream) {
    trace_source_ = std::make_unique<TraceSource>(this);
  }
}

Thread::~Thread() {
  store_.threads().erase(this);
}

void Thread::Mark() {
  for (auto&& frame : frames_) {
    frame.Mark(store_);
  }
  for (auto index : refs_) {
    store_.Mark(values_[index].Get<Ref>());
  }
  store_.Mark(exceptions_);
}

void Thread::PushValues(const ValueTypes& types, const Values& values) {
  assert(types.size() == values.size());
  for (size_t i = 0; i < types.size(); ++i) {
    if (IsReference(types[i])) {
      refs_.push_back(values_.size());
    }
    values_.push_back(values[i]);
  }
}

#define TRAP(msg) *out_trap = Trap::New(store_, (msg), frames_), RunResult::Trap
#define TRAP_IF(cond, msg)     \
  if (WABT_UNLIKELY((cond))) { \
    return TRAP(msg);          \
  }
#define TRAP_UNLESS(cond, msg) TRAP_IF(!(cond), msg)

Instance* Thread::GetCallerInstance() {
  if (frames_.size() < 2)
    return nullptr;
  return frames_[frames_.size() - 2].inst;
}

RunResult Thread::PushCall(Ref func, u32 offset, Trap::Ptr* out_trap) {
  TRAP_IF(frames_.size() == frames_.capacity(), "call stack exhausted");
  frames_.emplace_back(func, values_.size(), exceptions_.size(), offset, inst_,
                       mod_);
  return RunResult::Ok;
}

RunResult Thread::PushCall(const DefinedFunc& func, Trap::Ptr* out_trap) {
  TRAP_IF(frames_.size() == frames_.capacity(), "call stack exhausted");
  inst_ = store_.UnsafeGet<Instance>(func.instance()).get();
  mod_ = store_.UnsafeGet<Module>(inst_->module()).get();
  frames_.emplace_back(func.self(), values_.size(), exceptions_.size(),
                       func.desc().code_offset, inst_, mod_);
  return RunResult::Ok;
}

RunResult Thread::PushCall(const HostFunc& func, Trap::Ptr* out_trap) {
  TRAP_IF(frames_.size() == frames_.capacity(), "call stack exhausted");
  inst_ = nullptr;
  mod_ = nullptr;
  frames_.emplace_back(func.self(), values_.size(), exceptions_.size(), 0,
                       inst_, mod_);
  return RunResult::Ok;
}

RunResult Thread::PopCall() {
  // Sanity check that the exception stack was popped correctly.
  assert(frames_.back().exceptions == exceptions_.size());

  frames_.pop_back();
  if (frames_.empty()) {
    return RunResult::Return;
  }

  auto& frame = frames_.back();
  if (!frame.inst) {
    // Returning to a HostFunc called on this thread.
    return RunResult::Return;
  }

  inst_ = frame.inst;
  mod_ = frame.mod;
  return RunResult::Ok;
}

RunResult Thread::DoReturnCall(const Func::Ptr& func, Trap::Ptr* out_trap) {
  PopCall();
  DoCall(func, out_trap);
  return frames_.empty() ? RunResult::Return : RunResult::Ok;
}

void Thread::PopValues(const ValueTypes& types, Values* out_values) {
  assert(values_.size() >= types.size());
  out_values->resize(types.size());
  std::copy(values_.end() - types.size(), values_.end(), out_values->begin());
  values_.resize(values_.size() - types.size());
}

RunResult Thread::Run(Trap::Ptr* out_trap) {
  const int kDefaultInstructionCount = 1000;
  RunResult result;
  do {
    result = Run(kDefaultInstructionCount, out_trap);
  } while (result == RunResult::Ok);
  return result;
}

RunResult Thread::Run(int num_instructions, Trap::Ptr* out_trap) {
  DefinedFunc::Ptr func{store_, frames_.back().func};
  for (; num_instructions > 0; --num_instructions) {
    auto result = StepInternal(out_trap);
    if (result != RunResult::Ok) {
      return result;
    }
  }
  return RunResult::Ok;
}

RunResult Thread::Step(Trap::Ptr* out_trap) {
  DefinedFunc::Ptr func{store_, frames_.back().func};
  return StepInternal(out_trap);
}

Value& Thread::Pick(Index index) {
  assert(index > 0 && index <= values_.size());
  return values_[values_.size() - index];
}

template <typename T>
T WABT_VECTORCALL Thread::Pop() {
  return Pop().Get<T>();
}

Value Thread::Pop() {
  if (!refs_.empty() && refs_.back() >= values_.size()) {
    refs_.pop_back();
  }
  auto value = values_.back();
  values_.pop_back();
  return value;
}

u64 Thread::PopPtr(const Memory::Ptr& memory) {
  return memory->type().limits.is_64 ? Pop<u64>() : Pop<u32>();
}

u64 Thread::PopPtr(const Table::Ptr& table) {
  return table->type().limits.is_64 ? Pop<u64>() : Pop<u32>();
}

void Thread::PushPtr(const Memory::Ptr& memory, u64 value) {
  if (memory->type().limits.is_64) {
    Push<u64>(value);
  } else {
    Push<u32>(value);
  }
}

void Thread::PushPtr(const Table::Ptr& table, u64 value) {
  if (table->type().limits.is_64) {
    Push<u64>(value);
  } else {
    Push<u32>(value);
  }
}

template <typename T>
void WABT_VECTORCALL Thread::Push(T value) {
  Push(Value::Make(value));
}

template <>
void Thread::Push<bool>(bool value) {
  Push(Value::Make(static_cast<u32>(value ? 1 : 0)));
}

void Thread::Push(Value value) {
  values_.push_back(value);
}

void Thread::Push(Ref ref) {
  refs_.push_back(values_.size());
  values_.push_back(Value::Make(ref));
}

void Thread::SetRef(Index index) {
  assert(index > 0 && index <= values_.size());
  Index slot = values_.size() - index;
  // keep refs_ ordered!
  auto it = std::find_if(refs_.rbegin(), refs_.rend(),
                         [slot](u32 x) { return x <= slot; });
  if (it == refs_.rend() || *it != slot) {
    refs_.insert(it.base(), slot);
  }
}

RunResult Thread::StepInternal(Trap::Ptr* out_trap) {
  using O = Opcode;

  u32& pc = frames_.back().offset;
  auto& istream = mod_->desc().istream;

  if (trace_stream_) {
    istream.Trace(trace_stream_, pc, trace_source_.get());
  }

  // clang-format off
  auto instr = istream.Read(&pc);
  switch (instr.op) {
    case O::Unreachable:
      return TRAP("unreachable executed");

    case O::Br:
      pc = instr.imm_u32;
      break;

    case O::BrIf:
      if (Pop<u32>()) {
        pc = instr.imm_u32;
      }
      break;

    case O::BrTable: {
      auto key = Pop<u32>();
      if (key >= instr.imm_u32) {
        key = instr.imm_u32;
      }
      pc += key * Istream::kBrTableEntrySize;
      break;
    }

    case O::Return:
      return PopCall();

    case O::Call: {
      Ref new_func_ref = inst_->funcs()[instr.imm_u32];
      DefinedFunc::Ptr new_func{store_, new_func_ref};
      if (PushCall(new_func_ref, new_func->desc().code_offset, out_trap) ==
          RunResult::Trap) {
        return RunResult::Trap;
      }
      break;
    }

    case O::CallIndirect:
    case O::ReturnCallIndirect: {
      Table::Ptr table{store_, inst_->tables()[instr.imm_u32x2.fst]};
      auto&& func_type = mod_->desc().func_types[instr.imm_u32x2.snd];
      u64 entry = PopPtr(table);
      TRAP_IF(entry >= table->elements().size(), "undefined table index");
      auto new_func_ref = table->elements()[entry];
      TRAP_IF(new_func_ref == Ref::Null, "uninitialized table element");
      Func::Ptr new_func{store_, new_func_ref};
      TRAP_IF(
          Failed(Match(new_func->type(), func_type, nullptr)),
          "indirect call signature mismatch");  // TODO: don't use "signature"
      if (instr.op == O::ReturnCallIndirect) {
        return DoReturnCall(new_func, out_trap);
      } else {
        return DoCall(new_func, out_trap);
      }
    }

    case O::Drop:
      Pop();
      break;

    case O::Select: {
      auto cond = Pop<u32>();
      auto ref = false;
      // check if either is a ref
      ref |= !refs_.empty() && refs_.back() == values_.size();
      Value false_ = Pop();
      ref |= !refs_.empty() && refs_.back() == values_.size();
      Value true_ = Pop();
      if (ref) {
        refs_.push_back(values_.size());
      }
      Push(cond ? true_ : false_);
      break;
    }

    case O::InterpLocalGetRef:
      refs_.push_back(values_.size());
      [[fallthrough]];
    case O::LocalGet:
      Push(Pick(instr.imm_u32));
      break;

    case O::InterpLocalSetRef:
      if (Pick(instr.imm_u32).Get<Ref>() == Ref::Null) {
        SetRef(instr.imm_u32);
      }
      [[fallthrough]];
    case O::LocalSet: {
      Pick(instr.imm_u32) = Pick(1);
      Pop();
      break;
    }

    case O::InterpLocalTeeRef:
      if (Pick(instr.imm_u32).Get<Ref>() == Ref::Null) {
        SetRef(instr.imm_u32);
      }
      [[fallthrough]];
    case O::LocalTee:
      Pick(instr.imm_u32) = Pick(1);
      break;

    case O::InterpGlobalGetRef:
      refs_.push_back(values_.size());
      [[fallthrough]];
    case O::GlobalGet: {
      Global::Ptr global{store_, inst_->globals()[instr.imm_u32]};
      Push(global->Get());
      break;
    }

    case O::GlobalSet: {
      Global::Ptr global{store_, inst_->globals()[instr.imm_u32]};
      global->UnsafeSet(Pop());
      break;
    }

    case O::I32Load:    return DoLoad<u32>(instr, out_trap);
    case O::I64Load:    return DoLoad<u64>(instr, out_trap);
    case O::F32Load:    return DoLoad<f32>(instr, out_trap);
    case O::F64Load:    return DoLoad<f64>(instr, out_trap);
    case O::I32Load8S:  return DoLoad<s32, s8>(instr, out_trap);
    case O::I32Load8U:  return DoLoad<u32, u8>(instr, out_trap);
    case O::I32Load16S: return DoLoad<s32, s16>(instr, out_trap);
    case O::I32Load16U: return DoLoad<u32, u16>(instr, out_trap);
    case O::I64Load8S:  return DoLoad<s64, s8>(instr, out_trap);
    case O::I64Load8U:  return DoLoad<u64, u8>(instr, out_trap);
    case O::I64Load16S: return DoLoad<s64, s16>(instr, out_trap);
    case O::I64Load16U: return DoLoad<u64, u16>(instr, out_trap);
    case O::I64Load32S: return DoLoad<s64, s32>(instr, out_trap);
    case O::I64Load32U: return DoLoad<u64, u32>(instr, out_trap);

    case O::I32Store:   return DoStore<u32>(instr, out_trap);
    case O::I64Store:   return DoStore<u64>(instr, out_trap);
    case O::F32Store:   return DoStore<f32>(instr, out_trap);
    case O::F64Store:   return DoStore<f64>(instr, out_trap);
    case O::I32Store8:  return DoStore<u32, u8>(instr, out_trap);
    case O::I32Store16: return DoStore<u32, u16>(instr, out_trap);
    case O::I64Store8:  return DoStore<u64, u8>(instr, out_trap);
    case O::I64Store16: return DoStore<u64, u16>(instr, out_trap);
    case O::I64Store32: return DoStore<u64, u32>(instr, out_trap);

    case O::MemorySize: {
      Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32]};
      PushPtr(memory, memory->PageSize());
      break;
    }

    case O::MemoryGrow: {
      Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32]};
      u64 old_size = memory->PageSize();
      if (Failed(memory->Grow(PopPtr(memory)))) {
        PushPtr(memory, -1);
      } else {
        PushPtr(memory, old_size);
      }
      break;
    }

    case O::I32Const: Push(instr.imm_u32); break;
    case O::F32Const: Push(instr.imm_f32); break;
    case O::I64Const: Push(instr.imm_u64); break;
    case O::F64Const: Push(instr.imm_f64); break;

    case O::I32Eqz: return DoUnop(IntEqz<u32>);
    case O::I32Eq:  return DoBinop(Eq<u32>);
    case O::I32Ne:  return DoBinop(Ne<u32>);
    case O::I32LtS: return DoBinop(Lt<s32>);
    case O::I32LtU: return DoBinop(Lt<u32>);
    case O::I32GtS: return DoBinop(Gt<s32>);
    case O::I32GtU: return DoBinop(Gt<u32>);
    case O::I32LeS: return DoBinop(Le<s32>);
    case O::I32LeU: return DoBinop(Le<u32>);
    case O::I32GeS: return DoBinop(Ge<s32>);
    case O::I32GeU: return DoBinop(Ge<u32>);

    case O::I64Eqz: return DoUnop(IntEqz<u64>);
    case O::I64Eq:  return DoBinop(Eq<u64>);
    case O::I64Ne:  return DoBinop(Ne<u64>);
    case O::I64LtS: return DoBinop(Lt<s64>);
    case O::I64LtU: return DoBinop(Lt<u64>);
    case O::I64GtS: return DoBinop(Gt<s64>);
    case O::I64GtU: return DoBinop(Gt<u64>);
    case O::I64LeS: return DoBinop(Le<s64>);
    case O::I64LeU: return DoBinop(Le<u64>);
    case O::I64GeS: return DoBinop(Ge<s64>);
    case O::I64GeU: return DoBinop(Ge<u64>);

    case O::F32Eq:  return DoBinop(Eq<f32>);
    case O::F32Ne:  return DoBinop(Ne<f32>);
    case O::F32Lt:  return DoBinop(Lt<f32>);
    case O::F32Gt:  return DoBinop(Gt<f32>);
    case O::F32Le:  return DoBinop(Le<f32>);
    case O::F32Ge:  return DoBinop(Ge<f32>);

    case O::F64Eq:  return DoBinop(Eq<f64>);
    case O::F64Ne:  return DoBinop(Ne<f64>);
    case O::F64Lt:  return DoBinop(Lt<f64>);
    case O::F64Gt:  return DoBinop(Gt<f64>);
    case O::F64Le:  return DoBinop(Le<f64>);
    case O::F64Ge:  return DoBinop(Ge<f64>);

    case O::I32Clz:    return DoUnop(IntClz<u32>);
    case O::I32Ctz:    return DoUnop(IntCtz<u32>);
    case O::I32Popcnt: return DoUnop(IntPopcnt<u32>);
    case O::I32Add:    return DoBinop(Add<u32>);
    case O::I32Sub:    return DoBinop(Sub<u32>);
    case O::I32Mul:    return DoBinop(Mul<u32>);
    case O::I32DivS:   return DoBinop(IntDiv<s32>, out_trap);
    case O::I32DivU:   return DoBinop(IntDiv<u32>, out_trap);
    case O::I32RemS:   return DoBinop(IntRem<s32>, out_trap);
    case O::I32RemU:   return DoBinop(IntRem<u32>, out_trap);
    case O::I32And:    return DoBinop(IntAnd<u32>);
    case O::I32Or:     return DoBinop(IntOr<u32>);
    case O::I32Xor:    return DoBinop(IntXor<u32>);
    case O::I32Shl:    return DoBinop(IntShl<u32>);
    case O::I32ShrS:   return DoBinop(IntShr<s32>);
    case O::I32ShrU:   return DoBinop(IntShr<u32>);
    case O::I32Rotl:   return DoBinop(IntRotl<u32>);
    case O::I32Rotr:   return DoBinop(IntRotr<u32>);

    case O::I64Clz:    return DoUnop(IntClz<u64>);
    case O::I64Ctz:    return DoUnop(IntCtz<u64>);
    case O::I64Popcnt: return DoUnop(IntPopcnt<u64>);
    case O::I64Add:    return DoBinop(Add<u64>);
    case O::I64Sub:    return DoBinop(Sub<u64>);
    case O::I64Mul:    return DoBinop(Mul<u64>);
    case O::I64DivS:   return DoBinop(IntDiv<s64>, out_trap);
    case O::I64DivU:   return DoBinop(IntDiv<u64>, out_trap);
    case O::I64RemS:   return DoBinop(IntRem<s64>, out_trap);
    case O::I64RemU:   return DoBinop(IntRem<u64>, out_trap);
    case O::I64And:    return DoBinop(IntAnd<u64>);
    case O::I64Or:     return DoBinop(IntOr<u64>);
    case O::I64Xor:    return DoBinop(IntXor<u64>);
    case O::I64Shl:    return DoBinop(IntShl<u64>);
    case O::I64ShrS:   return DoBinop(IntShr<s64>);
    case O::I64ShrU:   return DoBinop(IntShr<u64>);
    case O::I64Rotl:   return DoBinop(IntRotl<u64>);
    case O::I64Rotr:   return DoBinop(IntRotr<u64>);

    case O::F32Abs:     return DoUnop(FloatAbs<f32>);
    case O::F32Neg:     return DoUnop(FloatNeg<f32>);
    case O::F32Ceil:    return DoUnop(FloatCeil<f32>);
    case O::F32Floor:   return DoUnop(FloatFloor<f32>);
    case O::F32Trunc:   return DoUnop(FloatTrunc<f32>);
    case O::F32Nearest: return DoUnop(FloatNearest<f32>);
    case O::F32Sqrt:    return DoUnop(FloatSqrt<f32>);
    case O::F32Add:      return DoBinop(Add<f32>);
    case O::F32Sub:      return DoBinop(Sub<f32>);
    case O::F32Mul:      return DoBinop(Mul<f32>);
    case O::F32Div:      return DoBinop(FloatDiv<f32>);
    case O::F32Min:      return DoBinop(FloatMin<f32>);
    case O::F32Max:      return DoBinop(FloatMax<f32>);
    case O::F32Copysign: return DoBinop(FloatCopysign<f32>);

    case O::F64Abs:     return DoUnop(FloatAbs<f64>);
    case O::F64Neg:     return DoUnop(FloatNeg<f64>);
    case O::F64Ceil:    return DoUnop(FloatCeil<f64>);
    case O::F64Floor:   return DoUnop(FloatFloor<f64>);
    case O::F64Trunc:   return DoUnop(FloatTrunc<f64>);
    case O::F64Nearest: return DoUnop(FloatNearest<f64>);
    case O::F64Sqrt:    return DoUnop(FloatSqrt<f64>);
    case O::F64Add:      return DoBinop(Add<f64>);
    case O::F64Sub:      return DoBinop(Sub<f64>);
    case O::F64Mul:      return DoBinop(Mul<f64>);
    case O::F64Div:      return DoBinop(FloatDiv<f64>);
    case O::F64Min:      return DoBinop(FloatMin<f64>);
    case O::F64Max:      return DoBinop(FloatMax<f64>);
    case O::F64Copysign: return DoBinop(FloatCopysign<f64>);

    case O::I32WrapI64:      return DoConvert<u32, u64>(out_trap);
    case O::I32TruncF32S:    return DoConvert<s32, f32>(out_trap);
    case O::I32TruncF32U:    return DoConvert<u32, f32>(out_trap);
    case O::I32TruncF64S:    return DoConvert<s32, f64>(out_trap);
    case O::I32TruncF64U:    return DoConvert<u32, f64>(out_trap);
    case O::I64ExtendI32S:   return DoConvert<s64, s32>(out_trap);
    case O::I64ExtendI32U:   return DoConvert<u64, u32>(out_trap);
    case O::I64TruncF32S:    return DoConvert<s64, f32>(out_trap);
    case O::I64TruncF32U:    return DoConvert<u64, f32>(out_trap);
    case O::I64TruncF64S:    return DoConvert<s64, f64>(out_trap);
    case O::I64TruncF64U:    return DoConvert<u64, f64>(out_trap);
    case O::F32ConvertI32S:  return DoConvert<f32, s32>(out_trap);
    case O::F32ConvertI32U:  return DoConvert<f32, u32>(out_trap);
    case O::F32ConvertI64S:  return DoConvert<f32, s64>(out_trap);
    case O::F32ConvertI64U:  return DoConvert<f32, u64>(out_trap);
    case O::F32DemoteF64:    return DoConvert<f32, f64>(out_trap);
    case O::F64ConvertI32S:  return DoConvert<f64, s32>(out_trap);
    case O::F64ConvertI32U:  return DoConvert<f64, u32>(out_trap);
    case O::F64ConvertI64S:  return DoConvert<f64, s64>(out_trap);
    case O::F64ConvertI64U:  return DoConvert<f64, u64>(out_trap);
    case O::F64PromoteF32:   return DoConvert<f64, f32>(out_trap);

    case O::I32ReinterpretF32: return DoReinterpret<u32, f32>();
    case O::F32ReinterpretI32: return DoReinterpret<f32, u32>();
    case O::I64ReinterpretF64: return DoReinterpret<u64, f64>();
    case O::F64ReinterpretI64: return DoReinterpret<f64, u64>();

    case O::I32Extend8S:   return DoUnop(IntExtend<u32, 7>);
    case O::I32Extend16S:  return DoUnop(IntExtend<u32, 15>);
    case O::I64Extend8S:   return DoUnop(IntExtend<u64, 7>);
    case O::I64Extend16S:  return DoUnop(IntExtend<u64, 15>);
    case O::I64Extend32S:  return DoUnop(IntExtend<u64, 31>);

    case O::InterpAlloca:
      values_.resize(values_.size() + instr.imm_u32);
      // refs_ doesn't need to be updated; We may be allocating space for
      // references, but they will be initialized to null, so it is OK if we
      // don't mark them.
      break;

    case O::InterpBrUnless:
      if (!Pop<u32>()) {
        pc = instr.imm_u32;
      }
      break;

    case O::InterpCallImport: {
      Ref new_func_ref = inst_->funcs()[instr.imm_u32];
      Func::Ptr new_func{store_, new_func_ref};
      return DoCall(new_func, out_trap);
    }

    case O::InterpDropKeep: {
      auto drop = instr.imm_u32x2.fst;
      auto keep = instr.imm_u32x2.snd;
      // Shift kept refs down.
      for (auto iter = refs_.rbegin(); iter != refs_.rend(); ++iter) {
        if (*iter >= values_.size() - keep) {
          *iter -= drop;
        } else {
          break;
        }
      }
      std::move(values_.end() - keep, values_.end(),
                values_.end() - drop - keep);
      values_.resize(values_.size() - drop);
      break;
    }

    case O::InterpCatchDrop: {
      auto drop = instr.imm_u32;
      for (u32 i = 0; i < drop; i++) {
        exceptions_.pop_back();
      }
      break;
    }

    // This operation adjusts the function reference of the reused frame
    // after a return_call. This ensures the correct exception handlers are
    // used for the call.
    case O::InterpAdjustFrameForReturnCall: {
      Ref new_func_ref = inst_->funcs()[instr.imm_u32];
      Frame& current_frame = frames_.back();
      current_frame.func = new_func_ref;
      break;
    }

    case O::I32TruncSatF32S: return DoUnop(IntTruncSat<s32, f32>);
    case O::I32TruncSatF32U: return DoUnop(IntTruncSat<u32, f32>);
    case O::I32TruncSatF64S: return DoUnop(IntTruncSat<s32, f64>);
    case O::I32TruncSatF64U: return DoUnop(IntTruncSat<u32, f64>);
    case O::I64TruncSatF32S: return DoUnop(IntTruncSat<s64, f32>);
    case O::I64TruncSatF32U: return DoUnop(IntTruncSat<u64, f32>);
    case O::I64TruncSatF64S: return DoUnop(IntTruncSat<s64, f64>);
    case O::I64TruncSatF64U: return DoUnop(IntTruncSat<u64, f64>);

    case O::MemoryInit: return DoMemoryInit(instr, out_trap);
    case O::DataDrop:   return DoDataDrop(instr);
    case O::MemoryCopy: return DoMemoryCopy(instr, out_trap);
    case O::MemoryFill: return DoMemoryFill(instr, out_trap);

    case O::TableInit: return DoTableInit(instr, out_trap);
    case O::ElemDrop:  return DoElemDrop(instr);
    case O::TableCopy: return DoTableCopy(instr, out_trap);
    case O::TableGet:  return DoTableGet(instr, out_trap);
    case O::TableSet:  return DoTableSet(instr, out_trap);
    case O::TableGrow: return DoTableGrow(instr, out_trap);
    case O::TableSize: return DoTableSize(instr);
    case O::TableFill: return DoTableFill(instr, out_trap);

    case O::RefNull:
      Push(Ref::Null);
      break;

    case O::RefIsNull:
      Push(Pop<Ref>() == Ref::Null);
      break;

    case O::RefFunc:
      Push(inst_->funcs()[instr.imm_u32]);
      break;

    case O::V128Load: return DoLoad<v128>(instr, out_trap);
    case O::V128Store: return DoStore<v128>(instr, out_trap);

    case O::V128Const:
      Push<v128>(instr.imm_v128);
      break;

    case O::I8X16Splat:        return DoSimdSplat<u8x16, u32>();
    case O::I8X16ExtractLaneS: return DoSimdExtract<s8x16, s32>(instr);
    case O::I8X16ExtractLaneU: return DoSimdExtract<u8x16, u32>(instr);
    case O::I8X16ReplaceLane:  return DoSimdReplace<u8x16, u32>(instr);
    case O::I16X8Splat:        return DoSimdSplat<u16x8, u32>();
    case O::I16X8ExtractLaneS: return DoSimdExtract<s16x8, s32>(instr);
    case O::I16X8ExtractLaneU: return DoSimdExtract<u16x8, u32>(instr);
    case O::I16X8ReplaceLane:  return DoSimdReplace<u16x8, u32>(instr);
    case O::I32X4Splat:        return DoSimdSplat<u32x4, u32>();
    case O::I32X4ExtractLane:  return DoSimdExtract<s32x4, u32>(instr);
    case O::I32X4ReplaceLane:  return DoSimdReplace<u32x4, u32>(instr);
    case O::I64X2Splat:        return DoSimdSplat<u64x2, u64>();
    case O::I64X2ExtractLane:  return DoSimdExtract<u64x2, u64>(instr);
    case O::I64X2ReplaceLane:  return DoSimdReplace<u64x2, u64>(instr);
    case O::F32X4Splat:        return DoSimdSplat<f32x4, f32>();
    case O::F32X4ExtractLane:  return DoSimdExtract<f32x4, f32>(instr);
    case O::F32X4ReplaceLane:  return DoSimdReplace<f32x4, f32>(instr);
    case O::F64X2Splat:        return DoSimdSplat<f64x2, f64>();
    case O::F64X2ExtractLane:  return DoSimdExtract<f64x2, f64>(instr);
    case O::F64X2ReplaceLane:  return DoSimdReplace<f64x2, f64>(instr);

    case O::I8X16Eq:  return DoSimdBinop(EqMask<u8>);
    case O::I8X16Ne:  return DoSimdBinop(NeMask<u8>);
    case O::I8X16LtS: return DoSimdBinop(LtMask<s8>);
    case O::I8X16LtU: return DoSimdBinop(LtMask<u8>);
    case O::I8X16GtS: return DoSimdBinop(GtMask<s8>);
    case O::I8X16GtU: return DoSimdBinop(GtMask<u8>);
    case O::I8X16LeS: return DoSimdBinop(LeMask<s8>);
    case O::I8X16LeU: return DoSimdBinop(LeMask<u8>);
    case O::I8X16GeS: return DoSimdBinop(GeMask<s8>);
    case O::I8X16GeU: return DoSimdBinop(GeMask<u8>);
    case O::I16X8Eq:  return DoSimdBinop(EqMask<u16>);
    case O::I16X8Ne:  return DoSimdBinop(NeMask<u16>);
    case O::I16X8LtS: return DoSimdBinop(LtMask<s16>);
    case O::I16X8LtU: return DoSimdBinop(LtMask<u16>);
    case O::I16X8GtS: return DoSimdBinop(GtMask<s16>);
    case O::I16X8GtU: return DoSimdBinop(GtMask<u16>);
    case O::I16X8LeS: return DoSimdBinop(LeMask<s16>);
    case O::I16X8LeU: return DoSimdBinop(LeMask<u16>);
    case O::I16X8GeS: return DoSimdBinop(GeMask<s16>);
    case O::I16X8GeU: return DoSimdBinop(GeMask<u16>);
    case O::I32X4Eq:  return DoSimdBinop(EqMask<u32>);
    case O::I32X4Ne:  return DoSimdBinop(NeMask<u32>);
    case O::I32X4LtS: return DoSimdBinop(LtMask<s32>);
    case O::I32X4LtU: return DoSimdBinop(LtMask<u32>);
    case O::I32X4GtS: return DoSimdBinop(GtMask<s32>);
    case O::I32X4GtU: return DoSimdBinop(GtMask<u32>);
    case O::I32X4LeS: return DoSimdBinop(LeMask<s32>);
    case O::I32X4LeU: return DoSimdBinop(LeMask<u32>);
    case O::I32X4GeS: return DoSimdBinop(GeMask<s32>);
    case O::I32X4GeU: return DoSimdBinop(GeMask<u32>);
    case O::I64X2Eq:  return DoSimdBinop(EqMask<u64>);
    case O::I64X2Ne:  return DoSimdBinop(NeMask<u64>);
    case O::I64X2LtS: return DoSimdBinop(LtMask<s64>);
    case O::I64X2GtS: return DoSimdBinop(GtMask<s64>);
    case O::I64X2LeS: return DoSimdBinop(LeMask<s64>);
    case O::I64X2GeS: return DoSimdBinop(GeMask<s64>);
    case O::F32X4Eq:  return DoSimdBinop(EqMask<f32>);
    case O::F32X4Ne:  return DoSimdBinop(NeMask<f32>);
    case O::F32X4Lt:  return DoSimdBinop(LtMask<f32>);
    case O::F32X4Gt:  return DoSimdBinop(GtMask<f32>);
    case O::F32X4Le:  return DoSimdBinop(LeMask<f32>);
    case O::F32X4Ge:  return DoSimdBinop(GeMask<f32>);
    case O::F64X2Eq:  return DoSimdBinop(EqMask<f64>);
    case O::F64X2Ne:  return DoSimdBinop(NeMask<f64>);
    case O::F64X2Lt:  return DoSimdBinop(LtMask<f64>);
    case O::F64X2Gt:  return DoSimdBinop(GtMask<f64>);
    case O::F64X2Le:  return DoSimdBinop(LeMask<f64>);
    case O::F64X2Ge:  return DoSimdBinop(GeMask<f64>);

    case O::V128Not:       return DoSimdUnop(IntNot<u64>);
    case O::V128And:       return DoSimdBinop(IntAnd<u64>);
    case O::V128Or:        return DoSimdBinop(IntOr<u64>);
    case O::V128Xor:       return DoSimdBinop(IntXor<u64>);
    case O::V128AnyTrue:      return DoSimdIsTrue<u8x16, 1>();

    case O::V128BitSelect:
    case O::I8X16RelaxedLaneSelect:
    case O::I16X8RelaxedLaneSelect:
    case O::I32X4RelaxedLaneSelect:
    case O::I64X2RelaxedLaneSelect:
      return DoSimdBitSelect();

    case O::I8X16Neg:          return DoSimdUnop(IntNeg<u8>);
    case O::I8X16Bitmask:      return DoSimdBitmask<s8x16>();
    case O::I8X16AllTrue:      return DoSimdIsTrue<u8x16, 16>();
    case O::I8X16Shl:          return DoSimdShift(IntShl<u8>);
    case O::I8X16ShrS:         return DoSimdShift(IntShr<s8>);
    case O::I8X16ShrU:         return DoSimdShift(IntShr<u8>);
    case O::I8X16Add:          return DoSimdBinop(Add<u8>);
    case O::I8X16AddSatS:      return DoSimdBinop(IntAddSat<s8>);
    case O::I8X16AddSatU:      return DoSimdBinop(IntAddSat<u8>);
    case O::I8X16Sub:          return DoSimdBinop(Sub<u8>);
    case O::I8X16SubSatS:      return DoSimdBinop(IntSubSat<s8>);
    case O::I8X16SubSatU:      return DoSimdBinop(IntSubSat<u8>);
    case O::I8X16MinS:         return DoSimdBinop(IntMin<s8>);
    case O::I8X16MinU:         return DoSimdBinop(IntMin<u8>);
    case O::I8X16MaxS:         return DoSimdBinop(IntMax<s8>);
    case O::I8X16MaxU:         return DoSimdBinop(IntMax<u8>);

    case O::I16X8Neg:          return DoSimdUnop(IntNeg<u16>);
    case O::I16X8Bitmask:      return DoSimdBitmask<s16x8>();
    case O::I16X8AllTrue:      return DoSimdIsTrue<u16x8, 8>();
    case O::I16X8Shl:          return DoSimdShift(IntShl<u16>);
    case O::I16X8ShrS:         return DoSimdShift(IntShr<s16>);
    case O::I16X8ShrU:         return DoSimdShift(IntShr<u16>);
    case O::I16X8Add:          return DoSimdBinop(Add<u16>);
    case O::I16X8AddSatS:      return DoSimdBinop(IntAddSat<s16>);
    case O::I16X8AddSatU:      return DoSimdBinop(IntAddSat<u16>);
    case O::I16X8Sub:          return DoSimdBinop(Sub<u16>);
    case O::I16X8SubSatS:      return DoSimdBinop(IntSubSat<s16>);
    case O::I16X8SubSatU:      return DoSimdBinop(IntSubSat<u16>);
    case O::I16X8Mul:          return DoSimdBinop(Mul<u16>);
    case O::I16X8MinS:         return DoSimdBinop(IntMin<s16>);
    case O::I16X8MinU:         return DoSimdBinop(IntMin<u16>);
    case O::I16X8MaxS:         return DoSimdBinop(IntMax<s16>);
    case O::I16X8MaxU:         return DoSimdBinop(IntMax<u16>);

    case O::I32X4Neg:          return DoSimdUnop(IntNeg<u32>);
    case O::I32X4Bitmask:      return DoSimdBitmask<s32x4>();
    case O::I32X4AllTrue:      return DoSimdIsTrue<u32x4, 4>();
    case O::I32X4Shl:          return DoSimdShift(IntShl<u32>);
    case O::I32X4ShrS:         return DoSimdShift(IntShr<s32>);
    case O::I32X4ShrU:         return DoSimdShift(IntShr<u32>);
    case O::I32X4Add:          return DoSimdBinop(Add<u32>);
    case O::I32X4Sub:          return DoSimdBinop(Sub<u32>);
    case O::I32X4Mul:          return DoSimdBinop(Mul<u32>);
    case O::I32X4MinS:         return DoSimdBinop(IntMin<s32>);
    case O::I32X4MinU:         return DoSimdBinop(IntMin<u32>);
    case O::I32X4MaxS:         return DoSimdBinop(IntMax<s32>);
    case O::I32X4MaxU:         return DoSimdBinop(IntMax<u32>);

    case O::I64X2Neg:          return DoSimdUnop(IntNeg<u64>);
    case O::I64X2Bitmask:      return DoSimdBitmask<s64x2>();
    case O::I64X2AllTrue:      return DoSimdIsTrue<u64x2, 2>();
    case O::I64X2Shl:          return DoSimdShift(IntShl<u64>);
    case O::I64X2ShrS:         return DoSimdShift(IntShr<s64>);
    case O::I64X2ShrU:         return DoSimdShift(IntShr<u64>);
    case O::I64X2Add:          return DoSimdBinop(Add<u64>);
    case O::I64X2Sub:          return DoSimdBinop(Sub<u64>);
    case O::I64X2Mul:          return DoSimdBinop(Mul<u64>);

    case O::F32X4Ceil:         return DoSimdUnop(FloatCeil<f32>);
    case O::F32X4Floor:        return DoSimdUnop(FloatFloor<f32>);
    case O::F32X4Trunc:        return DoSimdUnop(FloatTrunc<f32>);
    case O::F32X4Nearest:      return DoSimdUnop(FloatNearest<f32>);

    case O::F64X2Ceil:         return DoSimdUnop(FloatCeil<f64>);
    case O::F64X2Floor:        return DoSimdUnop(FloatFloor<f64>);
    case O::F64X2Trunc:        return DoSimdUnop(FloatTrunc<f64>);
    case O::F64X2Nearest:      return DoSimdUnop(FloatNearest<f64>);

    case O::F32X4Abs:          return DoSimdUnop(FloatAbs<f32>);
    case O::F32X4Neg:          return DoSimdUnop(FloatNeg<f32>);
    case O::F32X4Sqrt:         return DoSimdUnop(FloatSqrt<f32>);
    case O::F32X4Add:          return DoSimdBinop(Add<f32>);
    case O::F32X4Sub:          return DoSimdBinop(Sub<f32>);
    case O::F32X4Mul:          return DoSimdBinop(Mul<f32>);
    case O::F32X4Div:          return DoSimdBinop(FloatDiv<f32>);
    case O::F32X4PMin:         return DoSimdBinop(FloatPMin<f32>);
    case O::F32X4PMax:         return DoSimdBinop(FloatPMax<f32>);

    case O::F32X4Min:
    case O::F32X4RelaxedMin:
      return DoSimdBinop(FloatMin<f32>);

    case O::F32X4Max:
    case O::F32X4RelaxedMax:
      return DoSimdBinop(FloatMax<f32>);

    case O::F64X2Abs:          return DoSimdUnop(FloatAbs<f64>);
    case O::F64X2Neg:          return DoSimdUnop(FloatNeg<f64>);
    case O::F64X2Sqrt:         return DoSimdUnop(FloatSqrt<f64>);
    case O::F64X2Add:          return DoSimdBinop(Add<f64>);
    case O::F64X2Sub:          return DoSimdBinop(Sub<f64>);
    case O::F64X2Mul:          return DoSimdBinop(Mul<f64>);
    case O::F64X2Div:          return DoSimdBinop(FloatDiv<f64>);
    case O::F64X2PMin:         return DoSimdBinop(FloatPMin<f64>);
    case O::F64X2PMax:         return DoSimdBinop(FloatPMax<f64>);

    case O::F64X2Min:
    case O::F64X2RelaxedMin:
      return DoSimdBinop(FloatMin<f64>);

    case O::F64X2Max:
    case O::F64X2RelaxedMax:
      return DoSimdBinop(FloatMax<f64>);

    case O::I32X4TruncSatF32X4S:
    case O::I32X4RelaxedTruncF32X4S:
      return DoSimdUnop(IntTruncSat<s32, f32>);

    case O::I32X4TruncSatF32X4U:
    case O::I32X4RelaxedTruncF32X4U:
      return DoSimdUnop(IntTruncSat<u32, f32>);

    case O::I32X4TruncSatF64X2SZero:
    case O::I32X4RelaxedTruncF64X2SZero:
      return DoSimdUnopZero(IntTruncSat<s32, f64>);

    case O::I32X4TruncSatF64X2UZero:
    case O::I32X4RelaxedTruncF64X2UZero:
      return DoSimdUnopZero(IntTruncSat<u32, f64>);

    case O::F32X4ConvertI32X4S:  return DoSimdUnop(Convert<f32, s32>);
    case O::F32X4ConvertI32X4U:  return DoSimdUnop(Convert<f32, u32>);
    case O::F32X4DemoteF64X2Zero: return DoSimdUnopZero(Convert<f32, f64>);
    case O::F64X2PromoteLowF32X4: return DoSimdConvert<f64x2, f32x4, true>();
    case O::F64X2ConvertLowI32X4S: return DoSimdConvert<f64x2, s32x4, true>();
    case O::F64X2ConvertLowI32X4U: return DoSimdConvert<f64x2, u32x4, true>();

    case O::I8X16Swizzle:
    case O::I8X16RelaxedSwizzle:
      return DoSimdSwizzle();

    case O::I8X16Shuffle:     return DoSimdShuffle(instr);

    case O::V128Load8Splat:    return DoSimdLoadSplat<u8x16>(instr, out_trap);
    case O::V128Load16Splat:   return DoSimdLoadSplat<u16x8>(instr, out_trap);
    case O::V128Load32Splat:   return DoSimdLoadSplat<u32x4>(instr, out_trap);
    case O::V128Load64Splat:   return DoSimdLoadSplat<u64x2>(instr, out_trap);

    case O::V128Load8Lane:    return DoSimdLoadLane<u8x16>(instr, out_trap);
    case O::V128Load16Lane:   return DoSimdLoadLane<u16x8>(instr, out_trap);
    case O::V128Load32Lane:   return DoSimdLoadLane<u32x4>(instr, out_trap);
    case O::V128Load64Lane:   return DoSimdLoadLane<u64x2>(instr, out_trap);

    case O::V128Store8Lane:    return DoSimdStoreLane<u8x16>(instr, out_trap);
    case O::V128Store16Lane:   return DoSimdStoreLane<u16x8>(instr, out_trap);
    case O::V128Store32Lane:   return DoSimdStoreLane<u32x4>(instr, out_trap);
    case O::V128Store64Lane:   return DoSimdStoreLane<u64x2>(instr, out_trap);

    case O::V128Load32Zero: return DoSimdLoadZero<u32x4, u32>(instr, out_trap);
    case O::V128Load64Zero: return DoSimdLoadZero<u64x2, u64>(instr, out_trap);

    case O::I8X16NarrowI16X8S:    return DoSimdNarrow<s8x16, s16x8>();
    case O::I8X16NarrowI16X8U:    return DoSimdNarrow<u8x16, s16x8>();
    case O::I16X8NarrowI32X4S:    return DoSimdNarrow<s16x8, s32x4>();
    case O::I16X8NarrowI32X4U:    return DoSimdNarrow<u16x8, s32x4>();
    case O::I16X8ExtendLowI8X16S:  return DoSimdConvert<s16x8, s8x16, true>();
    case O::I16X8ExtendHighI8X16S: return DoSimdConvert<s16x8, s8x16, false>();
    case O::I16X8ExtendLowI8X16U:  return DoSimdConvert<u16x8, u8x16, true>();
    case O::I16X8ExtendHighI8X16U: return DoSimdConvert<u16x8, u8x16, false>();
    case O::I32X4ExtendLowI16X8S:  return DoSimdConvert<s32x4, s16x8, true>();
    case O::I32X4ExtendHighI16X8S: return DoSimdConvert<s32x4, s16x8, false>();
    case O::I32X4ExtendLowI16X8U:  return DoSimdConvert<u32x4, u16x8, true>();
    case O::I32X4ExtendHighI16X8U: return DoSimdConvert<u32x4, u16x8, false>();
    case O::I64X2ExtendLowI32X4S:  return DoSimdConvert<s64x2, s32x4, true>();
    case O::I64X2ExtendHighI32X4S: return DoSimdConvert<s64x2, s32x4, false>();
    case O::I64X2ExtendLowI32X4U:  return DoSimdConvert<u64x2, u32x4, true>();
    case O::I64X2ExtendHighI32X4U: return DoSimdConvert<u64x2, u32x4, false>();

    case O::V128Load8X8S:  return DoSimdLoadExtend<s16x8, s8x8>(instr, out_trap);
    case O::V128Load8X8U:  return DoSimdLoadExtend<u16x8, u8x8>(instr, out_trap);
    case O::V128Load16X4S: return DoSimdLoadExtend<s32x4, s16x4>(instr, out_trap);
    case O::V128Load16X4U: return DoSimdLoadExtend<u32x4, u16x4>(instr, out_trap);
    case O::V128Load32X2S: return DoSimdLoadExtend<s64x2, s32x2>(instr, out_trap);
    case O::V128Load32X2U: return DoSimdLoadExtend<u64x2, u32x2>(instr, out_trap);

    case O::V128Andnot: return DoSimdBinop(IntAndNot<u64>);
    case O::I8X16AvgrU: return DoSimdBinop(IntAvgr<u8>);
    case O::I16X8AvgrU: return DoSimdBinop(IntAvgr<u16>);

    case O::I8X16Abs: return DoSimdUnop(IntAbs<u8>);
    case O::I16X8Abs: return DoSimdUnop(IntAbs<u16>);
    case O::I32X4Abs: return DoSimdUnop(IntAbs<u32>);
    case O::I64X2Abs: return DoSimdUnop(IntAbs<u64>);

    case O::I8X16Popcnt: return DoSimdUnop(IntPopcnt<u8>);

    case O::I16X8ExtaddPairwiseI8X16S: return DoSimdExtaddPairwise<s16x8, s8x16>();
    case O::I16X8ExtaddPairwiseI8X16U: return DoSimdExtaddPairwise<u16x8, u8x16>();
    case O::I32X4ExtaddPairwiseI16X8S: return DoSimdExtaddPairwise<s32x4, s16x8>();
    case O::I32X4ExtaddPairwiseI16X8U: return DoSimdExtaddPairwise<u32x4, u16x8>();

    case O::I16X8ExtmulLowI8X16S: return DoSimdExtmul<s16x8, s8x16, true>();
    case O::I16X8ExtmulHighI8X16S: return DoSimdExtmul<s16x8, s8x16, false>();
    case O::I16X8ExtmulLowI8X16U: return DoSimdExtmul<u16x8, u8x16, true>();
    case O::I16X8ExtmulHighI8X16U: return DoSimdExtmul<u16x8, u8x16, false>();
    case O::I32X4ExtmulLowI16X8S: return DoSimdExtmul<s32x4, s16x8, true>();
    case O::I32X4ExtmulHighI16X8S: return DoSimdExtmul<s32x4, s16x8, false>();
    case O::I32X4ExtmulLowI16X8U: return DoSimdExtmul<u32x4, u16x8, true>();
    case O::I32X4ExtmulHighI16X8U: return DoSimdExtmul<u32x4, u16x8, false>();
    case O::I64X2ExtmulLowI32X4S: return DoSimdExtmul<s64x2, s32x4, true>();
    case O::I64X2ExtmulHighI32X4S: return DoSimdExtmul<s64x2, s32x4, false>();
    case O::I64X2ExtmulLowI32X4U: return DoSimdExtmul<u64x2, u32x4, true>();
    case O::I64X2ExtmulHighI32X4U: return DoSimdExtmul<u64x2, u32x4, false>();

    case O::I16X8Q15mulrSatS:
    case O::I16X8RelaxedQ15mulrS:
      return DoSimdBinop(SaturatingRoundingQMul<s16>);

    case O::I32X4DotI16X8S: return DoSimdDot<u32x4, s16x8>();
    case O::I16X8DotI8X16I7X16S: return DoSimdDot<u16x8, s8x16>();
    case O::I32X4DotI8X16I7X16AddS: return DoSimdDotAdd<u32x4, s16x8>();

    case O::F32X4RelaxedMadd: return DoSimdRelaxedMadd<f32>();
    case O::F32X4RelaxedNmadd: return DoSimdRelaxedNmadd<f32>();
    case O::F64X2RelaxedMadd: return DoSimdRelaxedMadd<f64>();
    case O::F64X2RelaxedNmadd: return DoSimdRelaxedNmadd<f64>();

    case O::AtomicFence:
    case O::MemoryAtomicNotify:
    case O::MemoryAtomicWait32:
    case O::MemoryAtomicWait64:
      return TRAP("not implemented");

    case O::I32AtomicLoad:       return DoAtomicLoad<u32>(instr, out_trap);
    case O::I64AtomicLoad:       return DoAtomicLoad<u64>(instr, out_trap);
    case O::I32AtomicLoad8U:     return DoAtomicLoad<u32, u8>(instr, out_trap);
    case O::I32AtomicLoad16U:    return DoAtomicLoad<u32, u16>(instr, out_trap);
    case O::I64AtomicLoad8U:     return DoAtomicLoad<u64, u8>(instr, out_trap);
    case O::I64AtomicLoad16U:    return DoAtomicLoad<u64, u16>(instr, out_trap);
    case O::I64AtomicLoad32U:    return DoAtomicLoad<u64, u32>(instr, out_trap);
    case O::I32AtomicStore:      return DoAtomicStore<u32>(instr, out_trap);
    case O::I64AtomicStore:      return DoAtomicStore<u64>(instr, out_trap);
    case O::I32AtomicStore8:     return DoAtomicStore<u32, u8>(instr, out_trap);
    case O::I32AtomicStore16:    return DoAtomicStore<u32, u16>(instr, out_trap);
    case O::I64AtomicStore8:     return DoAtomicStore<u64, u8>(instr, out_trap);
    case O::I64AtomicStore16:    return DoAtomicStore<u64, u16>(instr, out_trap);
    case O::I64AtomicStore32:    return DoAtomicStore<u64, u32>(instr, out_trap);
    case O::I32AtomicRmwAdd:     return DoAtomicRmw<u32>(Add<u32>, instr, out_trap);
    case O::I64AtomicRmwAdd:     return DoAtomicRmw<u64>(Add<u64>, instr, out_trap);
    case O::I32AtomicRmw8AddU:   return DoAtomicRmw<u32>(Add<u8>, instr, out_trap);
    case O::I32AtomicRmw16AddU:  return DoAtomicRmw<u32>(Add<u16>, instr, out_trap);
    case O::I64AtomicRmw8AddU:   return DoAtomicRmw<u64>(Add<u8>, instr, out_trap);
    case O::I64AtomicRmw16AddU:  return DoAtomicRmw<u64>(Add<u16>, instr, out_trap);
    case O::I64AtomicRmw32AddU:  return DoAtomicRmw<u64>(Add<u32>, instr, out_trap);
    case O::I32AtomicRmwSub:     return DoAtomicRmw<u32>(Sub<u32>, instr, out_trap);
    case O::I64AtomicRmwSub:     return DoAtomicRmw<u64>(Sub<u64>, instr, out_trap);
    case O::I32AtomicRmw8SubU:   return DoAtomicRmw<u32>(Sub<u8>, instr, out_trap);
    case O::I32AtomicRmw16SubU:  return DoAtomicRmw<u32>(Sub<u16>, instr, out_trap);
    case O::I64AtomicRmw8SubU:   return DoAtomicRmw<u64>(Sub<u8>, instr, out_trap);
    case O::I64AtomicRmw16SubU:  return DoAtomicRmw<u64>(Sub<u16>, instr, out_trap);
    case O::I64AtomicRmw32SubU:  return DoAtomicRmw<u64>(Sub<u32>, instr, out_trap);
    case O::I32AtomicRmwAnd:     return DoAtomicRmw<u32>(IntAnd<u32>, instr, out_trap);
    case O::I64AtomicRmwAnd:     return DoAtomicRmw<u64>(IntAnd<u64>, instr, out_trap);
    case O::I32AtomicRmw8AndU:   return DoAtomicRmw<u32>(IntAnd<u8>, instr, out_trap);
    case O::I32AtomicRmw16AndU:  return DoAtomicRmw<u32>(IntAnd<u16>, instr, out_trap);
    case O::I64AtomicRmw8AndU:   return DoAtomicRmw<u64>(IntAnd<u8>, instr, out_trap);
    case O::I64AtomicRmw16AndU:  return DoAtomicRmw<u64>(IntAnd<u16>, instr, out_trap);
    case O::I64AtomicRmw32AndU:  return DoAtomicRmw<u64>(IntAnd<u32>, instr, out_trap);
    case O::I32AtomicRmwOr:      return DoAtomicRmw<u32>(IntOr<u32>, instr, out_trap);
    case O::I64AtomicRmwOr:      return DoAtomicRmw<u64>(IntOr<u64>, instr, out_trap);
    case O::I32AtomicRmw8OrU:    return DoAtomicRmw<u32>(IntOr<u8>, instr, out_trap);
    case O::I32AtomicRmw16OrU:   return DoAtomicRmw<u32>(IntOr<u16>, instr, out_trap);
    case O::I64AtomicRmw8OrU:    return DoAtomicRmw<u64>(IntOr<u8>, instr, out_trap);
    case O::I64AtomicRmw16OrU:   return DoAtomicRmw<u64>(IntOr<u16>, instr, out_trap);
    case O::I64AtomicRmw32OrU:   return DoAtomicRmw<u64>(IntOr<u32>, instr, out_trap);
    case O::I32AtomicRmwXor:     return DoAtomicRmw<u32>(IntXor<u32>, instr, out_trap);
    case O::I64AtomicRmwXor:     return DoAtomicRmw<u64>(IntXor<u64>, instr, out_trap);
    case O::I32AtomicRmw8XorU:   return DoAtomicRmw<u32>(IntXor<u8>, instr, out_trap);
    case O::I32AtomicRmw16XorU:  return DoAtomicRmw<u32>(IntXor<u16>, instr, out_trap);
    case O::I64AtomicRmw8XorU:   return DoAtomicRmw<u64>(IntXor<u8>, instr, out_trap);
    case O::I64AtomicRmw16XorU:  return DoAtomicRmw<u64>(IntXor<u16>, instr, out_trap);
    case O::I64AtomicRmw32XorU:  return DoAtomicRmw<u64>(IntXor<u32>, instr, out_trap);
    case O::I32AtomicRmwXchg:    return DoAtomicRmw<u32>(Xchg<u32>, instr, out_trap);
    case O::I64AtomicRmwXchg:    return DoAtomicRmw<u64>(Xchg<u64>, instr, out_trap);
    case O::I32AtomicRmw8XchgU:  return DoAtomicRmw<u32>(Xchg<u8>, instr, out_trap);
    case O::I32AtomicRmw16XchgU: return DoAtomicRmw<u32>(Xchg<u16>, instr, out_trap);
    case O::I64AtomicRmw8XchgU:  return DoAtomicRmw<u64>(Xchg<u8>, instr, out_trap);
    case O::I64AtomicRmw16XchgU: return DoAtomicRmw<u64>(Xchg<u16>, instr, out_trap);
    case O::I64AtomicRmw32XchgU: return DoAtomicRmw<u64>(Xchg<u32>, instr, out_trap);

    case O::I32AtomicRmwCmpxchg:    return DoAtomicRmwCmpxchg<u32>(instr, out_trap);
    case O::I64AtomicRmwCmpxchg:    return DoAtomicRmwCmpxchg<u64>(instr, out_trap);
    case O::I32AtomicRmw8CmpxchgU:  return DoAtomicRmwCmpxchg<u32, u8>(instr, out_trap);
    case O::I32AtomicRmw16CmpxchgU: return DoAtomicRmwCmpxchg<u32, u16>(instr, out_trap);
    case O::I64AtomicRmw8CmpxchgU:  return DoAtomicRmwCmpxchg<u64, u8>(instr, out_trap);
    case O::I64AtomicRmw16CmpxchgU: return DoAtomicRmwCmpxchg<u64, u16>(instr, out_trap);
    case O::I64AtomicRmw32CmpxchgU: return DoAtomicRmwCmpxchg<u64, u32>(instr, out_trap);

    case O::Throw: {
      u32 tag_index = instr.imm_u32;
      Values params;
      Ref tag_ref = inst_->tags()[tag_index];
      Tag::Ptr tag{store_, tag_ref};
      PopValues(tag->type().signature, &params);
      Exception::Ptr exn = Exception::New(store_, tag_ref, params);
      return DoThrow(exn);
    }
    case O::Rethrow: {
      u32 exn_index = instr.imm_u32;
      Exception::Ptr exn{store_,
                         exceptions_[exceptions_.size() - exn_index - 1]};
      return DoThrow(exn);
    }
    case O::ThrowRef: {
      Ref ref = Pop<Ref>();
      assert(store_.HasValueType(ref, ValueType::ExnRef));
      // FIXME better error message?
      TRAP_IF(ref == Ref::Null, "expected exnref, got null");
      Exception::Ptr exn = store_.UnsafeGet<Exception>(ref);
      return DoThrow(exn);
    }

    // The following opcodes are either never generated or should never be
    // executed.
    case O::Nop:
    case O::Block:
    case O::Loop:
    case O::If:
    case O::Else:
    case O::End:
    case O::ReturnCall:
    case O::SelectT:

    case O::CallRef:
    case O::Try:
    case O::TryTable:
    case O::Catch:
    case O::CatchAll:
    case O::Delegate:
    case O::InterpData:
    case O::Invalid:
      WABT_UNREACHABLE;
      break;
  }
  // clang-format on

  return RunResult::Ok;
}

RunResult Thread::DoCall(const Func::Ptr& func, Trap::Ptr* out_trap) {
  if (auto* host_func = dyn_cast<HostFunc>(func.get())) {
    auto& func_type = host_func->type();

    Values params;
    PopValues(func_type.params, &params);
    if (PushCall(*host_func, out_trap) == RunResult::Trap) {
      return RunResult::Trap;
    }

    Values results(func_type.results.size());
    if (Failed(host_func->Call(*this, params, results, out_trap))) {
      return RunResult::Trap;
    }

    PopCall();
    PushValues(func_type.results, results);
  } else {
    if (PushCall(*cast<DefinedFunc>(func.get()), out_trap) == RunResult::Trap) {
      return RunResult::Trap;
    }
  }
  return RunResult::Ok;
}

template <typename T>
RunResult Thread::Load(Instr instr, T* out, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  u64 offset = PopPtr(memory);
  TRAP_IF(Failed(memory->Load(offset, instr.imm_u32x2.snd, out)),
          StringPrintf("out of bounds memory access: access at %" PRIu64
                       "+%" PRIzd " >= max value %" PRIu64,
                       offset + instr.imm_u32x2.snd, sizeof(T),
                       memory->ByteSize()));
  return RunResult::Ok;
}

template <typename T, typename V>
RunResult Thread::DoLoad(Instr instr, Trap::Ptr* out_trap) {
  V val;
  if (Load<V>(instr, &val, out_trap) != RunResult::Ok) {
    return RunResult::Trap;
  }
  Push(static_cast<T>(val));
  return RunResult::Ok;
}

template <typename T, typename V>
RunResult Thread::DoStore(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  V val = static_cast<V>(Pop<T>());
  u64 offset = PopPtr(memory);
  TRAP_IF(Failed(memory->Store(offset, instr.imm_u32x2.snd, val)),
          StringPrintf("out of bounds memory access: access at %" PRIu64
                       "+%" PRIzd " >= max value %" PRIu64,
                       offset + instr.imm_u32x2.snd, sizeof(V),
                       memory->ByteSize()));
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoUnop(UnopFunc<R, T> f) {
  Push<R>(f(Pop<T>()));
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoUnop(UnopTrapFunc<R, T> f, Trap::Ptr* out_trap) {
  T out;
  std::string msg;
  TRAP_IF(f(Pop<T>(), &out, &msg) == RunResult::Trap, msg);
  Push<R>(out);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoBinop(BinopFunc<R, T> f) {
  auto rhs = Pop<T>();
  auto lhs = Pop<T>();
  Push<R>(f(lhs, rhs));
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoBinop(BinopTrapFunc<R, T> f, Trap::Ptr* out_trap) {
  auto rhs = Pop<T>();
  auto lhs = Pop<T>();
  T out;
  std::string msg;
  TRAP_IF(f(lhs, rhs, &out, &msg) == RunResult::Trap, msg);
  Push<R>(out);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoConvert(Trap::Ptr* out_trap) {
  auto val = Pop<T>();
  if (std::is_integral<R>::value && std::is_floating_point<T>::value) {
    // Don't use std::isnan here because T may be a non-floating-point type.
    TRAP_IF(IsNaN(val), "invalid conversion to integer");
  }
  TRAP_UNLESS(CanConvert<R>(val), "integer overflow");
  Push<R>(Convert<R>(val));
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoReinterpret() {
  Push(Bitcast<R>(Pop<T>()));
  return RunResult::Ok;
}

RunResult Thread::DoMemoryInit(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  auto&& data = inst_->datas()[instr.imm_u32x2.snd];
  auto size = Pop<u32>();
  auto src = Pop<u32>();
  u64 dst = PopPtr(memory);
  TRAP_IF(Failed(memory->Init(dst, data, src, size)),
          "out of bounds memory access: memory.init out of bounds");
  return RunResult::Ok;
}

RunResult Thread::DoDataDrop(Instr instr) {
  inst_->datas()[instr.imm_u32].Drop();
  return RunResult::Ok;
}

RunResult Thread::DoMemoryCopy(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr mem_dst{store_, inst_->memories()[instr.imm_u32x2.fst]};
  Memory::Ptr mem_src{store_, inst_->memories()[instr.imm_u32x2.snd]};
  u64 size = PopPtr(mem_src);
  u64 src = PopPtr(mem_src);
  u64 dst = PopPtr(mem_dst);
  // TODO: change to "out of bounds"
  TRAP_IF(Failed(Memory::Copy(*mem_dst, dst, *mem_src, src, size)),
          "out of bounds memory access: memory.copy out of bound");
  return RunResult::Ok;
}

RunResult Thread::DoMemoryFill(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32]};
  u64 size = PopPtr(memory);
  auto value = Pop<u32>();
  u64 dst = PopPtr(memory);
  TRAP_IF(Failed(memory->Fill(dst, value, size)),
          "out of bounds memory access: memory.fill out of bounds");
  return RunResult::Ok;
}

RunResult Thread::DoTableInit(Instr instr, Trap::Ptr* out_trap) {
  Table::Ptr table{store_, inst_->tables()[instr.imm_u32x2.fst]};
  auto&& elem = inst_->elems()[instr.imm_u32x2.snd];
  auto size = Pop<u32>();
  auto src = Pop<u32>();
  u64 dst = PopPtr(table);
  TRAP_IF(Failed(table->Init(store_, dst, elem, src, size)),
          "out of bounds table access: table.init out of bounds");
  return RunResult::Ok;
}

RunResult Thread::DoElemDrop(Instr instr) {
  inst_->elems()[instr.imm_u32].Drop();
  return RunResult::Ok;
}

RunResult Thread::DoTableCopy(Instr instr, Trap::Ptr* out_trap) {
  Table::Ptr table_dst{store_, inst_->tables()[instr.imm_u32x2.fst]};
  Table::Ptr table_src{store_, inst_->tables()[instr.imm_u32x2.snd]};
  u64 size = PopPtr(table_src);
  u64 src = PopPtr(table_src);
  u64 dst = PopPtr(table_dst);
  TRAP_IF(Failed(Table::Copy(store_, *table_dst, dst, *table_src, src, size)),
          "out of bounds table access: table.copy out of bounds");
  return RunResult::Ok;
}

RunResult Thread::DoTableGet(Instr instr, Trap::Ptr* out_trap) {
  Table::Ptr table{store_, inst_->tables()[instr.imm_u32]};
  u64 index = PopPtr(table);
  Ref ref;
  TRAP_IF(Failed(table->Get(index, &ref)),
          StringPrintf("out of bounds table access: table.get at %" PRIu64
                       " >= max value %u",
                       index, table->size()));
  Push(ref);
  return RunResult::Ok;
}

RunResult Thread::DoTableSet(Instr instr, Trap::Ptr* out_trap) {
  Table::Ptr table{store_, inst_->tables()[instr.imm_u32]};
  auto ref = Pop<Ref>();
  u64 index = PopPtr(table);
  TRAP_IF(Failed(table->Set(store_, index, ref)),
          StringPrintf("out of bounds table access: table.set at %" PRIu64
                       " >= max value %u",
                       index, table->size()));
  return RunResult::Ok;
}

RunResult Thread::DoTableGrow(Instr instr, Trap::Ptr* out_trap) {
  Table::Ptr table{store_, inst_->tables()[instr.imm_u32]};
  u32 old_size = table->size();
  auto delta = PopPtr(table);
  auto ref = Pop<Ref>();
  if (Failed(table->Grow(store_, delta, ref))) {
    PushPtr(table, -1);
  } else {
    PushPtr(table, old_size);
  }
  return RunResult::Ok;
}

RunResult Thread::DoTableSize(Instr instr) {
  Table::Ptr table{store_, inst_->tables()[instr.imm_u32]};
  PushPtr(table, table->size());
  return RunResult::Ok;
}

RunResult Thread::DoTableFill(Instr instr, Trap::Ptr* out_trap) {
  Table::Ptr table{store_, inst_->tables()[instr.imm_u32]};
  u64 size = PopPtr(table);
  auto value = Pop<Ref>();
  u64 dst = PopPtr(table);
  TRAP_IF(Failed(table->Fill(store_, dst, value, size)),
          "out of bounds table access: table.fill out of bounds");
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoSimdSplat() {
  auto val = Pop<T>();
  R result;
  std::fill(std::begin(result.v), std::end(result.v), val);
  Push(result);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoSimdExtract(Instr instr) {
  Push<T>(Pop<R>()[instr.imm_u8]);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoSimdReplace(Instr instr) {
  auto val = Pop<T>();
  auto simd = Pop<R>();
  simd[instr.imm_u8] = val;
  Push(simd);
  return RunResult::Ok;
}

template <typename T> struct Simd128;
template <> struct Simd128<s8> { using Type = s8x16; };
template <> struct Simd128<u8> { using Type = u8x16; };
template <> struct Simd128<s16> { using Type = s16x8; };
template <> struct Simd128<u16> { using Type = u16x8; };
template <> struct Simd128<s32> { using Type = s32x4; };
template <> struct Simd128<u32> { using Type = u32x4; };
template <> struct Simd128<s64> { using Type = s64x2; };
template <> struct Simd128<u64> { using Type = u64x2; };
template <> struct Simd128<f32> { using Type = f32x4; };
template <> struct Simd128<f64> { using Type = f64x2; };

template <typename R, typename T>
RunResult Thread::DoSimdUnop(UnopFunc<R, T> f) {
  using ST = typename Simd128<T>::Type;
  using SR = typename Simd128<R>::Type;
  auto val = Pop<ST>();
  SR result;
  std::transform(std::begin(val.v), std::end(val.v), std::begin(result.v), f);
  Push(result);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoSimdUnopZero(UnopFunc<R, T> f) {
  using ST = typename Simd128<T>::Type;
  using SR = typename Simd128<R>::Type;
  auto val = Pop<ST>();
  SR result;
  for (u8 i = 0; i < ST::lanes; ++i) {
    result[i] = f(val[i]);
  }
  for (u8 i = ST::lanes; i < SR::lanes; ++i) {
    result[i] = 0;
  }
  Push(result);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoSimdBinop(BinopFunc<R, T> f) {
  using ST = typename Simd128<T>::Type;
  using SR = typename Simd128<R>::Type;
  static_assert(ST::lanes == SR::lanes, "SIMD lanes don't match");
  auto rhs = Pop<ST>();
  auto lhs = Pop<ST>();
  SR result;
  for (u8 i = 0; i < SR::lanes; ++i) {
    result[i] = f(lhs[i], rhs[i]);
  }
  Push(result);
  return RunResult::Ok;
}

RunResult Thread::DoSimdBitSelect() {
  using S = u64x2;
  auto c = Pop<S>();
  auto rhs = Pop<S>();
  auto lhs = Pop<S>();
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    result[i] = (lhs[i] & c[i]) | (rhs[i] & ~c[i]);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, u8 count>
RunResult Thread::DoSimdIsTrue() {
  using L = typename S::LaneType;
  auto val = Pop<S>();
  Push(std::count_if(std::begin(val.v), std::end(val.v),
                     [](L x) { return x != 0; }) >= count);
  return RunResult::Ok;
}

template <typename S>
RunResult Thread::DoSimdBitmask() {
  auto val = Pop<S>();
  u32 result = 0;
  for (u8 i = 0; i < S::lanes; ++i) {
    if (val[i] < 0) {
      result |= 1 << i;
    }
  }
  Push(result);
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoSimdShift(BinopFunc<R, T> f) {
  using ST = typename Simd128<T>::Type;
  using SR = typename Simd128<R>::Type;
  static_assert(ST::lanes == SR::lanes, "SIMD lanes don't match");
  auto amount = Pop<u32>();
  auto lhs = Pop<ST>();
  SR result;
  for (u8 i = 0; i < SR::lanes; ++i) {
    result[i] = f(lhs[i], amount);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S>
RunResult Thread::DoSimdLoadSplat(Instr instr, Trap::Ptr* out_trap) {
  using L = typename S::LaneType;
  L val;
  if (Load<L>(instr, &val, out_trap) != RunResult::Ok) {
    return RunResult::Trap;
  }
  S result;
  std::fill(std::begin(result.v), std::end(result.v), val);
  Push(result);
  return RunResult::Ok;
}

template <typename S>
RunResult Thread::DoSimdLoadLane(Instr instr, Trap::Ptr* out_trap) {
  using T = typename S::LaneType;
  auto result = Pop<S>();
  T val;
  if (Load<T>(instr, &val, out_trap) != RunResult::Ok) {
    return RunResult::Trap;
  }
  result[instr.imm_u32x2_u8.idx] = val;
  Push(result);
  return RunResult::Ok;
}

template <typename S>
RunResult Thread::DoSimdStoreLane(Instr instr, Trap::Ptr* out_trap) {
  using T = typename S::LaneType;
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2_u8.fst]};
  auto result = Pop<S>();
  T val = result[instr.imm_u32x2_u8.idx];
  u64 offset = PopPtr(memory);
  TRAP_IF(Failed(memory->Store(offset, instr.imm_u32x2_u8.snd, val)),
          StringPrintf("out of bounds memory access: access at %" PRIu64
                       "+%" PRIzd " >= max value %" PRIu64,
                       offset + instr.imm_u32x2_u8.snd, sizeof(T),
                       memory->ByteSize()));
  return RunResult::Ok;
}

template <typename S, typename T>
RunResult Thread::DoSimdLoadZero(Instr instr, Trap::Ptr* out_trap) {
  using L = typename S::LaneType;
  L val;
  if (Load<L>(instr, &val, out_trap) != RunResult::Ok) {
    return RunResult::Trap;
  }
  S result;
  std::fill(std::begin(result.v), std::end(result.v), 0);
  result[0] = val;
  Push(result);
  return RunResult::Ok;
}

RunResult Thread::DoSimdSwizzle() {
  using S = u8x16;
  auto rhs = Pop<S>();
  auto lhs = Pop<S>();
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    result[i] = rhs[i] < S::lanes ? lhs[rhs[i]] : 0;
  }
  Push(result);
  return RunResult::Ok;
}

RunResult Thread::DoSimdShuffle(Instr instr) {
  using S = u8x16;
  auto sel = Bitcast<S>(instr.imm_v128);
  auto rhs = Pop<S>();
  auto lhs = Pop<S>();
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    result[i] = sel[i] < S::lanes ? lhs[sel[i]] : rhs[sel[i] - S::lanes];
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T>
RunResult Thread::DoSimdNarrow() {
  using SL = typename S::LaneType;
  using TL = typename T::LaneType;
  auto rhs = Pop<T>();
  auto lhs = Pop<T>();
  S result;
  for (u8 i = 0; i < T::lanes; ++i) {
    result[i] = Saturate<SL, TL>(lhs[i]);
  }
  for (u8 i = 0; i < T::lanes; ++i) {
    result[T::lanes + i] = Saturate<SL, TL>(rhs[i]);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T, bool low>
RunResult Thread::DoSimdConvert() {
  using SL = typename S::LaneType;
  auto val = Pop<T>();
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    result[i] = Convert<SL>(val[(low ? 0 : S::lanes) + i]);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T, bool low>
RunResult Thread::DoSimdExtmul() {
  auto rhs = Pop<T>();
  auto lhs = Pop<T>();
  S result;
  using U = typename S::LaneType;
  for (u8 i = 0; i < S::lanes; ++i) {
    u8 laneidx = (low ? 0 : S::lanes) + i;
    result[i] = U(lhs[laneidx]) * U(rhs[laneidx]);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T>
RunResult Thread::DoSimdLoadExtend(Instr instr, Trap::Ptr* out_trap) {
  T val;
  if (Load<T>(instr, &val, out_trap) != RunResult::Ok) {
    return RunResult::Trap;
  }
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    result[i] = val[i];
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T>
RunResult Thread::DoSimdExtaddPairwise() {
  auto val = Pop<T>();
  S result;
  using U = typename S::LaneType;
  for (u8 i = 0; i < S::lanes; ++i) {
    u8 laneidx = i * 2;
    result[i] = U(val[laneidx]) + U(val[laneidx + 1]);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T>
RunResult Thread::DoSimdDot() {
  using SL = typename S::LaneType;
  auto rhs = Pop<T>();
  auto lhs = Pop<T>();
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    u8 laneidx = i * 2;
    SL lo = SL(lhs[laneidx] * rhs[laneidx]);
    SL hi = SL(lhs[laneidx + 1] * rhs[laneidx + 1]);
    result[i] = Add(lo, hi);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S, typename T>
RunResult Thread::DoSimdDotAdd() {
  using SL = typename S::LaneType;
  auto acc = Pop<S>();
  auto rhs = Pop<T>();
  auto lhs = Pop<T>();
  S result;
  for (u8 i = 0; i < S::lanes; ++i) {
    u8 laneidx = i * 2;
    SL lo = SL(lhs[laneidx] * rhs[laneidx]);
    SL hi = SL(lhs[laneidx + 1] * rhs[laneidx + 1]);
    result[i] = Add(lo, hi);
    result[i] = Add(result[i], acc[i]);
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S>
RunResult Thread::DoSimdRelaxedMadd() {
  using SS = typename Simd128<S>::Type;
  auto c = Pop<SS>();
  auto b = Pop<SS>();
  auto a = Pop<SS>();
  SS result;
  for (u8 i = 0; i < SS::lanes; ++i) {
    result[i] = a[i] * b[i] + c[i];
  }
  Push(result);
  return RunResult::Ok;
}

template <typename S>
RunResult Thread::DoSimdRelaxedNmadd() {
  using SS = typename Simd128<S>::Type;
  auto c = Pop<SS>();
  auto b = Pop<SS>();
  auto a = Pop<SS>();
  SS result;
  for (u8 i = 0; i < SS::lanes; ++i) {
    result[i] = -(a[i] * b[i]) + c[i];
  }
  Push(result);
  return RunResult::Ok;
}

template <typename T, typename V>
RunResult Thread::DoAtomicLoad(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  u64 offset = PopPtr(memory);
  V val;
  TRAP_IF(Failed(memory->AtomicLoad(offset, instr.imm_u32x2.snd, &val)),
          StringPrintf("invalid atomic access at %" PRIaddress "+%u", offset,
                       instr.imm_u32x2.snd));
  Push(static_cast<T>(val));
  return RunResult::Ok;
}

template <typename T, typename V>
RunResult Thread::DoAtomicStore(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  V val = static_cast<V>(Pop<T>());
  u64 offset = PopPtr(memory);
  TRAP_IF(Failed(memory->AtomicStore(offset, instr.imm_u32x2.snd, val)),
          StringPrintf("invalid atomic access at %" PRIaddress "+%u", offset,
                       instr.imm_u32x2.snd));
  return RunResult::Ok;
}

template <typename R, typename T>
RunResult Thread::DoAtomicRmw(BinopFunc<T, T> f,
                              Instr instr,
                              Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  T val = static_cast<T>(Pop<R>());
  u64 offset = PopPtr(memory);
  T old;
  TRAP_IF(Failed(memory->AtomicRmw(offset, instr.imm_u32x2.snd, val, f, &old)),
          StringPrintf("invalid atomic access at %" PRIaddress "+%u", offset,
                       instr.imm_u32x2.snd));
  Push(static_cast<R>(old));
  return RunResult::Ok;
}

template <typename T, typename V>
RunResult Thread::DoAtomicRmwCmpxchg(Instr instr, Trap::Ptr* out_trap) {
  Memory::Ptr memory{store_, inst_->memories()[instr.imm_u32x2.fst]};
  V replace = static_cast<V>(Pop<T>());
  V expect = static_cast<V>(Pop<T>());
  V old;
  u64 offset = PopPtr(memory);
  TRAP_IF(Failed(memory->AtomicRmwCmpxchg(offset, instr.imm_u32x2.snd, expect,
                                          replace, &old)),
          StringPrintf("invalid atomic access at %" PRIaddress "+%u", offset,
                       instr.imm_u32x2.snd));
  Push(static_cast<T>(old));
  return RunResult::Ok;
}

RunResult Thread::DoThrow(Exception::Ptr exn) {
  Istream::Offset target_offset = Istream::kInvalidOffset;
  u32 target_values, target_exceptions;
  Tag::Ptr exn_tag{store_, exn->tag()};
  bool popped_frame = false;
  bool had_catch_all = false;
  bool target_exnref = false;

  // DoThrow is responsible for unwinding the stack at the point at which an
  // exception is thrown, and also branching to the appropriate catch within
  // the target try-catch. In a compiler, the tag dispatch might be done in
  // generated code in a landing pad, but this is easier for the interpreter.
  while (!frames_.empty()) {
    const Frame& frame = frames_.back();
    DefinedFunc::Ptr func{store_, frame.func};
    u32 pc = frame.offset;
    auto handlers = func->desc().handlers;

    // We iterate in reverse order, in order to traverse handlers from most
    // specific (pushed last) to least specific within a nested stack of
    // try-catch blocks.
    auto iter = handlers.rbegin();
    while (iter != handlers.rend()) {
      const HandlerDesc& handler = *iter;
      // pc points to the *next* instruction by the time we're in DoThrow.
      if (pc > handler.try_start_offset && pc <= handler.try_end_offset) {
        // For a try-delegate, skip part of the traversal by directly going
        // up to an outer handler specified by the delegate depth.
        if (handler.kind == HandlerKind::Delegate) {
          // Subtract one as we're trying to get a reverse iterator that is
          // offset by `delegate_handler_index` from the first item.
          iter = handlers.rend() - handler.delegate_handler_index - 1;
          continue;
        }
        // Otherwise, check for a matching catch tag or catch_all.
        for (auto _catch : handler.catches) {
          // Here we have to be careful to use the target frame's instance
          // to look up the tag rather than the throw's instance.
          Ref catch_tag_ref = frame.inst->tags()[_catch.tag_index];
          Tag::Ptr catch_tag{store_, catch_tag_ref};
          if (exn_tag == catch_tag) {
            target_offset = _catch.offset;
            target_values = (*iter).values;
            target_exceptions = (*iter).exceptions;
            target_exnref = _catch.ref;
            goto found_handler;
          }
        }
        if (handler.catch_all_offset != Istream::kInvalidOffset) {
          target_offset = handler.catch_all_offset;
          target_values = (*iter).values;
          target_exceptions = (*iter).exceptions;
          had_catch_all = true;
          target_exnref = handler.catch_all_ref;
          goto found_handler;
        }
      }
      iter++;
    }
    frames_.pop_back();
    popped_frame = true;
  }

  // If the call frames are empty now, the exception is uncaught.
  assert(frames_.empty());
  return RunResult::Exception;

found_handler:
  assert(target_offset != Istream::kInvalidOffset);

  Frame& target_frame = frames_.back();
  // If the throw crosses call frames, we need to reset the state to that
  // call frame's values. The stack heights may need to be offset by the
  // handler's heights as we may be jumping into the middle of the function
  // code after some stack height changes.
  if (popped_frame) {
    inst_ = target_frame.inst;
    mod_ = target_frame.mod;
  }
  values_.resize(target_frame.values + target_values);
  exceptions_.resize(target_frame.exceptions + target_exceptions);
  // Jump to the handler.
  target_frame.offset = target_offset;
  // When an exception is caught, it needs to be tracked in a stack
  // to allow for rethrows. This stack is popped on leaving the try-catch
  // or by control instructions such as `br`.
  exceptions_.push_back(exn.ref());
  // Also push exception payload values if applicable.
  if (!had_catch_all) {
    PushValues(exn_tag->type().signature, exn->args());
  }
  if (target_exnref) {
    Push(exn.ref());
  }
  return RunResult::Ok;
}

Thread::TraceSource::TraceSource(Thread* thread) : thread_(thread) {}

std::string Thread::TraceSource::Header(Istream::Offset offset) {
  return StringPrintf("#%" PRIzd ". %4u: V:%-3" PRIzd,
                      thread_->frames_.size() - 1, offset,
                      thread_->values_.size());
}

std::string Thread::TraceSource::Pick(Index index, Instr instr) {
  Value val = thread_->Pick(index);
  const char* reftype;
  // Estimate number of operands.
  // TODO: Instead, record this accurately in opcode.def.
  Index num_operands = 3;
  for (Index i = 3; i >= 1; i--) {
    if (instr.op.GetParamType(i) == ValueType::Void) {
      num_operands--;
    } else {
      break;
    }
  }
  auto type = index > num_operands
                  ? Type(ValueType::Void)
                  : instr.op.GetParamType(num_operands - index + 1);
  if (type == ValueType::Void) {
    // Void should never be displayed normally; we only expect to see it when
    // the stack may have different a different type. This is likely to occur
    // with an index; try to see which type we should expect.
    switch (instr.op) {
      case Opcode::GlobalSet: type = GetGlobalType(instr.imm_u32); break;
      case Opcode::LocalSet:
      case Opcode::LocalTee:  type = GetLocalType(instr.imm_u32); break;
      case Opcode::TableSet:
      case Opcode::TableGrow:
      case Opcode::TableFill: type = GetTableElementType(instr.imm_u32); break;
      default: return "?";
    }
  }

  switch (type) {
    case ValueType::I32: return StringPrintf("%u", val.Get<u32>());
    case ValueType::I64: return StringPrintf("%" PRIu64, val.Get<u64>());
    case ValueType::F32: return StringPrintf("%g", val.Get<f32>());
    case ValueType::F64: return StringPrintf("%g", val.Get<f64>());
    case ValueType::V128: {
      auto v = val.Get<v128>();
      return StringPrintf("0x%08x 0x%08x 0x%08x 0x%08x", v.u32(0), v.u32(1),
                          v.u32(2), v.u32(3));
    }

      // clang-format off
    case ValueType::FuncRef:    reftype = "funcref"; break;
    case ValueType::ExternRef:  reftype = "externref"; break;
    case ValueType::ExnRef:     reftype = "exnref"; break;
      // clang-format on

    default:
      WABT_UNREACHABLE;
      break;
  }

  // Handle ref types.
  return StringPrintf("%s:%" PRIzd, reftype, val.Get<Ref>().index);
}

ValueType Thread::TraceSource::GetLocalType(Index stack_slot) {
  const Frame& frame = thread_->frames_.back();
  DefinedFunc::Ptr func{thread_->store_, frame.func};
  // When a function is called, the arguments are first pushed on the stack by
  // the caller, then the new call frame is pushed (which caches the current
  // height of the value stack). At that point, any local variables will be
  // allocated on the stack. For example, a function that has two parameters
  // and one local variable will have a stack like this:
  //
  //                 frame.values      top of stack
  //                 v                 v
  //   param1 param2 | local1 ..........
  //
  // When the instruction stream is generated, all local variable access is
  // translated into a value relative to the top of the stack, counting up from
  // 1. So in the previous example, if there are three values above the local
  // variable, the stack looks like:
  //
  //              param1 param2 | local1 value1 value2 value3
  // stack slot:    6      5        4      3      2      1
  // local index:   0      1        2
  //
  // local1 can be accessed with stack_slot 4, and param1 can be accessed with
  // stack_slot 6. The formula below takes these values into account to convert
  // the stack_slot into a local index.
  Index local_index =
      (thread_->values_.size() - frame.values + func->type().params.size()) -
      stack_slot;
  return func->desc().GetLocalType(local_index);
}

ValueType Thread::TraceSource::GetGlobalType(Index index) {
  return thread_->mod_->desc().globals[index].type.type;
}

ValueType Thread::TraceSource::GetTableElementType(Index index) {
  return thread_->mod_->desc().tables[index].type.element;
}

}  // namespace interp
}  // namespace wabt
