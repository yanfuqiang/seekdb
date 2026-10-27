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

#ifndef OCEANBASE_SQL_OB_FILE_TABLE_OPTIONS_H_
#define OCEANBASE_SQL_OB_FILE_TABLE_OPTIONS_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/string/ob_string.h"
#include "sql/table_function/ob_file_scan_common.h"

namespace oceanbase
{
namespace sql
{

// File table definitions are inferred at resolve time and must reach the scan
// operator without re-reading the file. The encoded form points all strings
// into the encoded buffer; callers keep that buffer alive for the plan lifetime.
class ObFileTableOptions
{
public:
  static int encode(const ObFileTableDef &def, common::ObIAllocator &allocator,
                    common::ObString &encoded);
  static int decode(const common::ObString &encoded, ObFileTableDef &def);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_FILE_TABLE_OPTIONS_H_
