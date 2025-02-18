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

#include <wasm.h>

#include "wabt/binary-reader.h"
#include "wabt/cast.h"
#include "wabt/error-formatter.h"
#include "wabt/interp/binary-reader-interp.h"
#include "wabt/interp/interp-util.h"
#include "wabt/interp/interp.h"

using namespace wabt;
using namespace wabt::interp;

#define own

#ifndef NDEBUG
#define TRACE0() TRACE("")
#define TRACE(str, ...) \
  fprintf(stderr, "CAPI: [%s] " str "\n", __func__, ##__VA_ARGS__)
#define TRACE_NO_NL(str, ...) \
  fprintf(stderr, "CAPI: [%s] " str, __func__, ##__VA_ARGS__)
#else
#define TRACE0(...)
#define TRACE(...)
#define TRACE_NO_NL(...)
#endif

static Features s_features;
static std::unique_ptr<FileStream> s_log_stream;
static std::unique_ptr<FileStream> s_stdout_stream;

// Type conversion utilities
static ValueType ToWabtValueType(wasm_valkind_t);
static wasm_valkind_t FromWabtValueType(ValueType);

static wasm_externkind_t FromWabtExternKind(ExternKind);

static ValueTypes ToWabtValueTypes(const wasm_valtype_vec_t* types);
static void FromWabtValueTypes(const ValueTypes&, wasm_valtype_vec_t* out);

static wasm_mutability_t FromWabtMutability(Mutability);
static Mutability ToWabtMutability(wasm_mutability_t);

static Limits ToWabtLimits(const wasm_limits_t&);
static wasm_limits_t FromWabtLimits(const Limits&);

// Value conversion utilities
static TypedValue ToWabtValue(const wasm_val_t&);
static wasm_val_t FromWabtValue(Store&, const TypedValue&);

static Values ToWabtValues(const wasm_val_vec_t* values);
static void FromWabtValues(Store& store,
                           wasm_val_vec_t* values,
                           const ValueTypes& types,
                           const Values& wabt_values);

// Structs
struct wasm_config_t {};

struct wasm_engine_t {};

struct wasm_valtype_t {
  ValueType I;
};

struct wasm_externtype_t {
  static std::unique_ptr<wasm_externtype_t> New(std::unique_ptr<ExternType>);

  std::unique_ptr<wasm_externtype_t> Clone() const { return New(I->Clone()); }

  virtual ~wasm_externtype_t() {}

  wasm_externtype_t(const wasm_externtype_t& other) = delete;
  wasm_externtype_t& operator=(const wasm_externtype_t& other) = delete;

  template <typename T>
  T* As() const {
    return cast<T>(I.get());
  }

  std::unique_ptr<ExternType> I;

 protected:
  wasm_externtype_t(std::unique_ptr<ExternType> et) : I(std::move(et)) {}
};

struct wasm_functype_t : wasm_externtype_t {
  wasm_functype_t(own wasm_valtype_vec_t* params,
                  own wasm_valtype_vec_t* results)
      : wasm_externtype_t{std::make_unique<FuncType>(
            ToWabtValueTypes(params),
            ToWabtValueTypes(results))},
        params(*params),
        results(*results) {}

  wasm_functype_t(FuncType ft)
      : wasm_externtype_t{std::make_unique<FuncType>(ft)} {
    FromWabtValueTypes(ft.params, &params);
    FromWabtValueTypes(ft.results, &results);
  }

  ~wasm_functype_t() {
    wasm_valtype_vec_delete(&params);
    wasm_valtype_vec_delete(&results);
  }

  // Stored here because API requires returning pointers.
  wasm_valtype_vec_t params;
  wasm_valtype_vec_t results;
};

struct wasm_globaltype_t : wasm_externtype_t {
  wasm_globaltype_t(own wasm_valtype_t* type, wasm_mutability_t mut)
      : wasm_externtype_t{std::make_unique<GlobalType>(type->I,
                                                       ToWabtMutability(mut))},
        valtype{*type} {
    wasm_valtype_delete(type);
  }

  wasm_globaltype_t(GlobalType gt)
      : wasm_externtype_t{std::make_unique<GlobalType>(gt)}, valtype{gt.type} {}

  // Stored here because API requires returning pointers.
  wasm_valtype_t valtype;
};

struct wasm_tabletype_t : wasm_externtype_t {
  wasm_tabletype_t(own wasm_valtype_t* type, const wasm_limits_t* limits)
      : wasm_externtype_t{std::make_unique<TableType>(type->I,
                                                      ToWabtLimits(*limits))},
        elemtype(*type),
        limits(*limits) {
    wasm_valtype_delete(type);
  }

  wasm_tabletype_t(TableType tt)
      : wasm_externtype_t{std::make_unique<TableType>(tt)},
        elemtype{tt.element},
        limits{FromWabtLimits(tt.limits)} {}

  // Stored here because API requires returning pointers.
  wasm_valtype_t elemtype;
  wasm_limits_t limits;
};

struct wasm_memorytype_t : wasm_externtype_t {
  wasm_memorytype_t(const wasm_limits_t* limits)
      : wasm_externtype_t{std::make_unique<MemoryType>(
            ToWabtLimits(*limits),
            WABT_DEFAULT_PAGE_SIZE)},  // wasm-c-api doesn't support
                                       // custom-page-sizes yet
        limits{*limits} {}

  wasm_memorytype_t(MemoryType mt)
      : wasm_externtype_t{std::make_unique<MemoryType>(mt)},
        limits{FromWabtLimits(mt.limits)} {}

  // Stored here because API requires returning pointers.
  wasm_limits_t limits;
};

// static
std::unique_ptr<wasm_externtype_t> wasm_externtype_t::New(
    std::unique_ptr<ExternType> ptr) {
  switch (ptr->kind) {
    case ExternKind::Func:
      return std::make_unique<wasm_functype_t>(*cast<FuncType>(ptr.get()));

    case ExternKind::Table:
      return std::make_unique<wasm_tabletype_t>(*cast<TableType>(ptr.get()));

    case ExternKind::Memory:
      return std::make_unique<wasm_memorytype_t>(*cast<MemoryType>(ptr.get()));

    case ExternKind::Global:
      return std::make_unique<wasm_globaltype_t>(*cast<GlobalType>(ptr.get()));

    case ExternKind::Tag:
      break;
  }

  assert(false);
  return {};
}

struct wasm_importtype_t {
  wasm_importtype_t(ImportType it)
      : I(it),
        extern_{wasm_externtype_t::New(it.type->Clone())},
        module{I.module.size(), const_cast<wasm_byte_t*>(I.module.data())},
        name{I.name.size(), const_cast<wasm_byte_t*>(I.name.data())} {}

  wasm_importtype_t(const wasm_importtype_t& other)
      : wasm_importtype_t(other.I) {}

  wasm_importtype_t& operator=(const wasm_importtype_t& other) {
    wasm_importtype_t copy(other);
    std::swap(I, copy.I);
    std::swap(extern_, copy.extern_);
    std::swap(module, copy.module);
    std::swap(name, copy.name);
    return *this;
  }

  ImportType I;
  // Stored here because API requires returning pointers.
  std::unique_ptr<wasm_externtype_t> extern_;
  wasm_name_t module;
  wasm_name_t name;
};

struct wasm_exporttype_t {
  wasm_exporttype_t(ExportType et)
      : I(et),
        extern_{wasm_externtype_t::New(et.type->Clone())},
        name{I.name.size(), const_cast<wasm_byte_t*>(I.name.data())} {}

  wasm_exporttype_t(const wasm_exporttype_t& other)
      : wasm_exporttype_t(other.I) {}

  wasm_exporttype_t& operator=(const wasm_exporttype_t& other) {
    wasm_exporttype_t copy(other);
    std::swap(I, copy.I);
    std::swap(extern_, copy.extern_);
    std::swap(name, copy.name);
    return *this;
  }

  ExportType I;
  // Stored here because API requires returning pointers.
  std::unique_ptr<wasm_externtype_t> extern_;
  wasm_name_t name;
};

struct wasm_store_t {
  wasm_store_t(const Features& features) : I(features) {}
  Store I;
};

struct wasm_ref_t {
  wasm_ref_t(RefPtr<Object> ptr) : I(ptr) {}

  template <typename T>
  T* As() const {
    return cast<T>(I.get());
  }

  RefPtr<Object> I;
};

struct wasm_frame_t {
  Frame I;
};

struct wasm_trap_t : wasm_ref_t {
  wasm_trap_t(RefPtr<Trap> ptr) : wasm_ref_t(ptr) {}
};

struct wasm_foreign_t : wasm_ref_t {
  wasm_foreign_t(RefPtr<Foreign> ptr) : wasm_ref_t(ptr) {}
};

struct wasm_module_t : wasm_ref_t {
  wasm_module_t(RefPtr<Module> ptr, const wasm_byte_vec_t* in)
      : wasm_ref_t(ptr) {
    wasm_byte_vec_copy(&binary, in);
  }

  wasm_module_t(const wasm_module_t& other) : wasm_ref_t(other.I) {
    wasm_byte_vec_copy(&binary, &other.binary);
  }

  wasm_module_t& operator=(const wasm_module_t& other) {
    wasm_module_t copy(other);
    std::swap(I, copy.I);
    std::swap(binary, copy.binary);
    return *this;
  }

  ~wasm_module_t() { wasm_byte_vec_delete(&binary); }
  // TODO: This is used for wasm_module_serialize/wasm_module_deserialize.
  // Currently the standard wasm binary bytes are cached here, but it would be
  // better to have a serialization of ModuleDesc instead.
  wasm_byte_vec_t binary;
};

struct wasm_shared_module_t : wasm_module_t {};

struct wasm_extern_t : wasm_ref_t {
  wasm_extern_t(RefPtr<Extern> ptr) : wasm_ref_t(ptr) {}
};

struct wasm_func_t : wasm_extern_t {
  wasm_func_t(RefPtr<Func> ptr) : wasm_extern_t(ptr) {}
};

struct wasm_global_t : wasm_extern_t {
  wasm_global_t(RefPtr<Global> ptr) : wasm_extern_t(ptr) {}
};

struct wasm_table_t : wasm_extern_t {
  wasm_table_t(RefPtr<Table> ptr) : wasm_extern_t(ptr) {}
};

struct wasm_memory_t : wasm_extern_t {
  wasm_memory_t(RefPtr<Memory> ptr) : wasm_extern_t(ptr) {}
};

struct wasm_instance_t : wasm_ref_t {
  wasm_instance_t(RefPtr<Instance> ptr) : wasm_ref_t(ptr) {}
};

// Type conversion utilities
static ValueType ToWabtValueType(wasm_valkind_t kind) {
  switch (kind) {
    case WASM_I32:
      return ValueType::I32;
    case WASM_I64:
      return ValueType::I64;
    case WASM_F32:
      return ValueType::F32;
    case WASM_F64:
      return ValueType::F64;
    case WASM_ANYREF:
      return ValueType::ExternRef;
    case WASM_FUNCREF:
      return ValueType::FuncRef;
    default:
      TRACE("unexpected wasm_valkind_t: %d", kind);
      WABT_UNREACHABLE;
  }
  WABT_UNREACHABLE;
}

static wasm_valkind_t FromWabtValueType(ValueType type) {
  switch (type) {
    case ValueType::I32:
      return WASM_I32;
    case ValueType::I64:
      return WASM_I64;
    case ValueType::F32:
      return WASM_F32;
    case ValueType::F64:
      return WASM_F64;
    case ValueType::ExternRef:
      return WASM_ANYREF;
    case ValueType::FuncRef:
      return WASM_FUNCREF;
    default:
      WABT_UNREACHABLE;
  }
}

static wasm_externkind_t FromWabtExternKind(ExternKind kind) {
  switch (kind) {
    case ExternalKind::Func:
      return WASM_EXTERN_FUNC;
    case ExternalKind::Global:
      return WASM_EXTERN_GLOBAL;
    case ExternalKind::Table:
      return WASM_EXTERN_TABLE;
    case ExternalKind::Memory:
      return WASM_EXTERN_MEMORY;
    case ExternalKind::Tag:
      WABT_UNREACHABLE;
  }
  WABT_UNREACHABLE;
}

static ValueTypes ToWabtValueTypes(const wasm_valtype_vec_t* types) {
  ValueTypes result;
  for (size_t i = 0; i < types->size; ++i) {
    result.push_back(types->data[i]->I);
  }
  return result;
}

static void FromWabtValueTypes(const ValueTypes& types,
                               wasm_valtype_vec_t* out) {
  wasm_valtype_vec_new_uninitialized(out, types.size());
  for (size_t i = 0; i < types.size(); ++i) {
    out->data[i] = wasm_valtype_new(FromWabtValueType(types[i]));
  }
}

static wasm_mutability_t FromWabtMutability(Mutability mut) {
  return mut == Mutability::Var ? WASM_VAR : WASM_CONST;
}

static Mutability ToWabtMutability(wasm_mutability_t mut) {
  return mut == WASM_VAR ? Mutability::Var : Mutability::Const;
}

// Value conversion utilities

static TypedValue ToWabtValue(const wasm_val_t& value) {
  TypedValue out;
  out.type = ToWabtValueType(value.kind);
  switch (value.kind) {
    case WASM_I32:
      out.value.Set(value.of.i32);
      break;
    case WASM_I64:
      out.value.Set(value.of.i64);
      break;
    case WASM_F32:
      out.value.Set(value.of.f32);
      break;
    case WASM_F64:
      out.value.Set(value.of.f64);
      break;
    case WASM_ANYREF:
    case WASM_FUNCREF:
      out.value.Set(value.of.ref ? value.of.ref->I->self() : Ref::Null);
      break;
    default:
      TRACE("unexpected wasm type: %d", value.kind);
      assert(false);
  }
  TRACE("-> %s", TypedValueToString(out).c_str());
  return out;
}

static wasm_val_t FromWabtValue(Store& store, const TypedValue& tv) {
  TRACE("%s", TypedValueToString(tv).c_str());
  wasm_val_t out_value;
  switch (tv.type) {
    case Type::I32:
      out_value.kind = WASM_I32;
      out_value.of.i32 = tv.value.Get<s32>();
      break;
    case Type::I64:
      out_value.kind = WASM_I64;
      out_value.of.i64 = tv.value.Get<s64>();
      break;
    case Type::F32:
      out_value.kind = WASM_F32;
      out_value.of.f32 = tv.value.Get<f32>();
      break;
    case Type::F64:
      out_value.kind = WASM_F64;
      out_value.of.f64 = tv.value.Get<f64>();
      break;
    case Type::FuncRef: {
      Ref ref = tv.value.Get<Ref>();
      out_value.kind = WASM_FUNCREF;
      out_value.of.ref = new wasm_func_t(store.UnsafeGet<Func>(ref));
      break;
    }
    case Type::ExternRef: {
      Ref ref = tv.value.Get<Ref>();
      out_value.kind = WASM_ANYREF;
      out_value.of.ref = new wasm_foreign_t(store.UnsafeGet<Foreign>(ref));
      break;
    }
    default:
      TRACE("unexpected wabt type: %d", static_cast<int>(tv.type));
      assert(false);
  }
  return out_value;
}

static Limits ToWabtLimits(const wasm_limits_t& limits) {
  return Limits(limits.min, limits.max);
}

static wasm_limits_t FromWabtLimits(const Limits& limits) {
  return wasm_limits_t{(uint32_t)limits.initial, (uint32_t)limits.max};
}

static Values ToWabtValues(const wasm_val_vec_t* values) {
  Values result;
  for (size_t i = 0; i < values->size; ++i) {
    result.push_back(ToWabtValue(values->data[i]).value);
  }
  return result;
}

static void FromWabtValues(Store& store,
                           wasm_val_vec_t* values,
                           const ValueTypes& types,
                           const Values& wabt_values) {
  assert(types.size() == wabt_values.size());
  assert(types.size() == values->size);
  for (size_t i = 0; i < types.size(); ++i) {
    values->data[i] =
        FromWabtValue(store, TypedValue{types[i], wabt_values[i]});
  }
}

static std::string ToString(const wasm_message_t* msg) {
  return std::string(msg->data, msg->size);
}

static wasm_message_t FromString(const std::string& s) {
  wasm_message_t result;
  wasm_byte_vec_new(&result, s.size() + 1, s.c_str());
  return result;
}

static ReadBinaryInterpOptions GetOptions() {
  s_features.EnableAll();
  if (getenv("WASM_API_DEBUG") != nullptr) {
    s_log_stream = FileStream::CreateStderr();
  }
  return ReadBinaryInterpOptions(s_features, s_log_stream.get());
}

extern "C" {

// wasm_valtype

own wasm_valtype_t* wasm_valtype_new(wasm_valkind_t kind) {
  return new wasm_valtype_t{ToWabtValueType(kind)};
}

wasm_valkind_t wasm_valtype_kind(const wasm_valtype_t* type) {
  assert(type);
  return FromWabtValueType(type->I);
}

// Helpers

static void print_sig(const FuncType& sig) {
#ifndef NDEBUG
  fprintf(stderr, "(");
  bool first = true;
  for (auto type : sig.params) {
    if (!first) {
      fprintf(stderr, ", ");
    }
    first = false;
    fprintf(stderr, "%s", type.GetName().c_str());
  }
  fprintf(stderr, ") -> (");
  first = true;
  for (auto type : sig.results) {
    if (!first) {
      fprintf(stderr, ", ");
    }
    first = false;
    fprintf(stderr, "%s", type.GetName().c_str());
  }
  fprintf(stderr, ")\n");
#endif
}

// wasm_val

void wasm_val_delete(own wasm_val_t* val) {
  assert(val);
  if (wasm_valkind_is_ref(val->kind)) {
    delete val->of.ref;
    val->of.ref = nullptr;
  }
}

void wasm_val_copy(own wasm_val_t* out, const wasm_val_t* in) {
  TRACE("%s", TypedValueToString(ToWabtValue(*in)).c_str());
  if (wasm_valkind_is_ref(in->kind)) {
    out->kind = in->kind;
    out->of.ref = in->of.ref ? new wasm_ref_t{*in->of.ref} : nullptr;
  } else {
    *out = *in;
  }
  TRACE("-> %p %s", out, TypedValueToString(ToWabtValue(*out)).c_str());
}

// wasm_trap

own wasm_trap_t* wasm_trap_new(wasm_store_t* store, const wasm_message_t* msg) {
  return new wasm_trap_t{Trap::New(store->I, ToString(msg))};
}

void wasm_trap_message(const wasm_trap_t* trap, own wasm_message_t* out) {
  assert(trap);
  *out = FromString(trap->As<Trap>()->message());
}

own wasm_frame_t* wasm_trap_origin(const wasm_trap_t* trap) {
  // TODO(sbc): Implement stack traces
  return nullptr;
}

void wasm_trap_trace(const wasm_trap_t* trap, own wasm_frame_vec_t* out) {
  assert(trap);
  assert(out);
  wasm_frame_vec_new_empty(out);
  // std::string msg(trap->message.data, trap->message.size);
  // TRACE(stderr, "error: %s\n", msg.c_str());
}

// wasm_config

own wasm_config_t* wasm_config_new() {
  return new wasm_config_t();
}

// wasm_engine

own wasm_engine_t* wasm_engine_new() {
  return new wasm_engine_t();
}

own wasm_engine_t* wasm_engine_new_with_config(own wasm_config_t*) {
  assert(false);
  return nullptr;
}

// wasm_store

own wasm_store_t* wasm_store_new(wasm_engine_t* engine) {
  assert(engine);
  return new wasm_store_t(s_features);
}

// wasm_module

own wasm_module_t* wasm_module_new(wasm_store_t* store,
                                   const wasm_byte_vec_t* binary) {
  Errors errors;
  ModuleDesc module_desc;
  if (Failed(ReadBinaryInterp("<internal>", binary->data, binary->size,
                              GetOptions(), &errors, &module_desc))) {
    FormatErrorsToFile(errors, Location::Type::Binary);
    return nullptr;
  }

  return new wasm_module_t{Module::New(store->I, module_desc), binary};
}

bool wasm_module_validate(wasm_store_t* store, const wasm_byte_vec_t* binary) {
  // TODO: Optimize this; for now it is generating a new module and discarding
  // it. But since this call only needs to validate, it could do much less.
  wasm_module_t* module = wasm_module_new(store, binary);
  if (module == nullptr) {
    return false;
  }
  wasm_module_delete(module);
  return true;
}

void wasm_module_imports(const wasm_module_t* module,
                         own wasm_importtype_vec_t* out) {
  auto&& import_types = module->As<Module>()->import_types();
  TRACE("%" PRIzx, import_types.size());
  wasm_importtype_vec_new_uninitialized(out, import_types.size());

  for (size_t i = 0; i < import_types.size(); ++i) {
    out->data[i] = new wasm_importtype_t{import_types[i]};
  }
}

void wasm_module_exports(const wasm_module_t* module,
                         own wasm_exporttype_vec_t* out) {
  auto&& export_types = module->As<Module>()->export_types();
  TRACE("%" PRIzx, export_types.size());
  wasm_exporttype_vec_new_uninitialized(out, export_types.size());

  for (size_t i = 0; i < export_types.size(); ++i) {
    out->data[i] = new wasm_exporttype_t{export_types[i]};
  }
}

void wasm_module_serialize(const wasm_module_t* module,
                           own wasm_byte_vec_t* out) {
  wasm_byte_vec_copy(out, &module->binary);
}

own wasm_module_t* wasm_module_deserialize(wasm_store_t* store,
                                           const wasm_byte_vec_t* bytes) {
  return wasm_module_new(store, bytes);
}

// wasm_importtype

own wasm_importtype_t* wasm_importtype_new(own wasm_name_t* module,
                                           own wasm_name_t* name,
                                           own wasm_externtype_t* type) {
  return new wasm_importtype_t{
      ImportType{ToString(module), ToString(name), type->I->Clone()}};
}

const wasm_name_t* wasm_importtype_module(const wasm_importtype_t* im) {
  return &im->module;
}

const wasm_name_t* wasm_importtype_name(const wasm_importtype_t* im) {
  return &im->name;
}

const wasm_externtype_t* wasm_importtype_type(const wasm_importtype_t* im) {
  return im->extern_.get();
}

// wasm_exporttype

own wasm_exporttype_t* wasm_exporttype_new(own wasm_name_t* name,
                                           wasm_externtype_t* type) {
  return new wasm_exporttype_t{ExportType{ToString(name), type->I->Clone()}};
}

const wasm_name_t* wasm_exporttype_name(const wasm_exporttype_t* ex) {
  return &ex->name;
}

const wasm_externtype_t* wasm_exporttype_type(const wasm_exporttype_t* ex) {
  TRACE("%p", ex);
  assert(ex);
  return ex->extern_.get();
}

// wasm_instance

own wasm_instance_t* wasm_instance_new(wasm_store_t* store,
                                       const wasm_module_t* module,
                                       const wasm_extern_vec_t* imports,
                                       own wasm_trap_t** trap_out) {
  TRACE("%p %p", store, module);
  assert(module);
  assert(store);

  RefVec import_refs;
  for (size_t i = 0; i < module->As<Module>()->import_types().size(); i++) {
    import_refs.push_back(imports->data[i]->I->self());
  }

  Trap::Ptr trap;
  auto instance =
      Instance::Instantiate(store->I, module->I->self(), import_refs, &trap);
  if (trap) {
    *trap_out = new wasm_trap_t{trap};
    return nullptr;
  }

  return new wasm_instance_t{instance};
}

void wasm_instance_exports(const wasm_instance_t* instance,
                           own wasm_extern_vec_t* out) {
  auto&& exports = instance->As<Instance>()->exports();
  wasm_extern_vec_new_uninitialized(out, exports.size());
  TRACE("%" PRIzx, exports.size());

  for (size_t i = 0; i < exports.size(); ++i) {
    out->data[i] =
        new wasm_extern_t{instance->I.store()->UnsafeGet<Extern>(exports[i])};
  }
}

// wasm_functype

own wasm_functype_t* wasm_functype_new(own wasm_valtype_vec_t* params,
                                       own wasm_valtype_vec_t* results) {
  TRACE("params=%" PRIzx " args=%" PRIzx, params->size, results->size);
  auto* res = new wasm_functype_t{params, results};
  TRACE_NO_NL("");
  print_sig(*res->As<FuncType>());
  return res;
}

const wasm_valtype_vec_t* wasm_functype_params(const wasm_functype_t* f) {
  TRACE("%" PRIzx, f->params.size);
  return &f->params;
}

const wasm_valtype_vec_t* wasm_functype_results(const wasm_functype_t* f) {
  return &f->results;
}

// wasm_func
own wasm_func_t* wasm_func_new(wasm_store_t* store,
                               const wasm_functype_t* type,
                               wasm_func_callback_t callback) {
  FuncType wabt_type = *type->As<FuncType>();
  auto lambda = [=](Thread& thread, const Values& wabt_params,
                    Values& wabt_results, Trap::Ptr* out_trap) -> Result {
    wasm_val_vec_t params, results;
    wasm_val_vec_new_uninitialized(&params, wabt_params.size());
    wasm_val_vec_new_uninitialized(&results, wabt_results.size());
    FromWabtValues(store->I, &params, wabt_type.params, wabt_params);
    auto trap = callback(&params, &results);
    wasm_val_vec_delete(&params);
    if (trap) {
      *out_trap = trap->I.As<Trap>();
      wasm_trap_delete(trap);
      // Can't use wasm_val_vec_delete since it wasn't populated.
      delete[] results.data;
      return Result::Error;
    }
    wabt_results = ToWabtValues(&results);
    wasm_val_vec_delete(&results);
    return Result::Ok;
  };

  return new wasm_func_t{HostFunc::New(store->I, wabt_type, lambda)};
}

own wasm_func_t* wasm_func_new_with_env(wasm_store_t* store,
                                        const wasm_functype_t* type,
                                        wasm_func_callback_with_env_t callback,
                                        void* env,
                                        void (*finalizer)(void*)) {
  FuncType wabt_type = *type->As<FuncType>();
  auto lambda = [=](Thread& thread, const Values& wabt_params,
                    Values& wabt_results, Trap::Ptr* out_trap) -> Result {
    wasm_val_vec_t params, results;
    wasm_val_vec_new_uninitialized(&params, wabt_params.size());
    wasm_val_vec_new_uninitialized(&results, wabt_results.size());
    FromWabtValues(store->I, &params, wabt_type.params, wabt_params);
    auto trap = callback(env, &params, &results);
    wasm_val_vec_delete(&params);
    if (trap) {
      *out_trap = trap->I.As<Trap>();
      wasm_trap_delete(trap);
      // Can't use wasm_val_vec_delete since it wasn't populated.
      delete[] results.data;
      return Result::Error;
    }
    wabt_results = ToWabtValues(&results);
    wasm_val_vec_delete(&results);
    return Result::Ok;
  };

  // TODO: This finalizer is different from the host_info finalizer.
  return new wasm_func_t{HostFunc::New(store->I, wabt_type, lambda)};
}

own wasm_functype_t* wasm_func_type(const wasm_func_t* func) {
  TRACE0();
  return new wasm_functype_t{func->As<Func>()->type()};
}

size_t wasm_func_result_arity(const wasm_func_t* func) {
  return func->As<Func>()->type().results.size();
}

size_t wasm_func_param_arity(const wasm_func_t* func) {
  return func->As<Func>()->type().params.size();
}

own wasm_trap_t* wasm_func_call(const wasm_func_t* f,
                                const wasm_val_vec_t* args,
                                wasm_val_vec_t* results) {
  // TODO: get some information about the function; name/index
  // TRACE("%d", f->index);

  auto&& func_type = f->As<Func>()->type();
  Values wabt_args = ToWabtValues(args);
  Values wabt_results;
  Trap::Ptr trap;
  if (Failed(
          f->As<Func>()->Call(*f->I.store(), wabt_args, wabt_results, &trap))) {
    return new wasm_trap_t{trap};
  }
  FromWabtValues(*f->I.store(), results, func_type.results, wabt_results);
  return nullptr;
}

// wasm_globaltype

own wasm_globaltype_t* wasm_globaltype_new(own wasm_valtype_t* type,
                                           wasm_mutability_t mut) {
  assert(type);
  auto* result = new wasm_globaltype_t{GlobalType{
      type->I, mut == WASM_CONST ? Mutability::Const : Mutability::Var}};
  wasm_valtype_delete(type);
  return result;
}

wasm_mutability_t wasm_globaltype_mutability(const wasm_globaltype_t* type) {
  assert(type);
  return FromWabtMutability(type->As<GlobalType>()->mut);
}

const wasm_valtype_t* wasm_globaltype_content(const wasm_globaltype_t* type) {
  assert(type);
  return &type->valtype;
}

// wasm_tabletype

own wasm_tabletype_t* wasm_tabletype_new(own wasm_valtype_t* type,
                                         const wasm_limits_t* limits) {
  return new wasm_tabletype_t{type, limits};
}

const wasm_valtype_t* wasm_tabletype_element(const wasm_tabletype_t* type) {
  return &type->elemtype;
}

const wasm_limits_t* wasm_tabletype_limits(const wasm_tabletype_t* type) {
  return &type->limits;
}

// wasm_memorytype

own wasm_memorytype_t* wasm_memorytype_new(const wasm_limits_t* limits) {
  return new wasm_memorytype_t(limits);
}

const wasm_limits_t* wasm_memorytype_limits(const wasm_memorytype_t* t) {
  return &t->limits;
}

// wasm_global

own wasm_global_t* wasm_global_new(wasm_store_t* store,
                                   const wasm_globaltype_t* type,
                                   const wasm_val_t* val) {
  assert(store && type);
  TypedValue value = ToWabtValue(*val);
  TRACE("%s", TypedValueToString(value).c_str());
  return new wasm_global_t{
      Global::New(store->I, *type->As<GlobalType>(), value.value)};
}

own wasm_globaltype_t* wasm_global_type(const wasm_global_t* global) {
  return new wasm_globaltype_t{global->As<Global>()->type()};
}

void wasm_global_get(const wasm_global_t* global, own wasm_val_t* out) {
  assert(global);
  TRACE0();
  TypedValue tv{global->As<Global>()->type().type, global->As<Global>()->Get()};
  TRACE(" -> %s", TypedValueToString(tv).c_str());
  *out = FromWabtValue(*global->I.store(), tv);
}

void wasm_global_set(wasm_global_t* global, const wasm_val_t* val) {
  TRACE0();
  if (wasm_valkind_is_ref(val->kind)) {
    global->As<Global>()->Set(*global->I.store(), val->of.ref->I->self());
  } else {
    global->As<Global>()->UnsafeSet(ToWabtValue(*val).value);
  }
}

// wasm_table

own wasm_table_t* wasm_table_new(wasm_store_t* store,
                                 const wasm_tabletype_t* type,
                                 wasm_ref_t* init) {
  return new wasm_table_t{Table::New(store->I, *type->As<TableType>())};
}

own wasm_tabletype_t* wasm_table_type(const wasm_table_t* table) {
  return new wasm_tabletype_t{table->As<Table>()->type()};
}

wasm_table_size_t wasm_table_size(const wasm_table_t* table) {
  return table->As<Table>()->size();
}

own wasm_ref_t* wasm_table_get(const wasm_table_t* table,
                               wasm_table_size_t index) {
  Ref ref;
  if (Failed(table->As<Table>()->Get(index, &ref))) {
    return nullptr;
  }
  if (ref == Ref::Null) {
    return nullptr;
  }
  return new wasm_ref_t{table->I.store()->UnsafeGet<Object>(ref)};
}

bool wasm_table_set(wasm_table_t* table,
                    wasm_table_size_t index,
                    wasm_ref_t* ref) {
  return Succeeded(table->As<Table>()->Set(*table->I.store(), index,
                                           ref ? ref->I->self() : Ref::Null));
}

bool wasm_table_grow(wasm_table_t* table,
                     wasm_table_size_t delta,
                     wasm_ref_t* init) {
  return Succeeded(table->As<Table>()->Grow(
      *table->I.store(), delta, init ? init->I->self() : Ref::Null));
}

// wams_memory

own wasm_memory_t* wasm_memory_new(wasm_store_t* store,
                                   const wasm_memorytype_t* type) {
  TRACE0();
  return new wasm_memory_t{Memory::New(store->I, *type->As<MemoryType>())};
}

own wasm_memorytype_t* wasm_memory_type(const wasm_memory_t* memory) {
  return new wasm_memorytype_t{memory->As<Memory>()->type()};
}

byte_t* wasm_memory_data(wasm_memory_t* memory) {
  return reinterpret_cast<byte_t*>(memory->As<Memory>()->UnsafeData());
}

wasm_memory_pages_t wasm_memory_size(const wasm_memory_t* memory) {
  return memory->As<Memory>()->PageSize();
}

size_t wasm_memory_data_size(const wasm_memory_t* memory) {
  return memory->As<Memory>()->ByteSize();
}

bool wasm_memory_grow(wasm_memory_t* memory, wasm_memory_pages_t delta) {
  return Succeeded(memory->As<Memory>()->Grow(delta));
}

// wasm_frame

own wasm_frame_t* wasm_frame_copy(const wasm_frame_t* frame) {
  return new wasm_frame_t{*frame};
}

wasm_instance_t* wasm_frame_instance(const wasm_frame_t* frame) {
  // TODO
  return nullptr;
}

size_t wasm_frame_module_offset(const wasm_frame_t* frame) {
  // TODO
  return 0;
}

size_t wasm_frame_func_offset(const wasm_frame_t* frame) {
  // TODO
  return 0;
}

uint32_t wasm_frame_func_index(const wasm_frame_t* frame) {
  // TODO
  return 0;
}

// wasm_externtype

wasm_externkind_t wasm_externtype_kind(const wasm_externtype_t* type) {
  assert(type);
  return FromWabtExternKind(type->I->kind);
}

// wasm_extern

own wasm_externtype_t* wasm_extern_type(const wasm_extern_t* extern_) {
  return wasm_externtype_t::New(extern_->As<Extern>()->extern_type().Clone())
      .release();
}

wasm_externkind_t wasm_extern_kind(const wasm_extern_t* extern_) {
  return FromWabtExternKind(extern_->As<Extern>()->extern_type().kind);
}

// wasm_foreign

own wasm_foreign_t* wasm_foreign_new(wasm_store_t* store) {
  return new wasm_foreign_t{Foreign::New(store->I, nullptr)};
}

// vector types

#define WASM_IMPL_OWN(name)                           \
  void wasm_##name##_delete(own wasm_##name##_t* t) { \
    assert(t);                                        \
    TRACE0();                                         \
    delete t;                                         \
  }

WASM_IMPL_OWN(frame);
WASM_IMPL_OWN(config);
WASM_IMPL_OWN(engine);
WASM_IMPL_OWN(store);

#define WASM_IMPL_VEC_BASE(name, ptr_or_none)                            \
  void wasm_##name##_vec_new_empty(own wasm_##name##_vec_t* out) {       \
    TRACE0();                                                            \
    wasm_##name##_vec_new_uninitialized(out, 0);                         \
  }                                                                      \
  void wasm_##name##_vec_new_uninitialized(own wasm_##name##_vec_t* vec, \
                                           size_t size) {                \
    TRACE("%" PRIzx, size);                                              \
    vec->size = size;                                                    \
    vec->data = size ? new wasm_##name##_t ptr_or_none[size] : nullptr;  \
  }

