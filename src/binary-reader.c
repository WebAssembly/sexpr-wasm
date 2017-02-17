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

#include "binary-reader.h"

#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "binary.h"
#include "config.h"
#include "stream.h"
#include "vector.h"

#if HAVE_ALLOCA
#include <alloca.h>
#endif

#define INDENT_SIZE 2

#define INITIAL_PARAM_TYPES_CAPACITY 128
#define INITIAL_BR_TABLE_TARGET_CAPACITY 1000

typedef uint32_t Uint32;
WABT_DEFINE_VECTOR(type, WabtType)
WABT_DEFINE_VECTOR(uint32, Uint32);

#define CALLBACK_CTX(member, ...)                                       \
  RAISE_ERROR_UNLESS(                                                   \
      WABT_SUCCEEDED(                                                   \
          ctx->reader->member                                           \
              ? ctx->reader->member(get_user_context(ctx), __VA_ARGS__) \
              : WABT_OK),                                               \
      #member " callback failed")

#define CALLBACK_CTX0(member)                                         \
  RAISE_ERROR_UNLESS(                                                 \
      WABT_SUCCEEDED(ctx->reader->member                              \
                         ? ctx->reader->member(get_user_context(ctx)) \
                         : WABT_OK),                                  \
      #member " callback failed")

#define CALLBACK_SECTION(member, section_size) \
  CALLBACK_CTX(member, section_size)

#define CALLBACK0(member)                                              \
  RAISE_ERROR_UNLESS(                                                  \
      WABT_SUCCEEDED(ctx->reader->member                               \
                         ? ctx->reader->member(ctx->reader->user_data) \
                         : WABT_OK),                                   \
      #member " callback failed")

#define CALLBACK(member, ...)                                            \
  RAISE_ERROR_UNLESS(                                                    \
      WABT_SUCCEEDED(                                                    \
          ctx->reader->member                                            \
              ? ctx->reader->member(__VA_ARGS__, ctx->reader->user_data) \
              : WABT_OK),                                                \
      #member " callback failed")

#define FORWARD0(member)                                                   \
  return ctx->reader->member ? ctx->reader->member(ctx->reader->user_data) \
                             : WABT_OK

#define FORWARD_CTX0(member)                  \
  if (!ctx->reader->member)                   \
    return WABT_OK;                           \
  WabtBinaryReaderContext new_ctx = *context; \
  new_ctx.user_data = ctx->reader->user_data; \
  return ctx->reader->member(&new_ctx);

#define FORWARD_CTX(member, ...)              \
  if (!ctx->reader->member)                   \
    return WABT_OK;                           \
  WabtBinaryReaderContext new_ctx = *context; \
  new_ctx.user_data = ctx->reader->user_data; \
  return ctx->reader->member(&new_ctx, __VA_ARGS__);

#define FORWARD(member, ...)                                            \
  return ctx->reader->member                                            \
             ? ctx->reader->member(__VA_ARGS__, ctx->reader->user_data) \
             : WABT_OK

#define RAISE_ERROR(...) \
  (ctx->reader->on_error ? raise_error(ctx, __VA_ARGS__) : (void)0)

#define RAISE_ERROR_UNLESS(cond, ...) \
  if (!(cond))                        \
    RAISE_ERROR(__VA_ARGS__);

typedef struct Context {
  const uint8_t* data;
  size_t data_size;
  size_t offset;
  size_t read_end; /* Either the section end or data_size. */
  WabtBinaryReaderContext user_ctx;
  WabtBinaryReader* reader;
  jmp_buf error_jmp_buf;
  WabtTypeVector param_types;
  Uint32Vector target_depths;
  const WabtReadBinaryOptions* options;
  WabtBinarySection last_known_section_code;
  uint32_t num_signatures;
  uint32_t num_imports;
  uint32_t num_func_imports;
  uint32_t num_table_imports;
  uint32_t num_memory_imports;
  uint32_t num_global_imports;
  uint32_t num_function_signatures;
  uint32_t num_tables;
  uint32_t num_memories;
  uint32_t num_globals;
  uint32_t num_exports;
  uint32_t num_function_bodies;
} Context;

typedef struct LoggingContext {
  WabtStream* stream;
  WabtBinaryReader* reader;
  int indent;
} LoggingContext;

static WabtBinaryReaderContext* get_user_context(Context* ctx) {
  ctx->user_ctx.user_data = ctx->reader->user_data;
  ctx->user_ctx.data = ctx->data;
  ctx->user_ctx.size = ctx->data_size;
  ctx->user_ctx.offset = ctx->offset;
  return &ctx->user_ctx;
}

static void WABT_PRINTF_FORMAT(2, 3)
    raise_error(Context* ctx, const char* format, ...) {
  WABT_SNPRINTF_ALLOCA(buffer, length, format);
  assert(ctx->reader->on_error);
  ctx->reader->on_error(get_user_context(ctx), buffer);
  longjmp(ctx->error_jmp_buf, 1);
}

#define IN_SIZE(type)                                       \
  if (ctx->offset + sizeof(type) > ctx->read_end) {         \
    RAISE_ERROR("unable to read " #type ": %s", desc);      \
  }                                                         \
  memcpy(out_value, ctx->data + ctx->offset, sizeof(type)); \
  ctx->offset += sizeof(type)

static void in_u8(Context* ctx, uint8_t* out_value, const char* desc) {
  IN_SIZE(uint8_t);
}

static void in_u32(Context* ctx, uint32_t* out_value, const char* desc) {
  IN_SIZE(uint32_t);
}

static void in_f32(Context* ctx, uint32_t* out_value, const char* desc) {
  IN_SIZE(float);
}

static void in_f64(Context* ctx, uint64_t* out_value, const char* desc) {
  IN_SIZE(double);
}

#undef IN_SIZE

#define BYTE_AT(type, i, shift) (((type)p[i] & 0x7f) << (shift))

#define LEB128_1(type) (BYTE_AT(type, 0, 0))
#define LEB128_2(type) (BYTE_AT(type, 1, 7) | LEB128_1(type))
#define LEB128_3(type) (BYTE_AT(type, 2, 14) | LEB128_2(type))
#define LEB128_4(type) (BYTE_AT(type, 3, 21) | LEB128_3(type))
#define LEB128_5(type) (BYTE_AT(type, 4, 28) | LEB128_4(type))
#define LEB128_6(type) (BYTE_AT(type, 5, 35) | LEB128_5(type))
#define LEB128_7(type) (BYTE_AT(type, 6, 42) | LEB128_6(type))
#define LEB128_8(type) (BYTE_AT(type, 7, 49) | LEB128_7(type))
#define LEB128_9(type) (BYTE_AT(type, 8, 56) | LEB128_8(type))
#define LEB128_10(type) (BYTE_AT(type, 9, 63) | LEB128_9(type))

#define SHIFT_AMOUNT(type, sign_bit) (sizeof(type) * 8 - 1 - (sign_bit))
#define SIGN_EXTEND(type, value, sign_bit)            \
  ((type)((value) << SHIFT_AMOUNT(type, sign_bit)) >> \
   SHIFT_AMOUNT(type, sign_bit))

size_t wabt_read_u32_leb128(const uint8_t* p,
                            const uint8_t* end,
                            uint32_t* out_value) {
  if (p < end && (p[0] & 0x80) == 0) {
    *out_value = LEB128_1(uint32_t);
    return 1;
  } else if (p + 1 < end && (p[1] & 0x80) == 0) {
    *out_value = LEB128_2(uint32_t);
    return 2;
  } else if (p + 2 < end && (p[2] & 0x80) == 0) {
    *out_value = LEB128_3(uint32_t);
    return 3;
  } else if (p + 3 < end && (p[3] & 0x80) == 0) {
    *out_value = LEB128_4(uint32_t);
    return 4;
  } else if (p + 4 < end && (p[4] & 0x80) == 0) {
    /* the top bits set represent values > 32 bits */
    if (p[4] & 0xf0)
      return 0;
    *out_value = LEB128_5(uint32_t);
    return 5;
  } else {
    /* past the end */
    *out_value = 0;
    return 0;
  }
}

static void in_u32_leb128(Context* ctx, uint32_t* out_value, const char* desc) {
  const uint8_t* p = ctx->data + ctx->offset;
  const uint8_t* end = ctx->data + ctx->read_end;
  size_t bytes_read = wabt_read_u32_leb128(p, end, out_value);
  if (!bytes_read)
    RAISE_ERROR("unable to read u32 leb128: %s", desc);
  ctx->offset += bytes_read;
}

size_t wabt_read_i32_leb128(const uint8_t* p,
                            const uint8_t* end,
                            uint32_t* out_value) {
  if (p < end && (p[0] & 0x80) == 0) {
    uint32_t result = LEB128_1(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 6);
    return 1;
  } else if (p + 1 < end && (p[1] & 0x80) == 0) {
    uint32_t result = LEB128_2(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 13);
    return 2;
  } else if (p + 2 < end && (p[2] & 0x80) == 0) {
    uint32_t result = LEB128_3(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 20);
    return 3;
  } else if (p + 3 < end && (p[3] & 0x80) == 0) {
    uint32_t result = LEB128_4(uint32_t);
    *out_value = SIGN_EXTEND(int32_t, result, 27);
    return 4;
  } else if (p + 4 < end && (p[4] & 0x80) == 0) {
    /* the top bits should be a sign-extension of the sign bit */
    WabtBool sign_bit_set = (p[4] & 0x8);
    int top_bits = p[4] & 0xf0;
    if ((sign_bit_set && top_bits != 0x70) ||
        (!sign_bit_set && top_bits != 0)) {
      return 0;
    }
    uint32_t result = LEB128_5(uint32_t);
    *out_value = result;
    return 5;
  } else {
    /* past the end */
    return 0;
  }
}

static void in_i32_leb128(Context* ctx, uint32_t* out_value, const char* desc) {
  const uint8_t* p = ctx->data + ctx->offset;
  const uint8_t* end = ctx->data + ctx->read_end;
  size_t bytes_read = wabt_read_i32_leb128(p, end, out_value);
  if (!bytes_read)
    RAISE_ERROR("unable to read i32 leb128: %s", desc);
  ctx->offset += bytes_read;
}

static void in_i64_leb128(Context* ctx, uint64_t* out_value, const char* desc) {
  const uint8_t* p = ctx->data + ctx->offset;
  const uint8_t* end = ctx->data + ctx->read_end;

  if (p < end && (p[0] & 0x80) == 0) {
    uint64_t result = LEB128_1(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 6);
    ctx->offset += 1;
  } else if (p + 1 < end && (p[1] & 0x80) == 0) {
    uint64_t result = LEB128_2(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 13);
    ctx->offset += 2;
  } else if (p + 2 < end && (p[2] & 0x80) == 0) {
    uint64_t result = LEB128_3(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 20);
    ctx->offset += 3;
  } else if (p + 3 < end && (p[3] & 0x80) == 0) {
    uint64_t result = LEB128_4(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 27);
    ctx->offset += 4;
  } else if (p + 4 < end && (p[4] & 0x80) == 0) {
    uint64_t result = LEB128_5(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 34);
    ctx->offset += 5;
  } else if (p + 5 < end && (p[5] & 0x80) == 0) {
    uint64_t result = LEB128_6(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 41);
    ctx->offset += 6;
  } else if (p + 6 < end && (p[6] & 0x80) == 0) {
    uint64_t result = LEB128_7(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 48);
    ctx->offset += 7;
  } else if (p + 7 < end && (p[7] & 0x80) == 0) {
    uint64_t result = LEB128_8(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 55);
    ctx->offset += 8;
  } else if (p + 8 < end && (p[8] & 0x80) == 0) {
    uint64_t result = LEB128_9(uint64_t);
    *out_value = SIGN_EXTEND(int64_t, result, 62);
    ctx->offset += 9;
  } else if (p + 9 < end && (p[9] & 0x80) == 0) {
    /* the top bits should be a sign-extension of the sign bit */
    WabtBool sign_bit_set = (p[9] & 0x1);
    int top_bits = p[9] & 0xfe;
    if ((sign_bit_set && top_bits != 0x7e) ||
        (!sign_bit_set && top_bits != 0)) {
      RAISE_ERROR("invalid i64 leb128: %s", desc);
    }
    uint64_t result = LEB128_10(uint64_t);
    *out_value = result;
    ctx->offset += 10;
  } else {
    /* past the end */
    RAISE_ERROR("unable to read i64 leb128: %s", desc);
  }
}

#undef BYTE_AT
#undef LEB128_1
#undef LEB128_2
#undef LEB128_3
#undef LEB128_4
#undef LEB128_5
#undef LEB128_6
#undef LEB128_7
#undef LEB128_8
#undef LEB128_9
#undef LEB128_10
#undef SHIFT_AMOUNT
#undef SIGN_EXTEND

static void in_type(Context* ctx, WabtType* out_value, const char* desc) {
  uint32_t type = 0;
  in_i32_leb128(ctx, &type, desc);
  /* Must be in the vs7 range: [-128, 127). */
  if ((int32_t)type < -128 || (int32_t)type > 127)
    RAISE_ERROR("invalid type: %d", type);
  *out_value = type;
}

static void in_str(Context* ctx, WabtStringSlice* out_str, const char* desc) {
  uint32_t str_len = 0;
  in_u32_leb128(ctx, &str_len, "string length");

  if (ctx->offset + str_len > ctx->read_end)
    RAISE_ERROR("unable to read string: %s", desc);

  out_str->start = (const char*)ctx->data + ctx->offset;
  out_str->length = str_len;
  ctx->offset += str_len;
}

static void in_bytes(Context* ctx,
                     const void** out_data,
                     uint32_t* out_data_size,
                     const char* desc) {
  uint32_t data_size = 0;
  in_u32_leb128(ctx, &data_size, "data size");

  if (ctx->offset + data_size > ctx->read_end)
    RAISE_ERROR("unable to read data: %s", desc);

  *out_data = (const uint8_t*)ctx->data + ctx->offset;
  *out_data_size = data_size;
  ctx->offset += data_size;
}

static WabtBool is_valid_external_kind(uint8_t kind) {
  return kind < WABT_NUM_EXTERNAL_KINDS;
}

static WabtBool is_concrete_type(WabtType type) {
  switch (type) {
    case WABT_TYPE_I32:
    case WABT_TYPE_I64:
    case WABT_TYPE_F32:
    case WABT_TYPE_F64:
      return WABT_TRUE;

    default:
      return WABT_FALSE;
  }
}

static WabtBool is_inline_sig_type(WabtType type) {
  return is_concrete_type(type) || type == WABT_TYPE_VOID;
}

static uint32_t num_total_funcs(Context* ctx) {
  return ctx->num_func_imports + ctx->num_function_signatures;
}

static uint32_t num_total_tables(Context* ctx) {
  return ctx->num_table_imports + ctx->num_tables;
}

static uint32_t num_total_memories(Context* ctx) {
  return ctx->num_memory_imports + ctx->num_memories;
}

static uint32_t num_total_globals(Context* ctx) {
  return ctx->num_global_imports + ctx->num_globals;
}

static void destroy_context(Context* ctx) {
  wabt_destroy_type_vector(&ctx->param_types);
  wabt_destroy_uint32_vector(&ctx->target_depths);
}

/* Logging */

static void indent(LoggingContext* ctx) {
  ctx->indent += INDENT_SIZE;
}

static void dedent(LoggingContext* ctx) {
  ctx->indent -= INDENT_SIZE;
  assert(ctx->indent >= 0);
}

static void write_indent(LoggingContext* ctx) {
  static char s_indent[] =
      "                                                                       "
      "                                                                       ";
  static size_t s_indent_len = sizeof(s_indent) - 1;
  size_t indent = ctx->indent;
  while (indent > s_indent_len) {
    wabt_write_data(ctx->stream, s_indent, s_indent_len, NULL);
    indent -= s_indent_len;
  }
  if (indent > 0) {
    wabt_write_data(ctx->stream, s_indent, indent, NULL);
  }
}

#define LOGF_NOINDENT(...) wabt_writef(ctx->stream, __VA_ARGS__)

#define LOGF(...)               \
  do {                          \
    write_indent(ctx);          \
    LOGF_NOINDENT(__VA_ARGS__); \
  } while (0)

static void logging_on_error(WabtBinaryReaderContext* ctx,
                             const char* message) {
  LoggingContext* logging_ctx = ctx->user_data;
  if (logging_ctx->reader->on_error) {
    WabtBinaryReaderContext new_ctx = *ctx;
    new_ctx.user_data = logging_ctx->reader->user_data;
    logging_ctx->reader->on_error(&new_ctx, message);
  }
}

static WabtResult logging_begin_custom_section(WabtBinaryReaderContext* context,
                                               uint32_t size,
                                               WabtStringSlice section_name) {
  LoggingContext* ctx = context->user_data;
  LOGF("begin_custom_section: '" PRIstringslice "' size=%d\n",
       WABT_PRINTF_STRING_SLICE_ARG(section_name), size);
  indent(ctx);
  FORWARD_CTX(begin_custom_section, size, section_name);
}

#define LOGGING_BEGIN(name)                                                \
  static WabtResult logging_begin_##name(WabtBinaryReaderContext* context, \
                                         uint32_t size) {                  \
    LoggingContext* ctx = context->user_data;                              \
    LOGF("begin_" #name "\n");                                             \
    indent(ctx);                                                           \
    FORWARD_CTX(begin_##name, size);                                       \
  }

#define LOGGING_END(name)                                                  \
  static WabtResult logging_end_##name(WabtBinaryReaderContext* context) { \
    LoggingContext* ctx = context->user_data;                              \
    dedent(ctx);                                                           \
    LOGF("end_" #name "\n");                                               \
    FORWARD_CTX0(end_##name);                                              \
  }

#define LOGGING_UINT32(name)                                          \
  static WabtResult logging_##name(uint32_t value, void* user_data) { \
    LoggingContext* ctx = user_data;                                  \
    LOGF(#name "(%u)\n", value);                                      \
    FORWARD(name, value);                                             \
  }

#define LOGGING_UINT32_CTX(name)                                     \
  static WabtResult logging_##name(WabtBinaryReaderContext* context, \
                                   uint32_t value) {                 \
    LoggingContext* ctx = context->user_data;                        \
    LOGF(#name "(%u)\n", value);                                     \
    FORWARD_CTX(name, value);                                        \
  }

#define LOGGING_UINT32_DESC(name, desc)                               \
  static WabtResult logging_##name(uint32_t value, void* user_data) { \
    LoggingContext* ctx = user_data;                                  \
    LOGF(#name "(" desc ": %u)\n", value);                            \
    FORWARD(name, value);                                             \
  }

#define LOGGING_UINT32_UINT32(name, desc0, desc1)                    \
  static WabtResult logging_##name(uint32_t value0, uint32_t value1, \
                                   void* user_data) {                \
    LoggingContext* ctx = user_data;                                 \
    LOGF(#name "(" desc0 ": %u, " desc1 ": %u)\n", value0, value1);  \
    FORWARD(name, value0, value1);                                   \
  }

#define LOGGING_UINT32_UINT32_CTX(name, desc0, desc1)                  \
  static WabtResult logging_##name(WabtBinaryReaderContext* context,   \
                                   uint32_t value0, uint32_t value1) { \
    LoggingContext* ctx = context->user_data;                          \
    LOGF(#name "(" desc0 ": %u, " desc1 ": %u)\n", value0, value1);    \
    FORWARD_CTX(name, value0, value1);                                 \
  }

#define LOGGING_OPCODE(name)                                             \
  static WabtResult logging_##name(WabtOpcode opcode, void* user_data) { \
    LoggingContext* ctx = user_data;                                     \
    LOGF(#name "(\"%s\" (%u))\n", wabt_get_opcode_name(opcode), opcode); \
    FORWARD(name, opcode);                                               \
  }

#define LOGGING0(name)                                \
  static WabtResult logging_##name(void* user_data) { \
    LoggingContext* ctx = user_data;                  \
    LOGF(#name "\n");                                 \
    FORWARD0(name);                                   \
  }

LOGGING_UINT32(begin_module)
LOGGING0(end_module)
LOGGING_END(custom_section)
LOGGING_BEGIN(signature_section)
LOGGING_UINT32(on_signature_count)
LOGGING_END(signature_section)
LOGGING_BEGIN(import_section)
LOGGING_UINT32(on_import_count)
LOGGING_END(import_section)
LOGGING_BEGIN(function_signatures_section)
LOGGING_UINT32(on_function_signatures_count)
LOGGING_UINT32_UINT32(on_function_signature, "index", "sig_index")
LOGGING_END(function_signatures_section)
LOGGING_BEGIN(table_section)
LOGGING_UINT32(on_table_count)
LOGGING_END(table_section)
LOGGING_BEGIN(memory_section)
LOGGING_UINT32(on_memory_count)
LOGGING_END(memory_section)
LOGGING_BEGIN(global_section)
LOGGING_UINT32(on_global_count)
LOGGING_UINT32(begin_global_init_expr)
LOGGING_UINT32(end_global_init_expr)
LOGGING_UINT32(end_global)
LOGGING_END(global_section)
LOGGING_BEGIN(export_section)
LOGGING_UINT32(on_export_count)
LOGGING_END(export_section)
LOGGING_BEGIN(start_section)
LOGGING_UINT32(on_start_function)
LOGGING_END(start_section)
LOGGING_BEGIN(function_bodies_section)
LOGGING_UINT32(on_function_bodies_count)
LOGGING_UINT32_CTX(begin_function_body)
LOGGING_UINT32(end_function_body)
LOGGING_UINT32(on_local_decl_count)
LOGGING_OPCODE(on_binary_expr)
LOGGING_UINT32_DESC(on_call_expr, "func_index")
LOGGING_UINT32_DESC(on_call_import_expr, "import_index")
LOGGING_UINT32_DESC(on_call_indirect_expr, "sig_index")
LOGGING_OPCODE(on_compare_expr)
LOGGING_OPCODE(on_convert_expr)
LOGGING0(on_current_memory_expr)
LOGGING0(on_drop_expr)
LOGGING0(on_else_expr)
LOGGING0(on_end_expr)
LOGGING_UINT32_DESC(on_get_global_expr, "index")
LOGGING_UINT32_DESC(on_get_local_expr, "index")
LOGGING0(on_grow_memory_expr)
LOGGING0(on_nop_expr)
LOGGING0(on_return_expr)
LOGGING0(on_select_expr)
LOGGING_UINT32_DESC(on_set_global_expr, "index")
LOGGING_UINT32_DESC(on_set_local_expr, "index")
LOGGING_UINT32_DESC(on_tee_local_expr, "index")
LOGGING0(on_unreachable_expr)
LOGGING_OPCODE(on_unary_expr)
LOGGING_END(function_bodies_section)
LOGGING_BEGIN(elem_section)
LOGGING_UINT32(on_elem_segment_count)
LOGGING_UINT32_UINT32(begin_elem_segment, "index", "table_index")
LOGGING_UINT32(begin_elem_segment_init_expr)
LOGGING_UINT32(end_elem_segment_init_expr)
LOGGING_UINT32_UINT32_CTX(on_elem_segment_function_index_count,
                          "index",
                          "count")
LOGGING_UINT32_UINT32(on_elem_segment_function_index, "index", "func_index")
LOGGING_UINT32(end_elem_segment)
LOGGING_END(elem_section)
LOGGING_BEGIN(data_section)
LOGGING_UINT32(on_data_segment_count)
LOGGING_UINT32_UINT32(begin_data_segment, "index", "memory_index")
LOGGING_UINT32(begin_data_segment_init_expr)
LOGGING_UINT32(end_data_segment_init_expr)
LOGGING_UINT32(end_data_segment)
LOGGING_END(data_section)
LOGGING_BEGIN(names_section)
LOGGING_UINT32(on_function_names_count)
LOGGING_UINT32_UINT32(on_local_names_count, "index", "count")
LOGGING_END(names_section)
LOGGING_BEGIN(reloc_section)
LOGGING_END(reloc_section)
LOGGING_UINT32_UINT32(on_init_expr_get_global_expr, "index", "global_index")

static void sprint_limits(char* dst, size_t size, const WabtLimits* limits) {
  int result;
  if (limits->has_max) {
    result = wabt_snprintf(dst, size, "initial: %" PRIu64 ", max: %" PRIu64,
                           limits->initial, limits->max);
  } else {
    result = wabt_snprintf(dst, size, "initial: %" PRIu64, limits->initial);
  }
  WABT_USE(result);
  assert((size_t)result < size);
}

static void log_types(LoggingContext* ctx,
                      uint32_t type_count,
                      WabtType* types) {
  uint32_t i;
  LOGF_NOINDENT("[");
  for (i = 0; i < type_count; ++i) {
    LOGF_NOINDENT("%s", wabt_get_type_name(types[i]));
    if (i != type_count - 1)
      LOGF_NOINDENT(", ");
  }
  LOGF_NOINDENT("]");
}

static WabtResult logging_on_signature(uint32_t index,
                                       uint32_t param_count,
                                       WabtType* param_types,
                                       uint32_t result_count,
                                       WabtType* result_types,
                                       void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_signature(index: %u, params: ", index);
  log_types(ctx, param_count, param_types);
  LOGF_NOINDENT(", results: ");
  log_types(ctx, result_count, result_types);
  LOGF_NOINDENT(")\n");
  FORWARD(on_signature, index, param_count, param_types, result_count,
          result_types);
}

static WabtResult logging_on_import(uint32_t index,
                                    WabtStringSlice module_name,
                                    WabtStringSlice field_name,
                                    void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_import(index: %u, module: \"" PRIstringslice
       "\", field: \"" PRIstringslice "\")\n",
       index, WABT_PRINTF_STRING_SLICE_ARG(module_name),
       WABT_PRINTF_STRING_SLICE_ARG(field_name));
  FORWARD(on_import, index, module_name, field_name);
}

static WabtResult logging_on_import_func(uint32_t import_index,
                                         uint32_t func_index,
                                         uint32_t sig_index,
                                         void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_import_func(import_index: %u, func_index: %u, sig_index: %u)\n",
       import_index, func_index, sig_index);
  FORWARD(on_import_func, import_index, func_index, sig_index);
}


static WabtResult logging_on_import_table(uint32_t import_index,
                                          uint32_t table_index,
                                          WabtType elem_type,
                                          const WabtLimits* elem_limits,
                                          void* user_data) {
  LoggingContext* ctx = user_data;
  char buf[100];
  sprint_limits(buf, sizeof(buf), elem_limits);
  LOGF(
      "on_import_table(import_index: %u, table_index: %u, elem_type: %s, %s)\n",
      import_index, table_index, wabt_get_type_name(elem_type), buf);
  FORWARD(on_import_table, import_index, table_index, elem_type, elem_limits);
}

static WabtResult logging_on_import_memory(uint32_t import_index,
                                           uint32_t memory_index,
                                           const WabtLimits* page_limits,
                                           void* user_data) {
  LoggingContext* ctx = user_data;
  char buf[100];
  sprint_limits(buf, sizeof(buf), page_limits);
  LOGF("on_import_memory(import_index: %u, memory_index: %u, %s)\n",
       import_index, memory_index, buf);
  FORWARD(on_import_memory, import_index, memory_index, page_limits);
}

static WabtResult logging_on_import_global(uint32_t import_index,
                                           uint32_t global_index,
                                           WabtType type,
                                           WabtBool mutable_,
                                           void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF(
      "on_import_global(import_index: %u, global_index: %u, type: %s, mutable: "
      "%s)\n",
      import_index, global_index, wabt_get_type_name(type),
      mutable_ ? "true" : "false");
  FORWARD(on_import_global, import_index, global_index, type, mutable_);
}

static WabtResult logging_on_table(uint32_t index,
                                   WabtType elem_type,
                                   const WabtLimits* elem_limits,
                                   void* user_data) {
  LoggingContext* ctx = user_data;
  char buf[100];
  sprint_limits(buf, sizeof(buf), elem_limits);
  LOGF("on_table(index: %u, elem_type: %s, %s)\n", index,
       wabt_get_type_name(elem_type), buf);
  FORWARD(on_table, index, elem_type, elem_limits);
}

static WabtResult logging_on_memory(uint32_t index,
                                    const WabtLimits* page_limits,
                                    void* user_data) {
  LoggingContext* ctx = user_data;
  char buf[100];
  sprint_limits(buf, sizeof(buf), page_limits);
  LOGF("on_memory(index: %u, %s)\n", index, buf);
  FORWARD(on_memory, index, page_limits);
}

static WabtResult logging_begin_global(uint32_t index,
                                       WabtType type,
                                       WabtBool mutable_,
                                       void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("begin_global(index: %u, type: %s, mutable: %s)\n", index,
       wabt_get_type_name(type), mutable_ ? "true" : "false");
  FORWARD(begin_global, index, type, mutable_);
}

static WabtResult logging_on_export(uint32_t index,
                                    WabtExternalKind kind,
                                    uint32_t item_index,
                                    WabtStringSlice name,
                                    void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_export(index: %u, kind: %s, item_index: %u, name: \"" PRIstringslice
       "\")\n",
       index, wabt_get_kind_name(kind), item_index,
       WABT_PRINTF_STRING_SLICE_ARG(name));
  FORWARD(on_export, index, kind, item_index, name);
}

static WabtResult logging_begin_function_body_pass(uint32_t index,
                                                   uint32_t pass,
                                                   void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("begin_function_body_pass(index: %u, pass: %u)\n", index, pass);
  indent(ctx);
  FORWARD(begin_function_body_pass, index, pass);
}

static WabtResult logging_on_local_decl(uint32_t decl_index,
                                        uint32_t count,
                                        WabtType type,
                                        void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_local_decl(index: %u, count: %u, type: %s)\n", decl_index, count,
       wabt_get_type_name(type));
  FORWARD(on_local_decl, decl_index, count, type);
}

static WabtResult logging_on_block_expr(uint32_t num_types,
                                        WabtType* sig_types,
                                        void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_block_expr(sig: ");
  log_types(ctx, num_types, sig_types);
  LOGF_NOINDENT(")\n");
  FORWARD(on_block_expr, num_types, sig_types);
}

static WabtResult logging_on_br_expr(uint32_t depth, void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_br_expr(depth: %u)\n", depth);
  FORWARD(on_br_expr, depth);
}

static WabtResult logging_on_br_if_expr(uint32_t depth, void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_br_if_expr(depth: %u)\n", depth);
  FORWARD(on_br_if_expr, depth);
}

static WabtResult logging_on_br_table_expr(WabtBinaryReaderContext* context,
                                           uint32_t num_targets,
                                           uint32_t* target_depths,
                                           uint32_t default_target_depth) {
  LoggingContext* ctx = context->user_data;
  LOGF("on_br_table_expr(num_targets: %u, depths: [", num_targets);
  uint32_t i;
  for (i = 0; i < num_targets; ++i) {
    LOGF_NOINDENT("%u", target_depths[i]);
    if (i != num_targets - 1)
      LOGF_NOINDENT(", ");
  }
  LOGF_NOINDENT("], default: %u)\n", default_target_depth);
  FORWARD_CTX(on_br_table_expr, num_targets, target_depths,
              default_target_depth);
}

static WabtResult logging_on_f32_const_expr(uint32_t value_bits,
                                            void* user_data) {
  LoggingContext* ctx = user_data;
  float value;
  memcpy(&value, &value_bits, sizeof(value));
  LOGF("on_f32_const_expr(%g (0x04%x))\n", value, value_bits);
  FORWARD(on_f32_const_expr, value_bits);
}

static WabtResult logging_on_f64_const_expr(uint64_t value_bits,
                                            void* user_data) {
  LoggingContext* ctx = user_data;
  double value;
  memcpy(&value, &value_bits, sizeof(value));
  LOGF("on_f64_const_expr(%g (0x08%" PRIx64 "))\n", value, value_bits);
  FORWARD(on_f64_const_expr, value_bits);
}

static WabtResult logging_on_i32_const_expr(uint32_t value, void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_i32_const_expr(%u (0x%x))\n", value, value);
  FORWARD(on_i32_const_expr, value);
}

static WabtResult logging_on_i64_const_expr(uint64_t value, void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_i64_const_expr(%" PRIu64 " (0x%" PRIx64 "))\n", value, value);
  FORWARD(on_i64_const_expr, value);
}

static WabtResult logging_on_if_expr(uint32_t num_types,
                                     WabtType* sig_types,
                                     void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_if_expr(sig: ");
  log_types(ctx, num_types, sig_types);
  LOGF_NOINDENT(")\n");
  FORWARD(on_if_expr, num_types, sig_types);
}

static WabtResult logging_on_load_expr(WabtOpcode opcode,
                                       uint32_t alignment_log2,
                                       uint32_t offset,
                                       void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_load_expr(opcode: \"%s\" (%u), align log2: %u, offset: %u)\n",
       wabt_get_opcode_name(opcode), opcode, alignment_log2, offset);
  FORWARD(on_load_expr, opcode, alignment_log2, offset);
}

static WabtResult logging_on_loop_expr(uint32_t num_types,
                                       WabtType* sig_types,
                                       void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_loop_expr(sig: ");
  log_types(ctx, num_types, sig_types);
  LOGF_NOINDENT(")\n");
  FORWARD(on_loop_expr, num_types, sig_types);
}

static WabtResult logging_on_store_expr(WabtOpcode opcode,
                                        uint32_t alignment_log2,
                                        uint32_t offset,
                                        void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_store_expr(opcode: \"%s\" (%u), align log2: %u, offset: %u)\n",
       wabt_get_opcode_name(opcode), opcode, alignment_log2, offset);
  FORWARD(on_store_expr, opcode, alignment_log2, offset);
}

static WabtResult logging_end_function_body_pass(uint32_t index,
                                                 uint32_t pass,
                                                 void* user_data) {
  LoggingContext* ctx = user_data;
  dedent(ctx);
  LOGF("end_function_body_pass(index: %u, pass: %u)\n", index, pass);
  FORWARD(end_function_body_pass, index, pass);
}

static WabtResult logging_on_data_segment_data(uint32_t index,
                                               const void* data,
                                               uint32_t size,
                                               void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_data_segment_data(index:%u, size:%u)\n", index, size);
  FORWARD(on_data_segment_data, index, data, size);
}

static WabtResult logging_on_function_name(uint32_t index,
                                           WabtStringSlice name,
                                           void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_function_name(index: %u, name: \"" PRIstringslice "\")\n", index,
       WABT_PRINTF_STRING_SLICE_ARG(name));
  FORWARD(on_function_name, index, name);
}

static WabtResult logging_on_local_name(uint32_t func_index,
                                        uint32_t local_index,
                                        WabtStringSlice name,
                                        void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_local_name(func_index: %u, local_index: %u, name: \"" PRIstringslice
       "\")\n",
       func_index, local_index, WABT_PRINTF_STRING_SLICE_ARG(name));
  FORWARD(on_local_name, func_index, local_index, name);
}

static WabtResult logging_on_init_expr_f32_const_expr(uint32_t index,
                                                      uint32_t value_bits,
                                                      void* user_data) {
  LoggingContext* ctx = user_data;
  float value;
  memcpy(&value, &value_bits, sizeof(value));
  LOGF("on_init_expr_f32_const_expr(index: %u, value: %g (0x04%x))\n", index,
       value, value_bits);
  FORWARD(on_init_expr_f32_const_expr, index, value_bits);
}

static WabtResult logging_on_init_expr_f64_const_expr(uint32_t index,
                                                      uint64_t value_bits,
                                                      void* user_data) {
  LoggingContext* ctx = user_data;
  double value;
  memcpy(&value, &value_bits, sizeof(value));
  LOGF("on_init_expr_f64_const_expr(index: %u value: %g (0x08%" PRIx64 "))\n",
       index, value, value_bits);
  FORWARD(on_init_expr_f64_const_expr, index, value_bits);
}

static WabtResult logging_on_init_expr_i32_const_expr(uint32_t index,
                                                      uint32_t value,
                                                      void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_init_expr_i32_const_expr(index: %u, value: %u)\n", index, value);
  FORWARD(on_init_expr_i32_const_expr, index, value);
}

static WabtResult logging_on_init_expr_i64_const_expr(uint32_t index,
                                                      uint64_t value,
                                                      void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_init_expr_i64_const_expr(index: %u, value: %" PRIu64 ")\n", index,
       value);
  FORWARD(on_init_expr_i64_const_expr, index, value);
}

static WabtResult logging_on_reloc_count(uint32_t count,
                                         WabtBinarySection section_code,
                                         WabtStringSlice section_name,
                                         void* user_data) {
  LoggingContext* ctx = user_data;
  LOGF("on_reloc_count(count: %d, section: %s, section_name: " PRIstringslice
       ")\n",
       count, wabt_get_section_name(section_code),
       WABT_PRINTF_STRING_SLICE_ARG(section_name));
  FORWARD(on_reloc_count, count, section_code, section_name);
}

static WabtBinaryReader s_logging_binary_reader = {
    .user_data = NULL,
    .on_error = logging_on_error,
    .begin_module = logging_begin_module,
    .end_module = logging_end_module,

    .begin_custom_section = logging_begin_custom_section,
    .end_custom_section = logging_end_custom_section,

    .begin_signature_section = logging_begin_signature_section,
    .on_signature_count = logging_on_signature_count,
    .on_signature = logging_on_signature,
    .end_signature_section = logging_end_signature_section,

    .begin_import_section = logging_begin_import_section,
    .on_import_count = logging_on_import_count,
    .on_import = logging_on_import,
    .on_import_func = logging_on_import_func,
    .on_import_table = logging_on_import_table,
    .on_import_memory = logging_on_import_memory,
    .on_import_global = logging_on_import_global,
    .end_import_section = logging_end_import_section,

    .begin_function_signatures_section =
        logging_begin_function_signatures_section,
    .on_function_signatures_count = logging_on_function_signatures_count,
    .on_function_signature = logging_on_function_signature,
    .end_function_signatures_section = logging_end_function_signatures_section,

    .begin_table_section = logging_begin_table_section,
    .on_table_count = logging_on_table_count,
    .on_table = logging_on_table,
    .end_table_section = logging_end_table_section,

    .begin_memory_section = logging_begin_memory_section,
    .on_memory_count = logging_on_memory_count,
    .on_memory = logging_on_memory,
    .end_memory_section = logging_end_memory_section,

    .begin_global_section = logging_begin_global_section,
    .on_global_count = logging_on_global_count,
    .begin_global = logging_begin_global,
    .begin_global_init_expr = logging_begin_global_init_expr,
    .end_global_init_expr = logging_end_global_init_expr,
    .end_global = logging_end_global,
    .end_global_section = logging_end_global_section,

    .begin_export_section = logging_begin_export_section,
    .on_export_count = logging_on_export_count,
    .on_export = logging_on_export,
    .end_export_section = logging_end_export_section,

    .begin_start_section = logging_begin_start_section,
    .on_start_function = logging_on_start_function,
    .end_start_section = logging_end_start_section,

    .begin_function_bodies_section = logging_begin_function_bodies_section,
    .on_function_bodies_count = logging_on_function_bodies_count,
    .begin_function_body_pass = logging_begin_function_body_pass,
    .begin_function_body = logging_begin_function_body,
    .on_local_decl_count = logging_on_local_decl_count,
    .on_local_decl = logging_on_local_decl,
    .on_binary_expr = logging_on_binary_expr,
    .on_block_expr = logging_on_block_expr,
    .on_br_expr = logging_on_br_expr,
    .on_br_if_expr = logging_on_br_if_expr,
    .on_br_table_expr = logging_on_br_table_expr,
    .on_call_expr = logging_on_call_expr,
    .on_call_import_expr = logging_on_call_import_expr,
    .on_call_indirect_expr = logging_on_call_indirect_expr,
    .on_compare_expr = logging_on_compare_expr,
    .on_convert_expr = logging_on_convert_expr,
    .on_drop_expr = logging_on_drop_expr,
    .on_else_expr = logging_on_else_expr,
    .on_end_expr = logging_on_end_expr,
    .on_f32_const_expr = logging_on_f32_const_expr,
    .on_f64_const_expr = logging_on_f64_const_expr,
    .on_get_global_expr = logging_on_get_global_expr,
    .on_get_local_expr = logging_on_get_local_expr,
    .on_grow_memory_expr = logging_on_grow_memory_expr,
    .on_i32_const_expr = logging_on_i32_const_expr,
    .on_i64_const_expr = logging_on_i64_const_expr,
    .on_if_expr = logging_on_if_expr,
    .on_load_expr = logging_on_load_expr,
    .on_loop_expr = logging_on_loop_expr,
    .on_current_memory_expr = logging_on_current_memory_expr,
    .on_nop_expr = logging_on_nop_expr,
    .on_return_expr = logging_on_return_expr,
    .on_select_expr = logging_on_select_expr,
    .on_set_global_expr = logging_on_set_global_expr,
    .on_set_local_expr = logging_on_set_local_expr,
    .on_store_expr = logging_on_store_expr,
    .on_tee_local_expr = logging_on_tee_local_expr,
    .on_unary_expr = logging_on_unary_expr,
    .on_unreachable_expr = logging_on_unreachable_expr,
    .end_function_body = logging_end_function_body,
    .end_function_body_pass = logging_end_function_body_pass,
    .end_function_bodies_section = logging_end_function_bodies_section,

    .begin_elem_section = logging_begin_elem_section,
    .on_elem_segment_count = logging_on_elem_segment_count,
    .begin_elem_segment = logging_begin_elem_segment,
    .begin_elem_segment_init_expr = logging_begin_elem_segment_init_expr,
    .end_elem_segment_init_expr = logging_end_elem_segment_init_expr,
    .on_elem_segment_function_index_count =
        logging_on_elem_segment_function_index_count,
    .on_elem_segment_function_index = logging_on_elem_segment_function_index,
    .end_elem_segment = logging_end_elem_segment,
    .end_elem_section = logging_end_elem_section,

    .begin_data_section = logging_begin_data_section,
    .on_data_segment_count = logging_on_data_segment_count,
    .begin_data_segment = logging_begin_data_segment,
    .begin_data_segment_init_expr = logging_begin_data_segment_init_expr,
    .end_data_segment_init_expr = logging_end_data_segment_init_expr,
    .on_data_segment_data = logging_on_data_segment_data,
    .end_data_segment = logging_end_data_segment,
    .end_data_section = logging_end_data_section,

    .begin_names_section = logging_begin_names_section,
    .on_function_names_count = logging_on_function_names_count,
    .on_function_name = logging_on_function_name,
    .on_local_names_count = logging_on_local_names_count,
    .on_local_name = logging_on_local_name,
    .end_names_section = logging_end_names_section,

    .begin_reloc_section = logging_begin_reloc_section,
    .on_reloc_count = logging_on_reloc_count,
    .end_reloc_section = logging_end_reloc_section,

    .on_init_expr_f32_const_expr = logging_on_init_expr_f32_const_expr,
    .on_init_expr_f64_const_expr = logging_on_init_expr_f64_const_expr,
    .on_init_expr_get_global_expr = logging_on_init_expr_get_global_expr,
    .on_init_expr_i32_const_expr = logging_on_init_expr_i32_const_expr,
    .on_init_expr_i64_const_expr = logging_on_init_expr_i64_const_expr,
};

static void read_init_expr(Context* ctx, uint32_t index) {
  uint8_t opcode;
  in_u8(ctx, &opcode, "opcode");
  switch (opcode) {
    case WABT_OPCODE_I32_CONST: {
      uint32_t value = 0;
      in_i32_leb128(ctx, &value, "init_expr i32.const value");
      CALLBACK(on_init_expr_i32_const_expr, index, value);
      break;
    }

    case WABT_OPCODE_I64_CONST: {
      uint64_t value = 0;
      in_i64_leb128(ctx, &value, "init_expr i64.const value");
      CALLBACK(on_init_expr_i64_const_expr, index, value);
      break;
    }

    case WABT_OPCODE_F32_CONST: {
      uint32_t value_bits = 0;
      in_f32(ctx, &value_bits, "init_expr f32.const value");
      CALLBACK(on_init_expr_f32_const_expr, index, value_bits);
      break;
    }

    case WABT_OPCODE_F64_CONST: {
      uint64_t value_bits = 0;
      in_f64(ctx, &value_bits, "init_expr f64.const value");
      CALLBACK(on_init_expr_f64_const_expr, index, value_bits);
      break;
    }

    case WABT_OPCODE_GET_GLOBAL: {
      uint32_t global_index;
      in_u32_leb128(ctx, &global_index, "init_expr get_global index");
      CALLBACK(on_init_expr_get_global_expr, index, global_index);
      break;
    }

    case WABT_OPCODE_END:
      return;

    default:
      RAISE_ERROR("unexpected opcode in initializer expression: %d (0x%x)",
                  opcode, opcode);
      break;
  }

  in_u8(ctx, &opcode, "opcode");
  RAISE_ERROR_UNLESS(opcode == WABT_OPCODE_END,
                     "expected END opcode after initializer expression");
}

static void read_table(Context* ctx,
                       WabtType* out_elem_type,
                       WabtLimits* out_elem_limits) {
  in_type(ctx, out_elem_type, "table elem type");
  RAISE_ERROR_UNLESS(*out_elem_type == WABT_TYPE_ANYFUNC,
                     "table elem type must by anyfunc");

  uint32_t flags;
  uint32_t initial;
  uint32_t max = 0;
  in_u32_leb128(ctx, &flags, "table flags");
  in_u32_leb128(ctx, &initial, "table initial elem count");
  WabtBool has_max = flags & WABT_BINARY_LIMITS_HAS_MAX_FLAG;
  if (has_max) {
    in_u32_leb128(ctx, &max, "table max elem count");
    RAISE_ERROR_UNLESS(initial <= max,
                       "table initial elem count must be <= max elem count");
  }

  out_elem_limits->has_max = has_max;
  out_elem_limits->initial = initial;
  out_elem_limits->max = max;
}

static void read_memory(Context* ctx, WabtLimits* out_page_limits) {
  uint32_t flags;
  uint32_t initial;
  uint32_t max = 0;
  in_u32_leb128(ctx, &flags, "memory flags");
  in_u32_leb128(ctx, &initial, "memory initial page count");
  WabtBool has_max = flags & WABT_BINARY_LIMITS_HAS_MAX_FLAG;
  RAISE_ERROR_UNLESS(initial <= WABT_MAX_PAGES, "invalid memory initial size");
  if (has_max) {
    in_u32_leb128(ctx, &max, "memory max page count");
    RAISE_ERROR_UNLESS(max <= WABT_MAX_PAGES, "invalid memory max size");
    RAISE_ERROR_UNLESS(initial <= max,
                       "memory initial size must be <= max size");
  }

  out_page_limits->has_max = has_max;
  out_page_limits->initial = initial;
  out_page_limits->max = max;
}

static void read_global_header(Context* ctx,
                               WabtType* out_type,
                               WabtBool* out_mutable) {
  WabtType global_type;
  uint8_t mutable_;
  in_type(ctx, &global_type, "global type");
  RAISE_ERROR_UNLESS(is_concrete_type(global_type),
                     "expected valid global type");

  in_u8(ctx, &mutable_, "global mutability");
  RAISE_ERROR_UNLESS(mutable_ <= 1, "global mutability must be 0 or 1");

  *out_type = global_type;
  *out_mutable = mutable_;
}

static void read_function_body(Context* ctx, uint32_t end_offset) {
  WabtBool seen_end_opcode = WABT_FALSE;
  while (ctx->offset < end_offset) {
    uint8_t opcode;
    in_u8(ctx, &opcode, "opcode");
    CALLBACK_CTX(on_opcode, opcode);
    switch (opcode) {
      case WABT_OPCODE_UNREACHABLE:
        CALLBACK0(on_unreachable_expr);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_BLOCK: {
        WabtType sig_type;
        in_type(ctx, &sig_type, "block signature type");
        RAISE_ERROR_UNLESS(is_inline_sig_type(sig_type),
                           "expected valid block signature type");
        uint32_t num_types = sig_type == WABT_TYPE_VOID ? 0 : 1;
        CALLBACK(on_block_expr, num_types, &sig_type);
        CALLBACK_CTX(on_opcode_block_sig, num_types, &sig_type);
        break;
      }

      case WABT_OPCODE_LOOP: {
        WabtType sig_type;
        in_type(ctx, &sig_type, "loop signature type");
        RAISE_ERROR_UNLESS(is_inline_sig_type(sig_type),
                           "expected valid block signature type");
        uint32_t num_types = sig_type == WABT_TYPE_VOID ? 0 : 1;
        CALLBACK(on_loop_expr, num_types, &sig_type);
        CALLBACK_CTX(on_opcode_block_sig, num_types, &sig_type);
        break;
      }

      case WABT_OPCODE_IF: {
        WabtType sig_type;
        in_type(ctx, &sig_type, "if signature type");
        RAISE_ERROR_UNLESS(is_inline_sig_type(sig_type),
                           "expected valid block signature type");
        uint32_t num_types = sig_type == WABT_TYPE_VOID ? 0 : 1;
        CALLBACK(on_if_expr, num_types, &sig_type);
        CALLBACK_CTX(on_opcode_block_sig, num_types, &sig_type);
        break;
      }

      case WABT_OPCODE_ELSE:
        CALLBACK0(on_else_expr);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_SELECT:
        CALLBACK0(on_select_expr);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_BR: {
        uint32_t depth;
        in_u32_leb128(ctx, &depth, "br depth");
        CALLBACK(on_br_expr, depth);
        CALLBACK_CTX(on_opcode_uint32, depth);
        break;
      }

      case WABT_OPCODE_BR_IF: {
        uint32_t depth;
        in_u32_leb128(ctx, &depth, "br_if depth");
        CALLBACK(on_br_if_expr, depth);
        CALLBACK_CTX(on_opcode_uint32, depth);
        break;
      }

      case WABT_OPCODE_BR_TABLE: {
        uint32_t num_targets;
        in_u32_leb128(ctx, &num_targets, "br_table target count");
        if (num_targets > ctx->target_depths.capacity) {
          wabt_reserve_uint32s(&ctx->target_depths, num_targets);
          ctx->target_depths.size = num_targets;
        }

        uint32_t i;
        for (i = 0; i < num_targets; ++i) {
          uint32_t target_depth;
          in_u32_leb128(ctx, &target_depth, "br_table target depth");
          ctx->target_depths.data[i] = target_depth;
        }

        uint32_t default_target_depth;
        in_u32_leb128(ctx, &default_target_depth,
                      "br_table default target depth");

        CALLBACK_CTX(on_br_table_expr, num_targets, ctx->target_depths.data,
                     default_target_depth);
        break;
      }

      case WABT_OPCODE_RETURN:
        CALLBACK0(on_return_expr);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_NOP:
        CALLBACK0(on_nop_expr);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_DROP:
        CALLBACK0(on_drop_expr);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_END:
        if (ctx->offset == end_offset)
          seen_end_opcode = WABT_TRUE;
        else
          CALLBACK0(on_end_expr);
        break;

      case WABT_OPCODE_I32_CONST: {
        uint32_t value = 0;
        in_i32_leb128(ctx, &value, "i32.const value");
        CALLBACK(on_i32_const_expr, value);
        CALLBACK_CTX(on_opcode_uint32, value);
        break;
      }

      case WABT_OPCODE_I64_CONST: {
        uint64_t value = 0;
        in_i64_leb128(ctx, &value, "i64.const value");
        CALLBACK(on_i64_const_expr, value);
        CALLBACK_CTX(on_opcode_uint64, value);
        break;
      }

      case WABT_OPCODE_F32_CONST: {
        uint32_t value_bits = 0;
        in_f32(ctx, &value_bits, "f32.const value");
        CALLBACK(on_f32_const_expr, value_bits);
        CALLBACK_CTX(on_opcode_f32, value_bits);
        break;
      }

      case WABT_OPCODE_F64_CONST: {
        uint64_t value_bits = 0;
        in_f64(ctx, &value_bits, "f64.const value");
        CALLBACK(on_f64_const_expr, value_bits);
        CALLBACK_CTX(on_opcode_f64, value_bits);
        break;
      }

      case WABT_OPCODE_GET_GLOBAL: {
        uint32_t global_index;
        in_u32_leb128(ctx, &global_index, "get_global global index");
        CALLBACK(on_get_global_expr, global_index);
        CALLBACK_CTX(on_opcode_uint32, global_index);
        break;
      }

      case WABT_OPCODE_GET_LOCAL: {
        uint32_t local_index;
        in_u32_leb128(ctx, &local_index, "get_local local index");
        CALLBACK(on_get_local_expr, local_index);
        CALLBACK_CTX(on_opcode_uint32, local_index);
        break;
      }

      case WABT_OPCODE_SET_GLOBAL: {
        uint32_t global_index;
        in_u32_leb128(ctx, &global_index, "set_global global index");
        CALLBACK(on_set_global_expr, global_index);
        CALLBACK_CTX(on_opcode_uint32, global_index);
        break;
      }

      case WABT_OPCODE_SET_LOCAL: {
        uint32_t local_index;
        in_u32_leb128(ctx, &local_index, "set_local local index");
        CALLBACK(on_set_local_expr, local_index);
        CALLBACK_CTX(on_opcode_uint32, local_index);
        break;
      }

      case WABT_OPCODE_CALL: {
        uint32_t func_index;
        in_u32_leb128(ctx, &func_index, "call function index");
        RAISE_ERROR_UNLESS(func_index < num_total_funcs(ctx),
                           "invalid call function index");
        CALLBACK(on_call_expr, func_index);
        CALLBACK_CTX(on_opcode_uint32, func_index);
        break;
      }

      case WABT_OPCODE_CALL_INDIRECT: {
        uint32_t sig_index;
        in_u32_leb128(ctx, &sig_index, "call_indirect signature index");
        RAISE_ERROR_UNLESS(sig_index < ctx->num_signatures,
                           "invalid call_indirect signature index");
        uint32_t reserved;
        in_u32_leb128(ctx, &reserved, "call_indirect reserved");
        RAISE_ERROR_UNLESS(reserved == 0,
                           "call_indirect reserved value must be 0");
        CALLBACK(on_call_indirect_expr, sig_index);
        CALLBACK_CTX(on_opcode_uint32_uint32, sig_index, reserved);
        break;
      }

      case WABT_OPCODE_TEE_LOCAL: {
        uint32_t local_index;
        in_u32_leb128(ctx, &local_index, "tee_local local index");
        CALLBACK(on_tee_local_expr, local_index);
        CALLBACK_CTX(on_opcode_uint32, local_index);
        break;
      }

      case WABT_OPCODE_I32_LOAD8_S:
      case WABT_OPCODE_I32_LOAD8_U:
      case WABT_OPCODE_I32_LOAD16_S:
      case WABT_OPCODE_I32_LOAD16_U:
      case WABT_OPCODE_I64_LOAD8_S:
      case WABT_OPCODE_I64_LOAD8_U:
      case WABT_OPCODE_I64_LOAD16_S:
      case WABT_OPCODE_I64_LOAD16_U:
      case WABT_OPCODE_I64_LOAD32_S:
      case WABT_OPCODE_I64_LOAD32_U:
      case WABT_OPCODE_I32_LOAD:
      case WABT_OPCODE_I64_LOAD:
      case WABT_OPCODE_F32_LOAD:
      case WABT_OPCODE_F64_LOAD: {
        uint32_t alignment_log2;
        in_u32_leb128(ctx, &alignment_log2, "load alignment");
        uint32_t offset;
        in_u32_leb128(ctx, &offset, "load offset");

        CALLBACK(on_load_expr, opcode, alignment_log2, offset);
        CALLBACK_CTX(on_opcode_uint32_uint32, alignment_log2, offset);
        break;
      }

      case WABT_OPCODE_I32_STORE8:
      case WABT_OPCODE_I32_STORE16:
      case WABT_OPCODE_I64_STORE8:
      case WABT_OPCODE_I64_STORE16:
      case WABT_OPCODE_I64_STORE32:
      case WABT_OPCODE_I32_STORE:
      case WABT_OPCODE_I64_STORE:
      case WABT_OPCODE_F32_STORE:
      case WABT_OPCODE_F64_STORE: {
        uint32_t alignment_log2;
        in_u32_leb128(ctx, &alignment_log2, "store alignment");
        uint32_t offset;
        in_u32_leb128(ctx, &offset, "store offset");

        CALLBACK(on_store_expr, opcode, alignment_log2, offset);
        CALLBACK_CTX(on_opcode_uint32_uint32, alignment_log2, offset);
        break;
      }

      case WABT_OPCODE_CURRENT_MEMORY: {
        uint32_t reserved;
        in_u32_leb128(ctx, &reserved, "current_memory reserved");
        RAISE_ERROR_UNLESS(reserved == 0,
                           "current_memory reserved value must be 0");
        CALLBACK0(on_current_memory_expr);
        CALLBACK_CTX(on_opcode_uint32, reserved);
        break;
      }

      case WABT_OPCODE_GROW_MEMORY: {
        uint32_t reserved;
        in_u32_leb128(ctx, &reserved, "grow_memory reserved");
        RAISE_ERROR_UNLESS(reserved == 0,
                           "grow_memory reserved value must be 0");
        CALLBACK0(on_grow_memory_expr);
        CALLBACK_CTX(on_opcode_uint32, reserved);
        break;
      }

      case WABT_OPCODE_I32_ADD:
      case WABT_OPCODE_I32_SUB:
      case WABT_OPCODE_I32_MUL:
      case WABT_OPCODE_I32_DIV_S:
      case WABT_OPCODE_I32_DIV_U:
      case WABT_OPCODE_I32_REM_S:
      case WABT_OPCODE_I32_REM_U:
      case WABT_OPCODE_I32_AND:
      case WABT_OPCODE_I32_OR:
      case WABT_OPCODE_I32_XOR:
      case WABT_OPCODE_I32_SHL:
      case WABT_OPCODE_I32_SHR_U:
      case WABT_OPCODE_I32_SHR_S:
      case WABT_OPCODE_I32_ROTR:
      case WABT_OPCODE_I32_ROTL:
      case WABT_OPCODE_I64_ADD:
      case WABT_OPCODE_I64_SUB:
      case WABT_OPCODE_I64_MUL:
      case WABT_OPCODE_I64_DIV_S:
      case WABT_OPCODE_I64_DIV_U:
      case WABT_OPCODE_I64_REM_S:
      case WABT_OPCODE_I64_REM_U:
      case WABT_OPCODE_I64_AND:
      case WABT_OPCODE_I64_OR:
      case WABT_OPCODE_I64_XOR:
      case WABT_OPCODE_I64_SHL:
      case WABT_OPCODE_I64_SHR_U:
      case WABT_OPCODE_I64_SHR_S:
      case WABT_OPCODE_I64_ROTR:
      case WABT_OPCODE_I64_ROTL:
      case WABT_OPCODE_F32_ADD:
      case WABT_OPCODE_F32_SUB:
      case WABT_OPCODE_F32_MUL:
      case WABT_OPCODE_F32_DIV:
      case WABT_OPCODE_F32_MIN:
      case WABT_OPCODE_F32_MAX:
      case WABT_OPCODE_F32_COPYSIGN:
      case WABT_OPCODE_F64_ADD:
      case WABT_OPCODE_F64_SUB:
      case WABT_OPCODE_F64_MUL:
      case WABT_OPCODE_F64_DIV:
      case WABT_OPCODE_F64_MIN:
      case WABT_OPCODE_F64_MAX:
      case WABT_OPCODE_F64_COPYSIGN:
        CALLBACK(on_binary_expr, opcode);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_I32_EQ:
      case WABT_OPCODE_I32_NE:
      case WABT_OPCODE_I32_LT_S:
      case WABT_OPCODE_I32_LE_S:
      case WABT_OPCODE_I32_LT_U:
      case WABT_OPCODE_I32_LE_U:
      case WABT_OPCODE_I32_GT_S:
      case WABT_OPCODE_I32_GE_S:
      case WABT_OPCODE_I32_GT_U:
      case WABT_OPCODE_I32_GE_U:
      case WABT_OPCODE_I64_EQ:
      case WABT_OPCODE_I64_NE:
      case WABT_OPCODE_I64_LT_S:
      case WABT_OPCODE_I64_LE_S:
      case WABT_OPCODE_I64_LT_U:
      case WABT_OPCODE_I64_LE_U:
      case WABT_OPCODE_I64_GT_S:
      case WABT_OPCODE_I64_GE_S:
      case WABT_OPCODE_I64_GT_U:
      case WABT_OPCODE_I64_GE_U:
      case WABT_OPCODE_F32_EQ:
      case WABT_OPCODE_F32_NE:
      case WABT_OPCODE_F32_LT:
      case WABT_OPCODE_F32_LE:
      case WABT_OPCODE_F32_GT:
      case WABT_OPCODE_F32_GE:
      case WABT_OPCODE_F64_EQ:
      case WABT_OPCODE_F64_NE:
      case WABT_OPCODE_F64_LT:
      case WABT_OPCODE_F64_LE:
      case WABT_OPCODE_F64_GT:
      case WABT_OPCODE_F64_GE:
        CALLBACK(on_compare_expr, opcode);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_I32_CLZ:
      case WABT_OPCODE_I32_CTZ:
      case WABT_OPCODE_I32_POPCNT:
      case WABT_OPCODE_I64_CLZ:
      case WABT_OPCODE_I64_CTZ:
      case WABT_OPCODE_I64_POPCNT:
      case WABT_OPCODE_F32_ABS:
      case WABT_OPCODE_F32_NEG:
      case WABT_OPCODE_F32_CEIL:
      case WABT_OPCODE_F32_FLOOR:
      case WABT_OPCODE_F32_TRUNC:
      case WABT_OPCODE_F32_NEAREST:
      case WABT_OPCODE_F32_SQRT:
      case WABT_OPCODE_F64_ABS:
      case WABT_OPCODE_F64_NEG:
      case WABT_OPCODE_F64_CEIL:
      case WABT_OPCODE_F64_FLOOR:
      case WABT_OPCODE_F64_TRUNC:
      case WABT_OPCODE_F64_NEAREST:
      case WABT_OPCODE_F64_SQRT:
        CALLBACK(on_unary_expr, opcode);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      case WABT_OPCODE_I32_TRUNC_S_F32:
      case WABT_OPCODE_I32_TRUNC_S_F64:
      case WABT_OPCODE_I32_TRUNC_U_F32:
      case WABT_OPCODE_I32_TRUNC_U_F64:
      case WABT_OPCODE_I32_WRAP_I64:
      case WABT_OPCODE_I64_TRUNC_S_F32:
      case WABT_OPCODE_I64_TRUNC_S_F64:
      case WABT_OPCODE_I64_TRUNC_U_F32:
      case WABT_OPCODE_I64_TRUNC_U_F64:
      case WABT_OPCODE_I64_EXTEND_S_I32:
      case WABT_OPCODE_I64_EXTEND_U_I32:
      case WABT_OPCODE_F32_CONVERT_S_I32:
      case WABT_OPCODE_F32_CONVERT_U_I32:
      case WABT_OPCODE_F32_CONVERT_S_I64:
      case WABT_OPCODE_F32_CONVERT_U_I64:
      case WABT_OPCODE_F32_DEMOTE_F64:
      case WABT_OPCODE_F32_REINTERPRET_I32:
      case WABT_OPCODE_F64_CONVERT_S_I32:
      case WABT_OPCODE_F64_CONVERT_U_I32:
      case WABT_OPCODE_F64_CONVERT_S_I64:
      case WABT_OPCODE_F64_CONVERT_U_I64:
      case WABT_OPCODE_F64_PROMOTE_F32:
      case WABT_OPCODE_F64_REINTERPRET_I64:
      case WABT_OPCODE_I32_REINTERPRET_F32:
      case WABT_OPCODE_I64_REINTERPRET_F64:
      case WABT_OPCODE_I32_EQZ:
      case WABT_OPCODE_I64_EQZ:
        CALLBACK(on_convert_expr, opcode);
        CALLBACK_CTX0(on_opcode_bare);
        break;

      default:
        RAISE_ERROR("unexpected opcode: %d (0x%x)", opcode, opcode);
    }
  }
  RAISE_ERROR_UNLESS(ctx->offset == end_offset,
                     "function body longer than given size");
  RAISE_ERROR_UNLESS(seen_end_opcode, "function body must end with END opcode");
}

static void read_custom_section(Context* ctx, uint32_t section_size) {
  WabtStringSlice section_name;
  in_str(ctx, &section_name, "section name");
  CALLBACK_CTX(begin_custom_section, section_size, section_name);

  WabtBool name_section_ok =
      ctx->last_known_section_code >= WABT_BINARY_SECTION_IMPORT;
  if (ctx->options->read_debug_names && name_section_ok &&
      strncmp(section_name.start, WABT_BINARY_SECTION_NAME,
              section_name.length) == 0) {
    CALLBACK_SECTION(begin_names_section, section_size);
    uint32_t i, num_functions;
    in_u32_leb128(ctx, &num_functions, "function name count");
    CALLBACK(on_function_names_count, num_functions);
    for (i = 0; i < num_functions; ++i) {
      WabtStringSlice function_name;
      in_str(ctx, &function_name, "function name");
      CALLBACK(on_function_name, i, function_name);

      uint32_t num_locals;
      in_u32_leb128(ctx, &num_locals, "local name count");
      CALLBACK(on_local_names_count, i, num_locals);
      uint32_t j;
      for (j = 0; j < num_locals; ++j) {
        WabtStringSlice local_name;
        in_str(ctx, &local_name, "local name");
        CALLBACK(on_local_name, i, j, local_name);
      }
    }
    CALLBACK0(end_names_section);
  } else if (strncmp(section_name.start, WABT_BINARY_SECTION_RELOC,
                     strlen(WABT_BINARY_SECTION_RELOC)) == 0) {
    CALLBACK_SECTION(begin_reloc_section, section_size);
    uint32_t i, num_relocs, section;
    in_u32_leb128(ctx, &section, "section");
    WABT_ZERO_MEMORY(section_name);
    if (section == WABT_BINARY_SECTION_CUSTOM)
      in_str(ctx, &section_name, "section name");
    in_u32_leb128(ctx, &num_relocs, "relocation count");
    CALLBACK(on_reloc_count, num_relocs, section, section_name);
    for (i = 0; i < num_relocs; ++i) {
      uint32_t reloc_type, offset;
      in_u32_leb128(ctx, &reloc_type, "relocation type");
      in_u32_leb128(ctx, &offset, "offset");
      CALLBACK(on_reloc, reloc_type, offset);
    }
    CALLBACK0(end_reloc_section);
  } else {
    /* This is an unknown custom section, skip it. */
    ctx->offset = ctx->read_end;
  }
  CALLBACK_CTX0(end_custom_section);
}

static void read_type_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_signature_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_signatures, "type count");
  CALLBACK(on_signature_count, ctx->num_signatures);

  for (i = 0; i < ctx->num_signatures; ++i) {
    WabtType form;
    in_type(ctx, &form, "type form");
    RAISE_ERROR_UNLESS(form == WABT_TYPE_FUNC, "unexpected type form");

    uint32_t num_params;
    in_u32_leb128(ctx, &num_params, "function param count");

    if (num_params > ctx->param_types.capacity)
      wabt_reserve_types(&ctx->param_types, num_params);

    uint32_t j;
    for (j = 0; j < num_params; ++j) {
      WabtType param_type;
      in_type(ctx, &param_type, "function param type");
      RAISE_ERROR_UNLESS(is_concrete_type(param_type),
                         "expected valid param type");
      ctx->param_types.data[j] = param_type;
    }

    uint32_t num_results;
    in_u32_leb128(ctx, &num_results, "function result count");
    RAISE_ERROR_UNLESS(num_results <= 1, "result count must be 0 or 1");

    WabtType result_type = WABT_TYPE_VOID;
    if (num_results) {
      in_type(ctx, &result_type, "function result type");
      RAISE_ERROR_UNLESS(is_concrete_type(result_type),
                         "expected valid result type");
    }

    CALLBACK(on_signature, i, num_params, ctx->param_types.data, num_results,
             &result_type);
  }
  CALLBACK_CTX0(end_signature_section);
}

static void read_import_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_import_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_imports, "import count");
  CALLBACK(on_import_count, ctx->num_imports);
  for (i = 0; i < ctx->num_imports; ++i) {
    WabtStringSlice module_name;
    in_str(ctx, &module_name, "import module name");
    WabtStringSlice field_name;
    in_str(ctx, &field_name, "import field name");
    CALLBACK(on_import, i, module_name, field_name);

    uint32_t kind;
    in_u32_leb128(ctx, &kind, "import kind");
    switch (kind) {
      case WABT_EXTERNAL_KIND_FUNC: {
        uint32_t sig_index;
        in_u32_leb128(ctx, &sig_index, "import signature index");
        RAISE_ERROR_UNLESS(sig_index < ctx->num_signatures,
                           "invalid import signature index");
        CALLBACK(on_import_func, i, ctx->num_func_imports, sig_index);
        ctx->num_func_imports++;
        break;
      }

      case WABT_EXTERNAL_KIND_TABLE: {
        WabtType elem_type;
        WabtLimits elem_limits;
        read_table(ctx, &elem_type, &elem_limits);
        CALLBACK(on_import_table, i, ctx->num_table_imports, elem_type,
                 &elem_limits);
        ctx->num_table_imports++;
        break;
      }

      case WABT_EXTERNAL_KIND_MEMORY: {
        WabtLimits page_limits;
        read_memory(ctx, &page_limits);
        CALLBACK(on_import_memory, i, ctx->num_memory_imports, &page_limits);
        ctx->num_memory_imports++;
        break;
      }

      case WABT_EXTERNAL_KIND_GLOBAL: {
        WabtType type;
        WabtBool mutable_;
        read_global_header(ctx, &type, &mutable_);
        CALLBACK(on_import_global, i, ctx->num_global_imports, type, mutable_);
        ctx->num_global_imports++;
        break;
      }

      default:
        RAISE_ERROR("invalid import kind: %d", kind);
    }
  }
  CALLBACK_CTX0(end_import_section);
}

static void read_function_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_function_signatures_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_function_signatures, "function signature count");
  CALLBACK(on_function_signatures_count, ctx->num_function_signatures);
  for (i = 0; i < ctx->num_function_signatures; ++i) {
    uint32_t func_index = ctx->num_func_imports + i;
    uint32_t sig_index;
    in_u32_leb128(ctx, &sig_index, "function signature index");
    RAISE_ERROR_UNLESS(sig_index < ctx->num_signatures,
                       "invalid function signature index: %d", sig_index);
    CALLBACK(on_function_signature, func_index, sig_index);
  }
  CALLBACK_CTX0(end_function_signatures_section);
}

static void read_table_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_table_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_tables, "table count");
  RAISE_ERROR_UNLESS(ctx->num_tables <= 1, "table count (%d) must be 0 or 1",
                     ctx->num_tables);
  CALLBACK(on_table_count, ctx->num_tables);
  for (i = 0; i < ctx->num_tables; ++i) {
    uint32_t table_index = ctx->num_table_imports + i;
    WabtType elem_type;
    WabtLimits elem_limits;
    read_table(ctx, &elem_type, &elem_limits);
    CALLBACK(on_table, table_index, elem_type, &elem_limits);
  }
  CALLBACK_CTX0(end_table_section);
}

