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

#define USING_LOG_PREFIX SQL_EXE
#include "sql/table_function/table_function.h"
#include "sql/table_function/ob_file_scan_utils.h"
#include "sql/table_function/ob_file_table_options.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{

bool wildcard_match(const char *pattern, const char *text)
{
  const char *star_pattern = nullptr;
  const char *star_text = nullptr;
  while ('\0' != *text) {
    if ('*' == *pattern) {
      star_pattern = pattern++;
      star_text = text;
    } else if ('?' == *pattern || *pattern == *text) {
      ++pattern;
      ++text;
    } else if (nullptr != star_pattern) {
      pattern = star_pattern + 1;
      text = ++star_text;
    } else {
      return false;
    }
  }
  while ('*' == *pattern) { ++pattern; }
  return '\0' == *pattern;
}

} // namespace

class ObFileTableScan : public ITableFunctionScan
{
public:
  explicit ObFileTableScan(const TableFunctionScanContext &context)
    : allocator_(context.allocator_),
      options_(context.options_),
      session_(context.exec_ctx_.get_my_session()),
      column_ids_(),
      def_(),
      projected_idxs_(),
      reader_(),
      cells_(),
      row_(),
      cell_buffer_(nullptr),
      collation_(CS_TYPE_UTF8MB4_GENERAL_CI),
      file_paths_(),
      next_file_idx_(0),
      current_file_(),
      opened_(false)
  {
  }
  virtual ~ObFileTableScan() { close(); }

  int open() override;
  int get_next_row(ObNewRow *&row) override;
  int rescan(const ObIArray<ObObj> &arguments) override;
  int close() override;

private:
  int init_context(const TableFunctionScanContext &context);
  int verify_session();
  int open_file_scan();
  int expand_directory();
  int prepare_row();
  int make_cell_obj(const ObFileTableColumnDef &column, const ObFileCell &cell, ObObj &obj);

private:
  ObIAllocator &allocator_;
  ObString options_;
  ObSQLSessionInfo *session_;
  ObSEArray<uint64_t, 16> column_ids_;
  ObFileTableDef def_;
  std::vector<int64_t> projected_idxs_;
  ObFileScanReader reader_;
  std::vector<ObFileCell> cells_;
  ObNewRow row_;
  ObObj *cell_buffer_;
  ObCollationType collation_;
  std::vector<std::string> file_paths_;
  size_t next_file_idx_;
  std::string current_file_;
  bool opened_;
};

int ObFileTableScan::init_context(const TableFunctionScanContext &context)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(session_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(column_ids_.assign(context.column_ids_))) {
  } else if (OB_FAIL(ObFileTableOptions::decode(options_, def_))) {
    LOG_WARN("failed to decode file table options", K(ret));
  } else if (ObFileTableKind::LIST == def_.kind_ && column_ids_.empty()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    collation_ = session_->get_local_collation_connection();
    projected_idxs_.reserve(column_ids_.count());
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids_.count(); ++i) {
      if (column_ids_.at(i) < OB_APP_MIN_COLUMN_ID) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const int64_t index = static_cast<int64_t>(column_ids_.at(i) - OB_APP_MIN_COLUMN_ID);
        if (ObFileTableKind::LIST == def_.kind_) {
          if (index != 0) {
            ret = OB_ERR_UNEXPECTED;
          }
        } else if (index >= static_cast<int64_t>(def_.columns_.count())) {
          ret = OB_ERR_UNEXPECTED;
        }
        if (OB_SUCC(ret)) {
          projected_idxs_.push_back(index);
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(prepare_row())) {
      LOG_WARN("failed to prepare file scan row", K(ret));
    }
  }
  return ret;
}

int ObFileTableScan::prepare_row()
{
  int ret = OB_SUCCESS;
  row_.cells_ = nullptr;
  row_.count_ = column_ids_.count();
  row_.projector_ = nullptr;
  row_.projector_size_ = 0;
  cell_buffer_ = nullptr;
  if (row_.count_ > 0) {
    cell_buffer_ = static_cast<ObObj *>(allocator_.alloc(sizeof(ObObj) * row_.count_));
    if (OB_ISNULL(cell_buffer_)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; i < row_.count_; ++i) {
        cell_buffer_[i].set_null();
      }
      row_.cells_ = cell_buffer_;
    }
  }
  return ret;
}

