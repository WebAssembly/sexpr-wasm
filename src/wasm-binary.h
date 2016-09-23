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

#ifndef WASM_BINARY_H_
#define WASM_BINARY_H_

#define WASM_BINARY_MAGIC 0x6d736100
#define WASM_BINARY_VERSION 0x0c
#define WASM_BINARY_ELEM_TYPE_ANYFUNC 0x20
#define WASM_BINARY_TYPE_FORM_FUNCTION 0x40
#define WASM_BINARY_LIMITS_HAS_MAX_FLAG 0x1

#define WASM_SECTION_NAME_TYPE "type"
#define WASM_SECTION_NAME_IMPORT "import"
#define WASM_SECTION_NAME_FUNCTION "function"
#define WASM_SECTION_NAME_TABLE "table"
#define WASM_SECTION_NAME_MEMORY "memory"
#define WASM_SECTION_NAME_GLOBAL "global"
#define WASM_SECTION_NAME_EXPORT "export"
#define WASM_SECTION_NAME_START "start"
#define WASM_SECTION_NAME_CODE "code"
#define WASM_SECTION_NAME_ELEM "elem"
#define WASM_SECTION_NAME_DATA "data"
#define WASM_SECTION_NAME_NAME "name"

#define WASM_FOREACH_SECTION(V) \
  V(TYPE)                       \
  V(IMPORT)                     \
  V(FUNCTION)                   \
  V(TABLE)                      \
  V(MEMORY)                     \
  V(GLOBAL)                     \
  V(EXPORT)                     \
  V(START)                      \
  V(CODE)                       \
  V(ELEM)                       \
  V(DATA)                       \
  V(NAME)

#endif /* WASM_BINARY_H_ */