static void read_memory_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_memory_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_memories, "memory count");
  RAISE_ERROR_UNLESS(ctx->num_memories <= 1, "memory count must be 0 or 1");
  CALLBACK(on_memory_count, ctx->num_memories);
  for (i = 0; i < ctx->num_memories; ++i) {
    uint32_t memory_index = ctx->num_memory_imports + i;
    WabtLimits page_limits;
    read_memory(ctx, &page_limits);
    CALLBACK(on_memory, memory_index, &page_limits);
  }
  CALLBACK_CTX0(end_memory_section);
}

static void read_global_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_global_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_globals, "global count");
  CALLBACK(on_global_count, ctx->num_globals);
  for (i = 0; i < ctx->num_globals; ++i) {
    uint32_t global_index = ctx->num_global_imports + i;
    WabtType global_type;
    WabtBool mutable_;
    read_global_header(ctx, &global_type, &mutable_);
    CALLBACK(begin_global, global_index, global_type, mutable_);
    CALLBACK(begin_global_init_expr, global_index);
    read_init_expr(ctx, global_index);
    CALLBACK(end_global_init_expr, global_index);
    CALLBACK(end_global, global_index);
  }
  CALLBACK_CTX0(end_global_section);
}

static void read_export_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_export_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_exports, "export count");
  CALLBACK(on_export_count, ctx->num_exports);
  for (i = 0; i < ctx->num_exports; ++i) {
    WabtStringSlice name;
    in_str(ctx, &name, "export item name");

    uint8_t external_kind;
    in_u8(ctx, &external_kind, "export external kind");
    RAISE_ERROR_UNLESS(is_valid_external_kind(external_kind),
                       "invalid export external kind");

    uint32_t item_index;
    in_u32_leb128(ctx, &item_index, "export item index");
    switch (external_kind) {
      case WABT_EXTERNAL_KIND_FUNC:
        RAISE_ERROR_UNLESS(item_index < num_total_funcs(ctx),
                           "invalid export func index: %d", item_index);
        break;
      case WABT_EXTERNAL_KIND_TABLE:
        RAISE_ERROR_UNLESS(item_index < num_total_tables(ctx),
                           "invalid export table index");
        break;
      case WABT_EXTERNAL_KIND_MEMORY:
        RAISE_ERROR_UNLESS(item_index < num_total_memories(ctx),
                           "invalid export memory index");
        break;
      case WABT_EXTERNAL_KIND_GLOBAL:
        RAISE_ERROR_UNLESS(item_index < num_total_globals(ctx),
                           "invalid export global index");
        break;
      case WABT_NUM_EXTERNAL_KINDS:
        assert(0);
        break;
    }

    CALLBACK(on_export, i, external_kind, item_index, name);
  }
  CALLBACK_CTX0(end_export_section);
}

