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

#include "wasm-binary-reader-interpreter.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#include "wasm-allocator.h"
#include "wasm-binary-reader.h"
#include "wasm-interpreter.h"
#include "wasm-writer.h"

#define LOG 0

#if LOG
#define LOGF(...) fprintf(stderr, __VA_ARGS__)
#else
#define LOGF(...) (void)0
#endif

#define INVALID_FUNC_INDEX ((uint32_t)~0)

#define CHECK_RESULT(expr) \
  do {                     \
    if (WASM_FAILED(expr)) \
      return WASM_ERROR;   \
  } while (0)

#define CHECK_DEPTH(ctx, depth)                                         \
  do {                                                                  \
    if ((depth) >= (ctx)->label_stack.size) {                           \
      print_error((ctx), "invalid depth: %d (max %" PRIzd ")", (depth), \
                  ((ctx)->label_stack.size));                           \
      return WASM_ERROR;                                                \
    }                                                                   \
  } while (0)

#define CHECK_LOCAL(ctx, local_index)                                       \
  do {                                                                      \
    uint32_t max_local_index =                                              \
        (ctx)->current_func->param_and_local_types.size;                    \
    if ((local_index) >= max_local_index) {                                 \
      print_error((ctx), "invalid local_index: %d (max %d)", (local_index), \
                  max_local_index);                                         \
      return WASM_ERROR;                                                    \
    }                                                                       \
  } while (0)

#define CHECK_GLOBAL(ctx, global_index)                                       \
  do {                                                                        \
    uint32_t max_global_index = (ctx)->module->globals.size;                  \
    if ((global_index) >= max_global_index) {                                 \
      print_error((ctx), "invalid global_index: %d (max %d)", (global_index), \
                  max_global_index);                                          \
      return WASM_ERROR;                                                      \
    }                                                                         \
  } while (0)

#define WASM_TYPE_ANY WASM_NUM_TYPES

static const char* s_type_names[] = {
    "void", "i32", "i64", "f32", "f64", "any",
};
WASM_STATIC_ASSERT(WASM_ARRAY_SIZE(s_type_names) == WASM_NUM_TYPES + 1);

/* TODO(binji): combine with the ones defined in wasm-check? */
#define V(rtype, type1, type2, mem_size, code, NAME, text) \
  [code] = WASM_TYPE_##rtype,
static WasmType s_opcode_rtype[] = {WASM_FOREACH_OPCODE(V)};
#undef V

#define V(rtype, type1, type2, mem_size, code, NAME, text) \
  [code] = WASM_TYPE_##type1,
static WasmType s_opcode_type1[] = {WASM_FOREACH_OPCODE(V)};
#undef V

#define V(rtype, type1, type2, mem_size, code, NAME, text) \
  [code] = WASM_TYPE_##type2,
static WasmType s_opcode_type2[] = {WASM_FOREACH_OPCODE(V)};
#undef V

#define V(rtype, type1, type2, mem_size, code, NAME, text) [code] = text,
static const char* s_opcode_name[] = {
    /* clang-format off */
  WASM_FOREACH_OPCODE(V)
  [WASM_OPCODE_ALLOCA] = "alloca",
  [WASM_OPCODE_DROP_KEEP] = "drop_keep",
    /* clang-format on */
};
#undef V

typedef uint32_t Uint32;
WASM_DEFINE_VECTOR(uint32, Uint32);
WASM_DEFINE_VECTOR(uint32_vector, Uint32Vector);

typedef enum LabelType {
  LABEL_TYPE_FUNC,
  LABEL_TYPE_BLOCK,
  LABEL_TYPE_LOOP,
  LABEL_TYPE_IF,
  LABEL_TYPE_ELSE,
} LabelType;

static const char* s_label_type_name[] = {
    "func", "block", "loop", "if", "else",
};

typedef struct Label {
  LabelType label_type;
  WasmType type;
  uint32_t expr_stack_size;
  uint32_t offset; /* branch location in the istream */
  uint32_t fixup_offset;
} Label;
WASM_DEFINE_VECTOR(label, Label);

typedef struct InterpreterFunc {
  uint32_t sig_index;
  uint32_t offset;
  uint32_t local_decl_count;
  uint32_t local_count;
  WasmTypeVector param_and_local_types;
} InterpreterFunc;
WASM_DEFINE_ARRAY(interpreter_func, InterpreterFunc);

typedef struct Context {
  WasmAllocator* allocator;
  WasmBinaryReader* reader;
  WasmBinaryErrorHandler* error_handler;
  WasmAllocator* memory_allocator;
  WasmInterpreterModule* module;
  InterpreterFuncArray funcs;
  InterpreterFunc* current_func;
  WasmTypeVector expr_stack;
  LabelVector label_stack;
  Uint32VectorVector func_fixups;
  Uint32VectorVector depth_fixups;
  uint32_t depth;
  uint32_t start_func_index;
  WasmMemoryWriter istream_writer;
  uint32_t istream_offset;
  WasmInterpreterTypedValue init_expr_value;
  uint32_t table_offset;
} Context;

static Label* get_label(Context* ctx, uint32_t depth) {
  assert(depth < ctx->label_stack.size);
  return &ctx->label_stack.data[depth];
}

static Label* top_label(Context* ctx) {
  return get_label(ctx, ctx->label_stack.size - 1);
}

static uint32_t get_value_count(WasmType result_type) {
  return (result_type == WASM_TYPE_VOID || result_type == WASM_TYPE_ANY) ? 0
                                                                         : 1;
}

static void on_error(uint32_t offset, const char* message, void* user_data);

static void print_error(Context* ctx, const char* format, ...) {
  WASM_SNPRINTF_ALLOCA(buffer, length, format);
  on_error(WASM_INVALID_OFFSET, buffer, ctx);
}

static InterpreterFunc* get_func(Context* ctx, uint32_t func_index) {
  assert(func_index < ctx->funcs.size);
  return &ctx->funcs.data[func_index];
}

static WasmInterpreterImport* get_import(Context* ctx, uint32_t import_index) {
  assert(import_index < ctx->module->imports.size);
  return &ctx->module->imports.data[import_index];
}

static WasmInterpreterExport* get_export(Context* ctx, uint32_t export_index) {
  assert(export_index < ctx->module->exports.size);
  return &ctx->module->exports.data[export_index];
}

