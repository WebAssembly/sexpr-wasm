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

#ifndef WABT_BINARY_READER_INTERP_H_
#define WABT_BINARY_READER_INTERP_H_

#include "wabt/binary-reader-options.h"
#include "wabt/common.h"
#include "wabt/error.h"
#include "wabt/feature.h"
#include "wabt/interp/interp.h"

namespace wabt {

class ReadBinaryInterpOptions : public ReadBinaryOptions {
 public:
  ReadBinaryInterpOptions() = default;
  ReadBinaryInterpOptions(const Features& features, Stream* log_stream)
      : features(features), log_stream(log_stream) {}
  Features features;
  Stream* log_stream = nullptr;

  const Features& GetFeatures() const override { return features; }
  Stream* GetLogStream() const override { return log_stream; }
  bool ReadDebugNames() const override { return true; }
};

namespace interp {

Result ReadBinaryInterp(std::string_view filename,
                        const void* data,
                        size_t size,
                        const ReadBinaryInterpOptions& options,
                        Errors*,
                        ModuleDesc* out_module);

}  // namespace interp
}  // namespace wabt

#endif /* WABT_BINARY_READER_INTERP_H_ */
