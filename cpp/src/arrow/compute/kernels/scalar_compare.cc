// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cmath>
#include <limits>

#include "arrow/compute/api_scalar.h"
#include "arrow/compute/kernels/common.h"
#include "arrow/util/bitmap_ops.h"

namespace arrow {

using internal::checked_cast;
using internal::checked_pointer_cast;
using util::string_view;

namespace compute {
namespace internal {

namespace {

struct Equal {
  template <typename T, typename Arg0, typename Arg1>
  static constexpr T Call(KernelContext*, const Arg0& left, const Arg1& right, Status*) {
    static_assert(std::is_same<T, bool>::value && std::is_same<Arg0, Arg1>::value, "");
    return left == right;
  }
};

struct NotEqual {
  template <typename T, typename Arg0, typename Arg1>
  static constexpr T Call(KernelContext*, const Arg0& left, const Arg1& right, Status*) {
    static_assert(std::is_same<T, bool>::value && std::is_same<Arg0, Arg1>::value, "");
    return left != right;
  }
};

struct Greater {
  template <typename T, typename Arg0, typename Arg1>
  static constexpr T Call(KernelContext*, const Arg0& left, const Arg1& right, Status*) {
    static_assert(std::is_same<T, bool>::value && std::is_same<Arg0, Arg1>::value, "");
    return left > right;
  }
};

struct GreaterEqual {
  template <typename T, typename Arg0, typename Arg1>
  static constexpr T Call(KernelContext*, const Arg0& left, const Arg1& right, Status*) {
    static_assert(std::is_same<T, bool>::value && std::is_same<Arg0, Arg1>::value, "");
    return left >= right;
  }
};

struct Minimum {
  template <typename T, typename Arg0, typename Arg1>
  static enable_if_floating_value<T> Call(Arg0 left, Arg1 right) {
    static_assert(std::is_same<T, Arg0>::value && std::is_same<Arg0, Arg1>::value, "");
    return std::fmin(left, right);
  }

  template <typename T, typename Arg0, typename Arg1>
  static enable_if_integer_value<T> Call(Arg0 left, Arg1 right) {
    static_assert(std::is_same<T, Arg0>::value && std::is_same<Arg0, Arg1>::value, "");
    return std::min(left, right);
  }

  template <typename T, typename Arg0, typename Arg1>
  static enable_if_decimal_value<T> Call(Arg0 left, Arg1 right) {
    static_assert(std::is_same<T, Arg0>::value && std::is_same<Arg0, Arg1>::value, "");
    return std::min(left, right);
  }

  static util::string_view CallBinary(util::string_view left, util::string_view right) {
    return std::min(left, right);
  }

  template <typename T>
  static constexpr enable_if_t<std::is_same<float, T>::value, T> antiextreme() {
    return std::nanf("");
  }

  template <typename T>
  static constexpr enable_if_t<std::is_same<double, T>::value, T> antiextreme() {
    return std::nan("");
  }

  template <typename T>
  static constexpr enable_if_integer_value<T> antiextreme() {
    return std::numeric_limits<T>::max();
  }

  template <typename T>
  static constexpr enable_if_decimal_value<T> antiextreme() {
    return T::GetMaxSentinel();
  }
};

struct Maximum {
  template <typename T, typename Arg0, typename Arg1>
  static enable_if_floating_value<T> Call(Arg0 left, Arg1 right) {
    static_assert(std::is_same<T, Arg0>::value && std::is_same<Arg0, Arg1>::value, "");
    return std::fmax(left, right);
  }

  template <typename T, typename Arg0, typename Arg1>
  static enable_if_integer_value<T> Call(Arg0 left, Arg1 right) {
    static_assert(std::is_same<T, Arg0>::value && std::is_same<Arg0, Arg1>::value, "");
    return std::max(left, right);
  }

  template <typename T, typename Arg0, typename Arg1>
  static enable_if_decimal_value<T> Call(Arg0 left, Arg1 right) {
    static_assert(std::is_same<T, Arg0>::value && std::is_same<Arg0, Arg1>::value, "");
    return std::max(left, right);
  }

  static util::string_view CallBinary(util::string_view left, util::string_view right) {
    return std::max(left, right);
  }

