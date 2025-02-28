/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/tests/test_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>

#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/literal_util.h"
#include "xla/primitive_util.h"
#include "xla/service/hlo_dataflow_analysis.h"
#include "xla/service/hlo_verifier.h"
#include "xla/service/transfer_manager.h"

namespace xla {

namespace {

template <typename FloatT, typename GeneratorT>
void PopulateWithRandomFloatingPointData(Literal* literal,
                                         std::minstd_rand0* engine) {
  std::uniform_real_distribution<GeneratorT> generator(-0.1f, 0.2f);
  for (FloatT& value : literal->data<FloatT>()) {
    value = static_cast<FloatT>(generator(*engine));
  }
}

// Populates a floating point literal with random floating points sampled from a
// uniform-log distribution spanning approximately the entire range of the
// representable floating point.
template <typename FloatT>
void PopulateWithRandomFullRangeFloatingPointData(Literal* literal,
                                                  std::minstd_rand0* engine) {
  constexpr float kSpecialValueProbability = 1e-6;
  constexpr float kSpecialValues[] = {+0.F,
                                      -0.F,
                                      1.F,
                                      -1.F,
                                      std::numeric_limits<float>::infinity(),
                                      -std::numeric_limits<float>::infinity()};
  constexpr int kNumSpecialValues = sizeof(kSpecialValues) / sizeof(float);
  std::uniform_real_distribution<float> special_value_gen(0, 1);

  // Generates floating points with a log-uniform distribution. This causes the
  // exponent of the floating point to have a uniform distribution.
  int min_exp, max_exp;
  if (std::is_same<FloatT, bfloat16>()) {
    min_exp = std::numeric_limits<float>::min_exponent;
    max_exp = std::numeric_limits<float>::max_exponent;
  } else {
    min_exp = std::numeric_limits<FloatT>::min_exponent;
    max_exp = std::numeric_limits<FloatT>::max_exponent;
  }
  std::uniform_real_distribution<double> generator(min_exp - 1, max_exp - 1);

  for (FloatT& value : literal->data<FloatT>()) {
    // Each special value has a kSpecialValueProbability chance to be generated
    // instead of sampling using the normal distributions.
    if (special_value_gen(*engine) <
        kSpecialValueProbability * kNumSpecialValues) {
      value =
          static_cast<FloatT>(kSpecialValues[(*engine)() % kNumSpecialValues]);
    } else {
      float sign = ((*engine)() % 2 == 0) ? 1 : -1;
      value = static_cast<FloatT>(pow(2, generator(*engine)) * sign);
    }
  }
}

template <typename FloatT>
void PopulateWithIntNext(Literal* literal);

template <>
void PopulateWithIntNext<half>(Literal* literal) {
  // Duplicates may be generated if we don't have enough bits.
  uint16_t next_value = 0;
  for (half& value : literal->data<half>()) {
    // Zero-out the MSB of the exponent to avoid Infs and NaNs, and put it into
    // the sign bit. We could be less wasteful, but this is best-effort anyway.
    uint16_t exponent_msb = next_value & 0x4000;
    value = Eigen::numext::bit_cast<half, uint16_t>((next_value & 0xBFFF) |
                                                    (exponent_msb << 1));
    next_value++;
  }
}

template <>
void PopulateWithIntNext<bfloat16>(Literal* literal) {
  // Duplicates may be generated if we don't have enough bits.
  // Start at 0x80 rather than 0 to avoid denormals.
  uint16_t next_value = 0x80;
  for (bfloat16& value : literal->data<bfloat16>()) {
    // Zero-out the MSB of the exponent to avoid Infs and NaNs, and put it into
    // the sign bit. We could be less wasteful, but this is best-effort anyway.
    uint16_t exponent_msb = next_value & 0x4000;
    value = Eigen::numext::bit_cast<bfloat16, uint16_t>((next_value & 0xBFFF) |
                                                        (exponent_msb << 1));
    next_value++;
  }
}

template <typename FloatT>
void PopulateWithNextAfter(Literal* literal) {
  // Duplicates may be generated if the number of elements in the literal
  // exceeds the number of positive values supported by the type.
  float next_value = std::numeric_limits<float>::min();
  for (float& value : literal->data<float>()) {
    value = next_value;
    next_value = std::nextafter(next_value, std::numeric_limits<float>::max());
  }
}

template <typename FloatT,
          typename std::enable_if<std::is_same<bfloat16, FloatT>::value ||
                                      std::is_same<half, FloatT>::value,
                                  int>::type = 0>
void PopulateWithNoDuplicateData(Literal* literal, std::minstd_rand0* engine) {
  PopulateWithIntNext<FloatT>(literal);
  std::shuffle(literal->data<FloatT>().begin(), literal->data<FloatT>().end(),
               *engine);
}

template <typename FloatT,
          typename std::enable_if<!std::is_same<bfloat16, FloatT>::value &&
                                      !std::is_same<half, FloatT>::value,
                                  int>::type = 0>
void PopulateWithNoDuplicateData(Literal* literal, std::minstd_rand0* engine) {
  PopulateWithNextAfter<FloatT>(literal);
  std::shuffle(literal->data<FloatT>().begin(), literal->data<FloatT>().end(),
               *engine);
}

template <typename FloatT>
void PopulateWithFloatingPointData(Literal* literal, std::minstd_rand0* engine,
                                   bool no_duplicates, bool use_large_range) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<FloatT>());
  if (no_duplicates) {
    PopulateWithNoDuplicateData<FloatT>(literal, engine);
  } else if (use_large_range) {
    PopulateWithRandomFullRangeFloatingPointData<FloatT>(literal, engine);
  } else {
    PopulateWithRandomFloatingPointData<FloatT, FloatT>(literal, engine);
  }
}

