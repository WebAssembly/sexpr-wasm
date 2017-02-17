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

#ifndef WABT_BINARY_WRITER_SPEC_H_
#define WABT_BINARY_WRITER_SPEC_H_

#include "ast.h"
#include "binary-writer.h"
#include "common.h"

struct WabtWriter;

#define WABT_WRITE_BINARY_SPEC_OPTIONS_DEFAULT \
  { NULL, WABT_WRITE_BINARY_OPTIONS_DEFAULT }

typedef struct WabtWriteBinarySpecOptions {
  const char* json_filename;
  WabtWriteBinaryOptions write_binary_options;
} WabtWriteBinarySpecOptions;

WABT_EXTERN_C_BEGIN
WabtResult wabt_write_binary_spec_script(struct WabtScript*,
                                         const char* source_filename,
                                         const WabtWriteBinarySpecOptions*);
WABT_EXTERN_C_END

#endif /* WABT_BINARY_WRITER_SPEC_H_ */