static WasmInterpreterFuncSignature* get_signature(Context* ctx,
                                                   uint32_t sig_index) {
  assert(sig_index < ctx->module->sigs.size);
  return &ctx->module->sigs.data[sig_index];
}

static WasmInterpreterFuncSignature* get_func_signature(Context* ctx,
                                                        InterpreterFunc* func) {
  return get_signature(ctx, func->sig_index);
}

static WasmType get_local_index_type(InterpreterFunc* func,
                                     uint32_t local_index) {
  assert(local_index < func->param_and_local_types.size);
  return func->param_and_local_types.data[local_index];
}

static WasmType get_global_index_type(Context* ctx, uint32_t global_index) {
  assert(global_index < ctx->module->globals.size);
  return ctx->module->globals.data[global_index].type;
}

static uint32_t translate_local_index(Context* ctx, uint32_t local_index) {
  assert(local_index < ctx->expr_stack.size);
  return ctx->expr_stack.size - local_index;
}

static uint32_t get_istream_offset(Context* ctx) {
  return ctx->istream_offset;
}

static WasmResult emit_data_at(Context* ctx,
                               size_t offset,
                               const void* data,
                               size_t size) {
  return ctx->istream_writer.base.write_data(
      offset, data, size, ctx->istream_writer.base.user_data);
}

static WasmResult emit_data(Context* ctx, const void* data, size_t size) {
  CHECK_RESULT(emit_data_at(ctx, ctx->istream_offset, data, size));
  ctx->istream_offset += size;
  return WASM_OK;
}

static WasmResult emit_opcode(Context* ctx, WasmOpcode opcode) {
  return emit_data(ctx, &opcode, sizeof(uint8_t));
}

static WasmResult emit_i8(Context* ctx, uint8_t value) {
  return emit_data(ctx, &value, sizeof(value));
}

static WasmResult emit_i32(Context* ctx, uint32_t value) {
  return emit_data(ctx, &value, sizeof(value));
}

static WasmResult emit_i64(Context* ctx, uint64_t value) {
  return emit_data(ctx, &value, sizeof(value));
}

static WasmResult emit_i32_at(Context* ctx, uint32_t offset, uint32_t value) {
  return emit_data_at(ctx, offset, &value, sizeof(value));
}

static WasmResult emit_drop_keep(Context* ctx, uint32_t drop, uint8_t keep) {
  assert(drop != UINT32_MAX);
  assert(keep <= 1);
  if (drop > 0) {
    if (drop == 1 && keep == 0) {
      LOGF("%3" PRIzd ": drop\n", ctx->expr_stack.size);
      CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DROP));
    } else {
      LOGF("%3" PRIzd ": drop_keep %u %u\n", ctx->expr_stack.size, drop, keep);
      CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DROP_KEEP));
      CHECK_RESULT(emit_i32(ctx, drop));
      CHECK_RESULT(emit_i8(ctx, keep));
    }
  }
  return WASM_OK;
}

static WasmResult emit_return(Context* ctx, WasmType result_type) {
  /* drop the locals and params, but keep the return value, if any */
  uint32_t keep_count = get_value_count(result_type);
  assert(ctx->expr_stack.size >= keep_count);
  uint32_t drop_count = ctx->expr_stack.size - keep_count;
  CHECK_RESULT(emit_drop_keep(ctx, drop_count, keep_count));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_RETURN));
  return WASM_OK;
}

static WasmResult append_fixup(Context* ctx,
                               Uint32VectorVector* fixups_vector,
                               uint32_t index) {
  if (index >= fixups_vector->size)
    wasm_resize_uint32_vector_vector(ctx->allocator, fixups_vector, index + 1);
  Uint32Vector* fixups = &fixups_vector->data[index];
  uint32_t offset = get_istream_offset(ctx);
  wasm_append_uint32_value(ctx->allocator, fixups, &offset);
  return WASM_OK;
}

static WasmResult emit_br_offset(Context* ctx,
                                 uint32_t depth,
                                 uint32_t offset) {
  if (offset == WASM_INVALID_OFFSET)
    CHECK_RESULT(append_fixup(ctx, &ctx->depth_fixups, depth));
  CHECK_RESULT(emit_i32(ctx, offset));
  return WASM_OK;
}

static WasmResult emit_br(Context* ctx, uint8_t arity, uint32_t depth) {
  Label* label = get_label(ctx, depth);
  assert(ctx->expr_stack.size >= label->expr_stack_size + arity);
  uint32_t drop_count = (ctx->expr_stack.size - label->expr_stack_size) - arity;
  CHECK_RESULT(emit_drop_keep(ctx, drop_count, arity));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR));
  CHECK_RESULT(emit_br_offset(ctx, depth, label->offset));
  return WASM_OK;
}

static WasmResult emit_br_table_offset(Context* ctx,
                                       uint8_t arity,
                                       uint32_t depth) {
  Label* label = get_label(ctx, depth);
  assert(ctx->expr_stack.size >= label->expr_stack_size + arity);
  uint32_t drop_count = (ctx->expr_stack.size - label->expr_stack_size) - arity;
  CHECK_RESULT(emit_br_offset(ctx, depth, label->offset));
  CHECK_RESULT(emit_i32(ctx, drop_count));
  CHECK_RESULT(emit_i8(ctx, arity));
  return WASM_OK;
}

static WasmResult fixup_top_label(Context* ctx, uint32_t offset) {
  uint32_t top = ctx->label_stack.size - 1;
  if (top >= ctx->depth_fixups.size) {
    /* nothing to fixup */
    return WASM_OK;
  }

  Uint32Vector* fixups = &ctx->depth_fixups.data[top];
  uint32_t i;
  for (i = 0; i < fixups->size; ++i)
    CHECK_RESULT(emit_i32_at(ctx, fixups->data[i], offset));
  /* reduce the size to 0 in case this gets reused. Keep the allocations for
   * later use */
  fixups->size = 0;
  return WASM_OK;
}

static WasmResult emit_func_offset(Context* ctx,
                                   InterpreterFunc* func,
                                   uint32_t func_index) {
  if (func->offset == WASM_INVALID_OFFSET)
    CHECK_RESULT(append_fixup(ctx, &ctx->func_fixups, func_index));
  CHECK_RESULT(emit_i32(ctx, func->offset));
  return WASM_OK;
}