template <typename ComplexT>
void PopulateWithComplexData(Literal* result, std::minstd_rand0* engine,
                             bool no_duplicates, bool use_large_range) {
  using InnerFloatT = typename ComplexT::value_type;
  CHECK(engine != nullptr);
  CHECK_EQ(result->shape().element_type(),
           primitive_util::NativeToPrimitiveType<ComplexT>());
  Shape floating_point_shape = ShapeUtil::ChangeElementType(
      result->shape(), primitive_util::NativeToPrimitiveType<InnerFloatT>());
  Literal real_lit(floating_point_shape);
  Literal imaginary_lit(floating_point_shape);

  PopulateWithFloatingPointData<InnerFloatT>(&real_lit, engine, no_duplicates,
                                             use_large_range);
  PopulateWithFloatingPointData<InnerFloatT>(&imaginary_lit, engine,
                                             no_duplicates, use_large_range);

  absl::Span<const InnerFloatT> real_data = real_lit.data<InnerFloatT>();
  absl::Span<const InnerFloatT> imaginary_data =
      imaginary_lit.data<InnerFloatT>();
  absl::Span<ComplexT> result_data = result->data<ComplexT>();
  for (int i = 0; i < real_lit.data<InnerFloatT>().size(); i++) {
    result_data[i] = ComplexT(real_data[i], imaginary_data[i]);
  }
}

template <>
void PopulateWithFloatingPointData<half>(Literal* literal,
                                         std::minstd_rand0* engine,
                                         bool no_duplicates,
                                         bool use_large_range) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<half>());
  if (no_duplicates) {
    PopulateWithNoDuplicateData<half>(literal, engine);
  } else if (use_large_range) {
    PopulateWithRandomFullRangeFloatingPointData<half>(literal, engine);
  } else {
    PopulateWithRandomFloatingPointData<half, float>(literal, engine);
  }
}

template <>
void PopulateWithFloatingPointData<bfloat16>(Literal* literal,
                                             std::minstd_rand0* engine,
                                             bool no_duplicates,
                                             bool use_large_range) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<bfloat16>());
  if (no_duplicates) {
    PopulateWithNoDuplicateData<bfloat16>(literal, engine);
  } else if (use_large_range) {
    PopulateWithRandomFullRangeFloatingPointData<bfloat16>(literal, engine);
  } else {
    PopulateWithRandomFloatingPointData<bfloat16, float>(literal, engine);
  }
}