#define WASM_IMPL_VEC_PLAIN(name)                                       \
  WASM_IMPL_VEC_BASE(name, )                                            \
  void wasm_##name##_vec_new(own wasm_##name##_vec_t* vec, size_t size, \
                             own wasm_##name##_t const src[]) {         \
    TRACE0();                                                           \
    wasm_##name##_vec_new_uninitialized(vec, size);                     \
    memcpy(vec->data, src, size * sizeof(wasm_##name##_t));             \
  }                                                                     \
  void wasm_##name##_vec_copy(own wasm_##name##_vec_t* out,             \
                              const wasm_##name##_vec_t* vec) {         \
    TRACE("%" PRIzx, vec->size);                                        \
    wasm_##name##_vec_new_uninitialized(out, vec->size);                \
    memcpy(out->data, vec->data, vec->size * sizeof(wasm_##name##_t));  \
  }                                                                     \
  void wasm_##name##_vec_delete(own wasm_##name##_vec_t* vec) {         \
    TRACE0();                                                           \
    delete[] vec->data;                                                 \
    vec->size = 0;                                                      \
  }

WASM_IMPL_VEC_PLAIN(byte);

// Special implementation for wasm_val_t, since it's weird.
WASM_IMPL_VEC_BASE(val, )
void wasm_val_vec_new(own wasm_val_vec_t* vec,
                      size_t size,
                      own wasm_val_t const src[]) {
  TRACE0();
  wasm_val_vec_new_uninitialized(vec, size);
  for (size_t i = 0; i < size; ++i) {
    vec->data[i] = src[i];
  }
}

void wasm_val_vec_copy(own wasm_val_vec_t* out, const wasm_val_vec_t* vec) {
  TRACE("%" PRIzx, vec->size);
  wasm_val_vec_new_uninitialized(out, vec->size);
  for (size_t i = 0; i < vec->size; ++i) {
    wasm_val_copy(&out->data[i], &vec->data[i]);
  }
}

void wasm_val_vec_delete(own wasm_val_vec_t* vec) {
  TRACE0();
  for (size_t i = 0; i < vec->size; ++i) {
    wasm_val_delete(&vec->data[i]);
  }
  delete[] vec->data;
  vec->size = 0;
}

#define WASM_IMPL_VEC_OWN(name)                                         \
  WASM_IMPL_VEC_BASE(name, *)                                           \
  void wasm_##name##_vec_new(own wasm_##name##_vec_t* vec, size_t size, \
                             own wasm_##name##_t* const src[]) {        \
    TRACE0();                                                           \
    wasm_##name##_vec_new_uninitialized(vec, size);                     \
    for (size_t i = 0; i < size; ++i) {                                 \
      vec->data[i] = src[i];                                            \
    }                                                                   \
  }                                                                     \
  void wasm_##name##_vec_copy(own wasm_##name##_vec_t* out,             \
                              const wasm_##name##_vec_t* vec) {         \
    TRACE("%" PRIzx, vec->size);                                        \
    wasm_##name##_vec_new_uninitialized(out, vec->size);                \
    for (size_t i = 0; i < vec->size; ++i) {                            \
      out->data[i] = wasm_##name##_copy(vec->data[i]);                  \
    }                                                                   \
  }                                                                     \
  void wasm_##name##_vec_delete(wasm_##name##_vec_t* vec) {             \
    TRACE0();                                                           \
    for (size_t i = 0; i < vec->size; ++i) {                            \
      delete vec->data[i];                                              \
    }                                                                   \
    delete[] vec->data;                                                 \
    vec->size = 0;                                                      \
  }

