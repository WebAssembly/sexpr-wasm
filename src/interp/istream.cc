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

#include "wabt/interp/istream.h"

#include <cinttypes>

namespace wabt {
namespace interp {

template <typename T>
void WABT_VECTORCALL Istream::EmitAt(Offset offset, T val) {
  u32 new_size = offset + sizeof(T);
  if (new_size > data_.size()) {
    data_.resize(new_size);
  }
  memcpy(data_.data() + offset, &val, sizeof(val));
}

template <typename T>
void WABT_VECTORCALL Istream::EmitInternal(T val) {
  EmitAt(end(), val);
}

void Istream::Emit(u32 val) {
  EmitInternal(val);
}

void Istream::Emit(Opcode::Enum op) {
  EmitInternal(static_cast<SerializedOpcode>(op));
}

void Istream::Emit(Opcode::Enum op, u8 val) {
  Emit(op);
  EmitInternal(val);
}

void Istream::Emit(Opcode::Enum op, u32 val) {
  Emit(op);
  EmitInternal(val);
}

void Istream::Emit(Opcode::Enum op, u64 val) {
  Emit(op);
  EmitInternal(val);
}

void Istream::Emit(Opcode::Enum op, v128 val) {
  Emit(op);
  EmitInternal(val);
}

void Istream::Emit(Opcode::Enum op, u32 val1, u32 val2) {
  Emit(op);
  EmitInternal(val1);
  EmitInternal(val2);
}

void Istream::Emit(Opcode::Enum op, u32 val1, u32 val2, u8 val3) {
  Emit(op);
  EmitInternal(val1);
  EmitInternal(val2);
  EmitInternal(val3);
}

void Istream::EmitDropKeep(u32 drop, u32 keep) {
  if (drop > 0) {
    if (drop == 1 && keep == 0) {
      Emit(Opcode::Drop);
    } else {
      Emit(Opcode::InterpDropKeep, drop, keep);
    }
  }
}

void Istream::EmitCatchDrop(u32 drop) {
  if (drop > 0) {
    Emit(Opcode::InterpCatchDrop, drop);
  }
}

Istream::Offset Istream::EmitFixupU32() {
  auto result = end();
  EmitInternal(kInvalidOffset);
  return result;
}

void Istream::ResolveFixupU32(Offset fixup_offset) {
  EmitAt(fixup_offset, end());
}

Istream::Offset Istream::end() const {
  return static_cast<u32>(data_.size());
}

template <typename T>
T WABT_VECTORCALL Istream::ReadAt(Offset* offset) const {
  assert(*offset + sizeof(T) <= data_.size());
  T result;
  memcpy(&result, data_.data() + *offset, sizeof(T));
  *offset += sizeof(T);
  return result;
}

Instr Istream::Read(Offset* offset) const {
  Instr instr;
  instr.op = static_cast<Opcode::Enum>(ReadAt<SerializedOpcode>(offset));

  switch (instr.op) {
    case Opcode::Drop:
    case Opcode::Nop:
    case Opcode::Return:
    case Opcode::Unreachable:
    case Opcode::ThrowRef:
    case Opcode::RefNull:
      // 0 immediates, 0 operands.
      instr.kind = InstrKind::Imm_0_Op_0;
      break;

    case Opcode::F32Abs:
    case Opcode::F32Ceil:
    case Opcode::F32ConvertI32S:
    case Opcode::F32ConvertI32U:
    case Opcode::F32ConvertI64S:
    case Opcode::F32ConvertI64U:
    case Opcode::F32DemoteF64:
    case Opcode::F32Floor:
    case Opcode::F32Nearest:
    case Opcode::F32Neg:
    case Opcode::F32ReinterpretI32:
    case Opcode::F32Sqrt:
    case Opcode::F32Trunc:
    case Opcode::F32X4Abs:
    case Opcode::F32X4Ceil:
    case Opcode::F32X4ConvertI32X4S:
    case Opcode::F32X4ConvertI32X4U:
    case Opcode::F32X4Floor:
    case Opcode::F32X4Nearest:
    case Opcode::F32X4Neg:
    case Opcode::F32X4Splat:
    case Opcode::F32X4Sqrt:
    case Opcode::F32X4Trunc:
    case Opcode::F64Abs:
    case Opcode::F64Ceil:
    case Opcode::F64ConvertI32S:
    case Opcode::F64ConvertI32U:
    case Opcode::F64ConvertI64S:
    case Opcode::F64ConvertI64U:
    case Opcode::F64Floor:
    case Opcode::F64Nearest:
    case Opcode::F64Neg:
    case Opcode::F64PromoteF32:
    case Opcode::F64ReinterpretI64:
    case Opcode::F64Sqrt:
    case Opcode::F64Trunc:
    case Opcode::F64X2Abs:
    case Opcode::F64X2Ceil:
    case Opcode::F64X2Floor:
    case Opcode::F64X2Nearest:
    case Opcode::F64X2Neg:
    case Opcode::F64X2Splat:
    case Opcode::F64X2Sqrt:
    case Opcode::F64X2Trunc:
    case Opcode::I16X8AllTrue:
    case Opcode::I16X8Bitmask:
    case Opcode::I16X8Neg:
    case Opcode::I16X8Splat:
    case Opcode::I16X8ExtendHighI8X16S:
    case Opcode::I16X8ExtendHighI8X16U:
    case Opcode::I16X8ExtendLowI8X16S:
    case Opcode::I16X8ExtendLowI8X16U:
    case Opcode::I32Clz:
    case Opcode::I32Ctz:
    case Opcode::I32Eqz:
    case Opcode::I32Extend16S:
    case Opcode::I32Extend8S:
    case Opcode::I32Popcnt:
    case Opcode::I32ReinterpretF32:
    case Opcode::I32TruncF32S:
    case Opcode::I32TruncF32U:
    case Opcode::I32TruncF64S:
    case Opcode::I32TruncF64U:
    case Opcode::I32TruncSatF32S:
    case Opcode::I32TruncSatF32U:
    case Opcode::I32TruncSatF64S:
    case Opcode::I32TruncSatF64U:
    case Opcode::I32WrapI64:
    case Opcode::I32X4AllTrue:
    case Opcode::I32X4Bitmask:
    case Opcode::I32X4Neg:
    case Opcode::I32X4Splat:
    case Opcode::I32X4TruncSatF32X4S:
    case Opcode::I32X4TruncSatF32X4U:
    case Opcode::I32X4ExtendHighI16X8S:
    case Opcode::I32X4ExtendHighI16X8U:
    case Opcode::I32X4ExtendLowI16X8S:
    case Opcode::I32X4ExtendLowI16X8U:
    case Opcode::I64Clz:
    case Opcode::I64Ctz:
    case Opcode::I64Eqz:
    case Opcode::I64Extend16S:
    case Opcode::I64Extend32S:
    case Opcode::I64Extend8S:
    case Opcode::I64ExtendI32S:
    case Opcode::I64ExtendI32U:
    case Opcode::I64Popcnt:
    case Opcode::I64ReinterpretF64:
    case Opcode::I64TruncF32S:
    case Opcode::I64TruncF32U:
    case Opcode::I64TruncF64S:
    case Opcode::I64TruncF64U:
    case Opcode::I64TruncSatF32S:
    case Opcode::I64TruncSatF32U:
    case Opcode::I64TruncSatF64S:
    case Opcode::I64TruncSatF64U:
    case Opcode::I64X2Neg:
    case Opcode::I64X2AllTrue:
    case Opcode::I64X2Bitmask:
    case Opcode::I64X2ExtendLowI32X4S:
    case Opcode::I64X2ExtendHighI32X4S:
    case Opcode::I64X2ExtendLowI32X4U:
    case Opcode::I64X2ExtendHighI32X4U:
    case Opcode::I64X2Splat:
    case Opcode::I8X16AllTrue:
    case Opcode::I8X16Bitmask:
    case Opcode::I8X16Neg:
    case Opcode::I8X16Popcnt:
    case Opcode::F32X4DemoteF64X2Zero:
    case Opcode::F64X2PromoteLowF32X4:
    case Opcode::I32X4TruncSatF64X2SZero:
    case Opcode::I32X4TruncSatF64X2UZero:
    case Opcode::F64X2ConvertLowI32X4S:
    case Opcode::F64X2ConvertLowI32X4U:
    case Opcode::I8X16Splat:
    case Opcode::RefIsNull:
    case Opcode::V128Not:
    case Opcode::V128AnyTrue:
    case Opcode::I8X16Abs:
    case Opcode::I16X8Abs:
    case Opcode::I32X4Abs:
    case Opcode::I64X2Abs:
    case Opcode::I16X8ExtaddPairwiseI8X16S:
    case Opcode::I16X8ExtaddPairwiseI8X16U:
    case Opcode::I32X4ExtaddPairwiseI16X8S:
    case Opcode::I32X4ExtaddPairwiseI16X8U:
    case Opcode::I32X4RelaxedTruncF32X4S:
    case Opcode::I32X4RelaxedTruncF32X4U:
    case Opcode::I32X4RelaxedTruncF64X2SZero:
    case Opcode::I32X4RelaxedTruncF64X2UZero:
      // 0 immediates, 1 operand.
      instr.kind = InstrKind::Imm_0_Op_1;
      break;

    case Opcode::F32Add:
    case Opcode::F32Copysign:
    case Opcode::F32Div:
    case Opcode::F32Eq:
    case Opcode::F32Ge:
    case Opcode::F32Gt:
    case Opcode::F32Le:
    case Opcode::F32Lt:
    case Opcode::F32Max:
    case Opcode::F32Min:
    case Opcode::F32Mul:
    case Opcode::F32Ne:
    case Opcode::F32Sub:
    case Opcode::F32X4Add:
    case Opcode::F32X4Div:
    case Opcode::F32X4Eq:
    case Opcode::F32X4Ge:
    case Opcode::F32X4Gt:
    case Opcode::F32X4Le:
    case Opcode::F32X4Lt:
    case Opcode::F32X4Max:
    case Opcode::F32X4Min:
    case Opcode::F32X4Mul:
    case Opcode::F32X4Ne:
    case Opcode::F32X4PMax:
    case Opcode::F32X4PMin:
    case Opcode::F32X4Sub:
    case Opcode::F64Add:
    case Opcode::F64Copysign:
    case Opcode::F64Div:
    case Opcode::F64Eq:
    case Opcode::F64Ge:
    case Opcode::F64Gt:
    case Opcode::F64Le:
    case Opcode::F64Lt:
    case Opcode::F64Max:
    case Opcode::F64Min:
    case Opcode::F64Mul:
    case Opcode::F64Ne:
    case Opcode::F64Sub:
    case Opcode::F64X2Add:
    case Opcode::F64X2Div:
    case Opcode::F64X2Eq:
    case Opcode::F64X2Ge:
    case Opcode::F64X2Gt:
    case Opcode::F64X2Le:
    case Opcode::F64X2Lt:
    case Opcode::F64X2Max:
    case Opcode::F64X2Min:
    case Opcode::F64X2Mul:
    case Opcode::F64X2Ne:
    case Opcode::F64X2PMax:
    case Opcode::F64X2PMin:
    case Opcode::F64X2Sub:
    case Opcode::I16X8Q15mulrSatS:
    case Opcode::I16X8Add:
    case Opcode::I16X8AddSatS:
    case Opcode::I16X8AddSatU:
    case Opcode::I16X8AvgrU:
    case Opcode::I16X8Eq:
    case Opcode::I16X8GeS:
    case Opcode::I16X8GeU:
    case Opcode::I16X8GtS:
    case Opcode::I16X8GtU:
    case Opcode::I16X8LeS:
    case Opcode::I16X8LeU:
    case Opcode::I16X8LtS:
    case Opcode::I16X8LtU:
    case Opcode::I16X8MaxS:
    case Opcode::I16X8MaxU:
    case Opcode::I16X8MinS:
    case Opcode::I16X8MinU:
    case Opcode::I16X8Mul:
    case Opcode::I16X8NarrowI32X4S:
    case Opcode::I16X8NarrowI32X4U:
    case Opcode::I16X8Ne:
    case Opcode::I16X8Shl:
    case Opcode::I16X8ShrS:
    case Opcode::I16X8ShrU:
    case Opcode::I16X8Sub:
    case Opcode::I16X8SubSatS:
    case Opcode::I16X8SubSatU:
    case Opcode::I16X8ExtmulLowI8X16S:
    case Opcode::I16X8ExtmulHighI8X16S:
    case Opcode::I16X8ExtmulLowI8X16U:
    case Opcode::I16X8ExtmulHighI8X16U:
    case Opcode::I32Add:
    case Opcode::I32And:
    case Opcode::I32DivS:
    case Opcode::I32DivU:
    case Opcode::I32Eq:
    case Opcode::I32GeS:
    case Opcode::I32GeU:
    case Opcode::I32GtS:
    case Opcode::I32GtU:
    case Opcode::I32LeS:
    case Opcode::I32LeU:
    case Opcode::I32LtS:
    case Opcode::I32LtU:
    case Opcode::I32Mul:
    case Opcode::I32Ne:
    case Opcode::I32Or:
    case Opcode::I32RemS:
    case Opcode::I32RemU:
    case Opcode::I32Rotl:
    case Opcode::I32Rotr:
    case Opcode::I32Shl:
    case Opcode::I32ShrS:
    case Opcode::I32ShrU:
    case Opcode::I32Sub:
    case Opcode::I32X4Add:
    case Opcode::I32X4Eq:
    case Opcode::I32X4GeS:
    case Opcode::I32X4GeU:
    case Opcode::I32X4GtS:
    case Opcode::I32X4GtU:
    case Opcode::I32X4LeS:
    case Opcode::I32X4LeU:
    case Opcode::I32X4LtS:
    case Opcode::I32X4LtU:
    case Opcode::I32X4MaxS:
    case Opcode::I32X4MaxU:
    case Opcode::I32X4MinS:
    case Opcode::I32X4MinU:
    case Opcode::I32X4Mul:
    case Opcode::I32X4Ne:
    case Opcode::I32X4Shl:
    case Opcode::I32X4ShrS:
    case Opcode::I32X4ShrU:
    case Opcode::I32X4Sub:
    case Opcode::I32X4DotI16X8S:
    case Opcode::I32X4ExtmulLowI16X8S:
    case Opcode::I32X4ExtmulHighI16X8S:
    case Opcode::I32X4ExtmulLowI16X8U:
    case Opcode::I32X4ExtmulHighI16X8U:
    case Opcode::I32Xor:
    case Opcode::I64Add:
    case Opcode::I64And:
    case Opcode::I64DivS:
    case Opcode::I64DivU:
    case Opcode::I64Eq:
    case Opcode::I64GeS:
    case Opcode::I64GeU:
    case Opcode::I64GtS:
    case Opcode::I64GtU:
    case Opcode::I64LeS:
    case Opcode::I64LeU:
    case Opcode::I64LtS:
    case Opcode::I64LtU:
    case Opcode::I64Mul:
    case Opcode::I64Ne:
    case Opcode::I64Or:
    case Opcode::I64RemS:
    case Opcode::I64RemU:
    case Opcode::I64Rotl:
    case Opcode::I64Rotr:
    case Opcode::I64Shl:
    case Opcode::I64ShrS:
    case Opcode::I64ShrU:
    case Opcode::I64Sub:
    case Opcode::I64X2Add:
    case Opcode::I64X2Shl:
    case Opcode::I64X2ShrS:
    case Opcode::I64X2ShrU:
    case Opcode::I64X2Sub:
    case Opcode::I64X2Mul:
    case Opcode::I64X2Eq:
    case Opcode::I64X2Ne:
    case Opcode::I64X2LtS:
    case Opcode::I64X2GtS:
    case Opcode::I64X2LeS:
    case Opcode::I64X2GeS:
    case Opcode::I64X2ExtmulLowI32X4S:
    case Opcode::I64X2ExtmulHighI32X4S:
    case Opcode::I64X2ExtmulLowI32X4U:
    case Opcode::I64X2ExtmulHighI32X4U:
    case Opcode::I64Xor:
    case Opcode::I8X16Add:
    case Opcode::I8X16AddSatS:
    case Opcode::I8X16AddSatU:
    case Opcode::I8X16AvgrU:
    case Opcode::I8X16Eq:
    case Opcode::I8X16GeS:
    case Opcode::I8X16GeU:
    case Opcode::I8X16GtS:
    case Opcode::I8X16GtU:
    case Opcode::I8X16LeS:
    case Opcode::I8X16LeU:
    case Opcode::I8X16LtS:
    case Opcode::I8X16LtU:
    case Opcode::I8X16MaxS:
    case Opcode::I8X16MaxU:
    case Opcode::I8X16MinS:
    case Opcode::I8X16MinU:
    case Opcode::I8X16NarrowI16X8S:
    case Opcode::I8X16NarrowI16X8U:
    case Opcode::I8X16Ne:
    case Opcode::I8X16Shl:
    case Opcode::I8X16ShrS:
    case Opcode::I8X16ShrU:
    case Opcode::I8X16Sub:
    case Opcode::I8X16SubSatS:
    case Opcode::I8X16SubSatU:
    case Opcode::V128And:
    case Opcode::V128Andnot:
    case Opcode::V128BitSelect:
    case Opcode::V128Or:
    case Opcode::V128Xor:
    case Opcode::I8X16Swizzle:
    case Opcode::I8X16RelaxedSwizzle:
    case Opcode::F32X4RelaxedMin:
    case Opcode::F32X4RelaxedMax:
    case Opcode::F64X2RelaxedMin:
    case Opcode::F64X2RelaxedMax:
    case Opcode::I16X8RelaxedQ15mulrS:
    case Opcode::I16X8DotI8X16I7X16S:
      // 0 immediates, 2 operands
      instr.kind = InstrKind::Imm_0_Op_2;
      break;

    case Opcode::Select:
    case Opcode::SelectT:
    case Opcode::F32X4RelaxedMadd:
    case Opcode::F32X4RelaxedNmadd:
    case Opcode::F64X2RelaxedMadd:
    case Opcode::F64X2RelaxedNmadd:
    case Opcode::I8X16RelaxedLaneSelect:
    case Opcode::I16X8RelaxedLaneSelect:
    case Opcode::I32X4RelaxedLaneSelect:
    case Opcode::I64X2RelaxedLaneSelect:
    case Opcode::I32X4DotI8X16I7X16AddS:
      // 0 immediates, 3 operands
      instr.kind = InstrKind::Imm_0_Op_3;
      break;

    case Opcode::Br:
      // Jump target immediate, 0 operands.
      instr.kind = InstrKind::Imm_Jump_Op_0;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::BrIf:
    case Opcode::BrTable:
    case Opcode::InterpBrUnless:
      // Jump target immediate, 1 operand.
      instr.kind = InstrKind::Imm_Jump_Op_1;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::GlobalGet:
    case Opcode::LocalGet:
    case Opcode::InterpLocalGetRef:
    case Opcode::InterpGlobalGetRef:
    case Opcode::MemorySize:
    case Opcode::TableSize:
    case Opcode::DataDrop:
    case Opcode::ElemDrop:
    case Opcode::RefFunc:
    case Opcode::Throw:
    case Opcode::Rethrow:
      // Index immediate, 0 operands.
      instr.kind = InstrKind::Imm_Index_Op_0;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::GlobalSet:
    case Opcode::LocalSet:
    case Opcode::LocalTee:
    case Opcode::InterpLocalSetRef:
    case Opcode::InterpLocalTeeRef:
    case Opcode::MemoryGrow:
    case Opcode::TableGet:
      // Index immediate, 1 operand.
      instr.kind = InstrKind::Imm_Index_Op_1;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::TableSet:
    case Opcode::TableGrow:
      // Index immediate, 2 operands.
      instr.kind = InstrKind::Imm_Index_Op_2;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::MemoryFill:
    case Opcode::TableFill:
      // Index immediate, 3 operands.
      instr.kind = InstrKind::Imm_Index_Op_3;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::Call:
    case Opcode::InterpCallImport:
      instr.kind = InstrKind::Imm_Index_Op_N;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::CallIndirect:
    case Opcode::ReturnCallIndirect:
      // Index immediate, N operands.
      instr.kind = InstrKind::Imm_Index_Index_Op_N;
      instr.imm_u32x2.fst = ReadAt<u32>(offset);
      instr.imm_u32x2.snd = ReadAt<u32>(offset);
      break;

    case Opcode::MemoryInit:
    case Opcode::TableInit:
    case Opcode::MemoryCopy:
    case Opcode::TableCopy:
      // Index + index immediates, 3 operands.
      instr.kind = InstrKind::Imm_Index_Index_Op_3;
      instr.imm_u32x2.fst = ReadAt<u32>(offset);
      instr.imm_u32x2.snd = ReadAt<u32>(offset);
      break;

    case Opcode::F32Load:
    case Opcode::F64Load:
    case Opcode::V128Load8X8S:
    case Opcode::V128Load8X8U:
    case Opcode::V128Load16Splat:
    case Opcode::I32AtomicLoad:
    case Opcode::I32AtomicLoad16U:
    case Opcode::I32AtomicLoad8U:
    case Opcode::I32Load:
    case Opcode::I32Load16S:
    case Opcode::I32Load16U:
    case Opcode::I32Load8S:
    case Opcode::I32Load8U:
    case Opcode::V128Load16X4S:
    case Opcode::V128Load16X4U:
    case Opcode::V128Load32Splat:
    case Opcode::I64AtomicLoad:
    case Opcode::I64AtomicLoad16U:
    case Opcode::I64AtomicLoad32U:
    case Opcode::I64AtomicLoad8U:
    case Opcode::I64Load:
    case Opcode::I64Load16S:
    case Opcode::I64Load16U:
    case Opcode::I64Load32S:
    case Opcode::I64Load32U:
    case Opcode::I64Load8S:
    case Opcode::I64Load8U:
    case Opcode::V128Load32X2S:
    case Opcode::V128Load32X2U:
    case Opcode::V128Load64Splat:
    case Opcode::V128Load8Splat:
    case Opcode::V128Load:
    case Opcode::V128Load32Zero:
    case Opcode::V128Load64Zero:
      // Index + memory offset immediates, 1 operand.
      instr.kind = InstrKind::Imm_Index_Offset_Op_1;
      instr.imm_u32x2.fst = ReadAt<u32>(offset);
      instr.imm_u32x2.snd = ReadAt<u32>(offset);
      break;

    case Opcode::MemoryAtomicNotify:
    case Opcode::F32Store:
    case Opcode::F64Store:
    case Opcode::I32AtomicRmw16AddU:
    case Opcode::I32AtomicRmw16AndU:
    case Opcode::I32AtomicRmw16OrU:
    case Opcode::I32AtomicRmw16SubU:
    case Opcode::I32AtomicRmw16XchgU:
    case Opcode::I32AtomicRmw16XorU:
    case Opcode::I32AtomicRmw8AddU:
    case Opcode::I32AtomicRmw8AndU:
    case Opcode::I32AtomicRmw8OrU:
    case Opcode::I32AtomicRmw8SubU:
    case Opcode::I32AtomicRmw8XchgU:
    case Opcode::I32AtomicRmw8XorU:
    case Opcode::I32AtomicRmwAdd:
    case Opcode::I32AtomicRmwAnd:
    case Opcode::I32AtomicRmwOr:
    case Opcode::I32AtomicRmwSub:
    case Opcode::I32AtomicRmwXchg:
    case Opcode::I32AtomicRmwXor:
    case Opcode::I32AtomicStore:
    case Opcode::I32AtomicStore16:
    case Opcode::I32AtomicStore8:
    case Opcode::I32Store:
    case Opcode::I32Store16:
    case Opcode::I32Store8:
    case Opcode::I64AtomicRmw16AddU:
    case Opcode::I64AtomicRmw16AndU:
    case Opcode::I64AtomicRmw16OrU:
    case Opcode::I64AtomicRmw16SubU:
    case Opcode::I64AtomicRmw16XchgU:
    case Opcode::I64AtomicRmw16XorU:
    case Opcode::I64AtomicRmw32AddU:
    case Opcode::I64AtomicRmw32AndU:
    case Opcode::I64AtomicRmw32OrU:
    case Opcode::I64AtomicRmw32SubU:
    case Opcode::I64AtomicRmw32XchgU:
    case Opcode::I64AtomicRmw32XorU:
    case Opcode::I64AtomicRmw8AddU:
    case Opcode::I64AtomicRmw8AndU:
    case Opcode::I64AtomicRmw8OrU:
    case Opcode::I64AtomicRmw8SubU:
    case Opcode::I64AtomicRmw8XchgU:
    case Opcode::I64AtomicRmw8XorU:
    case Opcode::I64AtomicRmwAdd:
    case Opcode::I64AtomicRmwAnd:
    case Opcode::I64AtomicRmwOr:
    case Opcode::I64AtomicRmwSub:
    case Opcode::I64AtomicRmwXchg:
    case Opcode::I64AtomicRmwXor:
    case Opcode::I64AtomicStore:
    case Opcode::I64AtomicStore16:
    case Opcode::I64AtomicStore32:
    case Opcode::I64AtomicStore8:
    case Opcode::I64Store:
    case Opcode::I64Store16:
    case Opcode::I64Store32:
    case Opcode::I64Store8:
    case Opcode::V128Store:
      // Index and memory offset immediates, 2 operands.
      instr.kind = InstrKind::Imm_Index_Offset_Op_2;
      instr.imm_u32x2.fst = ReadAt<u32>(offset);
      instr.imm_u32x2.snd = ReadAt<u32>(offset);
      break;

    case Opcode::V128Load8Lane:
    case Opcode::V128Load16Lane:
    case Opcode::V128Load32Lane:
    case Opcode::V128Load64Lane:
    case Opcode::V128Store8Lane:
    case Opcode::V128Store16Lane:
    case Opcode::V128Store32Lane:
    case Opcode::V128Store64Lane:
      // Index, memory offset, lane index immediates, 2 operands.
      instr.kind = InstrKind::Imm_Index_Offset_Lane_Op_2;
      instr.imm_u32x2_u8.fst = ReadAt<u32>(offset);
      instr.imm_u32x2_u8.snd = ReadAt<u32>(offset);
      instr.imm_u32x2_u8.idx = ReadAt<u8>(offset);
      break;

    case Opcode::I32AtomicRmw16CmpxchgU:
    case Opcode::I32AtomicRmw8CmpxchgU:
    case Opcode::I32AtomicRmwCmpxchg:
    case Opcode::I64AtomicRmw16CmpxchgU:
    case Opcode::I64AtomicRmw32CmpxchgU:
    case Opcode::I64AtomicRmw8CmpxchgU:
    case Opcode::I64AtomicRmwCmpxchg:
    case Opcode::MemoryAtomicWait32:
    case Opcode::MemoryAtomicWait64:
      // Index and memory offset immediates, 3 operands.
      instr.kind = InstrKind::Imm_Index_Offset_Op_3;
      instr.imm_u32x2.fst = ReadAt<u32>(offset);
      instr.imm_u32x2.snd = ReadAt<u32>(offset);
      break;

    case Opcode::AtomicFence:
    case Opcode::I32Const:
    case Opcode::InterpAlloca:
    case Opcode::InterpCatchDrop:
    case Opcode::InterpAdjustFrameForReturnCall:
      // i32/f32 immediate, 0 operands.
      instr.kind = InstrKind::Imm_I32_Op_0;
      instr.imm_u32 = ReadAt<u32>(offset);
      break;

    case Opcode::I64Const:
      // i64 immediate, 0 operands.
      instr.kind = InstrKind::Imm_I64_Op_0;
      instr.imm_u64 = ReadAt<u64>(offset);
      break;

    case Opcode::F32Const:
      // f32 immediate, 0 operands.
      instr.kind = InstrKind::Imm_F32_Op_0;
      instr.imm_f32 = ReadAt<f32>(offset);
      break;

    case Opcode::F64Const:
      // f64 immediate, 0 operands.
      instr.kind = InstrKind::Imm_F64_Op_0;
      instr.imm_f64 = ReadAt<f64>(offset);
      break;

    case Opcode::InterpDropKeep:
      // i32 and i32 immediates, 0 operands.
      instr.kind = InstrKind::Imm_I32_I32_Op_0;
      instr.imm_u32x2.fst = ReadAt<u32>(offset);
      instr.imm_u32x2.snd = ReadAt<u32>(offset);
      break;

    case Opcode::I8X16ExtractLaneS:
    case Opcode::I8X16ExtractLaneU:
    case Opcode::I16X8ExtractLaneS:
    case Opcode::I16X8ExtractLaneU:
    case Opcode::I32X4ExtractLane:
    case Opcode::I64X2ExtractLane:
    case Opcode::F32X4ExtractLane:
    case Opcode::F64X2ExtractLane:
      // u8 immediate, 1 operand.
      instr.kind = InstrKind::Imm_I8_Op_1;
      instr.imm_u8 = ReadAt<u8>(offset);
      break;

    case Opcode::I8X16ReplaceLane:
    case Opcode::I16X8ReplaceLane:
    case Opcode::I32X4ReplaceLane:
    case Opcode::I64X2ReplaceLane:
    case Opcode::F32X4ReplaceLane:
    case Opcode::F64X2ReplaceLane:
      // u8 immediate, 2 operands.
      instr.kind = InstrKind::Imm_I8_Op_2;
      instr.imm_u8 = ReadAt<u8>(offset);
      break;

    case Opcode::V128Const:
      // v128 immediate, 0 operands.
      instr.kind = InstrKind::Imm_V128_Op_0;
      instr.imm_v128 = ReadAt<v128>(offset);
      break;

    case Opcode::I8X16Shuffle:
      // v128 immediate, 2 operands.
      instr.kind = InstrKind::Imm_V128_Op_2;
      instr.imm_v128 = ReadAt<v128>(offset);
      break;

    case Opcode::CallRef:
    case Opcode::Block:
    case Opcode::Catch:
    case Opcode::CatchAll:
    case Opcode::Delegate:
    case Opcode::Else:
    case Opcode::End:
    case Opcode::If:
    case Opcode::InterpData:
    case Opcode::Invalid:
    case Opcode::Loop:
    case Opcode::Try:
    case Opcode::TryTable:
    case Opcode::ReturnCall:
      // Not used.
      break;
  }
  return instr;
}

void Istream::Disassemble(Stream* stream) const {
  Disassemble(stream, 0, data_.size());
}

std::string Istream::DisassemblySource::Header(Offset offset) {
  return StringPrintf("%4u", offset);
}

std::string Istream::DisassemblySource::Pick(Index index, Instr instr) {
  return StringPrintf("%%[-%d]", index);
}

Istream::Offset Istream::Disassemble(Stream* stream, Offset offset) const {
  DisassemblySource source;
  return Trace(stream, offset, &source);
}

void Istream::Disassemble(Stream* stream, Offset from, Offset to) const {
  DisassemblySource source;
  assert(from <= data_.size() && to <= data_.size() && from <= to);

  Offset pc = from;
  while (pc < to) {
    pc = Trace(stream, pc, &source);
  }
}

Istream::Offset Istream::Trace(Stream* stream,
                               Offset offset,
                               TraceSource* source) const {
  Offset start = offset;
  Instr instr = Read(&offset);
  stream->Writef("%s| %s", source->Header(start).c_str(), instr.op.GetName());

  switch (instr.kind) {
    case InstrKind::Imm_0_Op_0:
      stream->Writef("\n");
      break;

    case InstrKind::Imm_0_Op_1:
      stream->Writef(" %s\n", source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_0_Op_2:
      stream->Writef(" %s, %s\n", source->Pick(2, instr).c_str(),
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_0_Op_3:
      stream->Writef(" %s, %s, %s\n", source->Pick(3, instr).c_str(),
                     source->Pick(2, instr).c_str(),
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Jump_Op_0:
      stream->Writef(" @%u\n", instr.imm_u32);
      break;

    case InstrKind::Imm_Jump_Op_1:
      stream->Writef(" @%u, %s\n", instr.imm_u32,
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Op_0:
      stream->Writef(" $%u\n", instr.imm_u32);
      break;

    case InstrKind::Imm_Index_Op_1:
      stream->Writef(" $%u, %s\n", instr.imm_u32,
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Op_2:
      stream->Writef(" $%u, %s, %s\n", instr.imm_u32,
                     source->Pick(2, instr).c_str(),
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Op_3:
      stream->Writef(
          " $%u, %s, %s, %s\n", instr.imm_u32, source->Pick(3, instr).c_str(),
          source->Pick(2, instr).c_str(), source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Op_N:
      stream->Writef(" $%u\n", instr.imm_u32);  // TODO param/result count?
      break;

    case InstrKind::Imm_Index_Index_Op_3:
      stream->Writef(" $%u, $%u, %s, %s, %s\n", instr.imm_u32x2.fst,
                     instr.imm_u32x2.snd, source->Pick(3, instr).c_str(),
                     source->Pick(2, instr).c_str(),
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Index_Op_N:
      stream->Writef(" $%u, $%u\n", instr.imm_u32x2.fst,
                     instr.imm_u32x2.snd);  // TODO param/result count?
      break;

    case InstrKind::Imm_Index_Offset_Op_1:
      stream->Writef(" $%u:%s+$%u\n", instr.imm_u32x2.fst,
                     source->Pick(1, instr).c_str(), instr.imm_u32x2.snd);
      break;

    case InstrKind::Imm_Index_Offset_Op_2:
      stream->Writef(" $%u:%s+$%u, %s\n", instr.imm_u32x2.fst,
                     source->Pick(2, instr).c_str(), instr.imm_u32x2.snd,
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Offset_Op_3:
      stream->Writef(" $%u:%s+$%u, %s, %s\n", instr.imm_u32x2.fst,
                     source->Pick(3, instr).c_str(), instr.imm_u32x2.snd,
                     source->Pick(2, instr).c_str(),
                     source->Pick(1, instr).c_str());
      break;

    case InstrKind::Imm_Index_Offset_Lane_Op_2:
      stream->Writef(" $%u:%s+$%u, %s (Lane imm: $%u)\n",
                     instr.imm_u32x2_u8.fst, source->Pick(2, instr).c_str(),
                     instr.imm_u32x2_u8.snd, source->Pick(1, instr).c_str(),
                     instr.imm_u32x2_u8.idx);
      break;

    case InstrKind::Imm_I32_Op_0:
      stream->Writef(" %u\n", instr.imm_u32);
      break;

    case InstrKind::Imm_I64_Op_0:
      stream->Writef(" %" PRIu64 "\n", instr.imm_u64);
      break;

    case InstrKind::Imm_F32_Op_0:
      stream->Writef(" %g\n", instr.imm_f32);
      break;

    case InstrKind::Imm_F64_Op_0:
      stream->Writef(" %g\n", instr.imm_f64);
      break;

    case InstrKind::Imm_I32_I32_Op_0:
      stream->Writef(" $%u $%u\n", instr.imm_u32x2.fst, instr.imm_u32x2.snd);
      break;

    case InstrKind::Imm_I8_Op_1:
      // TODO: cleanup
      stream->Writef(" %s : (Lane imm: %u)\n", source->Pick(1, instr).c_str(),
                     instr.imm_u8);
      break;

    case InstrKind::Imm_I8_Op_2:
      // TODO: cleanup
      stream->Writef(" %s, %s : (Lane imm: $%u)\n",
                     source->Pick(2, instr).c_str(),
                     source->Pick(1, instr).c_str(), instr.imm_u8);
      break;

    case InstrKind::Imm_V128_Op_0:
      stream->Writef(" i32x4 0x%08x 0x%08x 0x%08x 0x%08x\n",
                     instr.imm_v128.u32(0), instr.imm_v128.u32(1),
                     instr.imm_v128.u32(2), instr.imm_v128.u32(3));
      break;

    case InstrKind::Imm_V128_Op_2:
      // TODO: cleanup
      stream->Writef(
          " %s, %s : (Lane imm: i32x4 0x%08x 0x%08x 0x%08x 0x%08x )\n",
          source->Pick(2, instr).c_str(), source->Pick(1, instr).c_str(),
          instr.imm_v128.u32(0), instr.imm_v128.u32(1), instr.imm_v128.u32(2),
          instr.imm_v128.u32(3));
      break;
  }
  return offset;
}

}  // namespace interp
}  // namespace wabt
