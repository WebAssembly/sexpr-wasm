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

#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "apply-names.h"
#include "common.h"
#include "config.h"
#include "generate-names.h"
#include "ir.h"
#include "option-parser.h"
#include "source-error-handler.h"
#include "stream.h"
#include "wast-parser.h"
#include "wat-writer.h"
#include "writer.h"

#define PROGRAM_NAME "wast-desugar"

using namespace wabt;

static const char* s_infile;
static const char* s_outfile;
static WriteWatOptions s_write_wat_options;
static bool s_generate_names;

enum {
  FLAG_HELP,
  FLAG_OUTPUT,
  FLAG_FOLD_EXPRS,
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

static Option s_options[] = {
    {FLAG_HELP, 'h', "help", nullptr, HasArgument::No,
     "print this help message"},
    {FLAG_OUTPUT, 'o', "output", "FILE", HasArgument::Yes,
     "output file for the formatted file"},
    {FLAG_FOLD_EXPRS, 'f', "fold-exprs", nullptr, HasArgument::No,
     "Write folded expressions where possible"},
    {FLAG_GENERATE_NAMES, 0, "generate-names", nullptr, HasArgument::No,
     "Give auto-generated names to non-named functions, types, etc."},
};
WABT_STATIC_ASSERT(NUM_FLAGS == WABT_ARRAY_SIZE(s_options));

static void on_option(struct OptionParser* parser,
                      struct Option* option,
                      const char* argument) {
  switch (option->id) {
    case FLAG_HELP:
      print_help(parser, PROGRAM_NAME);
      exit(0);
      break;

    case FLAG_OUTPUT:
      s_outfile = argument;
      break;

    case FLAG_FOLD_EXPRS:
      s_write_wat_options.fold_exprs = true;
      break;

    case FLAG_GENERATE_NAMES:
      s_generate_names = true;
      break;
  }
}

static void on_argument(struct OptionParser* parser, const char* argument) {
  s_infile = argument;
}

static void on_option_error(struct OptionParser* parser,
                            const char* message) {
  WABT_FATAL("%s\n", message);
}

static void parse_options(int argc, char** argv) {
  OptionParser parser;
  WABT_ZERO_MEMORY(parser);
  parser.description = s_description;
  parser.options = s_options;
  parser.num_options = WABT_ARRAY_SIZE(s_options);
  parser.on_option = on_option;
  parser.on_argument = on_argument;
  parser.on_error = on_option_error;
  parse_options(&parser, argc, argv);

  if (!s_infile) {
    print_help(&parser, PROGRAM_NAME);
    WABT_FATAL("No filename given.\n");
  }
}

struct Context {
  MemoryWriter json_writer;
  MemoryWriter module_writer;
  Stream json_stream;
  StringSlice output_filename_noext;
  char* module_filename;
  Result result;
};

int ProgramMain(int argc, char** argv) {
  init_stdio();
  parse_options(argc, argv);

  std::unique_ptr<WastLexer> lexer(WastLexer::CreateFileLexer(s_infile));
  if (!lexer)
    WABT_FATAL("unable to read %s\n", s_infile);

  SourceErrorHandlerFile error_handler;
  Script* script;
  Result result = parse_wast(lexer.get(), &script, &error_handler);

  if (WABT_SUCCEEDED(result)) {
    Module* module = script->GetFirstModule();
    if (!module)
      WABT_FATAL("no module in file.\n");

    if (s_generate_names)
      result = generate_names(module);

    if (WABT_SUCCEEDED(result))
      result = apply_names(module);

    if (WABT_SUCCEEDED(result)) {
      FileWriter writer(s_outfile ? FileWriter(s_outfile) : FileWriter(stdout));
      result = write_wat(&writer, module, &s_write_wat_options);
    }
  }

  delete script;
  return result != Result::Ok;
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