WASM_IMPL_VEC_OWN(frame);
WASM_IMPL_VEC_OWN(extern);

#define WASM_IMPL_TYPE(name)                                              \
  WASM_IMPL_OWN(name)                                                     \
  WASM_IMPL_VEC_OWN(name)                                                 \
  own wasm_##name##_t* wasm_##name##_copy(const wasm_##name##_t* other) { \
    TRACE0();                                                             \
    return new wasm_##name##_t(*other);                                   \
  }

WASM_IMPL_TYPE(valtype);
WASM_IMPL_TYPE(importtype);
WASM_IMPL_TYPE(exporttype);

#define WASM_IMPL_TYPE_CLONE(name)                                        \
  WASM_IMPL_OWN(name)                                                     \
  WASM_IMPL_VEC_OWN(name)                                                 \
  own wasm_##name##_t* wasm_##name##_copy(const wasm_##name##_t* other) { \
    TRACE0();                                                             \
    return static_cast<wasm_##name##_t*>(other->Clone().release());       \
  }

WASM_IMPL_TYPE_CLONE(functype);
WASM_IMPL_TYPE_CLONE(globaltype);
WASM_IMPL_TYPE_CLONE(tabletype);
WASM_IMPL_TYPE_CLONE(memorytype);
WASM_IMPL_TYPE_CLONE(externtype);