  template <typename T>
  static constexpr enable_if_t<std::is_same<float, T>::value, T> antiextreme() {
    return std::nanf("");
  }

  template <typename T>
  static constexpr enable_if_t<std::is_same<double, T>::value, T> antiextreme() {
    return std::nan("");
  }

  template <typename T>
  static constexpr enable_if_integer_value<T> antiextreme() {
    return std::numeric_limits<T>::min();
  }

  template <typename T>
  static constexpr enable_if_decimal_value<T> antiextreme() {
    return T::GetMinSentinel();
  }
};

// Implement Less, LessEqual by flipping arguments to Greater, GreaterEqual

template <typename OutType, typename ArgType, typename Op>
struct CompareTimestamps
    : public applicator::ScalarBinaryEqualTypes<OutType, ArgType, Op> {
  using Base = applicator::ScalarBinaryEqualTypes<OutType, ArgType, Op>;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const auto& lhs = checked_cast<const TimestampType&>(*batch[0].type());
    const auto& rhs = checked_cast<const TimestampType&>(*batch[1].type());
    if (lhs.timezone().empty() ^ rhs.timezone().empty()) {
      return Status::Invalid(
          "Cannot compare timestamp with timezone to timestamp without timezone, got: ",
          lhs, " and ", rhs);
    }
    return Base::Exec(ctx, batch, out);
  }
};

template <typename Op>
void AddIntegerCompare(const std::shared_ptr<DataType>& ty, ScalarFunction* func) {
  auto exec =
      GeneratePhysicalInteger<applicator::ScalarBinaryEqualTypes, BooleanType, Op>(*ty);
  DCHECK_OK(func->AddKernel({ty, ty}, boolean(), std::move(exec)));
}

template <typename InType, typename Op>
void AddGenericCompare(const std::shared_ptr<DataType>& ty, ScalarFunction* func) {
  DCHECK_OK(
      func->AddKernel({ty, ty}, boolean(),
                      applicator::ScalarBinaryEqualTypes<BooleanType, InType, Op>::Exec));
}

struct CompareFunction : ScalarFunction {
  using ScalarFunction::ScalarFunction;

  Result<const Kernel*> DispatchBest(std::vector<ValueDescr>* values) const override {
    RETURN_NOT_OK(CheckArity(*values));
    if (HasDecimal(*values)) {
      RETURN_NOT_OK(CastBinaryDecimalArgs(DecimalPromotion::kAdd, values));
    }

    using arrow::compute::detail::DispatchExactImpl;
    if (auto kernel = DispatchExactImpl(this, *values)) return kernel;

    EnsureDictionaryDecoded(values);
    ReplaceNullWithOtherType(values);

    if (auto type = CommonNumeric(*values)) {
      ReplaceTypes(type, values);
    } else if (auto type = CommonTemporal(values->data(), values->size())) {
      ReplaceTypes(type, values);
    } else if (auto type = CommonBinary(values->data(), values->size())) {
      ReplaceTypes(type, values);
    }

    if (auto kernel = DispatchExactImpl(this, *values)) return kernel;
    return arrow::compute::detail::NoMatchingKernel(this, *values);
  }
};

struct VarArgsCompareFunction : ScalarFunction {
  using ScalarFunction::ScalarFunction;