int ObFileTableScan::verify_session()
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  ObString secure_file_priv;
  if (OB_ISNULL(session_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(session_->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
  } else if (!enabled) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file table when enable_file_sql is off");
  } else if (OB_FAIL(session_->get_secure_file_priv(secure_file_priv))) {
  } else if (secure_file_priv != def_.secure_file_priv_) {
    ret = OB_SCHEMA_EAGAIN;
    LOG_WARN("secure_file_priv changed after file table binding", K(ret));
  }
  return ret;
}

int ObFileTableScan::open()
{
  int ret = OB_SUCCESS;
  if (opened_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(verify_session())) {
  } else if (ObFileTableKind::LIST == def_.kind_) {
    if (OB_FAIL(expand_directory())) {
      LOG_WARN("failed to expand file list pattern", K(ret));
    }
  } else if (ObFileTableKind::SCAN == def_.kind_) {
    if (OB_FAIL(open_file_scan())) {
      LOG_WARN("failed to open file scan", K(ret));
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret)) {
    opened_ = true;
  }
  return ret;
}

int ObFileTableScan::open_file_scan()
{
  int ret = OB_SUCCESS;
  std::vector<ObFileColumnSchema> columns;
  columns.reserve(def_.columns_.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < def_.columns_.count(); ++i) {
    const ObFileTableColumnDef &definition = def_.columns_.at(i);
    ObFileColumnSchema schema;
    schema.source_name_.assign(definition.source_name_.ptr(), definition.source_name_.length());
    schema.column_name_.assign(definition.column_name_.ptr(), definition.column_name_.length());
    schema.source_type_name_.assign(definition.source_type_name_.ptr(),
                                    definition.source_type_name_.length());
    schema.type_ = definition.type_;
    schema.nullable_ = definition.nullable_;
    schema.max_length_ = definition.max_length_;
    columns.push_back(schema);
  }
  const std::string path(def_.canonical_path_.ptr(), def_.canonical_path_.length());
  std::vector<int64_t> projected;
  projected.reserve(projected_idxs_.size());
  for (size_t i = 0; i < projected_idxs_.size(); ++i) {
    projected.push_back(projected_idxs_[i]);
  }
  if (OB_SUCC(ret) && OB_FAIL(reader_.open(path, def_.format_, columns,
                                           def_.device_, def_.inode_, def_.file_size_,
                                           def_.modified_time_ns_, projected))) {
    LOG_WARN("failed to open file reader", K(ret));
  }
  return ret;
}

int ObFileTableScan::expand_directory()
{
  int ret = OB_SUCCESS;
  std::string directory(def_.canonical_path_.ptr(), def_.canonical_path_.length());
  std::string pattern(def_.source_pattern_.ptr(), def_.source_pattern_.length());
  std::string canonical_directory;
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t modified_time_ns = 0;
  if (OB_UNLIKELY(directory.empty() || pattern.empty())) {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret)) {
    file_paths_.clear();
    next_file_idx_ = 0;
#ifdef _WIN32
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("glob is not supported on this platform", K(ret));
#else
    if (0 != stat(directory.c_str(), nullptr)) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      DIR *dir = opendir(directory.c_str());
      if (nullptr == dir) {
        ret = OB_FILE_NOT_OPENED;
      } else {
        struct dirent *entry = nullptr;
        while (OB_SUCC(ret) && nullptr != (entry = readdir(dir))) {
          if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
            continue;
          }
          if (!wildcard_match(pattern.c_str(), entry->d_name)) {
            continue;
          }
          const std::string full = directory + "/" + entry->d_name;
          struct stat file_stat;
          if (0 != stat(full.c_str(), &file_stat) || !S_ISREG(file_stat.st_mode)) {
            continue;
          }
          file_paths_.push_back(full);
        }
        closedir(dir);
        std::sort(file_paths_.begin(), file_paths_.end());
      }
    }
