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

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "apply-names.h"
#include "ast.h"
#include "ast-parser.h"
#include "ast-writer.h"
#include "common.h"
#include "config.h"
#include "generate-names.h"
#include "option-parser.h"
#include "stack-allocator.h"
#include "stream.h"
#include "writer.h"

#define PROGRAM_NAME "wast-desugar"

static const char* s_infile;
static const char* s_outfile;
static WasmBool s_use_libc_allocator;
static WasmBool s_generate_names;

static WasmSourceErrorHandler s_error_handler =
    WASM_SOURCE_ERROR_HANDLER_DEFAULT;

enum {
  FLAG_HELP,
  FLAG_OUTPUT,
  FLAG_USE_LIBC_ALLOCATOR,
  FLAG_GENERATE_NAMES,
  NUM_FLAGS
};

static const char s_description[] =
    "  read a file in the wasm s-expression format and format it.\n"
    "\n"
    "examples:\n"
    "  # write output to stdout\n"
    "  $ wast-desugar test.wast\n"
    "\n"
    "  # write output to test2.wast\n"
    "  $ wast-desugar test.wast -o test2.wast\n"
    "\n"
    "  # generate names for indexed variables\n"
    "  $ wast-desugar --generate-names test.wast\n";

static WasmOption s_options[] = {
    {FLAG_HELP, 'h', "help", NULL, WASM_OPTION_NO_ARGUMENT,
     "print this help message"},
    {FLAG_OUTPUT, 'o', "output", "FILE", WASM_OPTION_HAS_ARGUMENT,
     "output file for the formatted file"},
    {FLAG_USE_LIBC_ALLOCATOR, 0, "use-libc-allocator", NULL,
     WASM_OPTION_NO_ARGUMENT,
     "use malloc, free, etc. instead of stack allocator"},
    {FLAG_GENERATE_NAMES, 0, "generate-names", NULL, WASM_OPTION_NO_ARGUMENT,
     "Give auto-generated names to non-named functions, types, etc."},
};
WASM_STATIC_ASSERT(NUM_FLAGS == WASM_ARRAY_SIZE(s_options));

static void on_option(struct WasmOptionParser* parser,
                      struct WasmOption* option,
                      const char* argument) {
  switch (option->id) {
    case FLAG_HELP:
      wasm_print_help(parser, PROGRAM_NAME);
      exit(0);
      break;

    case FLAG_OUTPUT:
      s_outfile = argument;
      break;

    case FLAG_USE_LIBC_ALLOCATOR:
      s_use_libc_allocator = WASM_TRUE;
      break;

    case FLAG_GENERATE_NAMES:
      s_generate_names = WASM_TRUE;
      break;
  }
}

static void on_argument(struct WasmOptionParser* parser, const char* argument) {
  s_infile = argument;
}

static void on_option_error(struct WasmOptionParser* parser,
                            const char* message) {
  WASM_FATAL("%s\n", message);
}

static void parse_options(int argc, char** argv) {
  WasmOptionParser parser;
  WASM_ZERO_MEMORY(parser);
  parser.description = s_description;
  parser.options = s_options;
  parser.num_options = WASM_ARRAY_SIZE(s_options);
  parser.on_option = on_option;
  parser.on_argument = on_argument;
  parser.on_error = on_option_error;
  wasm_parse_options(&parser, argc, argv);

  if (!s_infile) {
    wasm_print_help(&parser, PROGRAM_NAME);
    WASM_FATAL("No filename given.\n");
  }
}

typedef struct Context {
  WasmAllocator* allocator;
  WasmMemoryWriter json_writer;
  WasmMemoryWriter module_writer;
  WasmStream json_stream;
  WasmStringSlice output_filename_noext;
  char* module_filename;
  WasmResult result;
} Context;

int main(int argc, char** argv) {
  WasmStackAllocator stack_allocator;
  WasmAllocator* allocator;

  wasm_init_stdio();
  parse_options(argc, argv);

  if (s_use_libc_allocator) {
    allocator = &g_wasm_libc_allocator;
  } else {
    wasm_init_stack_allocator(&stack_allocator, &g_wasm_libc_allocator);
    allocator = &stack_allocator.allocator;
  }

  WasmAstLexer* lexer = wasm_new_ast_file_lexer(allocator, s_infile);
  if (!lexer)
    WASM_FATAL("unable to read %s\n", s_infile);

  WasmScript script;
  WasmResult result = wasm_parse_ast(lexer, &script, &s_error_handler);

  if (WASM_SUCCEEDED(result)) {
    WasmModule* module = wasm_get_first_module(&script);
    if (!module)
      WASM_FATAL("no module in file.\n");

    if (s_generate_names)
      result = wasm_generate_names(allocator, module);

    if (WASM_SUCCEEDED(result))
      result = wasm_apply_names(allocator, module);

    if (WASM_SUCCEEDED(result)) {
      WasmFileWriter file_writer;
      if (s_outfile) {
        result = wasm_init_file_writer(&file_writer, s_outfile);
      } else {
        wasm_init_file_writer_existing(&file_writer, stdout);
      }

      if (WASM_SUCCEEDED(result)) {
        result = wasm_write_ast(allocator, &file_writer.base, module);
        wasm_close_file_writer(&file_writer);
      }
    }
  }

  wasm_destroy_ast_lexer(lexer);

  if (s_use_libc_allocator)
    wasm_destroy_script(&script);
  wasm_print_allocator_stats(allocator);
  wasm_destroy_allocator(allocator);
  return result;
}