  Result<const Kernel*> DispatchBest(std::vector<ValueDescr>* values) const override {
    RETURN_NOT_OK(CheckArity(*values));

    using arrow::compute::detail::DispatchExactImpl;
    if (auto kernel = DispatchExactImpl(this, *values)) return kernel;

    EnsureDictionaryDecoded(values);

    if (auto type = CommonNumeric(*values)) {
      ReplaceTypes(type, values);
    } else if (auto type = CommonTemporal(values->data(), values->size())) {
      ReplaceTypes(type, values);
    }

    if (auto kernel = DispatchExactImpl(this, *values)) return kernel;
    return arrow::compute::detail::NoMatchingKernel(this, *values);
  }
};

template <typename Op>
std::shared_ptr<ScalarFunction> MakeCompareFunction(std::string name,
                                                    const FunctionDoc* doc) {
  auto func = std::make_shared<CompareFunction>(name, Arity::Binary(), doc);

  DCHECK_OK(func->AddKernel(
      {boolean(), boolean()}, boolean(),
      applicator::ScalarBinary<BooleanType, BooleanType, BooleanType, Op>::Exec));

  for (const std::shared_ptr<DataType>& ty : IntTypes()) {
    AddIntegerCompare<Op>(ty, func.get());
  }
  AddIntegerCompare<Op>(date32(), func.get());
  AddIntegerCompare<Op>(date64(), func.get());

  AddGenericCompare<FloatType, Op>(float32(), func.get());
  AddGenericCompare<DoubleType, Op>(float64(), func.get());

  // Add timestamp kernels
  for (auto unit : TimeUnit::values()) {
    InputType in_type(match::TimestampTypeUnit(unit));
    DCHECK_OK(func->AddKernel({in_type, in_type}, boolean(),
                              CompareTimestamps<BooleanType, TimestampType, Op>::Exec));
  }

  // Duration
  for (auto unit : TimeUnit::values()) {
    InputType in_type(match::DurationTypeUnit(unit));
    auto exec =
        GeneratePhysicalInteger<applicator::ScalarBinaryEqualTypes, BooleanType, Op>(
            int64());
    DCHECK_OK(func->AddKernel({in_type, in_type}, boolean(), std::move(exec)));
  }

  // Time32 and Time64
  for (auto unit : {TimeUnit::SECOND, TimeUnit::MILLI}) {
    InputType in_type(match::Time32TypeUnit(unit));
    auto exec =
        GeneratePhysicalInteger<applicator::ScalarBinaryEqualTypes, BooleanType, Op>(
            int32());
    DCHECK_OK(func->AddKernel({in_type, in_type}, boolean(), std::move(exec)));
  }
  for (auto unit : {TimeUnit::MICRO, TimeUnit::NANO}) {
    InputType in_type(match::Time64TypeUnit(unit));
    auto exec =
        GeneratePhysicalInteger<applicator::ScalarBinaryEqualTypes, BooleanType, Op>(
            int64());
    DCHECK_OK(func->AddKernel({in_type, in_type}, boolean(), std::move(exec)));
  }

  for (const std::shared_ptr<DataType>& ty : BaseBinaryTypes()) {
    auto exec =
        GenerateVarBinaryBase<applicator::ScalarBinaryEqualTypes, BooleanType, Op>(*ty);
    DCHECK_OK(func->AddKernel({ty, ty}, boolean(), std::move(exec)));
  }

  for (const auto id : {Type::DECIMAL128, Type::DECIMAL256}) {
    auto exec = GenerateDecimal<applicator::ScalarBinaryEqualTypes, BooleanType, Op>(id);
    DCHECK_OK(
        func->AddKernel({InputType(id), InputType(id)}, boolean(), std::move(exec)));
  }

  {
    auto exec =
        applicator::ScalarBinaryEqualTypes<BooleanType, FixedSizeBinaryType, Op>::Exec;
    auto ty = InputType(Type::FIXED_SIZE_BINARY);
    DCHECK_OK(func->AddKernel({ty, ty}, boolean(), std::move(exec)));
  }

  return func;
}

std::shared_ptr<ScalarFunction> MakeFlippedFunction(std::string name,
                                                    const ScalarFunction& func,
                                                    const FunctionDoc* doc) {
  auto flipped_func = std::make_shared<CompareFunction>(name, Arity::Binary(), doc);
  for (const ScalarKernel* kernel : func.kernels()) {
    ScalarKernel flipped_kernel = *kernel;
    flipped_kernel.exec = MakeFlippedBinaryExec(kernel->exec);
    DCHECK_OK(flipped_func->AddKernel(std::move(flipped_kernel)));
  }
  return flipped_func;
}

using MinMaxState = OptionsWrapper<ElementWiseAggregateOptions>;

// Implement a variadic scalar min/max kernel.
template <typename OutType, typename Op>
struct ScalarMinMax {
  using OutValue = typename GetOutputType<OutType>::T;