static void on_error(uint32_t offset, const char* message, void* user_data) {
  Context* ctx = user_data;
  if (ctx->error_handler->on_error) {
    ctx->error_handler->on_error(offset, message,
                                 ctx->error_handler->user_data);
  }
}

static WasmResult on_signature_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_func_signature_array(ctx->allocator, &ctx->module->sigs,
                                            count);
  return WASM_OK;
}

static WasmResult on_signature(uint32_t index,
                               WasmType result_type,
                               uint32_t param_count,
                               WasmType* param_types,
                               void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFuncSignature* sig = get_signature(ctx, index);
  sig->result_type = result_type;

  wasm_reserve_types(ctx->allocator, &sig->param_types, param_count);
  sig->param_types.size = param_count;
  memcpy(sig->param_types.data, param_types, param_count * sizeof(WasmType));
  return WASM_OK;
}

static WasmResult on_import_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_import_array(ctx->allocator, &ctx->module->imports,
                                    count);
  return WASM_OK;
}

static WasmResult trapping_import_callback(
    WasmInterpreterModule* module,
    WasmInterpreterImport* import,
    uint32_t num_args,
    WasmInterpreterTypedValue* args,
    WasmInterpreterTypedValue* out_result,
    void* user_data) {
  return WASM_ERROR;
}

static WasmResult on_import(uint32_t index,
                            uint32_t sig_index,
                            WasmStringSlice module_name,
                            WasmStringSlice function_name,
                            void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterImport* import = &ctx->module->imports.data[index];
  import->module_name = wasm_dup_string_slice(ctx->allocator, module_name);
  import->func_name = wasm_dup_string_slice(ctx->allocator, function_name);
  assert(sig_index < ctx->module->sigs.size);
  import->sig_index = sig_index;
  import->callback = trapping_import_callback;
  import->user_data = NULL;
  return WASM_OK;
}

static WasmResult on_function_signatures_count(uint32_t count,
                                               void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_func_array(ctx->allocator, &ctx->funcs, count);
  wasm_resize_uint32_vector_vector(ctx->allocator, &ctx->func_fixups, count);
  return WASM_OK;
}

static WasmResult on_function_signature(uint32_t index,
                                        uint32_t sig_index,
                                        void* user_data) {
  Context* ctx = user_data;
  assert(sig_index < ctx->module->sigs.size);
  InterpreterFunc* func = get_func(ctx, index);
  func->offset = WASM_INVALID_OFFSET;
  func->sig_index = sig_index;
  return WASM_OK;
}

static WasmResult on_table_limits(WasmBool has_max,
                                  uint32_t initial_elems,
                                  uint32_t max_elems,
                                  void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_func_table_entry_array(
      ctx->allocator, &ctx->module->func_table, initial_elems);
  return WASM_OK;
}

static WasmResult on_memory_limits(WasmBool has_max,
                                   uint32_t initial_pages,
                                   uint32_t max_pages,
                                   void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterMemory* memory = &ctx->module->memory;
  memory->allocator = ctx->memory_allocator;
  memory->page_size = initial_pages;
  memory->byte_size = initial_pages * WASM_PAGE_SIZE;
  memory->data = wasm_alloc_zero(ctx->memory_allocator, memory->byte_size,
                                 WASM_DEFAULT_ALIGN);
  return WASM_OK;
}

static WasmResult on_global_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_typed_value_array(ctx->allocator, &ctx->module->globals,
                                         count);
  return WASM_OK;
}

static WasmResult begin_global(uint32_t index, WasmType type, void* user_data) {
  Context* ctx = user_data;
  assert(index < ctx->module->globals.size);
  WasmInterpreterTypedValue* typed_value = &ctx->module->globals.data[index];
  typed_value->type = type;
  return WASM_OK;
}

static WasmResult end_global_init_expr(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  assert(index < ctx->module->globals.size);
  WasmInterpreterTypedValue* typed_value = &ctx->module->globals.data[index];
  *typed_value = ctx->init_expr_value;
  return WASM_OK;
}

static WasmResult on_init_expr_f32_const_expr(uint32_t index,
                                              uint32_t value_bits,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_F32;
  ctx->init_expr_value.value.f32_bits = value_bits;
  return WASM_OK;
}

static WasmResult on_init_expr_f64_const_expr(uint32_t index,
                                              uint64_t value_bits,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_F64;
  ctx->init_expr_value.value.f64_bits = value_bits;
  return WASM_OK;
}

static WasmResult on_init_expr_get_global_expr(uint32_t index,
                                               uint32_t global_index,
                                               void* user_data) {
  Context* ctx = user_data;
  assert(global_index < ctx->module->globals.size);
  WasmInterpreterTypedValue* ref_typed_value =
      &ctx->module->globals.data[global_index];
  ctx->init_expr_value = *ref_typed_value;
  return WASM_OK;
}

static WasmResult on_init_expr_i32_const_expr(uint32_t index,
                                              uint32_t value,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_I32;
  ctx->init_expr_value.value.i32 = value;
  return WASM_OK;
}

static WasmResult on_init_expr_i64_const_expr(uint32_t index,
                                              uint64_t value,
                                              void* user_data) {
  Context* ctx = user_data;
  ctx->init_expr_value.type = WASM_TYPE_I64;
  ctx->init_expr_value.value.i64 = value;
  return WASM_OK;
}

static WasmResult on_export_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  wasm_new_interpreter_export_array(ctx->allocator, &ctx->module->exports,
                                    count);
  return WASM_OK;
}

static WasmResult on_export(uint32_t index,
                            uint32_t func_index,
                            WasmStringSlice name,
                            void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterExport* export = &ctx->module->exports.data[index];
  InterpreterFunc* func = get_func(ctx, func_index);
  export->name = wasm_dup_string_slice(ctx->allocator, name);
  export->func_index = func_index;
  export->sig_index = func->sig_index;
  export->func_offset = WASM_INVALID_OFFSET;
  return WASM_OK;
}

static WasmResult on_start_function(uint32_t func_index, void* user_data) {
  Context* ctx = user_data;
  /* can't get the function offset yet, because we haven't parsed the
   * functions. Just store the function index and resolve it later in
   * end_function_bodies_section. */
  assert(func_index < ctx->funcs.size);
  ctx->start_func_index = func_index;
  return WASM_OK;
}

static WasmResult end_elem_segment_init_expr(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  assert(ctx->init_expr_value.type == WASM_TYPE_I32);
  ctx->table_offset = ctx->init_expr_value.value.i32;
  return WASM_OK;
}