#define WASM_IMPL_REF_BASE(name)                                          \
  WASM_IMPL_OWN(name)                                                     \
  own wasm_##name##_t* wasm_##name##_copy(const wasm_##name##_t* ref) {   \
    TRACE0();                                                             \
    return new wasm_##name##_t(*ref);                                     \
  }                                                                       \
  bool wasm_##name##_same(const wasm_##name##_t* ref,                     \
                          const wasm_##name##_t* other) {                 \
    TRACE0();                                                             \
    return ref->I == other->I;                                            \
  }                                                                       \
  void* wasm_##name##_get_host_info(const wasm_##name##_t* ref) {         \
    return ref->I->host_info();                                           \
  }                                                                       \
  void wasm_##name##_set_host_info(wasm_##name##_t* ref, void* info) {    \
    ref->I->set_host_info(info);                                          \
  }                                                                       \
  void wasm_##name##_set_host_info_with_finalizer(                        \
      wasm_##name##_t* ref, void* info, void (*finalizer)(void*)) {       \
    ref->I->set_host_info(info);                                          \
    ref->I->set_finalizer([=](Object* o) { finalizer(o->host_info()); }); \
  }

#define WASM_IMPL_REF(name)                                                  \
  WASM_IMPL_REF_BASE(name)                                                   \
  wasm_ref_t* wasm_##name##_as_ref(wasm_##name##_t* subclass) {              \
    return subclass;                                                         \
  }                                                                          \
  wasm_##name##_t* wasm_ref_as_##name(wasm_ref_t* ref) {                     \
    return static_cast<wasm_##name##_t*>(ref);                               \
  }                                                                          \
  const wasm_ref_t* wasm_##name##_as_ref_const(                              \
      const wasm_##name##_t* subclass) {                                     \
    return subclass;                                                         \
  }                                                                          \
  const wasm_##name##_t* wasm_ref_as_##name##_const(const wasm_ref_t* ref) { \
    return static_cast<const wasm_##name##_t*>(ref);                         \
  }