static void read_start_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_start_section, section_size);
  uint32_t func_index;
  in_u32_leb128(ctx, &func_index, "start function index");
  RAISE_ERROR_UNLESS(func_index < num_total_funcs(ctx),
                     "invalid start function index");
  CALLBACK(on_start_function, func_index);
  CALLBACK_CTX0(end_start_section);
}

static void read_elem_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_elem_section, section_size);
  uint32_t i, num_elem_segments;
  in_u32_leb128(ctx, &num_elem_segments, "elem segment count");
  CALLBACK(on_elem_segment_count, num_elem_segments);
  RAISE_ERROR_UNLESS(num_elem_segments == 0 || num_total_tables(ctx) > 0,
                     "elem section without table section");
  for (i = 0; i < num_elem_segments; ++i) {
    uint32_t table_index;
    in_u32_leb128(ctx, &table_index, "elem segment table index");
    CALLBACK(begin_elem_segment, i, table_index);
    CALLBACK(begin_elem_segment_init_expr, i);
    read_init_expr(ctx, i);
    CALLBACK(end_elem_segment_init_expr, i);

    uint32_t j, num_function_indexes;
    in_u32_leb128(ctx, &num_function_indexes,
                  "elem segment function index count");
    CALLBACK_CTX(on_elem_segment_function_index_count, i, num_function_indexes);
    for (j = 0; j < num_function_indexes; ++j) {
      uint32_t func_index;
      in_u32_leb128(ctx, &func_index, "elem segment function index");
      CALLBACK(on_elem_segment_function_index, i, func_index);
    }
    CALLBACK(end_elem_segment, i);
  }
  CALLBACK_CTX0(end_elem_section);
}