static WasmResult on_elem_segment_function_index(uint32_t index,
                                                 uint32_t func_index,
                                                 void* user_data) {
  Context* ctx = user_data;
  assert(ctx->table_offset < ctx->module->func_table.size);
  assert(index < ctx->module->func_table.size);
  WasmInterpreterFuncTableEntry* entry =
      &ctx->module->func_table.data[ctx->table_offset++];
  InterpreterFunc* func = get_func(ctx, func_index);
  entry->sig_index = func->sig_index;
  entry->func_offset = func->offset;
  return WASM_OK;
}

static WasmResult on_data_segment_data(uint32_t index,
                                       const void* src_data,
                                       uint32_t size,
                                       void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterMemory* memory = &ctx->module->memory;
  assert(ctx->init_expr_value.type == WASM_TYPE_I32);
  uint32_t address = ctx->init_expr_value.value.i32;
  uint8_t* dst_data = memory->data;
  if ((uint64_t)address + (uint64_t)size > memory->byte_size)
    return WASM_ERROR;
  memcpy(&dst_data[address], src_data, size);
  return WASM_OK;
}

static WasmResult on_function_bodies_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  assert(count == ctx->funcs.size);
  WASM_USE(ctx);
  return WASM_OK;
}

static WasmResult type_mismatch(Context* ctx,
                                WasmType expected_type,
                                WasmType type,
                                const char* desc) {
  print_error(ctx, "type mismatch in %s, expected %s but got %s.", desc,
              s_type_names[expected_type], s_type_names[type]);
  return WASM_ERROR;
}

static WasmResult check_type(Context* ctx,
                             WasmType expected_type,
                             WasmType type,
                             const char* desc) {
  if (expected_type == WASM_TYPE_ANY || type == WASM_TYPE_ANY ||
      expected_type == WASM_TYPE_VOID) {
    return WASM_OK;
  }
  if (expected_type == type)
    return WASM_OK;
  return type_mismatch(ctx, expected_type, type, desc);
}

static void unify_type(WasmType* dest_type, WasmType type) {
  if (*dest_type == WASM_TYPE_ANY)
    *dest_type = type;
  else if (type != WASM_TYPE_ANY && *dest_type != type)
    *dest_type = WASM_TYPE_VOID;
}

static WasmResult unify_and_check_type(Context* ctx,
                                       WasmType* dest_type,
                                       WasmType type,
                                       const char* desc) {
  unify_type(dest_type, type);
  return check_type(ctx, *dest_type, type, desc);
}

static uint32_t translate_depth(Context* ctx, uint32_t depth) {
  assert(depth < ctx->label_stack.size);
  return ctx->label_stack.size - 1 - depth;
}

static void push_label(Context* ctx,
                       LabelType label_type,
                       WasmType type,
                       uint32_t offset,
                       uint32_t fixup_offset) {
  Label* label = wasm_append_label(ctx->allocator, &ctx->label_stack);
  label->label_type = label_type;
  label->type = type;
  label->expr_stack_size = ctx->expr_stack.size;
  label->offset = offset;
  label->fixup_offset = fixup_offset;
  LOGF("   : +depth %" PRIzd ":%s\n", ctx->label_stack.size - 1,
       s_type_names[type]);
}

static void pop_label(Context* ctx) {
  LOGF("   : -depth %" PRIzd "\n", ctx->label_stack.size - 1);
  assert(ctx->label_stack.size > 0);
  ctx->label_stack.size--;
  /* reduce the depth_fixups stack as well, but it may be smaller than
   * label_stack so only do it conditionally. */
  if (ctx->depth_fixups.size > ctx->label_stack.size) {
    uint32_t from = ctx->label_stack.size;
    uint32_t to = ctx->depth_fixups.size;
    uint32_t i;
    for (i = from; i < to; ++i)
      wasm_destroy_uint32_vector(ctx->allocator, &ctx->depth_fixups.data[i]);
    ctx->depth_fixups.size = ctx->label_stack.size;
  }
}

static void push_expr(Context* ctx, WasmType type, WasmOpcode opcode) {
  /* TODO: refactor to remove opcode param */
  if (type != WASM_TYPE_VOID) {
    Label* label = top_label(ctx);
    if (ctx->expr_stack.size > label->expr_stack_size) {
      WasmType top_type = ctx->expr_stack.data[ctx->expr_stack.size - 1];
      if (top_type == WASM_TYPE_ANY)
        return;
    }

    LOGF("%3" PRIzd ": push %s:%s\n", ctx->expr_stack.size,
         s_opcode_name[opcode], s_type_names[type]);
    wasm_append_type_value(ctx->allocator, &ctx->expr_stack, &type);
  }
}

static WasmType top_expr(Context* ctx) {
  Label* label = top_label(ctx);
  assert(ctx->expr_stack.size > label->expr_stack_size);
  return ctx->expr_stack.data[ctx->expr_stack.size - 1];
}

static WasmType pop_expr(Context* ctx) {
  WasmType type = top_expr(ctx);
  LOGF("%3" PRIzd ": pop  %s\n", ctx->expr_stack.size, s_type_names[type]);
  ctx->expr_stack.size--;
  return type;
}

static WasmType top_expr_if(Context* ctx, WasmBool cond) {
  if (cond)
    return top_expr(ctx);
  return WASM_TYPE_VOID;
}

static WasmResult check_end(Context* ctx, WasmType* top_type) {
  Label* label = top_label(ctx);
  assert(ctx->expr_stack.size >= label->expr_stack_size);
  size_t stack_size = ctx->expr_stack.size - label->expr_stack_size;
  if (stack_size == 0) {
    *top_type = WASM_TYPE_VOID;
    return WASM_OK;
  }

  WasmType type = top_expr(ctx);
  *top_type = type;
  if (type == WASM_TYPE_ANY || stack_size == 1)
    return WASM_OK;

  print_error(ctx, "maximum arity is 1, got %" PRIzd ".", stack_size);
  return WASM_ERROR;
}