  static void ExecScalar(const ExecBatch& batch,
                         const ElementWiseAggregateOptions& options, Scalar* out) {
    // All arguments are scalar
    OutValue value{};
    bool valid = false;
    for (const auto& arg : batch.values) {
      // Ignore non-scalar arguments so we can use it in the mixed-scalar-and-array case
      if (!arg.is_scalar()) continue;
      const auto& scalar = *arg.scalar();
      if (!scalar.is_valid) {
        if (options.skip_nulls) continue;
        out->is_valid = false;
        return;
      }
      if (!valid) {
        value = UnboxScalar<OutType>::Unbox(scalar);
        valid = true;
      } else {
        value = Op::template Call<OutValue, OutValue, OutValue>(
            value, UnboxScalar<OutType>::Unbox(scalar));
      }
    }
    out->is_valid = valid;
    if (valid) {
      BoxScalar<OutType>::Box(value, out);
    }
  }

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const ElementWiseAggregateOptions& options = MinMaxState::Get(ctx);
    const auto descrs = batch.GetDescriptors();
    const size_t scalar_count =
        static_cast<size_t>(std::count_if(batch.values.begin(), batch.values.end(),
                                          [](const Datum& d) { return d.is_scalar(); }));
    if (scalar_count == batch.values.size()) {
      ExecScalar(batch, options, out->scalar().get());
      return Status::OK();
    }

    ArrayData* output = out->mutable_array();

    // At least one array, two or more arguments
    ArrayDataVector arrays;
    for (const auto& arg : batch.values) {
      if (!arg.is_array()) continue;
      arrays.push_back(arg.array());
    }

    bool initialize_output = true;
    if (scalar_count > 0) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Scalar> temp_scalar,
                            MakeScalar(out->type(), 0));
      ExecScalar(batch, options, temp_scalar.get());
      if (temp_scalar->is_valid) {
        const auto value = UnboxScalar<OutType>::Unbox(*temp_scalar);
        initialize_output = false;
        OutValue* out = output->GetMutableValues<OutValue>(1);
        std::fill(out, out + batch.length, value);
      } else if (!options.skip_nulls) {
        // Abort early
        ARROW_ASSIGN_OR_RAISE(auto array, MakeArrayFromScalar(*temp_scalar, batch.length,
                                                              ctx->memory_pool()));
        *output = *array->data();
        return Status::OK();
      }
    }

    if (initialize_output) {
      OutValue* out = output->GetMutableValues<OutValue>(1);
      std::fill(out, out + batch.length, Op::template antiextreme<OutValue>());
    }

    // Precompute the validity buffer
    if (options.skip_nulls && initialize_output) {
      // OR together the validity buffers of all arrays
      if (std::all_of(arrays.begin(), arrays.end(),
                      [](const std::shared_ptr<ArrayData>& arr) {
                        return arr->MayHaveNulls();
                      })) {
        for (const auto& arr : arrays) {
          if (!arr->MayHaveNulls()) continue;
          if (!output->buffers[0]) {
            ARROW_ASSIGN_OR_RAISE(output->buffers[0], ctx->AllocateBitmap(batch.length));
            ::arrow::internal::CopyBitmap(arr->buffers[0]->data(), arr->offset,

                                          batch.length,
                                          output->buffers[0]->mutable_data(),
                                          /*dest_offset=*/0);
          } else {
            ::arrow::internal::BitmapOr(
                output->buffers[0]->data(), /*left_offset=*/0, arr->buffers[0]->data(),
                arr->offset, batch.length,
                /*out_offset=*/0, output->buffers[0]->mutable_data());
          }
        }
      }
    } else if (!options.skip_nulls) {
      // AND together the validity buffers of all arrays
      for (const auto& arr : arrays) {
        if (!arr->MayHaveNulls()) continue;
        if (!output->buffers[0]) {
          ARROW_ASSIGN_OR_RAISE(output->buffers[0], ctx->AllocateBitmap(batch.length));
          ::arrow::internal::CopyBitmap(arr->buffers[0]->data(), arr->offset,
                                        batch.length, output->buffers[0]->mutable_data(),
                                        /*dest_offset=*/0);
        } else {
          ::arrow::internal::BitmapAnd(output->buffers[0]->data(), /*left_offset=*/0,
                                       arr->buffers[0]->data(), arr->offset, batch.length,
                                       /*out_offset=*/0,
                                       output->buffers[0]->mutable_data());
        }
      }
    }