static void read_code_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_function_bodies_section, section_size);
  uint32_t i;
  in_u32_leb128(ctx, &ctx->num_function_bodies, "function body count");
  RAISE_ERROR_UNLESS(ctx->num_function_signatures == ctx->num_function_bodies,
                     "function signature count != function body count");
  CALLBACK(on_function_bodies_count, ctx->num_function_bodies);
  for (i = 0; i < ctx->num_function_bodies; ++i) {
    uint32_t func_index = ctx->num_func_imports + i;
    uint32_t func_offset = ctx->offset;
    ctx->offset = func_offset;
    CALLBACK_CTX(begin_function_body, func_index);
    uint32_t body_size;
    in_u32_leb128(ctx, &body_size, "function body size");
    uint32_t body_start_offset = ctx->offset;
    uint32_t end_offset = body_start_offset + body_size;

    uint32_t num_local_decls;
    in_u32_leb128(ctx, &num_local_decls, "local declaration count");
    CALLBACK(on_local_decl_count, num_local_decls);
    uint32_t k;
    for (k = 0; k < num_local_decls; ++k) {
      uint32_t num_local_types;
      in_u32_leb128(ctx, &num_local_types, "local type count");
      WabtType local_type;
      in_type(ctx, &local_type, "local type");
      RAISE_ERROR_UNLESS(is_concrete_type(local_type),
                         "expected valid local type");
      CALLBACK(on_local_decl, k, num_local_types, local_type);
    }

    read_function_body(ctx, end_offset);

    CALLBACK(end_function_body, func_index);
  }
  CALLBACK_CTX0(end_function_bodies_section);
}