static WasmResult begin_function_body(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  InterpreterFunc* func = get_func(ctx, index);
  WasmInterpreterFuncSignature* sig = get_signature(ctx, func->sig_index);

  func->offset = get_istream_offset(ctx);
  func->local_decl_count = 0;
  func->local_count = 0;

  ctx->current_func = func;
  ctx->depth_fixups.size = 0;
  ctx->expr_stack.size = sig->param_types.size;
  ctx->label_stack.size = 0;
  ctx->depth = 0;

  /* fixup function references */
  uint32_t i;
  Uint32Vector* fixups = &ctx->func_fixups.data[index];
  for (i = 0; i < fixups->size; ++i)
    CHECK_RESULT(emit_i32_at(ctx, fixups->data[i], func->offset));

  /* append param types */
  for (i = 0; i < sig->param_types.size; ++i) {
    wasm_append_type_value(ctx->allocator, &func->param_and_local_types,
                           &sig->param_types.data[i]);
  }

  /* push implicit func label (equivalent to return) */
  push_label(ctx, LABEL_TYPE_FUNC, sig->result_type, WASM_INVALID_OFFSET,
             WASM_INVALID_OFFSET);
  return WASM_OK;
}

static WasmResult end_function_body(uint32_t index, void* user_data) {
  Context* ctx = user_data;
  WasmInterpreterFuncSignature* sig =
      get_func_signature(ctx, ctx->current_func);

  WasmType top_type;
  CHECK_RESULT(check_end(ctx, &top_type));
  CHECK_RESULT(check_type(ctx, sig->result_type, top_type, "implicit return"));
  CHECK_RESULT(emit_return(ctx, sig->result_type));
  pop_label(ctx);
  ctx->current_func = NULL;
  ctx->expr_stack.size = 0;
  return WASM_OK;
}

static WasmResult on_local_decl_count(uint32_t count, void* user_data) {
  Context* ctx = user_data;
  InterpreterFunc* func = ctx->current_func;
  func->local_decl_count = count;
  return WASM_OK;
}

static WasmResult on_local_decl(uint32_t decl_index,
                                uint32_t count,
                                WasmType type,
                                void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": alloca\n", ctx->expr_stack.size);
  InterpreterFunc* func = ctx->current_func;
  func->local_count += count;

  uint32_t i;
  for (i = 0; i < count; ++i) {
    wasm_append_type_value(ctx->allocator, &func->param_and_local_types, &type);
    push_expr(ctx, type, WASM_OPCODE_ALLOCA);
  }

  if (decl_index == func->local_decl_count - 1) {
    /* last local declaration, allocate space for all locals. */
    CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_ALLOCA));
    CHECK_RESULT(emit_i32(ctx, func->local_count));
    /* fixup the function label's expr_stack_size to include these values. */
    Label* label = top_label(ctx);
    assert(label->label_type == LABEL_TYPE_FUNC);
    label->expr_stack_size += func->local_count;
  }
  return WASM_OK;
}

static WasmResult on_unary_expr(WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  const char* opcode_name = s_opcode_name[opcode];
  LOGF("%3" PRIzd ": %s\n", ctx->expr_stack.size, opcode_name);
  WasmType value = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, s_opcode_type1[opcode], value, opcode_name));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  push_expr(ctx, s_opcode_rtype[opcode], opcode);
  return WASM_OK;
}

static WasmResult on_binary_expr(WasmOpcode opcode, void* user_data) {
  Context* ctx = user_data;
  const char* opcode_name = s_opcode_name[opcode];
  LOGF("%3" PRIzd ": %s\n", ctx->expr_stack.size, opcode_name);
  WasmType right = pop_expr(ctx);
  WasmType left = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, s_opcode_type1[opcode], left, opcode_name));
  CHECK_RESULT(check_type(ctx, s_opcode_type2[opcode], right, opcode_name));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  push_expr(ctx, s_opcode_rtype[opcode], opcode);
  return WASM_OK;
}

static WasmResult on_block_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": block\n", ctx->expr_stack.size);
  push_label(ctx, LABEL_TYPE_BLOCK, WASM_TYPE_ANY, WASM_INVALID_OFFSET,
             WASM_INVALID_OFFSET);
  return WASM_OK;
}

static WasmResult on_loop_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": loop\n", ctx->expr_stack.size);
  push_label(ctx, LABEL_TYPE_LOOP, WASM_TYPE_VOID, get_istream_offset(ctx),
             WASM_INVALID_OFFSET);
  return WASM_OK;
}

static WasmResult on_if_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": if\n", ctx->expr_stack.size);
  WasmType cond = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, cond, "if"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR_UNLESS));
  uint32_t fixup_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  push_label(ctx, LABEL_TYPE_IF, WASM_TYPE_ANY, WASM_INVALID_OFFSET,
             fixup_offset);
  return WASM_OK;
}

static WasmResult on_else_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": else\n", ctx->expr_stack.size);
  Label* label = top_label(ctx);
  if (!label || label->label_type != LABEL_TYPE_IF) {
    print_error(ctx, "unexpected else operator");
    return WASM_ERROR;
  }

  WasmType top_type;
  CHECK_RESULT(check_end(ctx, &top_type));
  CHECK_RESULT(
      unify_and_check_type(ctx, &label->type, top_type, "if true branch"));

  label->label_type = LABEL_TYPE_ELSE;
  uint32_t fixup_cond_offset = label->fixup_offset;
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR));
  label->fixup_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  CHECK_RESULT(emit_i32_at(ctx, fixup_cond_offset, get_istream_offset(ctx)));
  /* reset the expr stack for the other branch arm */
  ctx->expr_stack.size = label->expr_stack_size;
  return WASM_OK;
}

static WasmResult on_end_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": end\n", ctx->expr_stack.size);
  Label* label = top_label(ctx);
  if (!label || label->label_type == LABEL_TYPE_FUNC) {
    print_error(ctx, "unexpected end operator");
    return WASM_ERROR;
  }

  if (label->label_type == LABEL_TYPE_IF)
    label->type = WASM_TYPE_VOID;

  WasmType top_type;
  CHECK_RESULT(check_end(ctx, &top_type));
  CHECK_RESULT(unify_and_check_type(ctx, &label->type, top_type,
                                    s_label_type_name[label->label_type]));

  if (label->label_type == LABEL_TYPE_IF ||
      label->label_type == LABEL_TYPE_ELSE) {
    uint32_t fixup_true_offset = label->fixup_offset;
    CHECK_RESULT(emit_i32_at(ctx, fixup_true_offset, get_istream_offset(ctx)));
  }

  fixup_top_label(ctx, get_istream_offset(ctx));
  ctx->expr_stack.size = label->expr_stack_size;
  pop_label(ctx);
  push_expr(ctx, label->type, WASM_OPCODE_END);
  return WASM_OK;
}

