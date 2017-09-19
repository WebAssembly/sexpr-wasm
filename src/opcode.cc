/*
 * Copyright 2017 WebAssembly Community Group participants
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

#include "src/opcode.h"

#include <algorithm>

#include "src/feature.h"

namespace wabt {

// static
Opcode::Info Opcode::infos_[] = {
#define WABT_OPCODE(rtype, type1, type2, mem_size, prefix, code, Name, text) \
  {text,     Type::rtype, Type::type1, Type::type2,                          \
   mem_size, prefix,      code,        PrefixCode(prefix, code)},
#include "src/opcode.def"
#undef WABT_OPCODE
};

#define WABT_OPCODE(rtype, type1, type2, mem_size, prefix, code, Name, text) \
  /* static */ Opcode Opcode::Name##_Opcode(Opcode::Name);
#include "src/opcode.def"
#undef WABT_OPCODE

// static
Opcode Opcode::FromCode(uint32_t code) {
  return FromCode(0, code);
}

// static
Opcode Opcode::FromCode(uint8_t prefix, uint32_t code) {
  uint32_t prefix_code = PrefixCode(prefix, code);
  auto begin = infos_;
  auto end = infos_ + WABT_ARRAY_SIZE(infos_);
  auto iter = std::lower_bound(begin, end, prefix_code,
                               [](const Info& info, uint32_t prefix_code) {
                                 return info.prefix_code < prefix_code;
                               });
  if (iter == end || iter->prefix_code != prefix_code) {
    return Opcode(EncodeInvalidOpcode(prefix_code));
  }

  return Opcode(static_cast<Enum>(iter - infos_));
}

Opcode::Info Opcode::GetInfo() const {
  if (enum_ < Invalid) {
    return infos_[enum_];
  }

  uint8_t prefix;
  uint32_t code;
  DecodeInvalidOpcode(enum_, &prefix, &code);
  const Info invalid_info = {
      "<invalid>", Type::Void, Type::Void, Type::Void,
      0,           prefix,     code,       PrefixCode(prefix, code),
  };
  return invalid_info;
}

bool Opcode::IsNaturallyAligned(Address alignment) const {
  Address opcode_align = GetMemorySize();
  return alignment == WABT_USE_NATURAL_ALIGNMENT || alignment == opcode_align;
}

Address Opcode::GetAlignment(Address alignment) const {
  if (alignment == WABT_USE_NATURAL_ALIGNMENT)
    return GetMemorySize();
  return alignment;
}

bool Opcode::IsEnabled(const Features& features) const {
  switch (enum_) {
    case Opcode::Try:
    case Opcode::Catch:
    case Opcode::Throw:
    case Opcode::Rethrow:
    case Opcode::CatchAll:
      return features.exceptions_enabled();

    case Opcode::I32TruncSSatF32:
    case Opcode::I32TruncUSatF32:
    case Opcode::I32TruncSSatF64:
    case Opcode::I32TruncUSatF64:
    case Opcode::I64TruncSSatF32:
    case Opcode::I64TruncUSatF32:
    case Opcode::I64TruncSSatF64:
    case Opcode::I64TruncUSatF64:
      return features.sat_float_to_int_enabled();

    case Opcode::I32Extend8S:
    case Opcode::I32Extend16S:
    case Opcode::I64Extend8S:
    case Opcode::I64Extend16S:
    case Opcode::I64Extend32S:
      return features.threads_enabled();

    // Interpreter opcodes are never "enabled".
    case Opcode::InterpreterAlloca:
    case Opcode::InterpreterBrUnless:
    case Opcode::InterpreterCallHost:
    case Opcode::InterpreterData:
    case Opcode::InterpreterDropKeep:
      return false;

    default:
      return true;
  }
}

}  // end anonymous namespace