template <>
void PopulateWithFloatingPointData<tsl::float8_e5m2>(Literal* literal,
                                                     std::minstd_rand0* engine,
                                                     bool no_duplicates,
                                                     bool use_large_range) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<tsl::float8_e5m2>());
  if (no_duplicates) {
    PopulateWithNoDuplicateData<tsl::float8_e5m2>(literal, engine);
  } else if (use_large_range) {
    PopulateWithRandomFullRangeFloatingPointData<tsl::float8_e5m2>(literal,
                                                                   engine);
  } else {
    PopulateWithRandomFloatingPointData<tsl::float8_e5m2, float>(literal,
                                                                 engine);
  }
}

template <>
void PopulateWithFloatingPointData<tsl::float8_e4m3fn>(
    Literal* literal, std::minstd_rand0* engine, bool no_duplicates,
    bool use_large_range) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<tsl::float8_e4m3fn>());
  if (no_duplicates) {
    PopulateWithNoDuplicateData<tsl::float8_e4m3fn>(literal, engine);
  } else if (use_large_range) {
    PopulateWithRandomFullRangeFloatingPointData<tsl::float8_e4m3fn>(literal,
                                                                     engine);
  } else {
    PopulateWithRandomFloatingPointData<tsl::float8_e4m3fn, float>(literal,
                                                                   engine);
  }
}

// uniform_int_distribution is not defined for 8-bit integers.
// Use 'short' for those types.
template <typename IntT>
struct RngT {
  using type = IntT;
};

template <>
struct RngT<int8_t> {
  using type = int16_t;
};

template <>
struct RngT<uint8_t> {
  using type = uint16_t;
};

template <typename IntT>
void PopulateWithRandomIntegralDataWithBounds(Literal* literal,
                                              std::minstd_rand0* engine,
                                              bool no_duplicates, IntT min,
                                              IntT max) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<IntT>());
  if (no_duplicates && ShapeUtil::ElementsIn(literal->shape()) < max) {
    std::iota(literal->data<IntT>().begin(), literal->data<IntT>().end(), 0);
    std::shuffle(literal->data<IntT>().begin(), literal->data<IntT>().end(),
                 *engine);
  } else {
    std::uniform_int_distribution<typename RngT<IntT>::type> generator(min,
                                                                       max);
    for (IntT& value : literal->data<IntT>()) {
      value = generator(*engine);
    }
  }
}

template <>
void PopulateWithRandomIntegralDataWithBounds<u4>(Literal* literal,
                                                  std::minstd_rand0* engine,
                                                  bool no_duplicates, u4 min,
                                                  u4 max) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<u4>());
  if (no_duplicates &&
      ShapeUtil::ElementsIn(literal->shape()) < static_cast<int64_t>(max)) {
    std::iota(literal->data<u4>().begin(), literal->data<u4>().end(), u4(0));
    std::shuffle(literal->data<u4>().begin(), literal->data<u4>().end(),
                 *engine);
  } else {
    std::uniform_int_distribution<uint8_t> generator(static_cast<uint8_t>(min),
                                                     static_cast<uint8_t>(max));
    for (u4& value : literal->data<u4>()) {
      value = static_cast<u4>(generator(*engine));
    }
  }
}

template <>
void PopulateWithRandomIntegralDataWithBounds<s4>(Literal* literal,
                                                  std::minstd_rand0* engine,
                                                  bool no_duplicates, s4 min,
                                                  s4 max) {
  CHECK(engine != nullptr);
  CHECK_EQ(literal->shape().element_type(),
           primitive_util::NativeToPrimitiveType<s4>());
  if (no_duplicates &&
      ShapeUtil::ElementsIn(literal->shape()) < static_cast<int64_t>(max)) {
    std::iota(literal->data<s4>().begin(), literal->data<s4>().end(), s4(0));
    std::shuffle(literal->data<s4>().begin(), literal->data<s4>().end(),
                 *engine);
  } else {
    std::uniform_int_distribution<uint8_t> generator(static_cast<uint8_t>(min),
                                                     static_cast<uint8_t>(max));
    for (s4& value : literal->data<s4>()) {
      value = static_cast<s4>(generator(*engine));
    }
  }
}