static WasmResult on_br_expr(uint8_t arity, uint32_t depth, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": br\n", ctx->expr_stack.size);
  WasmType value = top_expr_if(ctx, arity > 0);
  CHECK_DEPTH(ctx, depth);
  depth = translate_depth(ctx, depth);
  Label* label = get_label(ctx, depth);
  CHECK_RESULT(unify_and_check_type(ctx, &label->type, value, "br"));
  CHECK_RESULT(emit_br(ctx, arity, depth));
  push_expr(ctx, WASM_TYPE_ANY, WASM_OPCODE_BR);
  return WASM_OK;
}

static WasmResult on_br_if_expr(uint8_t arity,
                                uint32_t depth,
                                void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": br_if\n", ctx->expr_stack.size);
  WasmType cond = pop_expr(ctx);
  WasmType value = top_expr_if(ctx, arity > 0);
  CHECK_DEPTH(ctx, depth);
  depth = translate_depth(ctx, depth);
  Label* label = get_label(ctx, depth);
  CHECK_RESULT(unify_and_check_type(ctx, &label->type, value, "br_if"));
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, cond, "br_if"));
  /* flip the br_if so if <cond> is true it can drop values from the stack */
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR_UNLESS));
  uint32_t fixup_br_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  CHECK_RESULT(emit_br(ctx, arity, depth));
  CHECK_RESULT(emit_i32_at(ctx, fixup_br_offset, get_istream_offset(ctx)));
  /* clean up the value (if any), when the branch isn't taken */
  CHECK_RESULT(emit_drop_keep(ctx, arity, 0));
  ctx->expr_stack.size -= arity;
  push_expr(ctx, WASM_TYPE_VOID, WASM_OPCODE_BR_IF);
  return WASM_OK;
}

static WasmResult on_br_table_expr(uint8_t arity,
                                   uint32_t num_targets,
                                   uint32_t* target_depths,
                                   uint32_t default_target_depth,
                                   void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": br_table\n", ctx->expr_stack.size);
  WasmType key = pop_expr(ctx);
  WasmType value = top_expr_if(ctx, arity > 0);
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, key, "br_table"));

  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_BR_TABLE));
  CHECK_RESULT(emit_i32(ctx, num_targets));
  uint32_t fixup_table_offset = get_istream_offset(ctx);
  CHECK_RESULT(emit_i32(ctx, WASM_INVALID_OFFSET));
  /* not necessary for the interpreter, but it makes it easier to disassemble.
   * This opcode specifies how many bytes of data follow. */
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DATA));
  CHECK_RESULT(emit_i32(ctx, (num_targets + 1) * WASM_TABLE_ENTRY_SIZE));
  CHECK_RESULT(emit_i32_at(ctx, fixup_table_offset, get_istream_offset(ctx)));

  uint32_t i;
  for (i = 0; i <= num_targets; ++i) {
    uint32_t depth = i != num_targets ? target_depths[i] : default_target_depth;
    depth = translate_depth(ctx, depth);
    Label* label = get_label(ctx, depth);
    CHECK_RESULT(unify_and_check_type(ctx, &label->type, value, "br_table"));
    CHECK_RESULT(emit_br_table_offset(ctx, arity, depth));
  }

  push_expr(ctx, WASM_TYPE_ANY, WASM_OPCODE_BR_TABLE);
  return WASM_OK;
}

static WasmResult on_call_expr(uint32_t func_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": call\n", ctx->expr_stack.size);
  assert(func_index < ctx->funcs.size);
  InterpreterFunc* func = get_func(ctx, func_index);
  WasmInterpreterFuncSignature* sig = get_func_signature(ctx, func);

  uint32_t i;
  for (i = sig->param_types.size; i > 0; --i) {
    WasmType arg = pop_expr(ctx);
    CHECK_RESULT(check_type(ctx, sig->param_types.data[i - 1], arg, "call"));
  }

  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CALL_FUNCTION));
  CHECK_RESULT(emit_func_offset(ctx, func, func_index));
  push_expr(ctx, sig->result_type, WASM_OPCODE_CALL_FUNCTION);
  return WASM_OK;
}

static WasmResult on_call_import_expr(uint32_t import_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": call_import\n", ctx->expr_stack.size);
  assert(import_index < ctx->module->imports.size);
  WasmInterpreterImport* import = get_import(ctx, import_index);
  WasmInterpreterFuncSignature* sig = get_signature(ctx, import->sig_index);

  uint32_t i;
  for (i = sig->param_types.size; i > 0; --i) {
    WasmType arg = pop_expr(ctx);
    CHECK_RESULT(
        check_type(ctx, sig->param_types.data[i - 1], arg, "call_import"));
  }

  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CALL_IMPORT));
  CHECK_RESULT(emit_i32(ctx, import_index));
  push_expr(ctx, sig->result_type, WASM_OPCODE_CALL_IMPORT);
  return WASM_OK;
}

static WasmResult on_call_indirect_expr(uint32_t sig_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": call_indirect\n", ctx->expr_stack.size);
  WasmInterpreterFuncSignature* sig = get_signature(ctx, sig_index);
  WasmType entry_index = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, entry_index, "call_indirect"));

  uint32_t i;
  for (i = sig->param_types.size; i > 0; --i) {
    WasmType arg = pop_expr(ctx);
    CHECK_RESULT(
        check_type(ctx, sig->param_types.data[i - 1], arg, "call_indirect"));
  }

  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CALL_INDIRECT));
  CHECK_RESULT(emit_i32(ctx, sig_index));
  push_expr(ctx, sig->result_type, WASM_OPCODE_CALL_INDIRECT);
  return WASM_OK;
}

static WasmResult on_drop_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": drop\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_DROP));
  pop_expr(ctx);
  return WASM_OK;
}

static WasmResult on_i32_const_expr(uint32_t value, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": i32.const\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_I32_CONST));
  CHECK_RESULT(emit_i32(ctx, value));
  push_expr(ctx, WASM_TYPE_I32, WASM_OPCODE_I32_CONST);
  return WASM_OK;
}

static WasmResult on_i64_const_expr(uint64_t value, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": i64.const\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_I64_CONST));
  CHECK_RESULT(emit_i64(ctx, value));
  push_expr(ctx, WASM_TYPE_I64, WASM_OPCODE_I64_CONST);
  return WASM_OK;
}