WASM_IMPL_REF_BASE(ref);

WASM_IMPL_REF(extern);
WASM_IMPL_REF(foreign);
WASM_IMPL_REF(func);
WASM_IMPL_REF(global);
WASM_IMPL_REF(instance);
WASM_IMPL_REF(memory);
WASM_IMPL_REF(table);
WASM_IMPL_REF(trap);

#define WASM_IMPL_SHARABLE_REF(name)                                           \
  WASM_IMPL_REF(name)                                                          \
  WASM_IMPL_OWN(shared_##name)                                                 \
  own wasm_shared_##name##_t* wasm_##name##_share(const wasm_##name##_t* t) {  \
    return static_cast<wasm_shared_##name##_t*>(                               \
        const_cast<wasm_##name##_t*>(t));                                      \
  }                                                                            \
  own wasm_##name##_t* wasm_##name##_obtain(wasm_store_t*,                     \
                                            const wasm_shared_##name##_t* t) { \
    return static_cast<wasm_##name##_t*>(                                      \
        const_cast<wasm_shared_##name##_t*>(t));                               \
  }

WASM_IMPL_SHARABLE_REF(module)

#define WASM_IMPL_EXTERN(name)                                                 \
  const wasm_##name##type_t* wasm_externtype_as_##name##type_const(            \
      const wasm_externtype_t* t) {                                            \
    return static_cast<const wasm_##name##type_t*>(t);                         \
  }                                                                            \
  wasm_##name##type_t* wasm_externtype_as_##name##type(wasm_externtype_t* t) { \
    return static_cast<wasm_##name##type_t*>(t);                               \
  }                                                                            \
  wasm_externtype_t* wasm_##name##type_as_externtype(wasm_##name##type_t* t) { \
    return static_cast<wasm_externtype_t*>(t);                                 \
  }                                                                            \
  const wasm_externtype_t* wasm_##name##type_as_externtype_const(              \
      const wasm_##name##type_t* t) {                                          \
    return static_cast<const wasm_externtype_t*>(t);                           \
  }                                                                            \
  wasm_extern_t* wasm_##name##_as_extern(wasm_##name##_t* name) {              \
    return static_cast<wasm_extern_t*>(name);                                  \
  }                                                                            \
  const wasm_extern_t* wasm_##name##_as_extern_const(                          \
      const wasm_##name##_t* name) {                                           \
    return static_cast<const wasm_extern_t*>(name);                            \
  }                                                                            \
  wasm_##name##_t* wasm_extern_as_##name(wasm_extern_t* ext) {                 \
    return static_cast<wasm_##name##_t*>(ext);                                 \
  }

WASM_IMPL_EXTERN(table);
WASM_IMPL_EXTERN(func);
WASM_IMPL_EXTERN(global);
WASM_IMPL_EXTERN(memory);

}  // extern "C"