#endif
  }
  if (OB_SUCC(ret)) {
    uint64_t actual_device = 0;
    uint64_t actual_inode = 0;
    int64_t actual_mtime = 0;
    if (OB_FAIL(ObFileScanUtils::get_directory_fingerprint(directory, canonical_directory,
                                                           actual_device, actual_inode,
                                                           actual_mtime))) {
      LOG_WARN("failed to fingerprint glob directory", K(ret));
    } else if (canonical_directory != directory
               || actual_device != def_.device_
               || actual_inode != def_.inode_
               || actual_mtime != def_.modified_time_ns_) {
      ret = OB_SCHEMA_EAGAIN;
      LOG_WARN("glob directory changed after binding", K(ret));
    }
  }
  return ret;
}

int ObFileTableScan::make_cell_obj(const ObFileTableColumnDef &column, const ObFileCell &cell,
                                   ObObj &obj)
{
  int ret = OB_SUCCESS;
  if (cell.is_null_) {
    obj.set_null();
  } else if (ObFileColumnType::VARCHAR == column.type_) {
    obj.set_varchar(ObString(static_cast<int32_t>(cell.string_value_.size()),
                             cell.string_value_.data()));
    obj.set_collation_type(collation_);
  } else if (ObFileColumnType::BOOLEAN == column.type_) {
    obj.set_tinyint(cell.bool_value_ ? 1 : 0);
  } else if (ObFileColumnType::BIGINT == column.type_) {
    obj.set_int(cell.int_value_);
  } else if (ObFileColumnType::DOUBLE == column.type_) {
    obj.set_double(cell.double_value_);
  } else if (ObFileColumnType::DATE == column.type_) {
    obj.set_date(cell.date_value_);
  } else if (ObFileColumnType::DATETIME == column.type_) {
    obj.set_datetime(cell.datetime_value_);
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObFileTableScan::get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  row = nullptr;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (ObFileTableKind::LIST == def_.kind_) {
    if (next_file_idx_ >= file_paths_.size()) {
      uint64_t device = 0;
      uint64_t inode = 0;
      int64_t modified_time_ns = 0;
      const std::string directory(def_.canonical_path_.ptr(), def_.canonical_path_.length());
      std::string canonical_directory;
      if (OB_FAIL(ObFileScanUtils::get_directory_fingerprint(
                   directory, canonical_directory, device, inode, modified_time_ns))) {
      } else if (device != def_.device_ || inode != def_.inode_
                 || modified_time_ns != def_.modified_time_ns_) {
        ret = OB_SCHEMA_EAGAIN;
        LOG_WARN("glob directory changed while scanning", K(ret));
      } else {
        ret = OB_ITER_END;
      }
    } else {
      current_file_ = file_paths_.at(next_file_idx_++);
      cell_buffer_[0].set_varchar(ObString(static_cast<int32_t>(current_file_.size()),
                                           current_file_.data()));
      cell_buffer_[0].set_collation_type(collation_);
      row = &row_;
    }
  } else {
    cells_.resize(def_.columns_.count());
    if (OB_FAIL(reader_.get_next_row(cells_))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to read file row", K(ret));
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < projected_idxs_.size(); ++i) {
        const int64_t index = projected_idxs_[i];
        if (index < 0 || index >= static_cast<int64_t>(def_.columns_.count())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(make_cell_obj(def_.columns_.at(index), cells_.at(index),
                                         cell_buffer_[i]))) {
        }
      }
      if (OB_SUCC(ret)) {
        row = &row_;
      }
    }
  }
  return ret;
}

int ObFileTableScan::rescan(const ObIArray<ObObj> &arguments)
{
  UNUSED(arguments);
  close();
  return open();
}

int ObFileTableScan::close()
{
  reader_.close();
  cells_.clear();
  file_paths_.clear();
  next_file_idx_ = 0;
  current_file_.clear();
  opened_ = false;
  return OB_SUCCESS;
}

int create_file_table_scan(const TableFunctionScanContext &context, ITableFunctionScan *&scan)
{
  int ret = OB_SUCCESS;
  void *memory = context.allocator_.alloc(sizeof(ObFileTableScan));
  if (OB_ISNULL(memory)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    ObFileTableScan *file_scan = new (memory) ObFileTableScan(context);
    if (OB_FAIL(file_scan->init_context(context))) {
      file_scan->~ObFileTableScan();
    } else {
      scan = file_scan;
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