static WasmResult on_f32_const_expr(uint32_t value_bits, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": f32.const\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_F32_CONST));
  CHECK_RESULT(emit_i32(ctx, value_bits));
  push_expr(ctx, WASM_TYPE_F32, WASM_OPCODE_F32_CONST);
  return WASM_OK;
}

static WasmResult on_f64_const_expr(uint64_t value_bits, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": f64.const\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_F64_CONST));
  CHECK_RESULT(emit_i64(ctx, value_bits));
  push_expr(ctx, WASM_TYPE_F64, WASM_OPCODE_F64_CONST);
  return WASM_OK;
}

static WasmResult on_get_global_expr(uint32_t global_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": get_global\n", ctx->expr_stack.size);
  CHECK_GLOBAL(ctx, global_index);
  WasmType type = get_global_index_type(ctx, global_index);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_GET_GLOBAL));
  CHECK_RESULT(emit_i32(ctx, global_index));
  push_expr(ctx, type, WASM_OPCODE_GET_GLOBAL);
  return WASM_OK;
}

static WasmResult on_set_global_expr(uint32_t global_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": set_global\n", ctx->expr_stack.size);
  CHECK_GLOBAL(ctx, global_index);
  WasmType type = get_global_index_type(ctx, global_index);
  WasmType value = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, type, value, "set_global"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_SET_GLOBAL));
  CHECK_RESULT(emit_i32(ctx, global_index));
  return WASM_OK;
}

static WasmResult on_get_local_expr(uint32_t local_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": get_local\n", ctx->expr_stack.size);
  CHECK_LOCAL(ctx, local_index);
  WasmType type = get_local_index_type(ctx->current_func, local_index);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_GET_LOCAL));
  CHECK_RESULT(emit_i32(ctx, translate_local_index(ctx, local_index)));
  push_expr(ctx, type, WASM_OPCODE_GET_LOCAL);
  return WASM_OK;
}

static WasmResult on_set_local_expr(uint32_t local_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": set_local\n", ctx->expr_stack.size);
  CHECK_LOCAL(ctx, local_index);
  WasmType type = get_local_index_type(ctx->current_func, local_index);
  WasmType value = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, type, value, "set_local"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_SET_LOCAL));
  CHECK_RESULT(emit_i32(ctx, translate_local_index(ctx, local_index)));
  return WASM_OK;
}

static WasmResult on_tee_local_expr(uint32_t local_index, void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": tee_local\n", ctx->expr_stack.size);
  CHECK_LOCAL(ctx, local_index);
  WasmType type = get_local_index_type(ctx->current_func, local_index);
  WasmType value = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, type, value, "tee_local"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_TEE_LOCAL));
  CHECK_RESULT(emit_i32(ctx, translate_local_index(ctx, local_index)));
  return WASM_OK;
}

static WasmResult on_grow_memory_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": grow_memory\n", ctx->expr_stack.size);
  WasmType value = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, value, "grow_memory"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_GROW_MEMORY));
  push_expr(ctx, WASM_TYPE_I32, WASM_OPCODE_GROW_MEMORY);
  return WASM_OK;
}

static WasmResult on_load_expr(WasmOpcode opcode,
                               uint32_t alignment_log2,
                               uint32_t offset,
                               void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": %s\n", ctx->expr_stack.size, s_opcode_name[opcode]);
  WasmType addr = pop_expr(ctx);
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, addr, s_opcode_name[opcode]));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  CHECK_RESULT(emit_i32(ctx, offset));
  push_expr(ctx, s_opcode_rtype[opcode], opcode);
  return WASM_OK;
}

static WasmResult on_store_expr(WasmOpcode opcode,
                                uint32_t alignment_log2,
                                uint32_t offset,
                                void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": %s\n", ctx->expr_stack.size, s_opcode_name[opcode]);
  WasmType value = pop_expr(ctx);
  WasmType addr = pop_expr(ctx);
  WasmType type = s_opcode_rtype[opcode];
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, addr, s_opcode_name[opcode]));
  CHECK_RESULT(check_type(ctx, type, value, s_opcode_name[opcode]));
  CHECK_RESULT(emit_opcode(ctx, opcode));
  CHECK_RESULT(emit_i32(ctx, offset));
  push_expr(ctx, type, opcode);
  return WASM_OK;
}

static WasmResult on_current_memory_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": current_memory\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_CURRENT_MEMORY));
  push_expr(ctx, WASM_TYPE_I32, WASM_OPCODE_CURRENT_MEMORY);
  return WASM_OK;
}

static WasmResult on_nop_expr(void* user_data) {
  Context* ctx = user_data;
  WASM_USE(ctx);
  LOGF("%3" PRIzd ": nop\n", ctx->expr_stack.size);
  return WASM_OK;
}

static WasmResult on_return_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": return\n", ctx->expr_stack.size);
  WasmInterpreterFuncSignature* sig =
      get_func_signature(ctx, ctx->current_func);
  if (get_value_count(sig->result_type)) {
    WasmType value = top_expr(ctx);
    CHECK_RESULT(check_type(ctx, sig->result_type, value, "return"));
  }
  CHECK_RESULT(emit_return(ctx, sig->result_type));
  push_expr(ctx, WASM_TYPE_ANY, WASM_OPCODE_RETURN);
  return WASM_OK;
}

static WasmResult on_select_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": select\n", ctx->expr_stack.size);
  WasmType cond = pop_expr(ctx);
  WasmType right = pop_expr(ctx);
  WasmType left = pop_expr(ctx);
  WasmType type = WASM_TYPE_ANY;
  CHECK_RESULT(unify_and_check_type(ctx, &type, left, "select"));
  CHECK_RESULT(unify_and_check_type(ctx, &type, right, "select"));
  CHECK_RESULT(check_type(ctx, WASM_TYPE_I32, cond, "select"));
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_SELECT));
  push_expr(ctx, type, WASM_OPCODE_SELECT);
  return WASM_OK;
}

static WasmResult on_unreachable_expr(void* user_data) {
  Context* ctx = user_data;
  LOGF("%3" PRIzd ": unreachable\n", ctx->expr_stack.size);
  CHECK_RESULT(emit_opcode(ctx, WASM_OPCODE_UNREACHABLE));
  push_expr(ctx, WASM_TYPE_ANY, WASM_OPCODE_UNREACHABLE);
  return WASM_OK;
}