    for (const auto& array : arrays) {
      OutputArrayWriter<OutType> writer(out->mutable_array());
      ArrayIterator<OutType> out_it(*output);
      int64_t index = 0;
      VisitArrayValuesInline<OutType>(
          *array,
          [&](OutValue value) {
            auto u = out_it();
            if (!output->buffers[0] ||
                bit_util::GetBit(output->buffers[0]->data(), index)) {
              writer.Write(Op::template Call<OutValue, OutValue, OutValue>(u, value));
            } else {
              writer.Write(value);
            }
            index++;
          },
          [&]() {
            // RHS is null, preserve the LHS
            writer.values++;
            index++;
            out_it();
          });
    }
    output->null_count = output->buffers[0] ? -1 : 0;
    return Status::OK();
  }
};

template <typename Type, typename Op>
struct BinaryScalarMinMax {
  using ArrayType = typename TypeTraits<Type>::ArrayType;
  using BuilderType = typename TypeTraits<Type>::BuilderType;
  using offset_type = typename Type::offset_type;

  static Status Exec(KernelContext* ctx, const ExecBatch& batch, Datum* out) {
    const ElementWiseAggregateOptions& options = MinMaxState::Get(ctx);
    if (std::all_of(batch.values.begin(), batch.values.end(),
                    [](const Datum& d) { return d.is_scalar(); })) {
      return ExecOnlyScalar(ctx, options, batch, out);
    }
    return ExecContainingArrays(ctx, options, batch, out);
  }

  static Status ExecOnlyScalar(KernelContext* ctx,
                               const ElementWiseAggregateOptions& options,
                               const ExecBatch& batch, Datum* out) {
    if (batch.values.empty()) {
      return Status::OK();
    }
    BaseBinaryScalar* output = checked_cast<BaseBinaryScalar*>(out->scalar().get());
    const size_t num_args = batch.values.size();

    int64_t final_size = CalculateRowSize(options, batch, 0);
    if (final_size < 0) {
      output->is_valid = false;
      return Status::OK();
    }
    util::string_view result = UnboxScalar<Type>::Unbox(*batch.values.front().scalar());
    for (size_t i = 1; i < num_args; i++) {
      const Scalar& scalar = *batch[i].scalar();
      if (!scalar.is_valid && options.skip_nulls) {
        continue;
      }
      if (scalar.is_valid) {
        util::string_view value = UnboxScalar<Type>::Unbox(scalar);
        result = result.empty() ? value : Op::CallBinary(result, value);
      }
    }
    if (!result.empty()) {
      ARROW_ASSIGN_OR_RAISE(output->value, ctx->Allocate(final_size));
      uint8_t* buf = output->value->mutable_data();
      buf = std::copy(result.begin(), result.end(), buf);
      output->is_valid = true;
      DCHECK_GE(final_size, buf - output->value->mutable_data());
    }
    return Status::OK();
  }

  static Status ExecContainingArrays(KernelContext* ctx,
                                     const ElementWiseAggregateOptions& options,
                                     const ExecBatch& batch, Datum* out) {
    // Presize data to avoid reallocations
    int64_t final_size = 0;
    for (int64_t i = 0; i < batch.length; i++) {
      auto size = CalculateRowSize(options, batch, i);
      if (size > 0) final_size += size;
    }
    BuilderType builder(ctx->memory_pool());
    RETURN_NOT_OK(builder.Reserve(batch.length));
    RETURN_NOT_OK(builder.ReserveData(final_size));

    std::vector<util::string_view> valid_cols(batch.values.size());
    for (size_t row = 0; row < static_cast<size_t>(batch.length); row++) {
      size_t num_valid = 0;
      for (size_t col = 0; col < batch.values.size(); col++) {
        if (batch[col].is_scalar()) {
          const auto& scalar = *batch[col].scalar();
          if (scalar.is_valid) {
            valid_cols[col] = UnboxScalar<Type>::Unbox(scalar);
            num_valid++;
          } else {
            valid_cols[col] = util::string_view();
          }
        } else {
          const ArrayData& array = *batch[col].array();
          if (!array.MayHaveNulls() ||
              BitUtil::GetBit(array.buffers[0]->data(), array.offset + row)) {
            const offset_type* offsets = array.GetValues<offset_type>(1);
            const uint8_t* data = array.GetValues<uint8_t>(2, /*absolute_offset=*/0);
            const int64_t length = offsets[row + 1] - offsets[row];
            valid_cols[col] = util::string_view(
                reinterpret_cast<const char*>(data + offsets[row]), length);
            num_valid++;
          } else {
            valid_cols[col] = util::string_view();
          }
        }
      }

      if (num_valid < batch.values.size() && !options.skip_nulls) {
        // We had some nulls
        builder.UnsafeAppendNull();
        continue;
      }
      util::string_view result = valid_cols.front();
      for (size_t col = 1; col < batch.values.size(); ++col) {
        util::string_view value = valid_cols[col];
        if (value.empty()) {
          DCHECK(options.skip_nulls);
          continue;
        }
        result = result.empty() ? value : Op::CallBinary(result, value);
      }
      if (result.empty()) {
        builder.UnsafeAppendNull();
      } else {
        builder.UnsafeAppend(result);
      }
    }

    std::shared_ptr<Array> string_array;
    RETURN_NOT_OK(builder.Finish(&string_array));
    *out = *string_array->data();
    out->mutable_array()->type = batch[0].type();
    DCHECK_EQ(batch.length, out->array()->length);
    DCHECK_GE(final_size,
              checked_cast<const ArrayType&>(*string_array).total_values_length());
    return Status::OK();
  }