static void read_data_section(Context* ctx, uint32_t section_size) {
  CALLBACK_SECTION(begin_data_section, section_size);
  uint32_t i, num_data_segments;
  in_u32_leb128(ctx, &num_data_segments, "data segment count");
  CALLBACK(on_data_segment_count, num_data_segments);
  RAISE_ERROR_UNLESS(num_data_segments == 0 || num_total_memories(ctx) > 0,
                     "data section without memory section");
  for (i = 0; i < num_data_segments; ++i) {
    uint32_t memory_index;
    in_u32_leb128(ctx, &memory_index, "data segment memory index");
    CALLBACK(begin_data_segment, i, memory_index);
    CALLBACK(begin_data_segment_init_expr, i);
    read_init_expr(ctx, i);
    CALLBACK(end_data_segment_init_expr, i);

    uint32_t data_size;
    const void* data;
    in_bytes(ctx, &data, &data_size, "data segment data");
    CALLBACK(on_data_segment_data, i, data, data_size);
    CALLBACK(end_data_segment, i);
  }
  CALLBACK_CTX0(end_data_section);
}

static void read_sections(Context* ctx) {
  while (ctx->offset < ctx->data_size) {
    uint32_t section_code;
    uint32_t section_size;
    /* Temporarily reset read_end to the full data size so the next section
     * can be read. */
    ctx->read_end = ctx->data_size;
    in_u32_leb128(ctx, &section_code, "section code");
    in_u32_leb128(ctx, &section_size, "section size");
    ctx->read_end = ctx->offset + section_size;

    if (ctx->read_end > ctx->data_size)
      RAISE_ERROR("invalid section size: extends past end");

    if (ctx->last_known_section_code != WABT_NUM_BINARY_SECTIONS &&
        section_code != WABT_BINARY_SECTION_CUSTOM &&
        section_code <= ctx->last_known_section_code) {
      RAISE_ERROR("section %s out of order",
                  wabt_get_section_name(section_code));
    }

    CALLBACK_CTX(begin_section, section_code, section_size);

#define V(NAME, name, code)                   \
  case WABT_BINARY_SECTION_##NAME:            \
    read_##name##_section(ctx, section_size); \
    break;

    switch (section_code) {
      WABT_FOREACH_BINARY_SECTION(V)
      default:
        RAISE_ERROR("invalid section code: %u; max is %u", section_code,
                    WABT_NUM_BINARY_SECTIONS - 1);
    }

#undef V

    if (ctx->offset != ctx->read_end) {
      RAISE_ERROR("unfinished section (expected end: 0x%" PRIzx ")",
                  ctx->read_end);
    }

    if (section_code != WABT_BINARY_SECTION_CUSTOM)
      ctx->last_known_section_code = section_code;
  }
}

