// Copyright (c) 2018, NVIDIA CORPORATION.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "transformational.h"
#include "../lib/common/idioms.h"
#include "../lib/evaluate/integer.h"
#include <algorithm>
#include <bitset>
#include <cinttypes>
#include <memory>

namespace Fortran::runtime {

template<int BITS> inline std::int64_t LoadInt64(const char *p) {
  using Int = const evaluate::value::Integer<BITS>;
  Int *ip{reinterpret_cast<Int *>(p)};
  return ip->ToInt64();
}

static inline std::int64_t GetInt64(const char *p, std::size_t bytes) {
  switch (bytes) {
  case 1: return LoadInt64<8>(p);
  case 2: return LoadInt64<16>(p);
  case 4: return LoadInt64<32>(p);
  case 8: return LoadInt64<64>(p);
  default: CRASH_NO_CASE;
  }
}

// F2018 16.9.163
std::unique_ptr<Descriptor> RESHAPE(const Descriptor &source,
    const Descriptor &shape, const Descriptor *pad, const Descriptor *order) {
  // Compute and check the rank of the result.
  CHECK(shape.rank() == 1);
  CHECK(shape.type().IsInteger());
  SubscriptValue resultRank{shape.GetDimension(0).Extent()};
  CHECK(resultRank >= 0 && resultRank <= static_cast<SubscriptValue>(maxRank));

  // Extract and check the shape of the result; compute its element count.
  SubscriptValue lowerBound[maxRank];  // all 1's
  SubscriptValue resultExtent[maxRank];
  std::size_t shapeElementBytes{shape.ElementBytes()};
  std::size_t resultElements{1};
  SubscriptValue shapeSubscript{shape.GetDimension(0).LowerBound()};
  for (SubscriptValue j{0}; j < resultRank; ++j, ++shapeSubscript) {
    lowerBound[j] = 1;
    resultExtent[j] =
        GetInt64(shape.Element<char>(&shapeSubscript), shapeElementBytes);
    CHECK(resultExtent[j] >= 0);
    resultElements *= resultExtent[j];
  }

  // Check that there are sufficient elements in the SOURCE=, or that
  // the optional PAD= argument is present and nonempty.
  std::size_t elementBytes{source.ElementBytes()};
  std::size_t sourceElements{source.Elements()};
  std::size_t padElements{pad ? pad->Elements() : 0};
  if (resultElements < sourceElements) {
    CHECK(padElements > 0);
    CHECK(pad->ElementBytes() == elementBytes);
  }

  // Extract and check the optional ORDER= argument, which must be a
  // permutation of [1..resultRank].
  int dimOrder[maxRank];
  if (order != nullptr) {
    CHECK(order->rank() == 1);
    CHECK(order->type().IsInteger());
    CHECK(order->GetDimension(0).Extent() == resultRank);
    std::bitset<maxRank> values;
    SubscriptValue orderSubscript{order->GetDimension(0).LowerBound()};
    for (SubscriptValue j{0}; j < resultRank; ++j, ++orderSubscript) {
      auto k{GetInt64(order->Element<char>(orderSubscript), shapeElementBytes)};
      CHECK(k >= 1 && k <= resultRank && !values.test(k - 1));
      values.set(k - 1);
      dimOrder[k - 1] = j;
    }
  } else {
    for (int j{0}; j < resultRank; ++j) {
      dimOrder[j] = j;
    }
  }

  // Create and populate the result's descriptor.
  const DescriptorAddendum *sourceAddendum{source.Addendum()};
  const DerivedType *sourceDerivedType{
      sourceAddendum ? sourceAddendum->derivedType() : nullptr};
  std::unique_ptr<Descriptor> result;
  if (sourceDerivedType != nullptr) {
    result = Descriptor::Create(*sourceDerivedType, nullptr, resultRank,
        resultExtent, CFI_attribute_allocatable);
  } else {
    result = Descriptor::Create(source.type(), elementBytes, nullptr,
        resultRank, resultExtent,
        CFI_attribute_allocatable);  // TODO rearrange these arguments
  }
  DescriptorAddendum *resultAddendum{result->Addendum()};
  CHECK(resultAddendum != nullptr);
  resultAddendum->flags() |= DescriptorAddendum::DoNotFinalize;
  if (sourceDerivedType != nullptr) {
    std::size_t lenParameters{sourceDerivedType->lenParameters()};
    for (std::size_t j{0}; j < lenParameters; ++j) {
      resultAddendum->SetLenParameterValue(
          j, sourceAddendum->LenParameterValue(j));
    }
  }
  // Allocate storage for the result's data.
  int status{result->Allocate(lowerBound, resultExtent, elementBytes)};
  if (status != CFI_SUCCESS) {
    common::die("RESHAPE: Allocate failed (error %d)", status);
  }

  // Populate the result's elements.
  SubscriptValue resultSubscript[maxRank];
  result->GetLowerBounds(resultSubscript);
  SubscriptValue sourceSubscript[maxRank];
  source.GetLowerBounds(sourceSubscript);
  std::size_t resultElement{0};
  std::size_t elementsFromSource{std::min(resultElements, sourceElements)};
  for (; resultElement < elementsFromSource; ++resultElement) {
    std::memcpy(result->Element<void>(resultSubscript),
        source.Element<const void>(sourceSubscript), elementBytes);
    source.IncrementSubscripts(sourceSubscript);
    result->IncrementSubscripts(resultSubscript, dimOrder);
  }
  if (resultElement < resultElements) {
    // Remaining elements come from the optional PAD= argument.
    SubscriptValue padSubscript[maxRank];
    pad->GetLowerBounds(padSubscript);
    for (; resultElement < resultElements; ++resultElement) {
      std::memcpy(result->Element<void>(resultSubscript),
          pad->Element<const void>(padSubscript), elementBytes);
      pad->IncrementSubscripts(padSubscript);
      result->IncrementSubscripts(resultSubscript, dimOrder);
    }
  }

  return result;
}

}  // namespace Fortran::runtime