// Similar to MakeFakeLiteral but takes a random number generator engine to
// enable reusing the engine across randomly generated literals.
// 'limit' is a optional pair that contains the min and the max values to be
// sample for integers (integer format only).
// 'is_sorted' sorts the sample data for integers (integer format only).
// 'no_duplicates' indicates that there should be no duplicate values in each
// generated array. This is uniqueness is best-effort only. Some types
// (half and bfloat16) are not supported and uniqueness cannot be guaranteed if
// the number of elements exceeds the number of different values supported by
// the type. (floating point format only)
// 'use_large_range' indicates the sampled data is from the full range of the
// floating point format. (floating point format only)
StatusOr<Literal> MakeFakeLiteralInternal(
    const Shape& shape, std::minstd_rand0* engine,
    std::optional<std::pair<int64_t, int64_t>> limit, bool is_sorted,
    bool no_duplicates, bool use_large_range) {
  if (shape.IsTuple()) {
    std::vector<Literal> elements;
    const auto& shape_tuple_shapes = shape.tuple_shapes();
    elements.reserve(shape_tuple_shapes.size());
    for (const Shape& element_shape : shape_tuple_shapes) {
      TF_ASSIGN_OR_RETURN(
          Literal element,
          MakeFakeLiteralInternal(element_shape, engine, limit, is_sorted,
                                  no_duplicates, use_large_range));
      elements.push_back(std::move(element));
    }
    return LiteralUtil::MakeTupleOwned(std::move(elements));
  }
  if (engine == nullptr) {
    return Literal::CreateFromShape(shape);
  }
  // Clear tiles/element size in shape's layout before using it for creating
  // literal.
  Shape new_shape = shape;
  new_shape.mutable_layout()->clear_tiles();
  Literal literal(new_shape);

  int64_t max = std::numeric_limits<int64_t>::max();
  int64_t min = std::numeric_limits<int64_t>::lowest();
  switch (shape.element_type()) {
    case F8E5M2:
      PopulateWithFloatingPointData<tsl::float8_e5m2>(
          &literal, engine, no_duplicates, use_large_range);
      break;
    case F8E4M3FN:
      PopulateWithFloatingPointData<tsl::float8_e4m3fn>(
          &literal, engine, no_duplicates, use_large_range);
      break;
    case BF16:
      PopulateWithFloatingPointData<bfloat16>(&literal, engine, no_duplicates,
                                              use_large_range);
      break;
    case F16:
      PopulateWithFloatingPointData<half>(&literal, engine, no_duplicates,
                                          use_large_range);
      break;
    case F32:
      PopulateWithFloatingPointData<float>(&literal, engine, no_duplicates,
                                           use_large_range);
      break;
    case F64:
      PopulateWithFloatingPointData<double>(&literal, engine, no_duplicates,
                                            use_large_range);
      break;
    case S4: {
      max = static_cast<int64_t>(std::numeric_limits<s4>::max());
      min = static_cast<int64_t>(std::numeric_limits<s4>::lowest());
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<s4>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<s4>(min), static_cast<s4>(max));
      if (is_sorted) {
        std::sort(literal.data<s4>().begin(), literal.data<s4>().end());
      }
      break;
    }
    case S8:
      max = std::numeric_limits<int8_t>::max();
      min = std::numeric_limits<int8_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<int8_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<int8_t>(min), static_cast<int8_t>(max));
      if (is_sorted) {
        std::sort(literal.data<int8_t>().begin(), literal.data<int8_t>().end());
      }
      break;
    case U4:
      max = static_cast<int64_t>(std::numeric_limits<u4>::max());
      min = static_cast<int64_t>(std::numeric_limits<u4>::lowest());
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<u4>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<u4>(min), static_cast<u4>(max));
      if (is_sorted) {
        std::sort(literal.data<u4>().begin(), literal.data<u4>().end());
      }
      break;
    case U8:
      max = std::numeric_limits<uint8_t>::max();
      min = std::numeric_limits<uint8_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<uint8_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<uint8_t>(min), static_cast<uint8_t>(max));
      if (is_sorted) {
        std::sort(literal.data<uint8_t>().begin(),
                  literal.data<uint8_t>().end());
      }
      break;
    case S16:
      max = std::numeric_limits<int16_t>::max();
      min = std::numeric_limits<int16_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<int16_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<int16_t>(min), static_cast<int16_t>(max));
      if (is_sorted) {
        std::sort(literal.data<int16_t>().begin(),
                  literal.data<int16_t>().end());
      }
      break;
    case U16:
      max = std::numeric_limits<uint16_t>::max();
      min = std::numeric_limits<uint16_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<uint16_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<uint16_t>(min), static_cast<uint16_t>(max));
      if (is_sorted) {
        std::sort(literal.data<uint16_t>().begin(),
                  literal.data<uint16_t>().end());
      }
      break;
    case S32:
      max = std::numeric_limits<int32_t>::max();
      min = std::numeric_limits<int32_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<int32_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<int32_t>(min), static_cast<int32_t>(max));
      if (is_sorted) {
        std::sort(literal.data<int32_t>().begin(),
                  literal.data<int32_t>().end());
      }
      break;
    case U32:
      max = std::numeric_limits<uint32_t>::max();
      min = std::numeric_limits<uint32_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<uint32_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<uint32_t>(min), static_cast<uint32_t>(max));
      if (is_sorted) {
        std::sort(literal.data<uint32_t>().begin(),
                  literal.data<uint32_t>().end());
      }
      break;
    case S64:
      max = std::numeric_limits<int64_t>::max();
      min = std::numeric_limits<int64_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<int64_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<int64_t>(min), static_cast<int64_t>(max));
      if (is_sorted) {
        std::sort(literal.data<int64_t>().begin(),
                  literal.data<int64_t>().end());
      }
      break;
    case U64:
      max = std::numeric_limits<uint64_t>::max();
      min = std::numeric_limits<uint64_t>::lowest();
      if (limit.has_value()) {
        max = limit->second;
        min = limit->first;
      }
      PopulateWithRandomIntegralDataWithBounds<uint64_t>(
          &literal, engine, /*no_duplicate*/ no_duplicates,
          static_cast<uint64_t>(min), static_cast<uint64_t>(max));
      if (is_sorted) {
        std::sort(literal.data<uint64_t>().begin(),
                  literal.data<uint64_t>().end());
      }
      break;
    case C64:
      PopulateWithComplexData<complex64>(&literal, engine, no_duplicates,
                                         use_large_range);
      break;
    case C128:
      PopulateWithComplexData<complex128>(&literal, engine, no_duplicates,
                                          use_large_range);
      break;
    case PRED: {
      std::uniform_int_distribution<int> generator(0, 1);
      TF_CHECK_OK(
          literal.Populate<bool>([&](absl::Span<const int64_t> /*indices*/) {
            return generator(*engine);
          }));
      break;
    }
    default:
      return Unimplemented(
          "Unsupported type for fake random literal generation with bounds: "
          "%s",
          ShapeUtil::HumanString(shape));
  }
  return std::move(literal);
}