static WasmResult end_function_bodies_section(void* user_data) {
  Context* ctx = user_data;

  /* resolve the start function offset */
  if (ctx->start_func_index != INVALID_FUNC_INDEX) {
    InterpreterFunc* func = get_func(ctx, ctx->start_func_index);
    assert(func->offset != WASM_INVALID_OFFSET);
    ctx->module->start_func_offset = func->offset;
  }

  /* resolve the export function offsets */
  uint32_t i;
  for (i = 0; i < ctx->module->exports.size; ++i) {
    WasmInterpreterExport* export = get_export(ctx, i);
    InterpreterFunc* func = get_func(ctx, export->func_index);
    export->func_offset = func->offset;
  }
  return WASM_OK;
}

static WasmBinaryReader s_binary_reader = {
    .user_data = NULL,
    .on_error = on_error,

    .on_signature_count = on_signature_count,
    .on_signature = on_signature,

    .on_import_count = on_import_count,
    .on_import = on_import,

    .on_function_signatures_count = on_function_signatures_count,
    .on_function_signature = on_function_signature,

    .on_table_limits = on_table_limits,

    .on_memory_limits = on_memory_limits,

    .on_global_count = on_global_count,
    .begin_global = begin_global,
    .end_global_init_expr = end_global_init_expr,

    .on_export_count = on_export_count,
    .on_export = on_export,

    .on_start_function = on_start_function,

    .on_function_bodies_count = on_function_bodies_count,
    .begin_function_body = begin_function_body,
    .on_local_decl_count = on_local_decl_count,
    .on_local_decl = on_local_decl,
    .on_binary_expr = on_binary_expr,
    .on_block_expr = on_block_expr,
    .on_br_expr = on_br_expr,
    .on_br_if_expr = on_br_if_expr,
    .on_br_table_expr = on_br_table_expr,
    .on_call_expr = on_call_expr,
    .on_call_import_expr = on_call_import_expr,
    .on_call_indirect_expr = on_call_indirect_expr,
    .on_compare_expr = on_binary_expr,
    .on_convert_expr = on_unary_expr,
    .on_current_memory_expr = on_current_memory_expr,
    .on_drop_expr = on_drop_expr,
    .on_else_expr = on_else_expr,
    .on_end_expr = on_end_expr,
    .on_f32_const_expr = on_f32_const_expr,
    .on_f64_const_expr = on_f64_const_expr,
    .on_get_global_expr = on_get_global_expr,
    .on_get_local_expr = on_get_local_expr,
    .on_grow_memory_expr = on_grow_memory_expr,
    .on_i32_const_expr = on_i32_const_expr,
    .on_i64_const_expr = on_i64_const_expr,
    .on_if_expr = on_if_expr,
    .on_load_expr = on_load_expr,
    .on_loop_expr = on_loop_expr,
    .on_nop_expr = on_nop_expr,
    .on_return_expr = on_return_expr,
    .on_select_expr = on_select_expr,
    .on_set_global_expr = on_set_global_expr,
    .on_set_local_expr = on_set_local_expr,
    .on_store_expr = on_store_expr,
    .on_tee_local_expr = on_tee_local_expr,
    .on_unary_expr = on_unary_expr,
    .on_unreachable_expr = on_unreachable_expr,
    .end_function_body = end_function_body,
    .end_function_bodies_section = end_function_bodies_section,

    .end_elem_segment_init_expr = end_elem_segment_init_expr,
    .on_elem_segment_function_index = on_elem_segment_function_index,

    .on_data_segment_data = on_data_segment_data,

    .on_init_expr_f32_const_expr = on_init_expr_f32_const_expr,
    .on_init_expr_f64_const_expr = on_init_expr_f64_const_expr,
    .on_init_expr_get_global_expr = on_init_expr_get_global_expr,
    .on_init_expr_i32_const_expr = on_init_expr_i32_const_expr,
    .on_init_expr_i64_const_expr = on_init_expr_i64_const_expr,
};

static void wasm_destroy_interpreter_func(WasmAllocator* allocator,
                                          InterpreterFunc* func) {
  wasm_destroy_type_vector(allocator, &func->param_and_local_types);
}

static void destroy_context(Context* ctx) {
  wasm_destroy_type_vector(ctx->allocator, &ctx->expr_stack);
  wasm_destroy_label_vector(ctx->allocator, &ctx->label_stack);
  WASM_DESTROY_ARRAY_AND_ELEMENTS(ctx->allocator, ctx->funcs, interpreter_func);
  WASM_DESTROY_VECTOR_AND_ELEMENTS(ctx->allocator, ctx->depth_fixups,
                                   uint32_vector);
  WASM_DESTROY_VECTOR_AND_ELEMENTS(ctx->allocator, ctx->func_fixups,
                                   uint32_vector);
}

WasmResult wasm_read_binary_interpreter(WasmAllocator* allocator,
                                        WasmAllocator* memory_allocator,
                                        const void* data,
                                        size_t size,
                                        const WasmReadBinaryOptions* options,
                                        WasmBinaryErrorHandler* error_handler,
                                        WasmInterpreterModule* out_module) {
  Context ctx;
  WasmBinaryReader reader;

  WASM_ZERO_MEMORY(ctx);
  WASM_ZERO_MEMORY(reader);

  ctx.allocator = allocator;
  ctx.reader = &reader;
  ctx.error_handler = error_handler;
  ctx.memory_allocator = memory_allocator;
  ctx.module = out_module;
  ctx.start_func_index = INVALID_FUNC_INDEX;
  ctx.module->start_func_offset = WASM_INVALID_OFFSET;
  CHECK_RESULT(wasm_init_mem_writer(allocator, &ctx.istream_writer));

  reader = s_binary_reader;
  reader.user_data = &ctx;

  const uint32_t num_function_passes = 1;
  WasmResult result = wasm_read_binary(allocator, data, size, &reader,
                                       num_function_passes, options);
  if (WASM_SUCCEEDED(result)) {
    wasm_steal_mem_writer_output_buffer(&ctx.istream_writer,
                                        &out_module->istream);
    out_module->istream.size = ctx.istream_offset;
  } else {
    wasm_close_mem_writer(&ctx.istream_writer);
  }
  destroy_context(&ctx);
  return result;
}
