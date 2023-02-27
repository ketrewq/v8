// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_COMPILER_BACKEND_S390_INSTRUCTION_CODES_S390_H_
#define V8_COMPILER_BACKEND_S390_INSTRUCTION_CODES_S390_H_

namespace v8 {
namespace internal {
namespace compiler {

// S390-specific opcodes that specify which assembly sequence to emit.
// Most opcodes specify a single instruction.

#define TARGET_ARCH_OPCODE_LIST(V)          \
  V(S390_Peek)                              \
  V(S390_Abs32)                             \
  V(S390_Abs64)                             \
  V(S390_And32)                             \
  V(S390_And64)                             \
  V(S390_Or32)                              \
  V(S390_Or64)                              \
  V(S390_Xor32)                             \
  V(S390_Xor64)                             \
  V(S390_ShiftLeft32)                       \
  V(S390_ShiftLeft64)                       \
  V(S390_ShiftRight32)                      \
  V(S390_ShiftRight64)                      \
  V(S390_ShiftRightArith32)                 \
  V(S390_ShiftRightArith64)                 \
  V(S390_RotRight32)                        \
  V(S390_RotRight64)                        \
  V(S390_Not32)                             \
  V(S390_Not64)                             \
  V(S390_RotLeftAndClear64)                 \
  V(S390_RotLeftAndClearLeft64)             \
  V(S390_RotLeftAndClearRight64)            \
  V(S390_Lay)                               \
  V(S390_Add32)                             \
  V(S390_Add64)                             \
  V(S390_AddFloat)                          \
  V(S390_AddDouble)                         \
  V(S390_Sub32)                             \
  V(S390_Sub64)                             \
  V(S390_SubFloat)                          \
  V(S390_SubDouble)                         \
  V(S390_Mul32)                             \
  V(S390_Mul32WithOverflow)                 \
  V(S390_Mul64)                             \
  V(S390_Mul64WithOverflow)                 \
  V(S390_MulHighS64)                        \
  V(S390_MulHighU64)                        \
  V(S390_MulHigh32)                         \
  V(S390_MulHighU32)                        \
  V(S390_MulFloat)                          \
  V(S390_MulDouble)                         \
  V(S390_Div32)                             \
  V(S390_Div64)                             \
  V(S390_DivU32)                            \
  V(S390_DivU64)                            \
  V(S390_DivFloat)                          \
  V(S390_DivDouble)                         \
  V(S390_Mod32)                             \
  V(S390_Mod64)                             \
  V(S390_ModU32)                            \
  V(S390_ModU64)                            \
  V(S390_ModDouble)                         \
  V(S390_Neg32)                             \
  V(S390_Neg64)                             \
  V(S390_NegDouble)                         \
  V(S390_NegFloat)                          \
  V(S390_SqrtFloat)                         \
  V(S390_FloorFloat)                        \
  V(S390_CeilFloat)                         \
  V(S390_TruncateFloat)                     \
  V(S390_FloatNearestInt)                   \
  V(S390_AbsFloat)                          \
  V(S390_SqrtDouble)                        \
  V(S390_FloorDouble)                       \
  V(S390_CeilDouble)                        \
  V(S390_TruncateDouble)                    \
  V(S390_RoundDouble)                       \
  V(S390_DoubleNearestInt)                  \
  V(S390_MaxFloat)                          \
  V(S390_MaxDouble)                         \
  V(S390_MinFloat)                          \
  V(S390_MinDouble)                         \
  V(S390_AbsDouble)                         \
  V(S390_Cntlz32)                           \
  V(S390_Cntlz64)                           \
  V(S390_Popcnt32)                          \
  V(S390_Popcnt64)                          \
  V(S390_Cmp32)                             \
  V(S390_Cmp64)                             \
  V(S390_CmpFloat)                          \
  V(S390_CmpDouble)                         \
  V(S390_Tst32)                             \
  V(S390_Tst64)                             \
  V(S390_Push)                              \
  V(S390_PushFrame)                         \
  V(S390_StoreToStackSlot)                  \
  V(S390_SignExtendWord8ToInt32)            \
  V(S390_SignExtendWord16ToInt32)           \
  V(S390_SignExtendWord8ToInt64)            \
  V(S390_SignExtendWord16ToInt64)           \
  V(S390_SignExtendWord32ToInt64)           \
  V(S390_Uint32ToUint64)                    \
  V(S390_Int64ToInt32)                      \
  V(S390_Int64ToFloat32)                    \
  V(S390_Int64ToDouble)                     \
  V(S390_Uint64ToFloat32)                   \
  V(S390_Uint64ToDouble)                    \
  V(S390_Int32ToFloat32)                    \
  V(S390_Int32ToDouble)                     \
  V(S390_Uint32ToFloat32)                   \
  V(S390_Uint32ToDouble)                    \
  V(S390_Float32ToInt64)                    \
  V(S390_Float32ToUint64)                   \
  V(S390_Float32ToInt32)                    \
  V(S390_Float32ToUint32)                   \
  V(S390_Float32ToDouble)                   \
  V(S390_Float64SilenceNaN)                 \
  V(S390_DoubleToInt32)                     \
  V(S390_DoubleToUint32)                    \
  V(S390_DoubleToInt64)                     \
  V(S390_DoubleToUint64)                    \
  V(S390_DoubleToFloat32)                   \
  V(S390_DoubleExtractLowWord32)            \
  V(S390_DoubleExtractHighWord32)           \
  V(S390_DoubleInsertLowWord32)             \
  V(S390_DoubleInsertHighWord32)            \
  V(S390_DoubleConstruct)                   \
  V(S390_BitcastInt32ToFloat32)             \
  V(S390_BitcastFloat32ToInt32)             \
  V(S390_BitcastInt64ToDouble)              \
  V(S390_BitcastDoubleToInt64)              \
  V(S390_LoadWordS8)                        \
  V(S390_LoadWordU8)                        \
  V(S390_LoadWordS16)                       \
  V(S390_LoadWordU16)                       \
  V(S390_LoadWordS32)                       \
  V(S390_LoadWordU32)                       \
  V(S390_LoadAndTestWord32)                 \
  V(S390_LoadAndTestWord64)                 \
  V(S390_LoadAndTestFloat32)                \
  V(S390_LoadAndTestFloat64)                \
  V(S390_LoadReverse16RR)                   \
  V(S390_LoadReverse32RR)                   \
  V(S390_LoadReverse64RR)                   \
  V(S390_LoadReverseSimd128RR)              \
  V(S390_LoadReverseSimd128)                \
  V(S390_LoadReverse16)                     \
  V(S390_LoadReverse32)                     \
  V(S390_LoadReverse64)                     \
  V(S390_LoadWord64)                        \
  V(S390_LoadFloat32)                       \
  V(S390_LoadDouble)                        \
  V(S390_StoreWord8)                        \
  V(S390_StoreWord16)                       \
  V(S390_StoreWord32)                       \
  V(S390_StoreWord64)                       \
  V(S390_StoreReverse16)                    \
  V(S390_StoreReverse32)                    \
  V(S390_StoreReverse64)                    \
  V(S390_StoreReverseSimd128)               \
  V(S390_StoreFloat32)                      \
  V(S390_StoreDouble)                       \
  V(S390_Word64AtomicExchangeUint64)        \
  V(S390_Word64AtomicCompareExchangeUint64) \
  V(S390_Word64AtomicAddUint64)             \
  V(S390_Word64AtomicSubUint64)             \
  V(S390_Word64AtomicAndUint64)             \
  V(S390_Word64AtomicOrUint64)              \
  V(S390_Word64AtomicXorUint64)             \
  V(S390_F64x2Splat)                        \
  V(S390_F64x2ReplaceLane)                  \
  V(S390_F64x2Abs)                          \
  V(S390_F64x2Neg)                          \
  V(S390_F64x2Sqrt)                         \
  V(S390_F64x2Add)                          \
  V(S390_F64x2Sub)                          \
  V(S390_F64x2Mul)                          \
  V(S390_F64x2Div)                          \
  V(S390_F64x2Eq)                           \
  V(S390_F64x2Ne)                           \
  V(S390_F64x2Lt)                           \
  V(S390_F64x2Le)                           \
  V(S390_F64x2Min)                          \
  V(S390_F64x2Max)                          \
  V(S390_F64x2ExtractLane)                  \
  V(S390_F64x2Qfma)                         \
  V(S390_F64x2Qfms)                         \
  V(S390_F64x2Pmin)                         \
  V(S390_F64x2Pmax)                         \
  V(S390_F64x2Ceil)                         \
  V(S390_F64x2Floor)                        \
  V(S390_F64x2Trunc)                        \
  V(S390_F64x2NearestInt)                   \
  V(S390_F64x2ConvertLowI32x4S)             \
  V(S390_F64x2ConvertLowI32x4U)             \
  V(S390_F64x2PromoteLowF32x4)              \
  V(S390_F32x4Splat)                        \
  V(S390_F32x4ExtractLane)                  \
  V(S390_F32x4ReplaceLane)                  \
  V(S390_F32x4Add)                          \
  V(S390_F32x4Sub)                          \
  V(S390_F32x4Mul)                          \
  V(S390_F32x4Eq)                           \
  V(S390_F32x4Ne)                           \
  V(S390_F32x4Lt)                           \
  V(S390_F32x4Le)                           \
  V(S390_F32x4Abs)                          \
  V(S390_F32x4Neg)                          \
  V(S390_F32x4SConvertI32x4)                \
  V(S390_F32x4UConvertI32x4)                \
  V(S390_F32x4Sqrt)                         \
  V(S390_F32x4Div)                          \
  V(S390_F32x4Min)                          \
  V(S390_F32x4Max)                          \
  V(S390_F32x4Qfma)                         \
  V(S390_F32x4Qfms)                         \
  V(S390_F32x4Pmin)                         \
  V(S390_F32x4Pmax)                         \
  V(S390_F32x4Ceil)                         \
  V(S390_F32x4Floor)                        \
  V(S390_F32x4Trunc)                        \
  V(S390_F32x4NearestInt)                   \
  V(S390_F32x4DemoteF64x2Zero)              \
  V(S390_I64x2Neg)                          \
  V(S390_I64x2Add)                          \
  V(S390_I64x2Sub)                          \
  V(S390_I64x2Shl)                          \
  V(S390_I64x2ShrS)                         \
  V(S390_I64x2ShrU)                         \
  V(S390_I64x2Mul)                          \
  V(S390_I64x2Splat)                        \
  V(S390_I64x2ReplaceLane)                  \
  V(S390_I64x2ExtractLane)                  \
  V(S390_I64x2Eq)                           \
  V(S390_I64x2BitMask)                      \
  V(S390_I64x2ExtMulLowI32x4S)              \
  V(S390_I64x2ExtMulHighI32x4S)             \
  V(S390_I64x2ExtMulLowI32x4U)              \
  V(S390_I64x2ExtMulHighI32x4U)             \
  V(S390_I64x2SConvertI32x4Low)             \
  V(S390_I64x2SConvertI32x4High)            \
  V(S390_I64x2UConvertI32x4Low)             \
  V(S390_I64x2UConvertI32x4High)            \
  V(S390_I64x2Ne)                           \
  V(S390_I64x2GtS)                          \
  V(S390_I64x2GeS)                          \
  V(S390_I64x2Abs)                          \
  V(S390_I32x4Splat)                        \
  V(S390_I32x4ExtractLane)                  \
  V(S390_I32x4ReplaceLane)                  \
  V(S390_I32x4Add)                          \
  V(S390_I32x4Sub)                          \
  V(S390_I32x4Mul)                          \
  V(S390_I32x4MinS)                         \
  V(S390_I32x4MinU)                         \
  V(S390_I32x4MaxS)                         \
  V(S390_I32x4MaxU)                         \
  V(S390_I32x4Eq)                           \
  V(S390_I32x4Ne)                           \
  V(S390_I32x4GtS)                          \
  V(S390_I32x4GeS)                          \
  V(S390_I32x4GtU)                          \
  V(S390_I32x4GeU)                          \
  V(S390_I32x4Neg)                          \
  V(S390_I32x4Shl)                          \
  V(S390_I32x4ShrS)                         \
  V(S390_I32x4ShrU)                         \
  V(S390_I32x4SConvertF32x4)                \
  V(S390_I32x4UConvertF32x4)                \
  V(S390_I32x4SConvertI16x8Low)             \
  V(S390_I32x4SConvertI16x8High)            \
  V(S390_I32x4UConvertI16x8Low)             \
  V(S390_I32x4UConvertI16x8High)            \
  V(S390_I32x4Abs)                          \
  V(S390_I32x4BitMask)                      \
  V(S390_I32x4DotI16x8S)                    \
  V(S390_I32x4ExtMulLowI16x8S)              \
  V(S390_I32x4ExtMulHighI16x8S)             \
  V(S390_I32x4ExtMulLowI16x8U)              \
  V(S390_I32x4ExtMulHighI16x8U)             \
  V(S390_I32x4ExtAddPairwiseI16x8S)         \
  V(S390_I32x4ExtAddPairwiseI16x8U)         \
  V(S390_I32x4TruncSatF64x2SZero)           \
  V(S390_I32x4TruncSatF64x2UZero)           \
  V(S390_I32x4DotI8x16AddS)                 \
  V(S390_I16x8Splat)                        \
  V(S390_I16x8ExtractLaneU)                 \
  V(S390_I16x8ExtractLaneS)                 \
  V(S390_I16x8ReplaceLane)                  \
  V(S390_I16x8Add)                          \
  V(S390_I16x8Sub)                          \
  V(S390_I16x8Mul)                          \
  V(S390_I16x8MinS)                         \
  V(S390_I16x8MinU)                         \
  V(S390_I16x8MaxS)                         \
  V(S390_I16x8MaxU)                         \
  V(S390_I16x8Eq)                           \
  V(S390_I16x8Ne)                           \
  V(S390_I16x8GtS)                          \
  V(S390_I16x8GeS)                          \
  V(S390_I16x8GtU)                          \
  V(S390_I16x8GeU)                          \
  V(S390_I16x8Shl)                          \
  V(S390_I16x8ShrS)                         \
  V(S390_I16x8ShrU)                         \
  V(S390_I16x8Neg)                          \
  V(S390_I16x8SConvertI32x4)                \
  V(S390_I16x8UConvertI32x4)                \
  V(S390_I16x8SConvertI8x16Low)             \
  V(S390_I16x8SConvertI8x16High)            \
  V(S390_I16x8UConvertI8x16Low)             \
  V(S390_I16x8UConvertI8x16High)            \
  V(S390_I16x8AddSatS)                      \
  V(S390_I16x8SubSatS)                      \
  V(S390_I16x8AddSatU)                      \
  V(S390_I16x8SubSatU)                      \
  V(S390_I16x8RoundingAverageU)             \
  V(S390_I16x8Abs)                          \
  V(S390_I16x8BitMask)                      \
  V(S390_I16x8ExtMulLowI8x16S)              \
  V(S390_I16x8ExtMulHighI8x16S)             \
  V(S390_I16x8ExtMulLowI8x16U)              \
  V(S390_I16x8ExtMulHighI8x16U)             \
  V(S390_I16x8ExtAddPairwiseI8x16S)         \
  V(S390_I16x8ExtAddPairwiseI8x16U)         \
  V(S390_I16x8Q15MulRSatS)                  \
  V(S390_I16x8DotI8x16S)                    \
  V(S390_I8x16Splat)                        \
  V(S390_I8x16ExtractLaneU)                 \
  V(S390_I8x16ExtractLaneS)                 \
  V(S390_I8x16ReplaceLane)                  \
  V(S390_I8x16Add)                          \
  V(S390_I8x16Sub)                          \
  V(S390_I8x16MinS)                         \
  V(S390_I8x16MinU)                         \
  V(S390_I8x16MaxS)                         \
  V(S390_I8x16MaxU)                         \
  V(S390_I8x16Eq)                           \
  V(S390_I8x16Ne)                           \
  V(S390_I8x16GtS)                          \
  V(S390_I8x16GeS)                          \
  V(S390_I8x16GtU)                          \
  V(S390_I8x16GeU)                          \
  V(S390_I8x16Shl)                          \
  V(S390_I8x16ShrS)                         \
  V(S390_I8x16ShrU)                         \
  V(S390_I8x16Neg)                          \
  V(S390_I8x16SConvertI16x8)                \
  V(S390_I8x16UConvertI16x8)                \
  V(S390_I8x16AddSatS)                      \
  V(S390_I8x16SubSatS)                      \
  V(S390_I8x16AddSatU)                      \
  V(S390_I8x16SubSatU)                      \
  V(S390_I8x16RoundingAverageU)             \
  V(S390_I8x16Abs)                          \
  V(S390_I8x16BitMask)                      \
  V(S390_I8x16Shuffle)                      \
  V(S390_I8x16Swizzle)                      \
  V(S390_I8x16Popcnt)                       \
  V(S390_I64x2AllTrue)                      \
  V(S390_I32x4AllTrue)                      \
  V(S390_I16x8AllTrue)                      \
  V(S390_I8x16AllTrue)                      \
  V(S390_V128AnyTrue)                       \
  V(S390_S128And)                           \
  V(S390_S128Or)                            \
  V(S390_S128Xor)                           \
  V(S390_S128Const)                         \
  V(S390_S128Zero)                          \
  V(S390_S128AllOnes)                       \
  V(S390_S128Not)                           \
  V(S390_S128Select)                        \
  V(S390_S128AndNot)                        \
  V(S390_S128Load8Splat)                    \
  V(S390_S128Load16Splat)                   \
  V(S390_S128Load32Splat)                   \
  V(S390_S128Load64Splat)                   \
  V(S390_S128Load8x8S)                      \
  V(S390_S128Load8x8U)                      \
  V(S390_S128Load16x4S)                     \
  V(S390_S128Load16x4U)                     \
  V(S390_S128Load32x2S)                     \
  V(S390_S128Load32x2U)                     \
  V(S390_S128Load32Zero)                    \
  V(S390_S128Load64Zero)                    \
  V(S390_S128Load8Lane)                     \
  V(S390_S128Load16Lane)                    \
  V(S390_S128Load32Lane)                    \
  V(S390_S128Load64Lane)                    \
  V(S390_S128Store8Lane)                    \
  V(S390_S128Store16Lane)                   \
  V(S390_S128Store32Lane)                   \
  V(S390_S128Store64Lane)                   \
  V(S390_StoreSimd128)                      \
  V(S390_LoadSimd128)                       \
  V(S390_StoreCompressTagged)               \
  V(S390_LoadDecompressTaggedSigned)        \
  V(S390_LoadDecompressTagged)

// Addressing modes represent the "shape" of inputs to an instruction.
// Many instructions support multiple addressing modes. Addressing modes
// are encoded into the InstructionCode of the instruction and tell the
// code generator after register allocation which assembler method to call.
//
// We use the following local notation for addressing modes:
//
// R = register
// O = register or stack slot
// D = double register
// I = immediate (handle, external, int32)
// MRI = [register + immediate]
// MRR = [register + register]
#define TARGET_ADDRESSING_MODE_LIST(V) \
  V(MR)   /* [%r0          ] */        \
  V(MRI)  /* [%r0       + K] */        \
  V(MRR)  /* [%r0 + %r1    ] */        \
  V(MRRI) /* [%r0 + %r1 + K] */

}  // namespace compiler
}  // namespace internal
}  // namespace v8

#endif  // V8_COMPILER_BACKEND_S390_INSTRUCTION_CODES_S390_H_
