/*
 * Copyright 2020 OmniSci, Inc.
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

#pragma once

#include "ParquetInPlaceEncoder.h"

namespace foreign_storage {
template <typename V, typename T>
class ParquetTimeEncoder : public TypedParquetInPlaceEncoder<V, T> {
 public:
  ParquetTimeEncoder(Data_Namespace::AbstractBuffer* buffer,
                     const ColumnDescriptor* column_desciptor,
                     const parquet::ColumnDescriptor* parquet_column_descriptor)
      : TypedParquetInPlaceEncoder<V, T>(buffer,
                                         column_desciptor,
                                         parquet_column_descriptor) {
    auto time_logical_type = dynamic_cast<const parquet::TimeLogicalType*>(
        parquet_column_descriptor->logical_type().get());
    CHECK(time_logical_type);
    conversion_denominator_ =
        get_time_conversion_denominator(time_logical_type->time_unit());
  }

  void encodeAndCopy(const int8_t* parquet_data_bytes,
                     int8_t* omnisci_data_bytes) override {
    const auto& parquet_data_value = reinterpret_cast<const T*>(parquet_data_bytes)[0];
    auto& omnisci_data_value = reinterpret_cast<V*>(omnisci_data_bytes)[0];
    omnisci_data_value = parquet_data_value / conversion_denominator_;
  }

 private:
  V conversion_denominator_;
};

}  // namespace foreign_storage