enum class ConstantType { kUnknown, kZero, kOne };

// Return the constant type required by this computation, if known.
ConstantType GetInitValue(const HloComputation& computation) {
  // TODO(b/77635120): Add init values, for min, max, and their arg variants.
  const HloInstruction* const root = computation.root_instruction();
  if (computation.num_parameters() != 2 || root->operand_count() != 2 ||
      root->operand(0)->opcode() != HloOpcode::kParameter ||
      root->operand(1)->opcode() != HloOpcode::kParameter ||
      root->operand(0) == root->operand(1)) {
    return ConstantType::kUnknown;
  }

  switch (root->opcode()) {
    case HloOpcode::kAdd:
      return ConstantType::kZero;
    case HloOpcode::kMultiply:
      return ConstantType::kOne;
    default:
      return ConstantType::kUnknown;
  }
}

// Reduce, ReduceWindow, and SelectAndScatter ops may need a non-random
// initialization value.
bool NeedsInitValue(const HloUse& use) {
  const HloInstruction* const instruction = use.instruction;
  const HloOpcode opcode = instruction->opcode();
  const int64_t op_num = use.operand_number;
  return ((opcode == HloOpcode::kReduceWindow && op_num == 1) ||
          (opcode == HloOpcode::kSelectAndScatter && op_num == 2) ||
          (opcode == HloOpcode::kReduce &&
           op_num >= instruction->operand_count() / 2));
}

// Generate random values that are constrained to the input_shape minus the
// output_shape so as not to produce wrapping slices, for instance.
Literal MakeRandomIndex(int64_t index_bound, std::minstd_rand0* engine) {
  std::uniform_int_distribution<int32_t> generator(0, index_bound);
  return LiteralUtil::CreateR0<int32_t>(generator(*engine));
}

