// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/docdb/packed_row.h"

#include "yb/common/ql_value.h"
#include "yb/common/schema.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/primitive_value.h"
#include "yb/docdb/schema_packing.h"
#include "yb/docdb/value.h"
#include "yb/docdb/value_type.h"

#include "yb/gutil/casts.h"

#include "yb/gutil/map-util.h"
#include "yb/util/coding_consts.h"
#include "yb/util/fast_varint.h"
#include "yb/util/status.h"
#include "yb/util/tostring.h"

DECLARE_int64(db_block_size_bytes);

namespace yb {
namespace docdb {

namespace {

bool IsNull(const Slice& slice) {
  return slice.empty();
}

using ValueSlicePair = std::pair<Slice, Slice>;
using ValuePair = std::pair<Slice, const QLValuePB&>;

bool IsNull(const ValueSlicePair& value) {
  return value.first.empty() && value.second.empty();
}

void PackValue(const QLValuePB& value, ValueBuffer* result) {
  AppendEncodedValue(value, result);
}

size_t PackedValueSize(const QLValuePB& value) {
  return EncodedValueSize(value);
}

void PackValue(const ValuePair& value, ValueBuffer* result) {
  result->Append(value.first);
  AppendEncodedValue(value.second, result);
}

size_t PackedValueSize(const ValuePair& value) {
  return value.first.size() + EncodedValueSize(value.second);
}

bool IsNull(const ValuePair& value) {
  return value.first.empty() && IsNull(value.second);
}

void PackValue(const Slice& value, ValueBuffer* result) {
  result->Append(value);
}

size_t PackedValueSize(const Slice& value) {
  return value.size();
}

size_t PackedValueSize(const ValueSlicePair& value) {
  return value.first.size() + value.second.size();
}

void PackValue(const ValueSlicePair& value, ValueBuffer* result) {
  result->Reserve(result->size() + PackedValueSize(value));
  result->Append(value.first);
  result->Append(value.second);
}

size_t PackedSizeLimit(size_t value) {
  return value ? value : make_unsigned(FLAGS_db_block_size_bytes);
}

std::string ValueToString(const QLValuePB& value) {
  return value.ShortDebugString();
}

std::string ValueToString(const ValuePair& value) {
  auto result = value.second.ShortDebugString();
  if (!value.first.empty()) {
    Slice control_fields_slice = value.first;
    auto control_fields = ValueControlFields::Decode(&control_fields_slice);
    result += AsString(control_fields);
  }
  return result;
}

std::string ValueToString(const Slice& value) {
  return value.ToDebugHexString();
}

std::string ValueToString(const ValueSlicePair& value) {
  if (value.first.empty()) {
    return value.second.ToDebugHexString();
  }
  return value.first.ToDebugHexString() + "+" + value.second.ToDebugHexString();
}

} // namespace

RowPacker::RowPacker(
    SchemaVersion version, std::reference_wrapper<const SchemaPacking> packing,
    size_t packed_size_limit, const ValueControlFields& control_fields)
    : packing_(packing),
      packed_size_limit_(PackedSizeLimit(packed_size_limit)) {
  control_fields.AppendEncoded(&result_);
  Init(version);
}

RowPacker::RowPacker(
    SchemaVersion version, std::reference_wrapper<const SchemaPacking> packing,
    size_t packed_size_limit, const Slice& control_fields)
    : packing_(packing),
      packed_size_limit_(PackedSizeLimit(packed_size_limit)) {
  result_.Append(control_fields);
  Init(version);
}

void RowPacker::Init(SchemaVersion version) {
  size_t prefix_len = packing_.prefix_len();
  result_.Reserve(result_.size() + 1 + kMaxVarint32Length + prefix_len);
  result_.PushBack(ValueEntryTypeAsChar::kPackedRow);
  result_.Truncate(
      result_.size() +
      util::FastEncodeUnsignedVarInt(version, result_.mutable_data() + result_.size()));
  result_.GrowByAtLeast(prefix_len);
  prefix_end_ = result_.size();
  varlen_write_pos_ = prefix_end_ - prefix_len;
}

bool RowPacker::Finished() const {
  return idx_ == packing_.columns();
}

void RowPacker::Restart() {
  idx_ = 0;
  varlen_write_pos_ = prefix_end_ - packing_.prefix_len();
  result_.Truncate(prefix_end_);
}

Result<bool> RowPacker::AddValue(ColumnId column_id, const QLValuePB& value) {
  return DoAddValue(column_id, value, 0);
}

Result<bool> RowPacker::AddValue(
    ColumnId column_id, const Slice& control_fields, const QLValuePB& value) {
  return DoAddValue(column_id, ValuePair(control_fields, value), 0);
}

Result<bool> RowPacker::AddValue(ColumnId column_id, const Slice& value, ssize_t tail_size) {
  return DoAddValue(column_id, value, tail_size);
}

Result<bool> RowPacker::AddValue(
    ColumnId column_id, const Slice& value_prefix, const Slice& value_suffix, ssize_t tail_size) {
  return DoAddValue(column_id, ValueSlicePair(value_prefix, value_suffix), tail_size);
}

// Replaces the schema version in packed value with the provided schema version.
// Note: Value starts with the schema version (does not contain control fields, value type).
Status ReplaceSchemaVersionInPackedValue(const Slice& value,
                                         const ValueControlFields& control_fields,
                                         const cdc::XClusterSchemaVersionMap schema_versions_map,
                                         ValueBuffer *out) {
  CHECK(out != nullptr);
  out->Truncate(0);
  control_fields.AppendEncoded(out);
  out->Reserve(out->size() + 1 + kMaxVarint32Length + value.size());
  out->PushBack(ValueEntryTypeAsChar::kPackedRow);

  // Consume the bytes for existing schema version and append the mapped schema version and
  // rest of the value.
  Slice value_slice = value;
  auto schema_version = (SchemaVersion)VERIFY_RESULT(util::FastDecodeUnsignedVarInt(&value_slice));
  auto mapped_version = FindOrNull(schema_versions_map, schema_version);
  SCHECK(mapped_version, NotFound, "Schema version mapping for $0 not found", schema_version);
  util::FastAppendUnsignedVarInt(*mapped_version, out);
  out->Append(value_slice);
  return Status::OK();
}

template <class Value>
Result<bool> RowPacker::DoAddValue(ColumnId column_id, const Value& value, ssize_t tail_size) {
  if (idx_ >= packing_.columns()) {
    RSTATUS_DCHECK(
        packing_.SkippedColumn(column_id),
        InvalidArgument, "Add extra column $0, while already have $1 of $2 columns",
        column_id, idx_, packing_.columns());
    return false;
  }

  bool result = true;
  for (;;) {
    const auto& column_data = packing_.column_packing_data(idx_);
    if (column_data.id > column_id) {
      RSTATUS_DCHECK(
          packing_.SkippedColumn(column_id), InvalidArgument,
          "Add unexpected column $0, while $1 is expected", column_id, column_data.id);
      return false;
    }

    ++idx_;
    size_t prev_size = result_.size();
    if (column_data.id < column_id) {
      RSTATUS_DCHECK(
          column_data.nullable, InvalidArgument,
          "Missing value for non nullable column $0, while adding $1", column_data.id, column_id);
    } else if (!column_data.nullable || !IsNull(value)) {
      if (column_data.varlen() &&
          make_signed(prev_size + PackedValueSize(value)) + tail_size > packed_size_limit_) {
        result = false;
      } else {
        PackValue(value, &result_);
      }
    }
    if (column_data.varlen()) {
      LittleEndian::Store32(result_.mutable_data() + varlen_write_pos_,
                            narrow_cast<uint32_t>(result_.size() - prefix_end_));
      varlen_write_pos_ += sizeof(uint32_t);
    } else {
      RSTATUS_DCHECK(
          prev_size + column_data.size == result_.size(), Corruption,
          "Wrong encoded size: $0, column: $1, value: $2",
          result_.size() - prev_size, column_data, ValueToString(value));
    }

    if (column_data.id == column_id) {
      break;
    }
  }

  return result;
}

Result<Slice> RowPacker::Complete() {
  // In case of concurrent schema change YSQL does not send recently added columns.
  // Fill them with NULLs to keep the same behaviour like we have w/o packed row.
  while (idx_ < packing_.columns()) {
    const auto& packing_data = packing_.column_packing_data(idx_);
    RSTATUS_DCHECK(
        packing_data.nullable, InvalidArgument, "Non nullable column $0 was not specified",
        packing_data);
    RETURN_NOT_OK(AddValue(packing_data.id, Slice(), 0));
  }
  RSTATUS_DCHECK_EQ(
      varlen_write_pos_, prefix_end_, InvalidArgument, "Not all varlen columns packed");
  return result_.AsSlice();
}

ColumnId RowPacker::NextColumnId() const {
  return idx_ < packing_.columns() ? packing_.column_packing_data(idx_).id : kInvalidColumnId;
}

Result<const ColumnPackingData&> RowPacker::NextColumnData() const {
  RSTATUS_DCHECK(
      idx_ < packing_.columns(), IllegalState, "All columns already packed");
  return packing_.column_packing_data(idx_);
}

} // namespace docdb
} // namespace yb