  // Compute the length of the output for the given position, or -1 if it would be null.
  static int64_t CalculateRowSize(const ElementWiseAggregateOptions& options,
                                  const ExecBatch& batch, const int64_t index) {
    const auto num_args = batch.values.size();
    int64_t final_size = 0;
    for (size_t i = 0; i < num_args; i++) {
      int64_t element_size = 0;
      bool valid = true;
      if (batch[i].is_scalar()) {
        const Scalar& scalar = *batch[i].scalar();
        valid = scalar.is_valid;
        element_size = UnboxScalar<Type>::Unbox(scalar).size();
      } else {
        const ArrayData& array = *batch[i].array();
        valid = !array.MayHaveNulls() ||
                BitUtil::GetBit(array.buffers[0]->data(), array.offset + index);
        const offset_type* offsets = array.GetValues<offset_type>(1);
        element_size = offsets[index + 1] - offsets[index];
      }
      if (!valid) {
        if (options.skip_nulls) {
          continue;
        }
        return -1;
      }
      final_size = std::max(final_size, element_size);
    }
    return final_size;
  }
};

Result<ValueDescr> ResolveMinOrMaxOutputType(KernelContext*,
                                             const std::vector<ValueDescr>& args) {
  if (args.empty()) {
    return null();
  }
  auto first_type = args[0].type;
  for (size_t i = 1; i < args.size(); ++i) {
    auto type = args[i].type;
    if (*type != *first_type) {
      return Status::NotImplemented(
          "Different decimal types not implemented for {min, max}_element_wise");
    }
  }
  return ValueDescr(first_type, GetBroadcastShape(args));
}