// Returns true if `dest' is reachable from `src' through data-formatting and
// custom call instructions within the same computation.
bool ReachableViaDataFormatting(const HloInstruction* src,
                                const HloInstruction* dest,
                                bool treat_gte_as_data_formatting) {
  if (src == dest) {
    return true;
  }
  switch (dest->opcode()) {
    case HloOpcode::kReshape:
    case HloOpcode::kTranspose:
    case HloOpcode::kCopy:
    case HloOpcode::kSlice:
      break;
    case HloOpcode::kCustomCall:
      if (dest->custom_call_target() == "AssumeGatherIndicesInBound") {
        break;
      }
      return false;
    // TODO(b/249417724): a workaround for tuple param.
    case HloOpcode::kGetTupleElement:
      if (treat_gte_as_data_formatting) {
        break;
      } else {
        return false;
      }
    default:
      return false;
  }
  for (const auto* operand : dest->operands()) {
    if (ReachableViaDataFormatting(src, operand,
                                   treat_gte_as_data_formatting)) {
      return true;
    }
  }
  return false;
}

// Use dataflow analysis on each parameter to see if there are uses that would
// be problematic when generating input data.  Returns the list of
// instructions that correspond to their uses.
//
// Should be paired with the CreateLiteralForConstrainedUses() function below.
std::vector<HloInstruction*> FindConstrainedUses(
    const HloDataflowAnalysis& dataflow, const HloInstruction& param,
    bool treat_gte_as_data_formatting) {
  std::vector<HloInstruction*> constrained_uses;
  for (const auto& pair : dataflow.GetInstructionValueSet(&param)) {
    const HloValue& value = dataflow.GetUniqueValueAt(&param, pair.first);
    for (const HloUse& use : value.GetUses()) {
      HloInstruction* instruction = use.instruction;
      const HloOpcode opcode = instruction->opcode();
      const int64_t op_num = use.operand_number;
      if ((opcode == HloOpcode::kDynamicSlice && op_num >= 1) ||
          (opcode == HloOpcode::kDynamicUpdateSlice && op_num >= 2)) {
        constrained_uses.push_back(instruction);
      } else if ((opcode == HloOpcode::kGather ||
                  opcode == HloOpcode::kScatter) &&
                 op_num == 1) {
        constrained_uses.push_back(instruction);
      } else if (opcode == HloOpcode::kFusion) {
        const HloInstruction* const to_analyze =
            instruction->fused_parameter(op_num);
        auto fused_uses = FindConstrainedUses(dataflow, *to_analyze,
                                              treat_gte_as_data_formatting);
        constrained_uses.insert(constrained_uses.end(), fused_uses.begin(),
                                fused_uses.end());
      } else if (NeedsInitValue(use)) {
        constrained_uses.push_back(instruction);
      } else if (opcode == HloOpcode::kConvert ||
                 opcode == HloOpcode::kReducePrecision) {
        auto converted_uses = FindConstrainedUses(dataflow, *instruction,
                                                  treat_gte_as_data_formatting);
        constrained_uses.insert(constrained_uses.end(), converted_uses.begin(),
                                converted_uses.end());
      } else if (opcode == HloOpcode::kSort &&
                 instruction->operand_count() >= 2 && op_num == 0) {
        // Operand 0 of sort is the array of keys used for key/value
        // (two-operand) kSort instructions. Since sort stability is not
        // guaranteed, constrain keys of key-value sort not to have
        // duplicates, since otherwise the value order may legitimately
        // differ.
        constrained_uses.push_back(instruction);
      }
    }
  }

  for (auto* instruction : param.parent()->instructions()) {
    const HloOpcode opcode = instruction->opcode();
    if (opcode == HloOpcode::kGather || opcode == HloOpcode::kScatter) {
      if (instruction->operand(1) == &param) {
        // Above already covers this case.
        continue;
      }
      if (ReachableViaDataFormatting(&param, instruction->operand(1),
                                     treat_gte_as_data_formatting)) {
        constrained_uses.push_back(instruction);
      }
    }
  }
  return constrained_uses;
}

