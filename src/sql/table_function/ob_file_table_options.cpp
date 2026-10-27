/*
 * Copyright (c) 2026 OceanBase.
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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/table_function/ob_file_table_options.h"

#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/ob_print_utils.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{

constexpr uint32_t FILE_TABLE_OPTIONS_MAGIC = 0x4F465442; // OFTB
constexpr uint32_t FILE_TABLE_OPTIONS_VERSION = 1;
constexpr int64_t FILE_TABLE_MAX_COLUMNS = 1024;

int64_t encoded_string_size(const ObString &value)
{
  return static_cast<int64_t>(sizeof(uint64_t)) + value.length();
}

int64_t encoded_column_size(const ObFileTableColumnDef &column)
{
  return encoded_string_size(column.source_name_)
         + encoded_string_size(column.column_name_)
         + encoded_string_size(column.source_type_name_)
         + static_cast<int64_t>(sizeof(uint32_t))
         + static_cast<int64_t>(sizeof(uint32_t))
         + static_cast<int64_t>(sizeof(int64_t));
}

void append_raw(char *buffer, int64_t capacity, int64_t &pos, const void *value, int64_t size)
{
  if (OB_LIKELY(pos + size <= capacity)) {
    MEMCPY(buffer + pos, value, size);
  }
  pos += size;
}

void append_u32(char *buffer, int64_t capacity, int64_t &pos, uint32_t value)
{
  append_raw(buffer, capacity, pos, &value, sizeof(value));
}

void append_i64(char *buffer, int64_t capacity, int64_t &pos, int64_t value)
{
  append_raw(buffer, capacity, pos, &value, sizeof(value));
}

void append_u64(char *buffer, int64_t capacity, int64_t &pos, uint64_t value)
{
  append_raw(buffer, capacity, pos, &value, sizeof(value));
}

void append_string(char *buffer, int64_t capacity, int64_t &pos, const ObString &value)
{
  append_u64(buffer, capacity, pos, static_cast<uint64_t>(value.length()));
  if (value.length() > 0) {
    append_raw(buffer, capacity, pos, value.ptr(), value.length());
  }
}

bool has_remaining(const ObString &encoded, int64_t pos, int64_t size)
{
  return pos >= 0 && size >= 0 && pos + size <= encoded.length();
}

int read_u32(const ObString &encoded, int64_t &pos, uint32_t &value)
{
  int ret = OB_SUCCESS;
  if (!has_remaining(encoded, pos, sizeof(value))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(&value, encoded.ptr() + pos, sizeof(value));
    pos += sizeof(value);
  }
  return ret;
}

int read_i64(const ObString &encoded, int64_t &pos, int64_t &value)
{
  int ret = OB_SUCCESS;
  if (!has_remaining(encoded, pos, sizeof(value))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(&value, encoded.ptr() + pos, sizeof(value));
    pos += sizeof(value);
  }
  return ret;
}

int read_u64(const ObString &encoded, int64_t &pos, uint64_t &value)
{
  int ret = OB_SUCCESS;
  if (!has_remaining(encoded, pos, sizeof(value))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(&value, encoded.ptr() + pos, sizeof(value));
    pos += sizeof(value);
  }
  return ret;
}

int read_string(const ObString &encoded, int64_t &pos, ObString &value)
{
  int ret = OB_SUCCESS;
  uint64_t length = 0;
  if (OB_FAIL(read_u64(encoded, pos, length))) {
  } else if (length > static_cast<uint64_t>(INT32_MAX)
             || !has_remaining(encoded, pos, static_cast<int64_t>(length))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    value.assign_ptr(encoded.ptr() + pos, static_cast<int32_t>(length));
    pos += static_cast<int64_t>(length);
  }
  return ret;
}

int append_columns(char *buffer, int64_t capacity, int64_t &pos,
                   const ObIArray<ObFileTableColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  if (columns.count() > FILE_TABLE_MAX_COLUMNS) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    append_u32(buffer, capacity, pos, static_cast<uint32_t>(columns.count()));
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      const ObFileTableColumnDef &column = columns.at(i);
      append_string(buffer, capacity, pos, column.source_name_);
      append_string(buffer, capacity, pos, column.column_name_);
      append_string(buffer, capacity, pos, column.source_type_name_);
      append_u32(buffer, capacity, pos, static_cast<uint32_t>(column.type_));
      append_u32(buffer, capacity, pos, column.nullable_ ? 1 : 0);
      append_i64(buffer, capacity, pos, column.max_length_);
    }
  }
  return ret;
}

int read_columns(const ObString &encoded, int64_t &pos,
                 ObIArray<ObFileTableColumnDef> &columns)
{
  int ret = OB_SUCCESS;
  uint32_t count = 0;
  if (OB_FAIL(read_u32(encoded, pos, count))) {
  } else if (count > FILE_TABLE_MAX_COLUMNS) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (uint32_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObFileTableColumnDef column;
    uint32_t type = 0;
    uint32_t nullable = 0;
    if (OB_FAIL(read_string(encoded, pos, column.source_name_))
        || OB_FAIL(read_string(encoded, pos, column.column_name_))
        || OB_FAIL(read_string(encoded, pos, column.source_type_name_))
        || OB_FAIL(read_u32(encoded, pos, type))
        || OB_FAIL(read_u32(encoded, pos, nullable))
        || OB_FAIL(read_i64(encoded, pos, column.max_length_))) {
    } else if (type >= static_cast<uint32_t>(ObFileColumnType::INVALID)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      column.type_ = static_cast<ObFileColumnType>(type);
      column.nullable_ = 0 != nullable;
      OZ (columns.push_back(column));
    }
  }
  return ret;
}

} // namespace

int ObFileTableOptions::encode(const ObFileTableDef &def, ObIAllocator &allocator,
                               ObString &encoded)
{
  int ret = OB_SUCCESS;
  int64_t total = static_cast<int64_t>(sizeof(uint32_t)) * 4
                  + static_cast<int64_t>(sizeof(uint64_t)) * 2
                  + static_cast<int64_t>(sizeof(int64_t)) * 3
                  + encoded_string_size(def.canonical_path_)
                  + encoded_string_size(def.source_pattern_)
                  + encoded_string_size(def.secure_file_priv_);
  total += static_cast<int64_t>(sizeof(uint32_t)) * 2;
  for (int64_t i = 0; OB_SUCC(ret) && i < def.columns_.count(); ++i) {
    total += encoded_column_size(def.columns_.at(i));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < def.source_columns_.count(); ++i) {
    total += encoded_column_size(def.source_columns_.at(i));
  }
  if (OB_FAIL(ret)) {
  } else if (def.columns_.count() > FILE_TABLE_MAX_COLUMNS
             || def.source_columns_.count() > FILE_TABLE_MAX_COLUMNS) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char *buffer = static_cast<char *>(allocator.alloc(total));
    if (OB_ISNULL(buffer)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      append_u32(buffer, total, pos, FILE_TABLE_OPTIONS_MAGIC);
      append_u32(buffer, total, pos, FILE_TABLE_OPTIONS_VERSION);
      append_u32(buffer, total, pos, static_cast<uint32_t>(def.kind_));
      append_u32(buffer, total, pos, static_cast<uint32_t>(def.format_));
      append_u64(buffer, total, pos, def.device_);
      append_u64(buffer, total, pos, def.inode_);
      append_i64(buffer, total, pos, def.file_size_);
      append_i64(buffer, total, pos, def.modified_time_ns_);
      append_i64(buffer, total, pos, def.estimated_rows_);
      append_string(buffer, total, pos, def.canonical_path_);
      append_string(buffer, total, pos, def.source_pattern_);
      append_string(buffer, total, pos, def.secure_file_priv_);
      if (OB_FAIL(append_columns(buffer, total, pos, def.columns_))) {
      } else if (OB_FAIL(append_columns(buffer, total, pos, def.source_columns_))) {
      } else if (OB_UNLIKELY(pos != total)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        encoded.assign_ptr(buffer, static_cast<int32_t>(total));
      }
    }
  }
  return ret;
}

int ObFileTableOptions::decode(const ObString &encoded, ObFileTableDef &def)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t kind = 0;
  uint32_t format = 0;
  if (OB_FAIL(read_u32(encoded, pos, magic))) {
  } else if (FILE_TABLE_OPTIONS_MAGIC != magic) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(read_u32(encoded, pos, version))
             || OB_FAIL(read_u32(encoded, pos, kind))
             || OB_FAIL(read_u32(encoded, pos, format))) {
  } else if (FILE_TABLE_OPTIONS_VERSION != version
             || kind >= static_cast<uint32_t>(ObFileTableKind::INVALID)
             || format >= static_cast<uint32_t>(ObFileFormat::INVALID)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(read_u64(encoded, pos, def.device_))
             || OB_FAIL(read_u64(encoded, pos, def.inode_))
             || OB_FAIL(read_i64(encoded, pos, def.file_size_))
             || OB_FAIL(read_i64(encoded, pos, def.modified_time_ns_))
             || OB_FAIL(read_i64(encoded, pos, def.estimated_rows_))) {
  } else if (OB_FAIL(read_string(encoded, pos, def.canonical_path_))
             || OB_FAIL(read_string(encoded, pos, def.source_pattern_))
             || OB_FAIL(read_string(encoded, pos, def.secure_file_priv_))) {
  } else {
    def.kind_ = static_cast<ObFileTableKind>(kind);
    def.format_ = static_cast<ObFileFormat>(format);
    if (OB_FAIL(read_columns(encoded, pos, def.columns_))) {
    } else if (OB_FAIL(read_columns(encoded, pos, def.source_columns_))) {
    } else if (OB_UNLIKELY(pos != encoded.length())) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