WabtResult wabt_read_binary(const void* data,
                            size_t size,
                            WabtBinaryReader* reader,
                            uint32_t num_function_passes,
                            const WabtReadBinaryOptions* options) {
  LoggingContext logging_context;
  WABT_ZERO_MEMORY(logging_context);
  logging_context.reader = reader;
  logging_context.stream = options->log_stream;

  WabtBinaryReader logging_reader = s_logging_binary_reader;
  logging_reader.user_data = &logging_context;

  Context context;
  WABT_ZERO_MEMORY(context);
  /* all the macros assume a Context* named ctx */
  Context* ctx = &context;
  ctx->data = data;
  ctx->data_size = ctx->read_end = size;
  ctx->reader = options->log_stream ? &logging_reader : reader;
  ctx->options = options;
  ctx->last_known_section_code = WABT_NUM_BINARY_SECTIONS;

  if (setjmp(ctx->error_jmp_buf) == 1) {
    destroy_context(ctx);
    return WABT_ERROR;
  }

  wabt_reserve_types(&ctx->param_types, INITIAL_PARAM_TYPES_CAPACITY);
  wabt_reserve_uint32s(&ctx->target_depths, INITIAL_BR_TABLE_TARGET_CAPACITY);

  uint32_t magic;
  in_u32(ctx, &magic, "magic");
  RAISE_ERROR_UNLESS(magic == WABT_BINARY_MAGIC, "bad magic value");
  uint32_t version;
  in_u32(ctx, &version, "version");
  RAISE_ERROR_UNLESS(version == WABT_BINARY_VERSION,
                     "bad wasm file version: %#x (expected %#x)", version,
                     WABT_BINARY_VERSION);

  CALLBACK(begin_module, version);
  read_sections(ctx);
  CALLBACK0(end_module);
  destroy_context(ctx);
  return WABT_OK;
}