// Given a parameter, generate a random Literal to use as input if there exist
// no constrained uses in the dataflow graph.  If such constraints exist,
// generate a constrained literal (either bounded in the case of indices, or
// zero in the case of init_values for reductions).
StatusOr<Literal> CreateLiteralForConstrainedUses(
    const absl::Span<HloInstruction* const> constrained_uses,
    const HloInstruction& param, const Shape& param_shape,
    std::minstd_rand0* engine, bool use_large_range) {
  int64_t index_bound = INT64_MAX;
  bool no_duplicates = false;
  bool needs_constant = false;
  bool needs_sorted_indices = false;
  ConstantType constant_type = ConstantType::kUnknown;
  for (HloInstruction* use : constrained_uses) {
    switch (use->opcode()) {
      case HloOpcode::kDynamicSlice:
      case HloOpcode::kDynamicUpdateSlice: {
        const Shape& indexed_shape = use->operand(0)->shape();
        const Shape& slice_shape = use->opcode() == HloOpcode::kDynamicSlice
                                       ? use->shape()
                                       : use->operand(1)->shape();
        const int64_t first_index =
            Cast<HloDynamicIndexInstruction>(use)->first_index_operand_number();
        for (int64_t operand = first_index; operand < use->operand_count();
             ++operand) {
          if (use->operand(operand) == &param) {
            index_bound = std::min(
                index_bound,
                ShapeUtil::GetDimension(indexed_shape, operand - first_index) -
                    ShapeUtil::GetDimension(slice_shape,
                                            operand - first_index));
          }
        }
        break;
      }
      case HloOpcode::kGather:
      case HloOpcode::kScatter: {
        const Shape& operand_shape = use->operand(0)->shape();
        auto index_map = use->opcode() == HloOpcode::kGather
                             ? use->gather_dimension_numbers().start_index_map()
                             : use->scatter_dimension_numbers()
                                   .scatter_dims_to_operand_dims();
        for (const auto dim_in_operand : index_map) {
          index_bound = std::min(index_bound,
                                 operand_shape.dimensions(dim_in_operand) - 1);
        }
        if (use->opcode() == HloOpcode::kScatter) {
          needs_sorted_indices |=
              Cast<const HloScatterInstruction>(use)->indices_are_sorted();
        } else {
          needs_sorted_indices |=
              Cast<const HloGatherInstruction>(use)->indices_are_sorted();
        }
        break;
      }
      case HloOpcode::kReduce:
      case HloOpcode::kReduceWindow:
        needs_constant = true;
        constant_type = GetInitValue(*use->to_apply());
        break;

      case HloOpcode::kSelectAndScatter:
        needs_constant = true;
        constant_type = GetInitValue(*use->scatter());
        break;

      case HloOpcode::kSort:
        no_duplicates = true;
        break;

      default:
        return Unimplemented(
            "Constrained operand generation not implemented for %s.",
            use->ToString());
    }
  }
  int constraint_count = 0;
  constraint_count += no_duplicates ? 1 : 0;
  constraint_count += (index_bound != INT64_MAX) ? 1 : 0;
  constraint_count += needs_constant ? 1 : 0;
  if (constraint_count > 1) {
    return Unimplemented("Conflicting operand generation constraints.");
  }
  if (index_bound != INT64_MAX) {
    return MakeFakeLiteralInternal(
        param_shape, engine, std::pair<int64_t, int64_t>(0, index_bound),
        needs_sorted_indices, no_duplicates, use_large_range);
  } else if (needs_constant) {
    switch (constant_type) {
      case ConstantType::kZero:
        return LiteralUtil::Zero(param_shape.element_type());
      case ConstantType::kOne:
        return LiteralUtil::One(param_shape.element_type());
      case ConstantType::kUnknown:
        // We want the identity element for the computation, but we don't
        // really know what it is - so any value we generate will be just as
        // wrong.
        return MakeFakeLiteralInternal(
            param_shape, engine, /*limit=*/std::nullopt,
            /*is_sorted=*/needs_sorted_indices,
            /*no_duplicates=*/false, use_large_range);
    }
  } else {
    return MakeFakeLiteralInternal(param_shape, engine, /*limit=*/std::nullopt,
                                   /*is_sorted=*/needs_sorted_indices,
                                   no_duplicates, use_large_range);
  }
}