template <typename Op>
std::shared_ptr<ScalarFunction> MakeScalarMinMax(std::string name,
                                                 const FunctionDoc* doc) {
  static auto default_element_wise_aggregate_options =
      ElementWiseAggregateOptions::Defaults();

  auto func = std::make_shared<VarArgsCompareFunction>(
      name, Arity::VarArgs(), doc, &default_element_wise_aggregate_options);
  for (const auto& ty : NumericTypes()) {
    auto exec = GeneratePhysicalNumeric<ScalarMinMax, Op>(ty);
    ScalarKernel kernel{KernelSignature::Make({ty}, ty, /*is_varargs=*/true), exec,
                        MinMaxState::Init};
    kernel.null_handling = NullHandling::type::COMPUTED_NO_PREALLOCATE;
    kernel.mem_allocation = MemAllocation::type::PREALLOCATE;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  for (const auto& ty : TemporalTypes()) {
    auto exec = GeneratePhysicalNumeric<ScalarMinMax, Op>(ty);
    ScalarKernel kernel{KernelSignature::Make({ty}, ty, /*is_varargs=*/true), exec,
                        MinMaxState::Init};
    kernel.null_handling = NullHandling::type::COMPUTED_NO_PREALLOCATE;
    kernel.mem_allocation = MemAllocation::type::PREALLOCATE;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  for (const auto& ty : BaseBinaryTypes()) {
    auto exec = GenerateTypeAgnosticVarBinaryBase<BinaryScalarMinMax, Op>(ty);
    ScalarKernel kernel{KernelSignature::Make({ty}, ty, /*is_varargs=*/true), exec,
                        MinMaxState::Init};
    kernel.null_handling = NullHandling::COMPUTED_NO_PREALLOCATE;
    kernel.mem_allocation = MemAllocation::NO_PREALLOCATE;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  for (const auto id : {Type::DECIMAL128, Type::DECIMAL256}) {
    auto exec = GeneratePhysicalDecimal<ScalarMinMax, Op>(id);
    OutputType out_type(ResolveMinOrMaxOutputType);
    ScalarKernel kernel{KernelSignature::Make({InputType{id}}, out_type,
                                              /*is_varargs=*/true),
                        exec, MinMaxState::Init};
    kernel.null_handling = NullHandling::type::COMPUTED_NO_PREALLOCATE;
    kernel.mem_allocation = MemAllocation::type::PREALLOCATE;
    DCHECK_OK(func->AddKernel(std::move(kernel)));
  }
  return func;
}

const FunctionDoc equal_doc{"Compare values for equality (x == y)",
                            ("A null on either side emits a null comparison result."),
                            {"x", "y"}};

const FunctionDoc not_equal_doc{"Compare values for inequality (x != y)",
                                ("A null on either side emits a null comparison result."),
                                {"x", "y"}};

const FunctionDoc greater_doc{"Compare values for ordered inequality (x > y)",
                              ("A null on either side emits a null comparison result."),
                              {"x", "y"}};

const FunctionDoc greater_equal_doc{
    "Compare values for ordered inequality (x >= y)",
    ("A null on either side emits a null comparison result."),
    {"x", "y"}};

const FunctionDoc less_doc{"Compare values for ordered inequality (x < y)",
                           ("A null on either side emits a null comparison result."),
                           {"x", "y"}};

const FunctionDoc less_equal_doc{
    "Compare values for ordered inequality (x <= y)",
    ("A null on either side emits a null comparison result."),
    {"x", "y"}};

const FunctionDoc min_element_wise_doc{
    "Find the element-wise minimum value",
    ("Nulls are ignored (by default) or propagated.\n"
     "NaN is preferred over null, but not over any valid value."),
    {"*args"},
    "ElementWiseAggregateOptions"};

const FunctionDoc max_element_wise_doc{
    "Find the element-wise maximum value",
    ("Nulls are ignored (by default) or propagated.\n"
     "NaN is preferred over null, but not over any valid value."),
    {"*args"},
    "ElementWiseAggregateOptions"};

}  // namespace

void RegisterScalarComparison(FunctionRegistry* registry) {
  DCHECK_OK(registry->AddFunction(MakeCompareFunction<Equal>("equal", &equal_doc)));
  DCHECK_OK(
      registry->AddFunction(MakeCompareFunction<NotEqual>("not_equal", &not_equal_doc)));

  auto greater = MakeCompareFunction<Greater>("greater", &greater_doc);
  auto greater_equal =
      MakeCompareFunction<GreaterEqual>("greater_equal", &greater_equal_doc);

  auto less = MakeFlippedFunction("less", *greater, &less_doc);
  auto less_equal = MakeFlippedFunction("less_equal", *greater_equal, &less_equal_doc);
  DCHECK_OK(registry->AddFunction(std::move(less)));
  DCHECK_OK(registry->AddFunction(std::move(less_equal)));
  DCHECK_OK(registry->AddFunction(std::move(greater)));
  DCHECK_OK(registry->AddFunction(std::move(greater_equal)));

  // ----------------------------------------------------------------------
  // Variadic element-wise functions

  auto min_element_wise =
      MakeScalarMinMax<Minimum>("min_element_wise", &min_element_wise_doc);
  DCHECK_OK(registry->AddFunction(std::move(min_element_wise)));

  auto max_element_wise =
      MakeScalarMinMax<Maximum>("max_element_wise", &max_element_wise_doc);
  DCHECK_OK(registry->AddFunction(std::move(max_element_wise)));
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
