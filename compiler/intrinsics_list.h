/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_INTRINSICS_LIST_H_
#define ART_COMPILER_INTRINSICS_LIST_H_

// All intrinsics supported by ART. Format is name, then whether it is expected
// to be a HInvokeStaticOrDirect node (compared to HInvokeVirtual), then whether it requires an
// environment, may have side effects, or may throw exceptions.

// Note: adding a new intrinsic requires an art image version change,
// as the modifiers flag for some ArtMethods will need to be changed.

// Note: j.l.Integer.valueOf says kNoThrow even though it could throw an OOME.
// The kNoThrow should be renamed to kNoVisibleThrow, as it is ok to GVN Integer.valueOf
// (kNoSideEffects), and it is also OK to remove it if it's unused.

#define INTRINSICS_LIST(V) \
  V(DoubleDoubleToRawLongBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Double;", "doubleToRawLongBits", "(D)J") \
  V(DoubleDoubleToLongBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Double;", "doubleToLongBits", "(D)J") \
  V(DoubleIsInfinite, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Double;", "isInfinite", "(D)Z") \
  V(DoubleIsNaN, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Double;", "isNaN", "(D)Z") \
  V(DoubleLongBitsToDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Double;", "longBitsToDouble", "(J)D") \
  V(FloatFloatToRawIntBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Float;", "floatToRawIntBits", "(F)I") \
  V(FloatFloatToIntBits, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Float;", "floatToIntBits", "(F)I") \
  V(FloatIsInfinite, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Float;", "isInfinite", "(F)Z") \
  V(FloatIsNaN, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Float;", "isNaN", "(F)Z") \
  V(FloatIntBitsToFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Float;", "intBitsToFloat", "(I)F") \
  V(IntegerReverse, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "reverse", "(I)I") \
  V(IntegerReverseBytes, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "reverseBytes", "(I)I") \
  V(IntegerBitCount, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "bitCount", "(I)I") \
  V(IntegerCompare, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "compare", "(II)I") \
  V(IntegerHighestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "highestOneBit", "(I)I") \
  V(IntegerLowestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "lowestOneBit", "(I)I") \
  V(IntegerNumberOfLeadingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "numberOfLeadingZeros", "(I)I") \
  V(IntegerNumberOfTrailingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "numberOfTrailingZeros", "(I)I") \
  V(IntegerRotateRight, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "rotateRight", "(II)I") \
  V(IntegerRotateLeft, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "rotateLeft", "(II)I") \
  V(IntegerSignum, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "signum", "(I)I") \
  V(LongReverse, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "reverse", "(J)J") \
  V(LongReverseBytes, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "reverseBytes", "(J)J") \
  V(LongBitCount, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "bitCount", "(J)I") \
  V(LongCompare, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "compare", "(JJ)I") \
  V(LongHighestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "highestOneBit", "(J)J") \
  V(LongLowestOneBit, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "lowestOneBit", "(J)J") \
  V(LongNumberOfLeadingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "numberOfLeadingZeros", "(J)I") \
  V(LongNumberOfTrailingZeros, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "numberOfTrailingZeros", "(J)I") \
  V(LongRotateRight, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "rotateRight", "(JI)J") \
  V(LongRotateLeft, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "rotateLeft", "(JI)J") \
  V(LongSignum, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Long;", "signum", "(J)I") \
  V(ShortReverseBytes, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Short;", "reverseBytes", "(S)S") \
  V(MathAbsDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "abs", "(D)D") \
  V(MathAbsFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "abs", "(F)F") \
  V(MathAbsLong, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "abs", "(J)J") \
  V(MathAbsInt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "abs", "(I)I") \
  V(MathMinDoubleDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "min", "(DD)D") \
  V(MathMinFloatFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "min", "(FF)F") \
  V(MathMinLongLong, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "min", "(JJ)J") \
  V(MathMinIntInt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "min", "(II)I") \
  V(MathMaxDoubleDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "max", "(DD)D") \
  V(MathMaxFloatFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "max", "(FF)F") \
  V(MathMaxLongLong, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "max", "(JJ)J") \
  V(MathMaxIntInt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "max", "(II)I") \
  V(MathCos, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "cos", "(D)D") \
  V(MathSin, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "sin", "(D)D") \
  V(MathAcos, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "acos", "(D)D") \
  V(MathAsin, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "asin", "(D)D") \
  V(MathAtan, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "atan", "(D)D") \
  V(MathAtan2, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "atan2", "(DD)D") \
  V(MathCbrt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "cbrt", "(D)D") \
  V(MathCosh, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "cosh", "(D)D") \
  V(MathExp, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "exp", "(D)D") \
  V(MathExpm1, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "expm1", "(D)D") \
  V(MathHypot, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "hypot", "(DD)D") \
  V(MathLog, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "log", "(D)D") \
  V(MathLog10, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "log10", "(D)D") \
  V(MathNextAfter, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "nextAfter", "(DD)D") \
  V(MathSinh, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "sinh", "(D)D") \
  V(MathTan, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "tan", "(D)D") \
  V(MathTanh, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "tanh", "(D)D") \
  V(MathSqrt, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "sqrt", "(D)D") \
  V(MathCeil, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "ceil", "(D)D") \
  V(MathFloor, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "floor", "(D)D") \
  V(MathRint, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "rint", "(D)D") \
  V(MathRoundDouble, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "round", "(D)J") \
  V(MathRoundFloat, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Math;", "round", "(F)I") \
  V(SystemArrayCopyChar, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/System;", "arraycopy", "([CI[CII)V") \
  V(SystemArrayCopy, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/System;", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V") \
  V(ThreadCurrentThread, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Thread;", "currentThread", "()Ljava/lang/Thread;") \
  V(MemoryPeekByte, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Llibcore/io/Memory;", "peekByte", "(J)B") \
  V(MemoryPeekIntNative, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Llibcore/io/Memory;", "peekIntNative", "(J)I") \
  V(MemoryPeekLongNative, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Llibcore/io/Memory;", "peekLongNative", "(J)J") \
  V(MemoryPeekShortNative, kStatic, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Llibcore/io/Memory;", "peekShortNative", "(J)S") \
  V(MemoryPokeByte, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow, "Llibcore/io/Memory;", "pokeByte", "(JB)V") \
  V(MemoryPokeIntNative, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow, "Llibcore/io/Memory;", "pokeIntNative", "(JI)V") \
  V(MemoryPokeLongNative, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow, "Llibcore/io/Memory;", "pokeLongNative", "(JJ)V") \
  V(MemoryPokeShortNative, kStatic, kNeedsEnvironmentOrCache, kWriteSideEffects, kCanThrow, "Llibcore/io/Memory;", "pokeShortNative", "(JS)V") \
  V(StringCharAt, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Ljava/lang/String;", "charAt", "(I)C") \
  V(StringCompareTo, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Ljava/lang/String;", "compareTo", "(Ljava/lang/String;)I") \
  V(StringEquals, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Ljava/lang/String;", "equals", "(Ljava/lang/Object;)Z") \
  V(StringGetCharsNoCheck, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Ljava/lang/String;", "getCharsNoCheck", "(II[CI)V") \
  V(StringIndexOf, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kNoThrow, "Ljava/lang/String;", "indexOf", "(I)I") \
  V(StringIndexOfAfter, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kNoThrow, "Ljava/lang/String;", "indexOf", "(II)I") \
  V(StringStringIndexOf, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Ljava/lang/String;", "indexOf", "(Ljava/lang/String;)I") \
  V(StringStringIndexOfAfter, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kCanThrow, "Ljava/lang/String;", "indexOf", "(Ljava/lang/String;I)I") \
  V(StringIsEmpty, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kNoThrow, "Ljava/lang/String;", "isEmpty", "()Z") \
  V(StringLength, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kNoThrow, "Ljava/lang/String;", "length", "()I") \
  V(StringNewStringFromBytes, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringFactory;", "newStringFromBytes", "([BIII)Ljava/lang/String;") \
  V(StringNewStringFromChars, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringFactory;", "newStringFromChars", "(II[C)Ljava/lang/String;") \
  V(StringNewStringFromString, kStatic, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringFactory;", "newStringFromString", "(Ljava/lang/String;)Ljava/lang/String;") \
  V(StringBufferAppend, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringBuffer;", "append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;") \
  V(StringBufferLength, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kNoThrow, "Ljava/lang/StringBuffer;", "length", "()I") \
  V(StringBufferToString, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringBuffer;", "toString", "()Ljava/lang/String;") \
  V(StringBuilderAppend, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringBuilder;", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;") \
  V(StringBuilderLength, kVirtual, kNeedsEnvironmentOrCache, kReadSideEffects, kNoThrow, "Ljava/lang/StringBuilder;", "length", "()I") \
  V(StringBuilderToString, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/StringBuilder;", "toString", "()Ljava/lang/String;") \
  V(UnsafeCASInt, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "compareAndSwapInt", "(Ljava/lang/Object;JII)Z") \
  V(UnsafeCASLong, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "compareAndSwapLong", "(Ljava/lang/Object;JJJ)Z") \
  V(UnsafeCASObject, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "compareAndSwapObject", "(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z") \
  V(UnsafeGet, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getInt", "(Ljava/lang/Object;J)I") \
  V(UnsafeGetVolatile, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getIntVolatile", "(Ljava/lang/Object;J)I") \
  V(UnsafeGetObject, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getObject", "(Ljava/lang/Object;J)Ljava/lang/Object;") \
  V(UnsafeGetObjectVolatile, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getObjectVolatile", "(Ljava/lang/Object;J)Ljava/lang/Object;") \
  V(UnsafeGetLong, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getLong", "(Ljava/lang/Object;J)J") \
  V(UnsafeGetLongVolatile, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getLongVolatile", "(Ljava/lang/Object;J)J") \
  V(UnsafePut, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putInt", "(Ljava/lang/Object;JI)V") \
  V(UnsafePutOrdered, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putOrderedInt", "(Ljava/lang/Object;JI)V") \
  V(UnsafePutVolatile, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putIntVolatile", "(Ljava/lang/Object;JI)V") \
  V(UnsafePutObject, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putObject", "(Ljava/lang/Object;JLjava/lang/Object;)V") \
  V(UnsafePutObjectOrdered, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putOrderedObject", "(Ljava/lang/Object;JLjava/lang/Object;)V") \
  V(UnsafePutObjectVolatile, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putObjectVolatile", "(Ljava/lang/Object;JLjava/lang/Object;)V") \
  V(UnsafePutLong, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putLong", "(Ljava/lang/Object;JJ)V") \
  V(UnsafePutLongOrdered, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putOrderedLong", "(Ljava/lang/Object;JJ)V") \
  V(UnsafePutLongVolatile, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "putLongVolatile", "(Ljava/lang/Object;JJ)V") \
  V(UnsafeGetAndAddInt, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getAndAddInt", "(Ljava/lang/Object;JI)I") \
  V(UnsafeGetAndAddLong, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getAndAddLong", "(Ljava/lang/Object;JJ)J") \
  V(UnsafeGetAndSetInt, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getAndSetInt", "(Ljava/lang/Object;JI)I") \
  V(UnsafeGetAndSetLong, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getAndSetLong", "(Ljava/lang/Object;JJ)J") \
  V(UnsafeGetAndSetObject, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "getAndSetObject", "(Ljava/lang/Object;JLjava/lang/Object;)Ljava/lang/Object;") \
  V(UnsafeLoadFence, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "loadFence", "()V") \
  V(UnsafeStoreFence, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "storeFence", "()V") \
  V(UnsafeFullFence, kVirtual, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Lsun/misc/Unsafe;", "fullFence", "()V") \
  V(ReferenceGetReferent, kDirect, kNeedsEnvironmentOrCache, kAllSideEffects, kCanThrow, "Ljava/lang/ref/Reference;", "getReferent", "()Ljava/lang/Object;") \
  V(IntegerValueOf, kStatic, kNeedsEnvironmentOrCache, kNoSideEffects, kNoThrow, "Ljava/lang/Integer;", "valueOf", "(I)Ljava/lang/Integer;")

#endif  // ART_COMPILER_INTRINSICS_LIST_H_
#undef ART_COMPILER_INTRINSICS_LIST_H_   // #define is only for lint.
