/*
 * Copyright 2018 WebAssembly Community Group participants
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
#include <set>

#include "wabt/binary-reader-nop.h"
#include "wabt/binary-reader.h"
#include "wabt/binary.h"
#include "wabt/error-formatter.h"
#include "wabt/leb128.h"
#include "wabt/option-parser.h"
#include "wabt/stream.h"

using namespace wabt;

static std::string s_filename;
static std::string s_outfile;
static std::set<std::string_view> v_sections_to_keep{};
static std::set<std::string_view> v_sections_to_remove{};

static const char s_description[] =
    R"(  Remove sections of a WebAssembly binary file.

examples:
  # Remove all custom sections from test.wasm
  $ wasm-strip test.wasm
)";

static void ParseOptions(int argc, char** argv) {
  OptionParser parser("wasm-strip", s_description);

  parser.AddArgument("filename", OptionParser::ArgumentCount::One,
                     [](const char* argument) {
                       s_filename = argument;
                       ConvertBackslashToSlash(&s_filename);
                     });
  parser.AddOption('o', "output", "FILE", "output wasm binary file",
                   [](const char* argument) { s_outfile = argument; });
  parser.AddOption('k', "keep-section", "SECTION NAME",
                   "Section name to keep in the final output",
                   [](const char* value) {
                     v_sections_to_keep.insert(std::string_view{value});
                   });
  parser.AddOption('R', "remove-section", "SECTION NAME",
                   "Section to specifically remove, including all the rest",
                   [](const char* value) {
                     v_sections_to_remove.insert(std::string_view{value});
                   });
  parser.Parse(argc, argv);
}

class BinaryReaderStrip : public BinaryReaderNop {
 public:
  explicit BinaryReaderStrip(std::set<std::string_view> sections_to_keep,
                             std::set<std::string_view> sections_to_remove,
                             Errors* errors)
      : errors_(errors),
        sections_to_keep_(sections_to_keep),
        sections_to_remove_(sections_to_remove),
        section_start_(0) {
    stream_.WriteU32(WABT_BINARY_MAGIC, "WASM_BINARY_MAGIC");
    stream_.WriteU32(WABT_BINARY_VERSION, "WASM_BINARY_VERSION");
  }

  bool OnError(const Error& error) override {
    errors_->push_back(error);
    return true;
  }

  Result BeginSection(Index section_index,
                      BinarySection section_type,
                      Offset size) override {
    section_start_ = state->offset;
    if (section_type == BinarySection::Custom) {
      return Result::Ok;
    }
    stream_.WriteU8Enum(section_type, "section code");
    WriteU32Leb128(&stream_, size, "section size");
    stream_.WriteData(state->data + state->offset, size, "section data");
    return Result::Ok;
  }

  Result WriteToFile(std::string_view filename) {
    return stream_.WriteToFile(filename);
  }

  Result BeginCustomSection(Index section_index,
                            Offset size,
                            std::string_view section_name) override {
    if (sections_to_remove_.count(section_name) > 0) {
      return Result::Ok;
    }

    if (sections_to_keep_.count(section_name) > 0 ||
        !sections_to_remove_.empty()) {
      stream_.WriteU8Enum(BinarySection::Custom, "section code");
      WriteU32Leb128(&stream_, size, "section size");
      stream_.WriteData(state->data + section_start_, size, "section data");
    }
    return Result::Ok;
  }

 private:
  MemoryStream stream_;
  Errors* errors_;
  std::set<std::string_view> sections_to_keep_;
  std::set<std::string_view> sections_to_remove_;
  Offset section_start_;
};

int ProgramMain(int argc, char** argv) {
  Result result;

  InitStdio();
  ParseOptions(argc, argv);

  std::vector<uint8_t> file_data;
  result = ReadFile(s_filename.c_str(), &file_data);
  if (Failed(result)) {
    return Result::Error;
  }

  Errors errors;
  class ReadBinaryStripOptions : public ReadBinaryOptions {
    Features features;

   public:
    ReadBinaryStripOptions() { features.EnableAll(); }

    const Features& GetFeatures() const override { return features; }
    Stream* GetLogStream() const override { return nullptr; }
    bool ReadDebugNames() const override { return false; }
    bool FailOnCustomSectionError() const override { return false; }
  };
  ReadBinaryStripOptions options;

  BinaryReaderStrip reader(v_sections_to_keep, v_sections_to_remove, &errors);
  result = ReadBinary(file_data.data(), file_data.size(), &reader, options);
  FormatErrorsToFile(errors, Location::Type::Binary);
  if (Failed(result)) {
    return Result::Error;
  }

  if (s_outfile.empty()) {
    s_outfile = s_filename;
  }
  return reader.WriteToFile(s_outfile);
}

int main(int argc, char** argv) {
  WABT_TRY
  return ProgramMain(argc, argv);
  WABT_CATCH_BAD_ALLOC_AND_EXIT
}