// Given a module entry parameter, use the dataflow analysis to see if a
// special case literal must be created, or if we can generate fake data.
StatusOr<Literal> MakeConstrainedArgument(const HloDataflowAnalysis& dataflow,
                                          const HloInstruction& param,
                                          const Shape& param_shape,
                                          std::minstd_rand0* engine,
                                          bool use_large_range,
                                          bool treat_gte_as_data_formatting) {
  const auto constrained_uses =
      FindConstrainedUses(dataflow, param, treat_gte_as_data_formatting);
  return CreateLiteralForConstrainedUses(constrained_uses, param, param_shape,
                                         engine, use_large_range);
}

}  // namespace

StatusOr<Literal> MakeFakeLiteral(const Shape& shape, bool pseudo_random,
                                  bool use_large_range) {
  auto engine = pseudo_random ? std::make_unique<std::minstd_rand0>() : nullptr;
  return MakeFakeLiteralInternal(shape, engine.get(), /*limit=*/std::nullopt,
                                 /*is_sorted=*/false,
                                 /*no_duplicates=*/false, use_large_range);
}

StatusOr<std::vector<Literal>> MakeFakeArguments(
    const HloModule* module, bool pseudo_random, bool use_large_range,
    bool treat_gte_as_data_formatting) {
  auto engine = pseudo_random ? std::make_unique<std::minstd_rand0>() : nullptr;
  return MakeFakeArguments(module, engine.get(), use_large_range,
                           treat_gte_as_data_formatting);
}

StatusOr<std::vector<Literal>> MakeFakeArguments(
    const HloModule* module, std::minstd_rand0* engine, bool use_large_range,
    bool treat_gte_as_data_formatting) {
  TF_ASSIGN_OR_RETURN(auto dataflow, HloDataflowAnalysis::Run(*module));
  const auto params = module->entry_computation()->parameter_instructions();
  std::vector<Literal> arguments(params.size());
  for (int i = 0; i < params.size(); ++i) {
    const HloModuleConfig& module_config = module->config();
    const Shape& param_shape = (module_config.has_entry_computation_layout() &&
                                module_config.entry_computation_layout()
                                    .parameter_layout(i)
                                    .shape()
                                    .is_static())
                                   ? module_config.entry_computation_layout()
                                         .parameter_layout(i)
                                         .shape()
                                   : params[i]->shape();

    TF_ASSIGN_OR_RETURN(
        arguments[i],
        MakeConstrainedArgument(*dataflow, *params[i], param_shape, engine,
                                use_large_range, treat_gte_as_data_formatting));
  }
  return std::move(arguments);
}

Status VerifyHloModule(HloModule* const module, bool layout_sensitive,
                       bool allow_mixed_precision) {
  return HloVerifier(/*layout_sensitive=*/layout_sensitive,
                     /*allow_mixed_precision=*/allow_mixed_precision)
      .Run(module)
      .status();
}

std::unique_ptr<HloDotInstruction> CreateCanonicalDot(const Shape& shape,
                                                      HloInstruction* lhs,
                                                      HloInstruction* rhs) {
  CHECK_LE(lhs->shape().rank(), 2);
  CHECK_LE(rhs->shape().rank(), 2);
  PrecisionConfig precision_config;
  precision_config.mutable_operand_precision()->Resize(
      2, PrecisionConfig::DEFAULT);
  DotDimensionNumbers dot_dimension_numbers;
  dot_dimension_numbers.add_lhs_contracting_dimensions(
      lhs->shape().rank() > 1 ? 1 : 0);
  dot_dimension_numbers.add_rhs_contracting_dimensions(0);
  return std::make_unique<HloDotInstruction>(
      shape, lhs, rhs, dot_dimension_numbers, precision_config);
}

bool IsMlirLoweringEnabled() {
  char* xla_flags = getenv("XLA_FLAGS");
  if (!xla_flags) {
    return false;
  }
  return !absl::StrContains(xla_flags, "--xla_cpu_use_xla_runtime=false") &&
         (absl::StrContains(xla_flags, "--xla_cpu_use_xla_runtime"));
}

}  // namespace xla
