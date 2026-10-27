/*
 * Copyright (c) 2025 OceanBase.
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

#define USING_LOG_PREFIX SQL_ENG
#include "config_bridge.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/engine/expr/ob_datum_cast.h"
#include "share/rc/ob_server_runtime.h"
#include "sql/das/ob_das_insert_op.h"
#include "sql/das/ob_das_delete_op.h"
#include "sql/das/ob_das_update_op.h"
#include "sql/das/ob_das_lock_op.h"
#include "sql/das/ob_das_utils.h"
#include "sql/engine/dml/ob_trigger_handler.h"
#include "share/autoincrement/ob_i_tablet_autoincrement_service.h"
#include "data_plane/lob/ob_lob_read.h"
#include "data_plane/transaction/ob_i_transaction_service.h"
#include "sql/pl/ob_pl.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "share/geo/ob_geo_utils.h"
#include "sql/engine/ob_batch_rows.h"
#include "sql/engine/ob_physical_plan.h"
#include "query/vector/ob_vector_index_util.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/schema/ob_schema_struct.h"
namespace oceanbase
{
using namespace common;
using namespace share;
using namespace transaction;
namespace sql
{

bool ObDMLService::check_cascaded_reference(const ObExpr *expr, const ObExprPtrIArray &row)
{
  bool bret = false;
  if (OB_ISNULL(expr) || expr->parent_cnt_ <= 0) {
    bret = false;
  } else {
    for (int i = 0; !bret && i < expr->parent_cnt_; ++i) {
      ObExpr *parent_expr = expr->parents_[i];
      if (parent_expr != nullptr) {
        if (parent_expr->type_ == T_FUN_COLUMN_CONV) {
          bret = has_exist_in_array(row, parent_expr);
        }
      }
      if (!bret) {
        bret = check_cascaded_reference(parent_expr, row);
      }
    }
  }
  return bret;
}

int ObDMLService::check_row_null(const ObExprPtrIArray &row,
                                 ObEvalCtx &eval_ctx,
                                 int64_t row_num,
                                 const ColContentIArray &column_infos,
                                 bool is_ignore,
                                 bool is_single_value,
                                 ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  CK(row.count() >= column_infos.count());
  if (OB_ISNULL(session = dml_op.get_exec_ctx().get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int i = 0; OB_SUCC(ret) && i < column_infos.count(); i++) {
    ObDatum *datum = NULL;
    const bool is_nullable = column_infos.at(i).is_nullable_;
    uint64_t col_idx = column_infos.at(i).projector_index_;
    if (OB_FAIL(row.at(col_idx)->eval(eval_ctx, datum))) {
      common::ObString column_name = column_infos.at(i).column_name_;
      ret = ObDMLService::log_user_error_inner(ret, row_num, column_name, dml_op.get_exec_ctx(),
                                               row.at(col_idx)->datum_meta_.type_);
    } else if (OB_ISNULL(datum)) {
      // impossible
    } else if (OB_FAIL(check_column_null(dml_op.get_eval_ctx(),
                                         row,
                                         column_infos.at(i),
                                         *datum,
                                         is_ignore,
                                         is_single_value,
                                         session->get_sql_mode()))) {
    }
  }
  return ret;
}

int ObDMLService::check_column_null(
    ObEvalCtx &eval_ctx,
    const ObExprPtrIArray &row,
    const ColumnContent &column_info,
    ObDatum &datum,
    const bool is_ignore,
    const bool is_single_value,
    const ObSQLMode sql_mode)
{
  int ret = OB_SUCCESS;
  const bool is_nullable = column_info.is_nullable_;
  uint64_t col_idx = column_info.projector_index_;
  ObExpr *expr = row.at(col_idx);
  if (!is_nullable && datum.is_null()) {
    if (is_ignore || (!is_single_value
                      && !is_strict_mode(sql_mode))) {
      ObObj zero_obj;
      ObExprStrResAlloc res_alloc(*expr, eval_ctx);
      ObDatum &row_datum = expr->locate_datum_for_write(eval_ctx);
      bool is_decimal_int = ob_is_decimal_int(expr->datum_meta_.type_);
      if (ob_is_geometry(expr->obj_meta_.get_type())) {
          ret = OB_BAD_NULL_ERROR;
          LOG_WARN("dml with ignore not supported in geometry type");
          LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_info.column_name_.length(), column_info.column_name_.ptr());
      } else if (ob_is_collection_sql_type(expr->obj_meta_.get_type())) {
          ret = OB_BAD_NULL_ERROR;
          LOG_WARN("dml with ignore not supported in collection type");
          LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_info.column_name_.length(), column_info.column_name_.ptr());
      } else if (check_cascaded_reference(expr, row)) {
        //This column is dependent on other columns and cannot be modified again;
        //otherwise, it will necessitate a cascading recalculation of the dependent expression results.
        ret = OB_BAD_NULL_ERROR;
        LOG_WARN("dml with ignore not supported with cascaded column", KPC(expr));
        LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_info.column_name_.length(), column_info.column_name_.ptr());
      } else if (OB_FAIL(sql::get_obj_zero_value(
          expr->obj_meta_.get_type(),
          expr->obj_meta_.get_collation_type(),
          zero_obj))) {
      } else if (is_decimal_int) {
        ObDecimalIntBuilder dec_val;
        dec_val.set_zero(wide::ObDecimalIntConstValue::get_int_bytes_by_precision(
            expr->datum_meta_.precision_));
        row_datum.set_decimal_int(dec_val.get_decimal_int(), dec_val.get_int_bytes());
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObDASUtils::padding_fixed_string_value(expr->max_length_,
                                                                res_alloc,
                                                                zero_obj))) {
      } else if (!is_decimal_int && OB_FAIL(row_datum.from_obj(zero_obj))) {
      } else if (zero_obj.is_lob_storage() && zero_obj.has_lob_header() != expr->obj_meta_.has_lob_header()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        //output warning msg
        const ObString &column_name = column_info.column_name_;
        LOG_USER_WARN(OB_BAD_NULL_ERROR, column_name.length(), column_name.ptr());
      }
    } else if (ob_is_user_defined_type(expr->obj_meta_.get_type())) {
      // do nothing for xml column
    } else {
      //output warning msg
      const ObString &column_name = column_info.column_name_;
      ret = OB_BAD_NULL_ERROR;
      LOG_USER_ERROR(OB_BAD_NULL_ERROR, column_name.length(), column_name.ptr());
    }
  }
  return ret;
}

int ObDMLService::check_column_type(const ExprFixedArray &dml_row,
                                    int64_t row_num,
                                    const ObIArray<ColumnContent> &column_infos,
                                    ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  CK(dml_row.count() >= column_infos.count());
  ObArenaAllocator tmp_allocator(ObModIds::OB_LOB_ACCESS_BUFFER, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObUserLoggingCtx::Guard logging_ctx_guard(*dml_op.get_exec_ctx().get_user_logging_ctx());
  dml_op.get_exec_ctx().set_cur_rownum(row_num);
  for (int64_t i = 0; OB_SUCC(ret) && i < column_infos.count(); ++i) {
    const ColumnContent &column_info = column_infos.at(i);
    common::ObString column_name = column_infos.at(i).column_name_;
    dml_op.get_exec_ctx().set_cur_column_name(&column_name);
    ObExpr *expr = dml_row.at(column_info.projector_index_);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(dml_op.get_eval_ctx(), datum))) {
      ret = ObDMLService::log_user_error_inner(ret, row_num, column_name, dml_op.get_exec_ctx(),
                                               expr->datum_meta_.type_);
    } else if (OB_ISNULL(datum)) {
      // impossible
    } else if (OB_FAIL(check_geometry_type(dml_op.get_eval_ctx(),
                                           dml_row,
                                           column_info,
                                           tmp_allocator,
                                           *datum))) {
    }
  }
  return ret;
}

int ObDMLService::check_geometry_type(
    ObEvalCtx &eval_ctx,
    const ExprFixedArray &dml_row,
    const ColumnContent &column_info,
    ObIAllocator &allocator,
    ObDatum &datum)
{
  int ret = OB_SUCCESS;
  ObExpr *expr = dml_row.at(column_info.projector_index_);
  if (!datum.is_null() && expr->obj_meta_.is_geometry()) {
    const uint32_t column_srid = column_info.srs_info_.srid_;
    const ObGeoType column_geo_type = static_cast<ObGeoType>(column_info.srs_info_.geo_type_);
    ObString wkb = datum.get_string();
    uint32_t input_srid = UINT32_MAX;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
        eval_ctx.exec_ctx_, allocator, datum,
        expr->datum_meta_, expr->obj_meta_.has_lob_header(), wkb))) {
    } else if (OB_FAIL(ObGeoTypeUtil::check_geo_type(column_geo_type, wkb))) {
      ret = OB_ERR_CANT_CREATE_GEOMETRY_OBJECT;
      LOG_USER_ERROR(OB_ERR_CANT_CREATE_GEOMETRY_OBJECT);
    } else if (OB_FAIL(ObGeoTypeUtil::get_srid_from_wkb(wkb, input_srid))) {
    } else if (OB_ISNULL(eval_ctx.exec_ctx_.get_srs_provider())) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(ObSqlGeoUtils::check_srid(
                   *eval_ctx.exec_ctx_.get_srs_provider(),
                   column_srid,
                   input_srid))) {
      ret = OB_ERR_WRONG_SRID_FOR_COLUMN;
      LOG_USER_ERROR(OB_ERR_WRONG_SRID_FOR_COLUMN, static_cast<uint64_t>(input_srid),
          static_cast<uint64_t>(column_srid));
    }
  }
  return ret;
}

int ObDMLService::check_rowkey_is_null(const ObExprPtrIArray &row,
                                       int64_t rowkey_cnt,
                                       ObEvalCtx &eval_ctx,
                                       bool &is_null)
{
  int ret = OB_SUCCESS;
  is_null = false;
  CK(row.count() >= rowkey_cnt);
  ObDatum *datum = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && !is_null && i < rowkey_cnt; ++i) {
    if (OB_FAIL(row.at(i)->eval(eval_ctx, datum))) {
    } else {
      is_null = datum->is_null();
    }
  }
  return ret;
}

int ObDMLService::check_table_cycle(const DASTableIdList* parent_table_set, const uint64_t table_id, bool &exist)
{
  int ret = OB_SUCCESS;
  exist = false;
  if (OB_NOT_NULL(parent_table_set)) {
    DASTableIdList::const_iterator it = parent_table_set->begin();
    for (; it != parent_table_set->end(); it++) {
      if (*it == table_id) {
        exist = true;
        break;
      }
    }
  }
  return ret;
}

int ObDMLService::delete_table_id_from_parent_table_set(ObDMLRtCtx &dml_rtctx, const ObDMLBaseCtDef &dml_ctdef)
{
  int ret = OB_SUCCESS;
  ObExecContext* root_ctx;
  if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_rtctx.get_exec_ctx(), root_ctx))) {
  } else if (OB_ISNULL(root_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    DASTableIdList& parent_table_set = root_ctx->get_das_ctx().get_parent_table_set();
    DASTableIdList::iterator it = parent_table_set.begin();
    bool found = false;
    for (; it != parent_table_set.end() && !found; it++) {
      if (*it == dml_ctdef.das_base_ctdef_.index_tid_) {
        found = true;
        parent_table_set.erase(it);
      }
    }
    if (!found) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObDMLService::check_rowkey_whether_distinct(const ObExprPtrIArray &row,
                                                DistinctType distinct_algo,
                                                ObEvalCtx &eval_ctx,
                                                ObExecContext &root_ctx,
                                                ObRowkey &tmp_table_rowkey,
                                                SeRowkeyDistCtx *rowkey_dist_ctx,
                                                bool &is_dist)
{
  int ret = OB_SUCCESS;
  is_dist = true;
  if (T_DISTINCT_NONE != distinct_algo) {
    if (T_HASH_DISTINCT == distinct_algo) {
      ObIAllocator &allocator = root_ctx.get_allocator();
      const int64_t rowkey_cnt = tmp_table_rowkey.get_obj_cnt();
      if (OB_ISNULL(rowkey_dist_ctx)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        //step1: Init ObObj of ObTableRowkey
        ObObj *tmp_obj_ptr = tmp_table_rowkey.get_obj_ptr();
        for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
          ObExpr *expr = row.at(i);
          ObDatum *col_datum = nullptr;
          if (OB_ISNULL(expr)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(expr->eval(eval_ctx, col_datum))) {
          } else if (OB_ISNULL(col_datum)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(col_datum->to_obj(tmp_obj_ptr[i], expr->obj_meta_, expr->obj_datum_map_))) {
          }
        }

        //step2: Perform distinct check use ObRowkey
        {
          ret = rowkey_dist_ctx->exist_refactored(tmp_table_rowkey);
          if (OB_HASH_EXIST == ret) {
            ret = OB_SUCCESS;
            is_dist = false;
          } else if (OB_HASH_NOT_EXIST == ret) {
            //step3: if not exist, deep copy data and add ObRowkey to hash set
            //step3.1: Init the buffer of ObObj Array
            ret = OB_SUCCESS;
            ObObj *obj_ptr = nullptr;
            void *buf = nullptr;
            if (OB_ISNULL(buf = allocator.alloc(sizeof(ObObj) * rowkey_cnt))) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              obj_ptr = new(buf) ObObj[rowkey_cnt];
            }

            //step3.2: deep copy data to ObObj
            for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_cnt; ++i) {
              if (OB_FAIL(ob_write_obj(allocator, tmp_obj_ptr[i], obj_ptr[i]))) {
              }
            }
            ObRowkey table_rowkey(obj_ptr, rowkey_cnt);
            //step3.3: add ObRowkey to hash set
            if (OB_SUCC(ret) && OB_FAIL(rowkey_dist_ctx->set_refactored(table_rowkey))) {
            }
          } else {
          }
        }
      }
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "Check Update/Delete row with merge distinct");
    }
  }
  return ret;
}

int ObDMLService::create_rowkey_check_hashset(int64_t estimate_row,
                                           ObExecContext *root_ctx,
                                           SeRowkeyDistCtx *&rowkey_dist_ctx)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = root_ctx->get_allocator();
  if (OB_ISNULL(rowkey_dist_ctx)) {
    //create rowkey distinct context
    void *buf = allocator.alloc(sizeof(SeRowkeyDistCtx));
    ObSQLSessionInfo *my_session = root_ctx->get_my_session();
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(my_session)) {
        ret = OB_ERR_UNEXPECTED;
    } else {
      rowkey_dist_ctx = new (buf) SeRowkeyDistCtx();
      int64_t match_rows = estimate_row > ObDMLBaseCtDef::MIN_ROWKEY_DISTINCT_BUCKET_NUM ?
                            estimate_row : ObDMLBaseCtDef::MIN_ROWKEY_DISTINCT_BUCKET_NUM;
      //
      // match_rows is the result of the optimizer's row estimation, if this value is very large,
      // Directly creating a hashmap with so many buckets will allocate
      // Not enough memory, here the limit is set to 64k to prevent out of memory errors
      const int64_t max_bucket_num = match_rows > ObDMLBaseCtDef::MAX_ROWKEY_DISTINCT_BUCKET_NUM ?
                              ObDMLBaseCtDef::MAX_ROWKEY_DISTINCT_BUCKET_NUM : match_rows;
      if (OB_FAIL(rowkey_dist_ctx->create(max_bucket_num,
                                      ObModIds::OB_DML_CHECK_ROWKEY_DISTINCT_BUCKET,
                                      ObModIds::OB_DML_CHECK_ROWKEY_DISTINCT_NODE))) {
      }
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObDMLService::check_lob_column_changed(ObEvalCtx &eval_ctx,
            const ObExpr& old_expr, ObDatum& old_datum,
            const ObExpr& new_expr, ObDatum& new_datum,
            int64_t& result) {
  INIT_SUCC(ret);
  int64_t timeout = 0;
  int64_t query_st = eval_ctx.exec_ctx_.get_my_session()->get_query_start_time();
  if (OB_FAIL(eval_ctx.exec_ctx_.get_my_session()->get_query_timeout(timeout))) {
  } else {
    timeout += query_st;
    ObString old_str = old_datum.get_string();
    ObString new_str = new_datum.get_string();
    bool old_set_has_lob_header = old_expr.obj_meta_.has_lob_header() && old_str.length() > 0;
    bool new_set_has_lob_header = new_expr.obj_meta_.has_lob_header() && new_str.length() > 0;
    ObLobLocatorV2 old_lob(old_str, old_set_has_lob_header);
    ObLobLocatorV2 new_lob(new_str, new_set_has_lob_header);
    if (old_set_has_lob_header && new_set_has_lob_header) {
      bool is_equal = false;
      if (old_lob.is_persist_lob() && new_lob.is_delta_temp_lob()) {
        if (OB_FAIL(ObDeltaLob::has_diff(new_lob, result))) {
        }
      } else if (OB_FAIL(data_plane::lob_binary_equal(
                     old_lob, new_lob, timeout,
                     eval_ctx.exec_ctx_.get_my_session()->get_tx_desc(), is_equal))) {
      } else {
        result = is_equal ? 0 : 1;
      }
    } else {
      result = ObDatum::binary_equal(old_datum, new_datum) ? 0 : 1;
    }
  }
  return ret;
}

int ObDMLService::check_row_whether_changed(const ObUpdCtDef &upd_ctdef,
                                            ObUpdRtDef &upd_rtdef,
                                            ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  NG_TRACE_TIMES(2, update_start_check_row);
  if (!upd_ctdef.is_primary_index_) {
    // if in pdml, upd_rtdef.primary_rtdef_ is null
    // in normal dml stmt, upd_rtdef.primary_rtdef_ is not null
    upd_rtdef.is_row_changed_ = false;
    if (OB_NOT_NULL(upd_rtdef.primary_rtdef_)) {
      upd_rtdef.is_row_changed_ = upd_rtdef.primary_rtdef_->is_row_changed_;
    } else {
      // whether the global index row is updated is subject to the result of the primary index
      // pdml e.g.:
      // create table t22(a int primary key,
      //                 b int,
      //                 c int,
      //                 d timestamp DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
      // ) partition by hash(a) partitions 5;
      // create index idx on t22(c, d) global;
      // insert into t22 values(1, 1, 1, now());
      // set _force_parallel_dml_dop = 3;
      // update t22 set b=2, c=1 where a=1;
      // this case, we must update data_table and index_table
      const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
      const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
      FOREACH_CNT_X(info, upd_ctdef.assign_columns_, OB_SUCC(ret) && !upd_rtdef.is_row_changed_) {
        const uint64_t idx = info->projector_index_;
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
        } else {
          if(is_lob_storage(old_row.at(idx)->datum_meta_.type_)
              && is_lob_storage(new_row.at(idx)->datum_meta_.type_))
          {
            int64_t cmp_res = 0;
            if(OB_FAIL(check_lob_column_changed(eval_ctx, *old_row.at(idx), *old_datum, *new_row.at(idx), *new_datum, cmp_res))) {
            } else {
              upd_rtdef.is_row_changed_ = (cmp_res != 0);
            }
          } else {
            upd_rtdef.is_row_changed_ = !ObDatum::binary_equal(*old_datum, *new_datum);
          }
        }
      }
    }
  } else {
    upd_rtdef.is_row_changed_ = false;
    const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
    const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
    FOREACH_CNT_X(info, upd_ctdef.assign_columns_, OB_SUCC(ret) && !upd_rtdef.is_row_changed_) {
      const uint64_t idx = info->projector_index_;
      if (OB_LIKELY(!info->auto_filled_timestamp_)) {
        ObDatum *old_datum = NULL;
        ObDatum *new_datum = NULL;
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
        } else {
          if(is_lob_storage(old_row.at(idx)->datum_meta_.type_)
              && is_lob_storage(new_row.at(idx)->datum_meta_.type_))
          {
            int64_t cmp_res = 0;
            if(OB_FAIL(check_lob_column_changed(eval_ctx, *old_row.at(idx), *old_datum, *new_row.at(idx), *new_datum, cmp_res))) {
            } else {
              upd_rtdef.is_row_changed_ = (cmp_res != 0);
            }
          } else {
            upd_rtdef.is_row_changed_ = !ObDatum::binary_equal(*old_datum, *new_datum);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) &&
      upd_rtdef.is_row_changed_ &&
      upd_ctdef.is_primary_index_ &&
      upd_ctdef.dupd_ctdef_.is_batch_stmt_) {
    //check predicate column whether changed in batch stmt execution
    const ObExprPtrIArray &old_row = upd_ctdef.old_row_;
    const ObExprPtrIArray &new_row = upd_ctdef.new_row_;
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_ctdef.assign_columns_.count(); ++i) {
      const ColumnContent &info = upd_ctdef.assign_columns_.at(i);
      uint64_t idx = info.projector_index_;
      ObDatum *old_datum = NULL;
      ObDatum *new_datum = NULL;
      if (info.is_predicate_column_) {
        if (OB_FAIL(old_row.at(idx)->eval(eval_ctx, old_datum))
            || OB_FAIL(new_row.at(idx)->eval(eval_ctx, new_datum))) {
        } else if (!ObDatum::binary_equal(*old_datum, *new_datum)) {
          //update the predicate column, will lead to the next stmt result change, need rollback
          ret = OB_BATCHED_MULTI_STMT_ROLLBACK;
        }
      }
    }
  }
  LOG_DEBUG("after check update row changed", K(ret), K(upd_rtdef.is_row_changed_),
              "old_row", ROWEXPR2STR(eval_ctx, upd_ctdef.old_row_),
              "new_row", ROWEXPR2STR(eval_ctx, upd_ctdef.new_row_));
  NG_TRACE_TIMES(2, update_end_check_row);
  return ret;
}

int ObDMLService::filter_row_for_check_cst(const ExprFixedArray &cst_exprs,
                                           ObEvalCtx &eval_ctx,
                                           bool &filtered)
{
  int ret = OB_SUCCESS;
  filtered = false;
  for (int64_t i = 0; OB_SUCC(ret) && !filtered && i < cst_exprs.count(); ++i) {
    ObExpr *expr = cst_exprs.at(i);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
      LOG_INFO("cover original errno while calulating check expr in mysql mode", K(ret));
      filtered = true;
      ret = OB_SUCCESS;
    } else {
      OB_ASSERT(ob_is_int_tc(expr->datum_meta_.type_));
      if (!datum->is_null() && 0 == datum->get_int()) {
        filtered = true;
      }
    }
  }
  return ret;
}

int ObDMLService::filter_row_for_view_check(const ExprFixedArray &cst_exprs,
                                           ObEvalCtx &eval_ctx,
                                           bool &filtered)
{
  int ret = OB_SUCCESS;
  filtered = false;
  for (int64_t i = 0; OB_SUCC(ret) && !filtered && i < cst_exprs.count(); ++i) {
    ObExpr *expr = cst_exprs.at(i);
    ObDatum *datum = nullptr;
    if (OB_FAIL(expr->eval(eval_ctx, datum))) {
    } else {
      OB_ASSERT(ob_is_int_tc(expr->datum_meta_.type_));
      if (datum->is_null() || 0 == datum->get_int()) {
        filtered = true;
      }
    }
  }
  return ret;
}

int ObDMLService::init_heap_table_pk_for_ins(const ObInsCtDef &ins_ctdef, ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  if (ins_ctdef.is_primary_index_ && ins_ctdef.is_table_without_pk_) {
    ObExpr *auto_inc_expr = ins_ctdef.new_row_.at(0);
    if (OB_ISNULL(auto_inc_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
      datum.set_null();
      auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
    }
  }
  return ret;
}

int ObDMLService::process_insert_row(const ObInsCtDef &ins_ctdef,
                                     ObInsRtDef &ins_rtdef,
                                     ObTableModifyOp &dml_op,
                                     bool &is_skipped)
{
  int ret = OB_SUCCESS;
  if (ins_ctdef.is_primary_index_) {
    bool is_filtered = false;
    is_skipped = false;
    ObEvalCtx &eval_ctx = dml_op.get_eval_ctx();
    uint64_t ref_table_id = ins_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    //first, check insert value whether matched column type
    if (OB_FAIL(check_column_type(ins_ctdef.new_row_,
                                  ins_rtdef.cur_row_num_,
                                  ins_ctdef.column_infos_,
                                  dml_op))) {
    } else if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), ins_ctdef.das_ctdef_.index_tid_))) {
    } else if (OB_FAIL(TriggerHandle::init_param_new_row(
        eval_ctx, ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
    } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
        dml_op, ins_ctdef.das_base_ctdef_, ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(check_row_null(ins_ctdef.new_row_,
                                      dml_op.get_eval_ctx(),
                                      ins_rtdef.cur_row_num_,
                                      ins_ctdef.column_infos_,
                                      ins_ctdef.das_ctdef_.is_ignore_,
                                      ins_ctdef.is_single_value_,
                                      dml_op))) {
    } else if (OB_FAIL(check_filter_row(ins_ctdef, dml_op.get_eval_ctx(), is_skipped))) {
    }

  }
  return ret;
}

int ObDMLService::check_filter_row(
    const ObInsCtDef &ins_ctdef,
    ObEvalCtx &eval_ctx,
    bool &is_skipped)
{
  int ret = OB_SUCCESS;
  bool is_filtered = false;
  if (OB_FAIL(filter_row_for_view_check(ins_ctdef.view_check_exprs_,
                                        eval_ctx, is_filtered))) {
  } else if (OB_UNLIKELY(is_filtered)) {
    ret = OB_ERR_CHECK_OPTION_VIOLATED;
  } else if (OB_FAIL(filter_row_for_check_cst(ins_ctdef.check_cst_exprs_,
                                              eval_ctx, is_filtered))) {
  } else if (OB_UNLIKELY(is_filtered)) {
    if (ins_ctdef.das_ctdef_.is_ignore_) {
      is_skipped = true;
      LOG_USER_WARN(OB_ERR_CHECK_CONSTRAINT_VIOLATED);
    } else {
      ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED;
    }
  }
  return ret;
}

int ObDMLService::process_lock_row(const ObLockCtDef &lock_ctdef,
                                   ObLockRtDef &lock_rtdef,
                                   bool &is_skipped,
                                   ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  bool is_null = false;
  UNUSED(dml_op);
  if (lock_ctdef.need_check_filter_null_ &&
      OB_FAIL(check_rowkey_is_null(lock_ctdef.old_row_,
                                   lock_ctdef.das_ctdef_.rowkey_cnt_,
                                   dml_op.get_eval_ctx(),
                                   is_null))) {
  } else if (is_null) {
    // no need to lock
    is_skipped = true;
  }
  return ret;
}

int ObDMLService::process_delete_row(const ObDelCtDef &del_ctdef,
                                     ObDelRtDef &del_rtdef,
                                     bool &is_skipped,
                                     ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  if (del_ctdef.is_primary_index_) {
    uint64_t ref_table_id = del_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), del_ctdef.das_ctdef_.index_tid_))) {
    }
    if (OB_SUCC(ret) && del_ctdef.need_check_filter_null_) {
      bool is_null = false;
      if (OB_FAIL(check_rowkey_is_null(del_ctdef.old_row_,
                                       del_ctdef.das_ctdef_.rowkey_cnt_,
                                       dml_op.get_eval_ctx(),
                                       is_null))) {
      } else if (is_null) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped && OB_NOT_NULL(del_rtdef.se_rowkey_dist_ctx_)) {
      bool is_distinct = false;
      ObExecContext *root_ctx = nullptr;
      if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
      } else if (OB_ISNULL(root_ctx)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(check_rowkey_whether_distinct(del_ctdef.distinct_key_,
                                                      T_HASH_DISTINCT,
                                                      dml_op.get_eval_ctx(),
                                                      *root_ctx,
                                                      del_rtdef.table_rowkey_,
                                                      del_rtdef.se_rowkey_dist_ctx_,
                                                      is_distinct))) {
      } else if (!is_distinct) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      if (OB_FAIL(TriggerHandle::init_param_old_row(
        dml_op.get_eval_ctx(), del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_))) {
      } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
          dml_op, del_ctdef.das_base_ctdef_, del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_))) {
      }
    }
    LOG_DEBUG("process delete row", K(ret), K(is_skipped),
               "old_row", ROWEXPR2STR(dml_op.get_eval_ctx(), del_ctdef.old_row_));
  }

  return ret;
}

int ObDMLService::process_update_row(const ObUpdCtDef &upd_ctdef,
                                     ObUpdRtDef &upd_rtdef,
                                     bool &is_skipped,
                                     ObTableModifyOp &dml_op)
{
  int ret = OB_SUCCESS;
  is_skipped = false;
  if (upd_ctdef.is_primary_index_) {
    uint64_t ref_table_id = upd_ctdef.das_base_ctdef_.index_tid_;
    ObSQLSessionInfo *my_session = NULL;
    if (OB_SUCC(ret) && upd_ctdef.need_check_filter_null_) {
      bool is_null = false;
      if (OB_FAIL(check_rowkey_is_null(upd_ctdef.old_row_,
                                       upd_ctdef.dupd_ctdef_.rowkey_cnt_,
                                       dml_op.get_eval_ctx(),
                                       is_null))) {
      } else if (is_null) {
        is_skipped = true;
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      if (upd_ctdef.is_table_without_pk_ &&
          OB_FAIL(copy_heap_table_hidden_pk(dml_op.get_eval_ctx(), upd_ctdef))) {
      }
    }

    if (OB_SUCC(ret) && !is_skipped) {
      bool is_distinct = false;
      if (OB_FAIL(check_rowkey_whether_distinct(upd_ctdef.distinct_key_,
                                                upd_ctdef.distinct_algo_,
                                                dml_op.get_eval_ctx(),
                                                dml_op.get_exec_ctx(),
                                                upd_rtdef.table_rowkey_,
                                                upd_rtdef.se_rowkey_dist_ctx_,
                                                is_distinct))) {
      } else if (!is_distinct) {
        is_skipped = true;
      }
    }
    if (OB_SUCC(ret) && !is_skipped) {
      bool is_filtered = false;
      //first, check assignment column whether matched column type
      if (OB_FAIL(check_column_type(upd_ctdef.new_row_,
                                    upd_rtdef.cur_row_num_,
                                    upd_ctdef.assign_columns_,
                                    dml_op))) {
      } else if (OB_ISNULL(my_session = dml_op.get_exec_ctx().get_my_session())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(check_nested_sql_legality(dml_op.get_exec_ctx(), upd_ctdef.dupd_ctdef_.index_tid_))) {
      } else if (OB_FAIL(TriggerHandle::init_param_rows(
          dml_op.get_eval_ctx(), upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
      } else if (OB_FAIL(TriggerHandle::do_handle_before_row(
          dml_op, upd_ctdef.das_base_ctdef_, upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(check_row_null(upd_ctdef.new_row_,
                                        dml_op.get_eval_ctx(),
                                        upd_rtdef.cur_row_num_,
                                        upd_ctdef.assign_columns_,
                                        upd_ctdef.dupd_ctdef_.is_ignore_,
                                        false,
                                        dml_op))) {
      } else if (OB_FAIL(check_row_whether_changed(upd_ctdef, upd_rtdef, dml_op.get_eval_ctx()))) {
      } else if (OB_UNLIKELY(!upd_rtdef.is_row_changed_)) {
        //do nothing
      } else if (OB_FAIL(filter_row_for_view_check(upd_ctdef.view_check_exprs_, dml_op.get_eval_ctx(), is_filtered))) {
      } else if (OB_UNLIKELY(is_filtered)) {
        ret = OB_ERR_CHECK_OPTION_VIOLATED;
      } else if (OB_FAIL(filter_row_for_check_cst(upd_ctdef.check_cst_exprs_, dml_op.get_eval_ctx(), is_filtered))) {
      } else if (OB_UNLIKELY(is_filtered)) {
        if (upd_ctdef.dupd_ctdef_.is_ignore_) {
          is_skipped = true;
          LOG_USER_WARN(OB_ERR_CHECK_CONSTRAINT_VIOLATED);
        } else {
          ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED;
        }
      }
    }
  } else {
    //for global index, only check whether the updated row is changed
    if (OB_FAIL(check_row_whether_changed(upd_ctdef, upd_rtdef, dml_op.get_eval_ctx()))) {
    }
  }

  if (OB_FAIL(ret)) {
  } else if (!is_skipped && upd_rtdef.has_table_cycle_) {
    ret = OB_ERR_ROW_IS_REFERENCED;
  }

  if (OB_SUCC(ret) && !is_skipped) {
    LOG_DEBUG("process update row", K(ret), K(is_skipped), K(upd_ctdef), K(upd_rtdef),
                "old_row", ROWEXPR2STR(dml_op.get_eval_ctx(), upd_ctdef.old_row_),
                "new_row", ROWEXPR2STR(dml_op.get_eval_ctx(), upd_ctdef.new_row_));
  }
  return ret;
}

int ObDMLService::insert_row(const ObInsCtDef &ins_ctdef,
                             ObInsRtDef &ins_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        ins_ctdef.new_row_,
                                        ins_ctdef,
                                        ins_rtdef))) {
  } else if (OB_FAIL(insert_row(ins_ctdef.das_ctdef_,
                                ins_rtdef.das_rtdef_,
                                tablet_loc,
                                dml_rtctx,
                                ins_ctdef.new_row_,
                                stored_row))) {
  }
  return ret;
}

int ObDMLService::insert_row(const ObDASInsCtDef &ins_ctdef,
                             ObDASInsRtDef &ins_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             const ExprFixedArray &new_row,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  ret = write_row_to_das_op<DAS_OP_TABLE_INSERT>(ins_ctdef,
                                                 ins_rtdef,
                                                 tablet_loc,
                                                 dml_rtctx,
                                                 new_row,
                                                 nullptr,
                                                 stored_row);
  return ret;
}

int ObDMLService::delete_row(const ObDelCtDef &del_ctdef,
                             ObDelRtDef &del_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        del_ctdef.old_row_,
                                        del_ctdef,
                                        del_rtdef))) {
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_DELETE>(del_ctdef.das_ctdef_,
                                                              del_rtdef.das_rtdef_,
                                                              tablet_loc,
                                                              dml_rtctx,
                                                              del_ctdef.old_row_,
                                                              del_ctdef.trans_info_expr_,
                                                              stored_row))) {
  }
  return ret;
}
int ObDMLService::lock_row(const ObLockCtDef &lock_ctdef,
                           ObLockRtDef &lock_rtdef,
                           const ObDASTabletLoc *tablet_loc,
                           ObDMLRtCtx &dml_rtctx)
{
  int ret = OB_SUCCESS;
  ObChunkDatumStore::StoredRow* stored_row = nullptr;
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *tablet_loc,
                                        lock_ctdef.old_row_,
                                        lock_ctdef,
                                        lock_rtdef))) {
  } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_LOCK>(lock_ctdef.das_ctdef_,
                                                lock_rtdef.das_rtdef_,
                                                tablet_loc,
                                                dml_rtctx,
                                                lock_ctdef.old_row_,
                                                nullptr,
                                                stored_row))) {
  }
  return ret;
}

int ObDMLService::lock_row(const ObDASLockCtDef &dlock_ctdef,
                           ObDASLockRtDef &dlock_rtdef,
                           const ObDASTabletLoc *tablet_loc,
                           ObDMLRtCtx &das_rtctx,
                           const ExprFixedArray &old_row)
{
  ObChunkDatumStore::StoredRow* stored_row = nullptr;
  return write_row_to_das_op<DAS_OP_TABLE_LOCK>(dlock_ctdef,
                                                dlock_rtdef,
                                                tablet_loc,
                                                das_rtctx,
                                                old_row,
                                                nullptr,
                                                stored_row);
}

int ObDMLService::split_upd_to_del_and_ins(const ObUpdCtDef &upd_ctdef,
                                           ObUpdRtDef &upd_rtdef,
                                           const ObDASTabletLoc *old_tablet_loc,
                                           const ObDASTabletLoc *new_tablet_loc,
                                           ObDMLRtCtx &dml_rtctx,
                                           ObChunkDatumStore::StoredRow *&old_row,
                                           ObChunkDatumStore::StoredRow *&new_row)
{
  int ret = OB_SUCCESS;
  if (dml_rtctx.das_ref_.get_parallel_type() == DAS_STREAMING_PARALLEL) {
    dml_rtctx.das_ref_.get_das_parallel_ctx().set_das_parallel_type(DAS_BLOCKING_PARALLEL);
  }
  if (OB_LIKELY(old_tablet_loc != new_tablet_loc && !upd_ctdef.multi_ctdef_->is_enable_row_movement_)) {
    ret = OB_ERR_UPD_CAUSE_PART_CHANGE;
  } else if (OB_ISNULL(upd_rtdef.ddel_rtdef_)) {
    if (OB_FAIL(init_das_del_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
    } else if (OB_FAIL(init_das_ins_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
    }
  }

  if (OB_SUCC(ret)) {
    //because of this bug:
    //if the updated row is moved across partitions, we must delete old row at first
    //and then store new row to a temporary buffer,
    //only when all old rows have been deleted, new rows can be inserted
    if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_DELETE>(*upd_ctdef.ddel_ctdef_,
                                                         *upd_rtdef.ddel_rtdef_,
                                                         old_tablet_loc,
                                                         dml_rtctx,
                                                         upd_ctdef.old_row_,
                                                         upd_ctdef.trans_info_expr_,
                                                         old_row))) {
    } else if (((upd_ctdef.is_table_without_pk_ && old_tablet_loc != new_tablet_loc) || (upd_ctdef.is_vec_hnsw_index_vid_opt_)) &&
        OB_FAIL(set_update_hidden_pk(dml_rtctx.get_eval_ctx(),
                                     upd_ctdef,
                                     new_tablet_loc->tablet_id_))) {
    } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_INSERT>(*upd_ctdef.dins_ctdef_,
                                                                *upd_rtdef.dins_rtdef_,
                                                                new_tablet_loc,
                                                                dml_rtctx,
                                                                upd_ctdef.new_row_,
                                                                nullptr,
                                                                new_row))) {
    } else {
      LOG_DEBUG("update pkey changed", K(ret), KPC(old_tablet_loc), KPC(new_tablet_loc),
                "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
    }
  }
  return ret;
}


/*
 * Note: During the update process,
 * ObDMLService::check_row_whether_changed() and ObDMLService::update_row() must be executed together
 * within a single iteration,
 * because the update_row process relies on check_row_whether_changed to determine
 * whether the new and old values of the row being updated have changed.
 **/
int ObDMLService::update_row(const ObUpdCtDef &upd_ctdef,
                             ObUpdRtDef &upd_rtdef,
                             const ObDASTabletLoc *old_tablet_loc,
                             const ObDASTabletLoc *new_tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             ObChunkDatumStore::StoredRow *&old_row,
                             ObChunkDatumStore::StoredRow *&new_row,
                             ObChunkDatumStore::StoredRow *&full_row)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(dml_rtctx.get_exec_ctx());
  if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                        *old_tablet_loc,
                                        upd_ctdef.old_row_,
                                        upd_ctdef,
                                        upd_rtdef))) {
  } else if (OB_FAIL(check_dml_tablet_validity(dml_rtctx,
                                               *new_tablet_loc,
                                               upd_ctdef.new_row_,
                                               upd_ctdef,
                                               upd_rtdef))) {
  } else if (OB_UNLIKELY(!upd_rtdef.is_row_changed_)) {
    //old row is equal to new row, only need to lock row
    if (OB_ISNULL(upd_rtdef.dlock_rtdef_)) {
      ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
      if (OB_FAIL(init_das_lock_rtdef_for_update(dml_rtctx, upd_ctdef, upd_rtdef))) {
      } else {
        upd_rtdef.dlock_rtdef_->for_upd_wait_time_ = plan_ctx->get_ps_timeout_timestamp();
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_LOCK>(*upd_ctdef.dlock_ctdef_,
                                                         *upd_rtdef.dlock_rtdef_,
                                                         old_tablet_loc,
                                                         dml_rtctx,
                                                         upd_ctdef.old_row_,
                                                         nullptr,
                                                         old_row))) {
      } else {
        new_row = old_row;
      }
    }
  } else if (upd_ctdef.das_base_ctdef_.is_update_pk_) {
    if (!upd_rtdef.dupd_rtdef_.is_immediate_row_conflict_check_ && !upd_ctdef.dupd_ctdef_.is_ignore_) {
      if (OB_FAIL(split_upd_to_del_and_ins(upd_ctdef,
                                           upd_rtdef,
                                           old_tablet_loc,
                                           new_tablet_loc,
                                           dml_rtctx,
                                           old_row,
                                           new_row))) {
      }
    } else {
      if (new_tablet_loc != old_tablet_loc) {
        if (upd_ctdef.dupd_ctdef_.is_ignore_) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "Cross-partition update ignore");
        } else if (OB_FAIL(split_upd_to_del_and_ins(upd_ctdef,
                                                    upd_rtdef,
                                                    old_tablet_loc,
                                                    new_tablet_loc,
                                                    dml_rtctx,
                                                    old_row,
                                                    new_row))) {
        }
      } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_UPDATE>(upd_ctdef.dupd_ctdef_,
                                                                upd_rtdef.dupd_rtdef_,
                                                                old_tablet_loc,
                                                                dml_rtctx,
                                                                upd_ctdef.full_row_,
                                                                upd_ctdef.trans_info_expr_,
                                                                full_row))) {
      } else {
        LOG_DEBUG("update pkey", K(ret), KPC(old_tablet_loc),
                  "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                  "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
      }
    }
  } else {
    // do update
    if (new_tablet_loc != old_tablet_loc) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(write_row_to_das_op<DAS_OP_TABLE_UPDATE>(upd_ctdef.dupd_ctdef_,
                                                                upd_rtdef.dupd_rtdef_,
                                                                old_tablet_loc,
                                                                dml_rtctx,
                                                                upd_ctdef.full_row_,
                                                                upd_ctdef.trans_info_expr_,
                                                                full_row))) {
    } else {
      LOG_DEBUG("not update pkey", K(ret), KPC(old_tablet_loc),
                "old row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.old_row_),
                "new row", ROWEXPR2STR(dml_rtctx.get_eval_ctx(), upd_ctdef.new_row_));
    }
  }
  return ret;
}

int ObDMLService::update_row(const ObDASUpdCtDef &ctdef,
                             ObDASUpdRtDef &rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &dml_rtctx,
                             const ExprFixedArray &full_row)
{
  ObChunkDatumStore::StoredRow* stored_row = nullptr;
  return write_row_to_das_op<DAS_OP_TABLE_UPDATE>(ctdef,
                                                  rtdef,
                                                  tablet_loc,
                                                  dml_rtctx,
                                                  full_row,
                                                  nullptr,
                                                  stored_row);
}

int ObDMLService::delete_row(const ObDASDelCtDef &das_del_ctdef,
                             ObDASDelRtDef &das_del_rtdef,
                             const ObDASTabletLoc *tablet_loc,
                             ObDMLRtCtx &das_rtctx,
                             const ExprFixedArray &old_row,
                             ObChunkDatumStore::StoredRow *&stored_row)
{
  return write_row_to_das_op<DAS_OP_TABLE_DELETE>(das_del_ctdef,
                                                  das_del_rtdef,
                                                  tablet_loc,
                                                  das_rtctx,
                                                  old_row,
                                                  nullptr,
                                                  stored_row);
}

int ObDMLService::prepare_dml_execution(
    const ObDASDMLBaseCtDef &base_ctdef,
    ObDASDMLBaseRtDef &base_rtdef,
    transaction::ObTxReadSnapshot &snapshot,
    const int16_t write_branch_id,
    ObIAllocator &das_alloc,
    data_plane::ObWriteContext &write_context,
    data_plane::ObDmlExecution &execution,
    bool use_snapshot_opt)
{
  int ret = OB_SUCCESS;
  data_plane::ObDmlWriteSpec write_spec;
  concurrent_control::ObWriteFlag write_flag;
  write_spec.timeout_ = base_rtdef.timeout_ts_;
  write_spec.schema_version_ = base_ctdef.schema_version_;
  write_spec.sql_mode_ = base_rtdef.sql_mode_;
  write_spec.tz_info_ = &base_ctdef.tz_info_;
  write_spec.branch_id_ = write_branch_id;
  write_spec.is_total_quantity_log_ = base_ctdef.is_total_quantity_log_;
  write_spec.prelock_ = base_rtdef.prelock_;
  write_spec.is_batch_stmt_ = base_ctdef.is_batch_stmt_;
  write_spec.is_main_table_in_fts_ddl_ =
      base_ctdef.is_main_table_in_fts_ddl_;
  write_spec.check_schema_version_ =
      !base_ctdef.skip_check_schema_version_;
  write_spec.access_vector_id_as_master_table_ =
      base_ctdef.is_access_vidx_as_master_table_;
  init_dml_write_flag(
      base_ctdef, base_rtdef, write_flag, use_snapshot_opt);
  data_plane::ObIDmlService *dml_service =
      ::oceanbase::share::server_service<::oceanbase::data_plane::ObIDmlService>();
  if (OB_ISNULL(dml_service)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(dml_service->prepare_execution(
                 write_spec,
                 base_ctdef.table_param_,
                 snapshot,
                 das_alloc,
                 write_context,
                 write_flag,
                 execution))) {
  }
  return ret;
}

void ObDMLService::init_dml_write_flag(const ObDASDMLBaseCtDef &base_ctdef,
                                       ObDASDMLBaseRtDef &base_rtdef,
                                       concurrent_control::ObWriteFlag &write_flag,
                                       bool use_snapshot_opt)
{
  if (base_ctdef.is_batch_stmt_) {
    write_flag.set_is_dml_batch_opt();
  }
  if (base_ctdef.is_insert_up_) {
    write_flag.set_is_insert_up();
  }
  if (base_ctdef.table_param_.get_data_table().is_storage_index_table()
      && !base_ctdef.table_param_.get_data_table().can_read_index()) {
    write_flag.set_is_write_only_index();
  }
  if (base_rtdef.is_for_foreign_key_check_) {
    write_flag.set_check_row_locked();
  }
  if (base_ctdef.is_update_uk_) {
    write_flag.set_update_uk();
  }
  if (base_ctdef.is_update_pk_with_dop_) {
    write_flag.set_update_pk_dop();
  }
  if (base_rtdef.is_immediate_row_conflict_check_ && base_ctdef.is_update_pk_) {
    write_flag.set_immediate_row_check();
  }
  if (use_snapshot_opt) {
    write_flag.set_snapshot_opt();
  }
}

int ObDMLService::init_das_dml_rtdef(ObDMLRtCtx &dml_rtctx,
                                     const ObDASDMLBaseCtDef &das_ctdef,
                                     ObDASDMLBaseRtDef &das_rtdef,
                                     const ObDASTableLocMeta *loc_meta)
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(dml_rtctx.get_exec_ctx());
  ObSQLSessionInfo *my_session = GET_MY_SESSION(dml_rtctx.get_exec_ctx());
  ObDASCtx &das_ctx = dml_rtctx.get_exec_ctx().get_das_ctx();
  uint64_t table_loc_id = das_ctdef.table_id_;
  uint64_t ref_table_id = das_ctdef.index_tid_;
  das_rtdef.ctdef_ = &das_ctdef;
  das_rtdef.timeout_ts_ = plan_ctx->get_ps_timeout_timestamp();
  das_rtdef.prelock_ = my_session->get_prelock();
  das_rtdef.runtime_schema_version_ = plan_ctx->get_runtime_schema_version();
  das_rtdef.sql_mode_ = my_session->get_sql_mode();
  das_rtdef.is_immediate_row_conflict_check_ =  my_session->enable_immediate_row_conflict_check();
  if (OB_ISNULL(das_rtdef.table_loc_ = dml_rtctx.op_.get_input()->get_table_loc())) {
    das_rtdef.table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id);
    if (OB_ISNULL(das_rtdef.table_loc_)) {
      if (OB_ISNULL(loc_meta)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(das_ctx.extended_table_loc(*loc_meta, das_rtdef.table_loc_))) {
      }
    }
  }
  if (ObSQLUtils::is_fk_nested_sql(&dml_rtctx.get_exec_ctx())) {
    das_rtdef.is_for_foreign_key_check_ = true;
  }
  return ret;
}

int ObDMLService::init_trigger_for_insert(
  ObDMLRtCtx &dml_rtctx,
  const ObInsCtDef &ins_ctdef,
  ObInsRtDef &ins_rtdef,
  ObIArray<ObExpr*> &clear_exprs)
{
  int ret = OB_SUCCESS;
  if (!ins_ctdef.is_primary_index_ || 0 >= ins_ctdef.trig_ctdef_.tg_args_.count()) {
    // nothing
  } else if (OB_FAIL(TriggerHandle::init_trigger_params(dml_rtctx, ins_ctdef.trig_ctdef_.tg_event_,
                          ins_ctdef.trig_ctdef_, ins_rtdef.trig_rtdef_))) {
  } else {
    append(clear_exprs, ins_ctdef.trig_ctdef_.new_row_exprs_);
  }
  return ret;
}

int ObDMLService::init_fk_checker_array(ObDMLRtCtx &dml_rtctx,
                                        const ObDMLBaseCtDef &dml_ctdef,
                                        FkCheckerArray &fk_checker_array)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObForeignKeyArgArray &fk_args = dml_ctdef.fk_args_;
  if (!fk_args.empty()) {
    if (OB_FAIL(fk_checker_array.allocate_array(allocator, fk_args.count()))) {
    } else {
      for (int i = 0; OB_SUCC(ret) && i < fk_args.count(); ++i) {
        fk_checker_array.at(i) = nullptr;
      }
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < fk_args.count(); ++i) {
    const ObForeignKeyArg &fk_arg = fk_args.at(i);
    ObForeignKeyChecker *fk_checker = nullptr;
    if (fk_arg.use_das_scan_) {
      if (OB_ISNULL(fk_arg.fk_ctdef_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        // create fk_checker here
        void *checker_buf = allocator.alloc(sizeof(ObForeignKeyChecker));
        if (OB_ISNULL(checker_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          fk_checker = new(checker_buf) ObForeignKeyChecker(dml_rtctx.get_eval_ctx(), *fk_arg.fk_ctdef_);
          fk_checker_array.at(i) = fk_checker;
          const ObExprFrameInfo *expr_frame_info = NULL;
          expr_frame_info = nullptr != dml_rtctx.op_.get_spec().expr_frame_info_
                               ? dml_rtctx.op_.get_spec().expr_frame_info_
                               : &(dml_rtctx.op_.get_spec().plan_->get_expr_frame_info());
          int64_t estimate_row = dml_rtctx.op_.get_spec().rows_;
          ObIAllocator *allocator = &dml_rtctx.op_.get_exec_ctx().get_allocator();
          if (OB_FAIL(fk_checker->init_foreign_key_checker(estimate_row, expr_frame_info, *fk_arg.fk_ctdef_,
                                                           dml_ctdef.new_row_, allocator))) {
          }
        }
      }
    }
  }
  return ret;
}

int ObDMLService::init_ins_rtdef(
  ObDMLRtCtx &dml_rtctx,
  ObInsRtDef &ins_rtdef,
  const ObInsCtDef &ins_ctdef,
  ObIArray<ObExpr*> &clear_exprs,
  ObIArray<ObForeignKeyChecker*> &fk_checkers)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_INSERTING);
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(ins_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(ins_ctdef.das_ctdef_.index_tid_, dml_rtctx.get_exec_ctx()))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                       ins_ctdef.das_ctdef_,
                                       ins_rtdef.das_rtdef_,
                                       loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, ins_ctdef.related_ctdefs_, ins_rtdef.related_rtdefs_))) {
  } else if (OB_FAIL(init_trigger_for_insert(dml_rtctx, ins_ctdef, ins_rtdef, clear_exprs))) {
  } else if (OB_FAIL(init_fk_checker_array(dml_rtctx, ins_ctdef, ins_rtdef.fk_checker_array_))) {
  } else {
    ins_rtdef.das_rtdef_.related_ctdefs_ = &ins_ctdef.related_ctdefs_;
    ins_rtdef.das_rtdef_.related_rtdefs_ = &ins_rtdef.related_rtdefs_;
    for (int64_t i = 0; OB_SUCC(ret) && i < ins_rtdef.fk_checker_array_.count(); ++i) {
      if (OB_NOT_NULL(ins_rtdef.fk_checker_array_.at(i))) {
        fk_checkers.push_back(ins_rtdef.fk_checker_array_.at(i));
      }
    }
  }
  return ret;
}

int ObDMLService::init_trigger_for_delete(
  ObDMLRtCtx &dml_rtctx, const ObDelCtDef &del_ctdef, ObDelRtDef &del_rtdef)
{
  int ret = OB_SUCCESS;
  if (del_ctdef.is_primary_index_ && 0 < del_ctdef.trig_ctdef_.tg_args_.count()) {
    OZ(TriggerHandle::init_trigger_params(dml_rtctx, del_ctdef.trig_ctdef_.tg_event_,
                                del_ctdef.trig_ctdef_, del_rtdef.trig_rtdef_));
    LOG_DEBUG("debug delete trigger init", K(ret), K(del_ctdef.is_primary_index_),
      K(del_ctdef.trig_ctdef_.tg_args_.count()));
  }
  return ret;
}

int ObDMLService::init_del_rtdef(ObDMLRtCtx &dml_rtctx,
                                 ObDelRtDef &del_rtdef,
                                 const ObDelCtDef &del_ctdef)
{
  int ret = OB_SUCCESS;
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_DELETING);
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(del_ctdef.multi_ctdef_);
    if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(del_ctdef.das_ctdef_.index_tid_, dml_rtctx.get_exec_ctx()))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                       del_ctdef.das_ctdef_,
                                       del_rtdef.das_rtdef_,
                                       loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, del_ctdef.related_ctdefs_, del_rtdef.related_rtdefs_))) {
  } else if (OB_FAIL(init_trigger_for_delete(dml_rtctx, del_ctdef, del_rtdef))) {
  } else if (OB_FAIL(init_fk_checker_array(dml_rtctx, del_ctdef, del_rtdef.fk_checker_array_))) {
  } else {
    del_rtdef.das_rtdef_.related_ctdefs_ = &del_ctdef.related_ctdefs_;
    del_rtdef.das_rtdef_.related_rtdefs_ = &del_rtdef.related_rtdefs_;
  }

  if (OB_SUCC(ret)) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    const uint64_t del_table_id = del_ctdef.das_base_ctdef_.index_tid_;
    ObExecContext *root_ctx = nullptr;
    if (T_DISTINCT_NONE != del_ctdef.distinct_algo_) {
      if (dml_op.is_fk_nested_session()) {
        // for delete distinct check that has foreign key, perform global distinct check between nested session,
        // to avoid delete same row mutiple times between different nested sqls
        if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
        } else if (OB_ISNULL(root_ctx)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          DASDelCtxList& del_ctx_list = root_ctx->get_das_ctx().get_das_del_ctx_list();
          if (ObDMLService::has_nested_delete_ctx(del_table_id, del_ctx_list)) {
            // for table deleted at parent session too, no need to create a new hash set
            if (OB_FAIL(ObDMLService::get_nested_delete_ctx(del_table_id, del_ctx_list, del_rtdef.se_rowkey_dist_ctx_))) {
            }
          } else {
            // for table not deleted at parent session, create a new hash set and add to the list at root ctx
            DmlRowkeyDistCtx del_ctx;
            del_ctx.table_id_ = del_table_id;
            if (OB_FAIL(ObDMLService::create_rowkey_check_hashset(dml_op.get_spec().rows_, root_ctx, del_ctx.deleted_rows_))) {
            } else if (OB_FAIL(del_ctx_list.push_back(del_ctx))) {
              if (nullptr != del_ctx.deleted_rows_) {
                del_ctx.deleted_rows_->destroy();
                del_ctx.deleted_rows_ = nullptr;
              }
            } else {
              del_rtdef.se_rowkey_dist_ctx_ = del_ctx.deleted_rows_;
            }
          }
        }
      } else {
        // for delete distinct check without foreign key, perform distinct check at current sql
        DASDelCtxList& del_ctx_list = dml_op.get_exec_ctx().get_das_ctx().get_das_del_ctx_list();
        DmlRowkeyDistCtx del_ctx;
        del_ctx.table_id_ = del_table_id;
        if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
        } else if (OB_ISNULL(root_ctx) || root_ctx != &dml_op.get_exec_ctx()) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ObDMLService::create_rowkey_check_hashset(dml_op.get_spec().rows_, root_ctx, del_ctx.deleted_rows_))) {
        } else if (OB_FAIL(del_ctx_list.push_back(del_ctx))) {
          if (nullptr != del_ctx.deleted_rows_) {
            del_ctx.deleted_rows_->destroy();
            del_ctx.deleted_rows_ = nullptr;
          }
        } else {
          del_rtdef.se_rowkey_dist_ctx_ = del_ctx.deleted_rows_;
        }
      }
    } else { //T_DISTINCT_NONE == del_ctdef.distinct_algo_, means optimizer think don't need to create a new hash set for distinct check
      if (dml_op.is_fk_nested_session()) { //for delete triggered by delete cascade, need to check whether upper nested sqls will delete the same table
        if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
        } else if (OB_ISNULL(root_ctx)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          DASDelCtxList& del_ctx_list = root_ctx->get_das_ctx().get_das_del_ctx_list();
          if (ObDMLService::has_nested_delete_ctx(del_table_id, del_ctx_list)) {
            // Reuse the root context when an outer cascade already deletes this table.
            if (OB_FAIL(ObDMLService::get_nested_delete_ctx(del_table_id, del_ctx_list, del_rtdef.se_rowkey_dist_ctx_))) {
            }
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_NOT_NULL(del_rtdef.se_rowkey_dist_ctx_)) {
    const int64_t rowkey_cnt = del_ctdef.distinct_key_.count();
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(init_ob_rowkey(allocator, del_ctdef.distinct_key_.count(), del_rtdef.table_rowkey_))) {
    }
  }
  return ret;
}

int ObDMLService::init_trigger_for_update(
  ObDMLRtCtx &dml_rtctx,
  const ObUpdCtDef &upd_ctdef,
  ObUpdRtDef &upd_rtdef,
  ObTableModifyOp &dml_op,
  ObIArray<ObExpr*> &clear_exprs)
{
  UNUSED(dml_op);
  int ret = OB_SUCCESS;
  if (!upd_ctdef.is_primary_index_ || 0 >= upd_ctdef.trig_ctdef_.tg_args_.count()) {
    // nothing
  } else if (OB_FAIL(TriggerHandle::init_trigger_params(dml_rtctx, upd_ctdef.trig_ctdef_.tg_event_,
                          upd_ctdef.trig_ctdef_, upd_rtdef.trig_rtdef_))) {
  } else {
    ObSEArray<ObExpr *, 4> new_exprs;
    for (int64_t i = 0; i < upd_ctdef.trig_ctdef_.new_row_exprs_.count() && OB_SUCC(ret); ++i) {
      if (upd_ctdef.trig_ctdef_.new_row_exprs_.at(i) != upd_ctdef.trig_ctdef_.old_row_exprs_.at(i)) {
        OZ(new_exprs.push_back(upd_ctdef.trig_ctdef_.new_row_exprs_.at(i)));
      }
    }
    if (OB_SUCC(ret)) {
      append(clear_exprs, new_exprs);
    }
  }
  return ret;
}

int ObDMLService::init_upd_rtdef(
  ObDMLRtCtx &dml_rtctx,
  ObUpdRtDef &upd_rtdef,
  const ObUpdCtDef &upd_ctdef,
  ObIArray<ObExpr*> &clear_exprs,
  ObIArray<ObForeignKeyChecker*> &fk_checkers)
{
  int ret = OB_SUCCESS;
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  dml_rtctx.get_exec_ctx().set_dml_event(ObDmlEventType::DE_UPDATING);
  if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(upd_ctdef.dupd_ctdef_.index_tid_, dml_rtctx.get_exec_ctx()))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                       upd_ctdef.dupd_ctdef_,
                                       upd_rtdef.dupd_rtdef_,
                                       loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_upd_ctdefs_, upd_rtdef.related_upd_rtdefs_))) {
  } else if (OB_FAIL(init_trigger_for_update(dml_rtctx, upd_ctdef, upd_rtdef, dml_rtctx.op_, clear_exprs))) {
  } else if (OB_FAIL(init_fk_checker_array(dml_rtctx, upd_ctdef, upd_rtdef.fk_checker_array_))) {
  } else {
    upd_rtdef.dupd_rtdef_.related_ctdefs_ = &upd_ctdef.related_upd_ctdefs_;
    upd_rtdef.dupd_rtdef_.related_rtdefs_ = &upd_rtdef.related_upd_rtdefs_;
    dml_rtctx.get_exec_ctx().set_update_columns(&upd_ctdef.assign_columns_);
    for (int64_t i = 0; OB_SUCC(ret) && i < upd_rtdef.fk_checker_array_.count(); ++i) {
      if (OB_NOT_NULL(upd_rtdef.fk_checker_array_.at(i))) {
        fk_checkers.push_back(upd_rtdef.fk_checker_array_.at(i));
      }
    }
  }

  if (OB_SUCC(ret) && T_DISTINCT_NONE != upd_ctdef.distinct_algo_) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    if (OB_FAIL(create_rowkey_check_hashset(dml_op.get_spec().rows_,
                                            &dml_op.get_exec_ctx(),
                                            upd_rtdef.se_rowkey_dist_ctx_))) {
    }
  }

  if (OB_SUCC(ret) && OB_NOT_NULL(upd_rtdef.se_rowkey_dist_ctx_)) {
    const int64_t rowkey_cnt = upd_ctdef.distinct_key_.count();
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(init_ob_rowkey(allocator, upd_ctdef.distinct_key_.count(), upd_rtdef.table_rowkey_))) {
    }
  }

  // Calculate if there exists table cycle.
  // A repeated table in the parent set means the cascade contains a cycle.
  // Otherwise it is false.
  if (OB_SUCC(ret) && upd_ctdef.is_primary_index_) {
    ObTableModifyOp &dml_op = dml_rtctx.op_;
    const uint64_t upd_table_id = upd_ctdef.das_base_ctdef_.index_tid_;
    ObExecContext *root_ctx = nullptr;
    if (OB_FAIL(get_root_exec_ctx_for_fk_cascading(&dml_op.get_exec_ctx(), root_ctx))) {
    } else if (OB_ISNULL(root_ctx)) {
      ret = OB_ERR_UNEXPECTED;
    }
    if (OB_SUCC(ret)) {
      DASTableIdList* parent_table_set = &root_ctx->get_das_ctx().get_parent_table_set();
      // check table cycle even if upd_ctdef.need_check_table_cycle_ = false.
      // For example, create table A(a int primary key, a2 int, foreign key(a2) references A(a) on update cascade)
      // execute 'update A set a = a * 10;', it will first update A(a), then update A(a2)
      // When it updates A(a), upd_ctdef.need_check_table_cycle_ = true. 'A' is added to the parent table set.
      // When it updates A(a2) through the inner sql, upd_ctdef.need_check_table_cycle_ = false.
      // because there is no cycle if we only update A(a2).
      // but we need to check the cycle, because it may be from A(a).
      if (OB_FAIL(check_table_cycle(parent_table_set, upd_table_id, upd_rtdef.has_table_cycle_))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!upd_rtdef.has_table_cycle_ && upd_ctdef.need_check_table_cycle_) {
        if (OB_FAIL(parent_table_set->push_front(upd_table_id))) {
          ret = OB_ERR_UNEXPECTED;
        }
      }
    }
  }
  return ret;
}

int ObDMLService::init_ob_rowkey( ObIAllocator &allocator, const int64_t rowkey_cnt, ObRowkey &table_rowkey)
{
  int ret = OB_SUCCESS;
  ObObj *obj_ptr = nullptr;
  void *buf = nullptr;
  if (OB_ISNULL(buf = allocator.alloc(sizeof(ObObj) * rowkey_cnt))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    obj_ptr = new(buf) ObObj[rowkey_cnt];
  }
  table_rowkey.assign(obj_ptr, rowkey_cnt);
  return ret;
}

int ObDMLService::add_trans_info_datum(ObExpr *trans_info_expr,
                                       ObEvalCtx &eval_ctx,
                                       ObChunkDatumStore::StoredRow *stored_row)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;

  if (OB_ISNULL(trans_info_expr) || OB_ISNULL(stored_row)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(trans_info_expr->eval(eval_ctx, datum))) {
  } else if (OB_ISNULL(datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    char *buf = static_cast<char *>(stored_row->get_extra_payload());
    *static_cast<int32_t *>(stored_row->get_extra_payload()) = datum->len_;
    int64_t pos = sizeof(int32_t);
    MEMCPY(buf + pos, datum->ptr_, datum->len_);
  }

  return ret;
}

int ObDMLService::init_das_ins_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                const ObUpdCtDef &upd_ctdef,
                                                ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_INSERT,
                                                allocator,
                                                upd_rtdef.dins_rtdef_))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.dins_ctdef_,
                                        *upd_rtdef.dins_rtdef_,
                                        loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_ins_ctdefs_, upd_rtdef.related_ins_rtdefs_))) {
  } else {
    upd_rtdef.dins_rtdef_->related_ctdefs_ = &upd_ctdef.related_ins_ctdefs_;
    upd_rtdef.dins_rtdef_->related_rtdefs_ = &upd_rtdef.related_ins_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_das_del_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                const ObUpdCtDef &upd_ctdef,
                                                ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_DELETE,
                                                allocator,
                                                upd_rtdef.ddel_rtdef_))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.ddel_ctdef_,
                                        *upd_rtdef.ddel_rtdef_,
                                        loc_meta))) {
  } else if (OB_FAIL(init_related_das_rtdef(dml_rtctx, upd_ctdef.related_del_ctdefs_, upd_rtdef.related_del_rtdefs_))) {
  } else {
    upd_rtdef.ddel_rtdef_->related_ctdefs_ = &upd_ctdef.related_del_ctdefs_;
    upd_rtdef.ddel_rtdef_->related_rtdefs_ = &upd_rtdef.related_del_rtdefs_;
  }
  return ret;
}

int ObDMLService::init_das_lock_rtdef_for_update(ObDMLRtCtx &dml_rtctx,
                                                 const ObUpdCtDef &upd_ctdef,
                                                 ObUpdRtDef &upd_rtdef)
{
  int ret = OB_SUCCESS;
  ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(upd_ctdef.multi_ctdef_);
  if (OB_FAIL(ObDASTaskFactory::alloc_das_rtdef(DAS_OP_TABLE_LOCK,
                                                allocator,
                                                upd_rtdef.dlock_rtdef_))) {
  } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                        *upd_ctdef.dlock_ctdef_,
                                        *upd_rtdef.dlock_rtdef_,
                                        loc_meta))) {
  }
  return ret;
}
int ObDMLService::init_lock_rtdef(ObDMLRtCtx &dml_rtctx,
                                  const ObLockCtDef &lock_ctdef,
                                  ObLockRtDef &lock_rtdef,
                                  int64_t wait_ts)
{
  lock_rtdef.das_rtdef_.for_upd_wait_time_ = wait_ts;
  const ObDASTableLocMeta *loc_meta = get_table_loc_meta(lock_ctdef.multi_ctdef_);
  return init_das_dml_rtdef(dml_rtctx, lock_ctdef.das_ctdef_, lock_rtdef.das_rtdef_, loc_meta);
}

int ObDMLService::check_agg_task_state(ObDMLRtCtx &dml_rtctx, ObIDASTaskOp *das_op, int64_t row_size, bool &reach_mem_limit)
{
  int ret = OB_SUCCESS;
  ObDasAggregatedTask *agg_task = nullptr;
  reach_mem_limit = false;
  int64_t simulate_buffer_size = - EVENT_CALL(EventTable::EN_DAS_SIMULATE_AGG_TASK_BUFF_LIMIT);
  int64_t buffer_size_limit = das::OB_DAS_TASK_BUFFER_SIZE;
  if (OB_UNLIKELY(simulate_buffer_size > 0)) {
    buffer_size_limit = simulate_buffer_size;
  }
  if (dml_rtctx.das_ref_.get_parallel_type() != DAS_SERIALIZATION) {
    if (OB_ISNULL(agg_task = das_op->get_agg_task())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(agg_task->add_mem_used(row_size))) {
      // do nothing
    } else if (agg_task->get_mem_used() >= buffer_size_limit) {
      agg_task->start_status_ = DAS_AGG_TASK_REACH_MEM_LIMIT;
      reach_mem_limit = true;
      LOG_TRACE("this agg_task buff is reach memory limit", K(agg_task->get_mem_used()), K(buffer_size_limit));
    }
  }
  return ret;
}

int ObDMLService::parallel_submit_das_task(ObDMLRtCtx &dml_rtctx, ObDasAggregatedTask *agg_task)
{
  int ret = OB_SUCCESS;
  if (dml_rtctx.das_ref_.get_parallel_type() == DAS_SERIALIZATION) {
    // not support parallel_submit
    LOG_TRACE("can't parallel submit", K(dml_rtctx.das_ref_.get_parallel_type()));
  } else if (dml_rtctx.das_ref_.get_parallel_type() == DAS_STREAMING_PARALLEL) {
    if (dml_rtctx.das_ref_.get_submitted_task_count() >= dml_rtctx.das_ref_.get_das_dop()) {
      LOG_INFO("submitted tasks reach dop", "submitted_cnt", dml_rtctx.das_ref_.get_submitted_task_count(),
          "das_dop", dml_rtctx.das_ref_.get_das_dop());
    } else if (OB_FAIL(dml_rtctx.das_ref_.parallel_submit_agg_task(agg_task))) {
    } else {
      dml_rtctx.add_das_parallel_task_size(agg_task->get_mem_used());
      dml_rtctx.das_ref_.get_das_parallel_ctx().add_submitted_task_count(1);
      LOG_TRACE("succeed parallel submit one agg_task", K(agg_task), K(agg_task->get_start_status()),
              K(dml_rtctx.get_das_task_memory_size()), K(dml_rtctx.get_das_parallel_task_size()));
    }
  } else if (dml_rtctx.das_ref_.get_parallel_type() == DAS_BLOCKING_PARALLEL) {
    // Blocking submission, temporarily not committing, but will wait until there are enough parallel task numbers to truly submit all at once
    if (dml_rtctx.das_ref_.get_submitted_task_count() >= dml_rtctx.das_ref_.get_das_dop()) {
      LOG_INFO("submitted tasks reach dop", "submitted_cnt", dml_rtctx.das_ref_.get_submitted_task_count(),
          "das_dop", dml_rtctx.das_ref_.get_das_dop());
    } else {
      dml_rtctx.add_das_parallel_task_size(agg_task->get_mem_used());
      dml_rtctx.das_ref_.get_das_parallel_ctx().add_submitted_task_count(1);
      LOG_TRACE("block parallel submit one agg_task", K(agg_task), K(agg_task->get_start_status()),
              K(dml_rtctx.get_das_task_memory_size()), K(dml_rtctx.get_das_parallel_task_size()));
    }
  }
  return ret;
}

template <int N>
int ObDMLService::write_row_to_das_op(const ObDASDMLBaseCtDef &ctdef,
                                      ObDASDMLBaseRtDef &rtdef,
                                      const ObDASTabletLoc *tablet_loc,
                                      ObDMLRtCtx &dml_rtctx,
                                      const ExprFixedArray &row,
                                      ObExpr *trans_info_expr,
                                      ObChunkDatumStore::StoredRow *&stored_row)
{
  int ret = OB_SUCCESS;
  bool need_retry = false;
  bool reach_agg_mem_limit = false;
  bool is_strict_defensive_check = trans_info_expr == nullptr ? false : true;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASCtDef CtDefType;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASRtDef RtDefType;
  typedef typename das_reg::ObDASOpTypeTraits<N>::DASOp OpType;
  OB_ASSERT(typeid(ctdef) == typeid(CtDefType));
  OB_ASSERT(typeid(rtdef) == typeid(RtDefType));
  int64_t extend_size = is_strict_defensive_check ?
      ObDASWriteBuffer::DAS_WITH_TRANS_INFO_EXTEND_SIZE : ObDASWriteBuffer::DAS_ROW_DEFAULT_EXTEND_SIZE;
  // 1. find das dml op
  OpType *dml_op = nullptr;
  if (OB_UNLIKELY(!dml_rtctx.das_ref_.has_das_op(tablet_loc, dml_op))) {
    if (OB_FAIL(dml_rtctx.das_ref_.prepare_das_task(tablet_loc, dml_op))) {
    } else if (OB_FAIL(dml_op->init_task_info(extend_size))) {
    } else {
      dml_op->set_das_ctdef(static_cast<const CtDefType*>(&ctdef));
      dml_op->set_das_rtdef(static_cast<RtDefType*>(&rtdef));
    }
    bool has_domain_index = ctdef.table_param_.get_data_table().is_domain_index();
    if (!has_domain_index && OB_NOT_NULL(rtdef.related_ctdefs_)) {
      for (int64_t i = 0; !has_domain_index && i < rtdef.related_ctdefs_->count(); ++i) {
        const ObDASDMLBaseCtDef *related_ctdef = rtdef.related_ctdefs_->at(i);
        has_domain_index = OB_NOT_NULL(related_ctdef)
            && related_ctdef->table_param_.get_data_table().is_domain_index();
      }
    }
    if (OB_SUCC(ret) && has_domain_index) {
      const common::ObLobReadOptions *lob_read_options = nullptr;
      if (OB_FAIL(dml_rtctx.get_exec_ctx().get_lob_read_options(lob_read_options))) {
      } else {
        dml_op->set_srs_provider(dml_rtctx.get_exec_ctx().get_srs_provider());
        dml_op->set_lob_read_options(lob_read_options);
      }
    }
    if (OB_SUCC(ret) &&
        rtdef.related_ctdefs_ != nullptr && !rtdef.related_ctdefs_->empty()) {
      if (OB_FAIL(add_related_index_info(*tablet_loc,
                                         *rtdef.related_ctdefs_,
                                         *rtdef.related_rtdefs_,
                                         *dml_op))) {
      }
    }
  }

  // 2. add row to das dml buffer
  if (OB_FAIL(ret)) {
    // do nothing
  } else if (OB_FAIL(dml_op->write_row(row, dml_rtctx.get_eval_ctx(), stored_row))) {
  } else if (OB_ISNULL(stored_row)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(dml_rtctx.add_das_task_memory_size(stored_row->row_size_))) {
    // do nothing
  } else if (OB_NOT_NULL(trans_info_expr) &&
      OB_FAIL(ObDMLService::add_trans_info_datum(trans_info_expr, dml_rtctx.get_eval_ctx(), stored_row))) {
  } else if (dml_rtctx.das_ref_.get_parallel_type() != DAS_SERIALIZATION) {
    // parallel submit
    if (OB_FAIL(check_agg_task_state(dml_rtctx, dml_op, stored_row->row_size_, reach_agg_mem_limit))) {
    } else if (!reach_agg_mem_limit) {
    } else if (OB_FAIL(parallel_submit_das_task(dml_rtctx, dml_op->get_agg_task()))) {
    } else {
      dml_rtctx.das_ref_.clear_task_map();
    }
  } else {
    // serialize submit
  }
  return ret;
}

int ObDMLService::add_related_index_info(const ObDASTabletLoc &tablet_loc,
                                         const DASDMLCtDefArray &related_ctdefs,
                                         DASDMLRtDefArray &related_rtdefs,
                                         ObIDASTaskOp &das_op)
{
  int ret = OB_SUCCESS;
  das_op.get_related_ctdefs().set_capacity(related_ctdefs.count());
  das_op.get_related_rtdefs().set_capacity(related_rtdefs.count());
  das_op.get_related_tablet_ids().set_capacity(related_ctdefs.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < related_ctdefs.count(); ++i) {
    ObDASTabletLoc *index_tablet_loc = ObDASUtils::get_related_tablet_loc(
        const_cast<ObDASTabletLoc&>(tablet_loc), related_ctdefs.at(i)->index_tid_);
    if (OB_ISNULL(index_tablet_loc)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(das_op.get_related_ctdefs().push_back(related_ctdefs.at(i)))) {
    } else if (OB_FAIL(das_op.get_related_rtdefs().push_back(related_rtdefs.at(i)))) {
    } else if (OB_FAIL(das_op.get_related_tablet_ids().push_back(index_tablet_loc->tablet_id_))) {
    }
  }
  return ret;
}


int ObDMLService::copy_heap_table_hidden_pk(ObEvalCtx &eval_ctx,
                                            const ObUpdCtDef &upd_ctdef)
{
  int ret = OB_SUCCESS;
  ObExpr *old_hidden_pk = upd_ctdef.old_row_.at(0);
  ObExpr *new_hidden_pk = upd_ctdef.new_row_.at(0);
  ObDatum *hidden_pk_datum = NULL;
  if (OB_ISNULL(old_hidden_pk) || OB_ISNULL(new_hidden_pk)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!upd_ctdef.is_table_without_pk_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (new_hidden_pk->type_ != T_TABLET_AUTOINC_NEXTVAL) {
  } else if (OB_FAIL(old_hidden_pk->eval(eval_ctx, hidden_pk_datum))) {
  } else if (OB_ISNULL(hidden_pk_datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!old_hidden_pk->obj_meta_.is_uint64()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObDatum &datum = new_hidden_pk->locate_datum_for_write(eval_ctx);
    datum.set_uint(hidden_pk_datum->get_uint());
    new_hidden_pk->get_eval_info(eval_ctx).evaluated_ = true;
  }
  return ret;
}

int ObDMLService::set_update_hidden_pk(ObEvalCtx &eval_ctx,
                                       const ObUpdCtDef &upd_ctdef,
                                       const ObTabletID &tablet_id)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  if (upd_ctdef.is_table_without_pk_ && upd_ctdef.is_primary_index_) {
    ObExpr *auto_inc_expr = upd_ctdef.new_row_.at(0);
    ObSQLSessionInfo *my_session = eval_ctx.exec_ctx_.get_my_session();
    
    if (OB_FAIL(get_heap_table_hidden_pk(tablet_id, autoinc_seq))) {
    } else if (OB_ISNULL(auto_inc_expr)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
      datum.set_uint(autoinc_seq);
      auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
    }
  }
  return ret;
}

int ObDMLService::get_heap_table_hidden_pk(const ObTabletID &tablet_id,
                                           uint64_t &pk)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  share::ObITabletAutoincrementService *auto_inc =
      ::oceanbase::share::server_service<::oceanbase::share::ObITabletAutoincrementService>();
  if (OB_ISNULL(auto_inc)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(auto_inc->next_value(tablet_id, autoinc_seq))) {
  } else {
    pk = autoinc_seq;
  }
  return ret;
}

int ObDMLService::set_heap_table_hidden_pk(const ObInsCtDef &ins_ctdef,
                                           const ObTabletID &tablet_id,
                                           ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  uint64_t autoinc_seq = 0;
  if (ins_ctdef.is_table_without_pk_ && ins_ctdef.is_primary_index_) {
    ObSQLSessionInfo *my_session = eval_ctx.exec_ctx_.get_my_session();
    
    if (OB_FAIL(ObDMLService::get_heap_table_hidden_pk(tablet_id,
                                                       autoinc_seq))) {
    } else {
      ObExpr *auto_inc_expr = ins_ctdef.new_row_.at(0);
      if (auto_inc_expr->type_ != T_TABLET_AUTOINC_NEXTVAL) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObDatum &datum = auto_inc_expr->locate_datum_for_write(eval_ctx);
        datum.set_uint(autoinc_seq);
        auto_inc_expr->get_eval_info(eval_ctx).evaluated_ = true;
      }
    }
  }
  return ret;
}

int ObDMLService::check_nested_sql_legality(ObExecContext &ctx, common::ObTableID ref_table_id)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (session->get_is_deserialized() && ctx.get_parent_ctx() != nullptr) {
    // Nested SQL in a distributed worker lacks transaction scheduler control.
    pl::ObPLContext *pl_ctx = ctx.get_parent_ctx()->get_pl_stack_ctx();
    //this nested sql require transaction scheduler control
    //but the session is remote, means this sql executing without transaction scheduler control
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "Perform a DML operation inside a query or remote/distributed sql");
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObDASUtils::check_nested_sql_mutating(ref_table_id, ctx))) {
    }
  }
  return ret;
}

int ObDMLService::create_anonymous_savepoint(ObTxDesc &tx_desc, transaction::ObTxSEQ &savepoint)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObSqlTransControl::create_anonymous_savepoint(tx_desc, savepoint))) {
  }
  return ret;
}

int ObDMLService::rollback_local_savepoint(ObTxDesc &tx_desc, const transaction::ObTxSEQ savepoint, int64_t expire_ts)
{
  int ret = OB_SUCCESS;
  data_plane::ObITransactionService *tx =
      data_plane::query_transaction_service();
  if (OB_ISNULL(tx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(tx->rollback_to_implicit_savepoint(
                 tx_desc, savepoint, expire_ts, false))) {
  }
  return ret;
}

int ObDMLService::check_local_index_affected_rows(int64_t table_affected_rows,
                                                  int64_t index_affected_rows,
                                                  const ObDASDMLBaseCtDef &ctdef,
                                                  ObDASDMLBaseRtDef &rtdef,
                                                  const ObDASDMLBaseCtDef &related_ctdef,
                                                  ObDASDMLBaseRtDef &related_rtdef)
{
  int ret = OB_SUCCESS;
  if ((config::_enable_defensive_check() > 0)) {
    if (table_affected_rows != index_affected_rows
        && !related_ctdef.table_param_.get_data_table().is_domain_index()) {
      ret = OB_ERR_DEFENSIVE_CHECK;
      ObString func_name = ObString::make_string("check_local_index_affected_rows");
      LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
      SQL_DAS_LOG(ERROR, "Fatal Error!!! data table affected row is not match with index table", K(ret),
                  "table_affected_rows", table_affected_rows,
                  "index_affected_rows", index_affected_rows,
                  "table_ctdef", ctdef,
                  "index_ctdef", related_ctdef,
                  "table_rtdef", rtdef,
                  "index_rtdef", related_rtdef);
    }
  }
  return ret;
}

template <typename T>
const ObDASTableLocMeta *ObDMLService::get_table_loc_meta(const T *multi_ctdef)
{
  const ObDASTableLocMeta *loc_meta = nullptr;
  if (multi_ctdef != nullptr) {
    loc_meta = &(multi_ctdef->loc_meta_);
  }
  return loc_meta;
}

int ObDMLService::init_related_das_rtdef(ObDMLRtCtx &dml_rtctx,
                                         const DASDMLCtDefArray &das_ctdefs,
                                         DASDMLRtDefArray &das_rtdefs)
{
  int ret = OB_SUCCESS;
  if (!das_ctdefs.empty()) {
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_FAIL(das_rtdefs.allocate_array(allocator, das_ctdefs.count()))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < das_ctdefs.count(); ++i) {
    ObDASTaskFactory &das_factory = dml_rtctx.get_exec_ctx().get_das_ctx().get_das_factory();
    ObDASBaseRtDef *das_rtdef = nullptr;
    if (OB_FAIL(das_factory.create_das_rtdef(das_ctdefs.at(i)->op_type_, das_rtdef))) {
    } else if (OB_FAIL(init_das_dml_rtdef(dml_rtctx,
                                          *das_ctdefs.at(i),
                                          static_cast<ObDASDMLBaseRtDef&>(*das_rtdef),
                                          nullptr))) {
    } else {
      das_rtdefs.at(i) = static_cast<ObDASDMLBaseRtDef*>(das_rtdef);
    }
  }
  return ret;
}

int ObDMLService::check_dml_tablet_validity(ObDMLRtCtx &dml_rtctx,
                                            const ObDASTabletLoc &tablet_loc,
                                            const ExprFixedArray &row,
                                            const ObDMLBaseCtDef &dml_ctdef,
                                            ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if ((config::_enable_defensive_check() == 2)) {
    //only strict defensive check need to check tablet validity, otherwise ignore it
    ObTableLocation *tmp_location = nullptr;
    ObSEArray<ObTabletID, 1> tablet_ids;
    ObSEArray<ObObjectID, 1> partition_ids;
    uint64_t table_id = tablet_loc.loc_meta_->ref_table_id_;
    ObIAllocator &allocator = dml_rtctx.get_exec_ctx().get_allocator();
    if (OB_ISNULL(dml_rtdef.check_location_)) {
      void *location_buf = allocator.alloc(sizeof(ObTableLocation));
      if (OB_ISNULL(location_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        dml_rtdef.check_location_ = new(location_buf) ObTableLocation(allocator);
        tmp_location = dml_rtdef.check_location_;
        ObSchemaGetterGuard *schema_guard = dml_rtctx.get_exec_ctx().get_sql_ctx()->schema_guard_;
        ObSQLSessionInfo *session = dml_rtctx.get_exec_ctx().get_my_session();
        ObSqlSchemaGuard sql_schema_guard;
        sql_schema_guard.set_schema_guard(schema_guard);
        // Here, judge the schema version at the check table level.
        // If the table-level schema_version is not equal, directly report an error schema_again
        if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(tmp_location->init_table_location_with_column_ids(
            sql_schema_guard, table_id, dml_ctdef.column_ids_, dml_rtctx.get_exec_ctx()))) {
        }
      }
    } else {
      tmp_location = dml_rtdef.check_location_;
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(convert_exprs_to_row(row,
                                       dml_rtctx.get_eval_ctx(),
                                       dml_rtdef,
                                       dml_rtctx.get_exec_ctx().get_allocator()))) {
      } else if (OB_FAIL(tmp_location->calculate_tablet_id_by_row(dml_rtctx.get_exec_ctx(),
                                                                  table_id,
                                                                  dml_ctdef.column_ids_,
                                                                  *dml_rtdef.check_row_,
                                                                  tablet_ids,
                                                                  partition_ids))) {
      } else if (tablet_ids.count() != 1 || tablet_loc.tablet_id_ != tablet_ids.at(0)) {
        ObSchemaGetterGuard *schema_guard = dml_rtctx.get_exec_ctx().get_sql_ctx()->schema_guard_;
        ObSQLSessionInfo *session = dml_rtctx.get_exec_ctx().get_my_session();
        const ObTableSchema *table_schema = nullptr;
        // Here, judge the schema version at the check table level.
        // If the table-level schema_version is not equal, directly report an error schema_again
        if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(schema_guard->get_table_schema( table_id, table_schema))) {
        } else if (OB_ISNULL(table_schema)) {
          ret = OB_SCHEMA_ERROR;
        } else if (table_schema->get_schema_version() != dml_ctdef.das_base_ctdef_.schema_version_) {
          ret = OB_SCHEMA_EAGAIN;
          LOG_WARN("table version mismatch", K(ret), K(table_id),
              K(table_schema->get_schema_version()), K(dml_ctdef.das_base_ctdef_.schema_version_));
        } else {
          ret = OB_ERR_DEFENSIVE_CHECK;
          ObString func_name = ObString::make_string("check_dml_tablet_validity");
          LOG_USER_ERROR(OB_ERR_DEFENSIVE_CHECK, func_name.length(), func_name.ptr());
          LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret),
                    K(tablet_loc), K(tablet_ids),
                    KPC(dml_rtdef.check_row_), KPC(dml_rtdef.check_location_));
          LOG_ERROR("Fatal Error!!! Catch a defensive error!", K(ret), K(table_schema->get_schema_version()), K(dml_ctdef.das_base_ctdef_.schema_version_),
                    KPC(tmp_location), KPC(table_schema));
        }
      }
    }
  }
  return ret;
}

int ObDMLService::convert_exprs_to_row(const ExprFixedArray &exprs,
                                       ObEvalCtx &eval_ctx,
                                       ObDMLBaseRtDef &dml_rtdef,
                                       ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dml_rtdef.check_row_)) {
    if (OB_FAIL(ob_create_row(allocator, exprs.count(), dml_rtdef.check_row_))) {
    }
  }
  for (int i = 0; OB_SUCC(ret) && i < exprs.count(); ++i) {
    ObDatum *datum = nullptr;
    if (OB_FAIL(exprs.at(i)->eval(eval_ctx, datum))) {
    } else if (OB_FAIL(datum->to_obj(dml_rtdef.check_row_->cells_[i], exprs.at(i)->obj_meta_))) {
    }
  }
  return ret;
}

bool ObDMLService::has_nested_delete_ctx(const uint64_t table_id, DASDelCtxList &del_ctx_list)
{
  bool ret = false;
  DASDelCtxList::iterator iter = del_ctx_list.begin();
  for (; !ret && iter != del_ctx_list.end(); iter++) {
    DmlRowkeyDistCtx del_ctx = *iter;
    if (del_ctx.table_id_ == table_id) {
      ret = true;
    }
  }
  return ret;
}

int ObDMLService::get_nested_delete_ctx(const uint64_t table_id,
                                        DASDelCtxList &del_ctx_list,
                                        SeRowkeyDistCtx *&rowkey_dist_ctx)
{
  int ret = OB_SUCCESS;
  bool find = false;
  DASDelCtxList::iterator iter = del_ctx_list.begin();
  for (; !find && iter != del_ctx_list.end(); iter++) {
    DmlRowkeyDistCtx del_ctx = *iter;
    if (del_ctx.table_id_ == table_id) {
      find = true;
      rowkey_dist_ctx = del_ctx.deleted_rows_;
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////
// AI column maintenance.
//
// The AI column value of a changed row is invalidated and a task
// row is written into __all_ai_column_task through nested SQL
// executed on the session of the caller DML statement, so that the
// invalidation and the task share the transaction of the caller.

int64_t ObDMLService::next_ai_column_request_id()
{
  static int64_t request_id_seed = 0;
  if (OB_UNLIKELY(0 == ATOMIC_LOAD(&request_id_seed))) {
    int64_t init_val = ObTimeUtility::current_time() << 10;
    if (!ATOMIC_BCAS(&request_id_seed, 0, init_val)) {
      // another thread initialized the seed first
    }
  }
  return ATOMIC_AAF(&request_id_seed, 1);
}

static bool ai_column_find_slot(const ObDMLBaseCtDef &dml_ctdef,
                                const uint64_t column_id,
                                int64_t &slot)
{
  bool found = false;
  slot = -1;
  for (int64_t i = 0; !found && i < dml_ctdef.column_ids_.count(); ++i) {
    if (column_id == dml_ctdef.column_ids_.at(i)) {
      slot = i;
      found = true;
    }
  }
  return found;
}

static int ai_column_eval_datum(ObEvalCtx &eval_ctx,
                                const ObExprPtrIArray &row,
                                const int64_t slot,
                                ObDatum *&datum)
{
  int ret = OB_SUCCESS;
  datum = NULL;
  if (OB_UNLIKELY(slot < 0 || slot >= row.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai: invalid row slot", K(ret), K(slot), K(row.count()));
  } else if (OB_FAIL(row.at(slot)->eval(eval_ctx, datum))) {
    LOG_WARN("ai: eval row expr failed", K(ret), K(slot));
  }
  return ret;
}

static int ai_column_value_changed(ObEvalCtx &eval_ctx,
                                   const ObExprPtrIArray &old_row,
                                   const ObExprPtrIArray &new_row,
                                   const int64_t slot,
                                   bool &changed)
{
  int ret = OB_SUCCESS;
  ObDatum *old_datum = NULL;
  ObDatum *new_datum = NULL;
  changed = false;
  if (OB_FAIL(ai_column_eval_datum(eval_ctx, old_row, slot, old_datum))) {
  } else if (OB_FAIL(ai_column_eval_datum(eval_ctx, new_row, slot, new_datum))) {
  } else if (is_lob_storage(old_row.at(slot)->datum_meta_.type_)
             && is_lob_storage(new_row.at(slot)->datum_meta_.type_)) {
    int64_t cmp_res = 0;
    if (OB_FAIL(ObDMLService::check_lob_column_changed(eval_ctx,
                                                       *old_row.at(slot), *old_datum,
                                                       *new_row.at(slot), *new_datum,
                                                       cmp_res))) {
    } else {
      changed = (0 != cmp_res);
    }
  } else {
    changed = !ObDatum::binary_equal(*old_datum, *new_datum);
  }
  return ret;
}

static int ai_column_check_rowkey_changed(ObTableModifyOp &op,
                                          const ObTableSchema &table_schema,
                                          const ObDMLBaseCtDef &dml_ctdef,
                                          const ObExprPtrIArray &old_row,
                                          const ObExprPtrIArray &new_row,
                                          bool &changed)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  changed = false;
  for (int64_t i = 0; OB_SUCC(ret) && !changed && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    int64_t slot = -1;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
      LOG_WARN("ai: get rowkey column id failed", K(ret), K(i));
    } else if (OB_UNLIKELY(!ai_column_find_slot(dml_ctdef, column_id, slot))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: rowkey column not found in dml ctdef", K(ret), K(column_id));
    } else if (OB_FAIL(ai_column_value_changed(eval_ctx, old_row, new_row, slot, changed))) {
    }
  }
  return ret;
}

static int ai_column_build_rowkey_predicate(ObTableModifyOp &op,
                                            const ObTableSchema &table_schema,
                                            const ObDMLBaseCtDef &dml_ctdef,
                                            const ObExprPtrIArray &row,
                                            common::ObIAllocator &alloc,
                                            char *&buf,
                                            int64_t &buf_len,
                                            int64_t &buf_pos)
{
  int ret = OB_SUCCESS;
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  const ObObjPrintParams &print_params = op.get_obj_print_params();
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const ObColumnSchemaV2 *column_schema = NULL;
    int64_t slot = -1;
    ObDatum *datum = NULL;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
      LOG_WARN("ai: get rowkey column id failed", K(ret), K(i));
    } else if (OB_ISNULL(column_schema = table_schema.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: rowkey column schema is null", K(ret), K(column_id));
    } else if (OB_UNLIKELY(!ai_column_find_slot(dml_ctdef, column_id, slot))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: rowkey column not found in dml ctdef", K(ret), K(column_id));
    } else if (OB_FAIL(ai_column_eval_datum(eval_ctx, row, slot, datum))) {
    } else if (OB_ISNULL(datum) || datum->is_null()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: rowkey value is null", K(ret), K(column_id));
    } else if (i > 0 && OB_FAIL(databuff_printf(buf, buf_len, buf_pos, alloc, " and "))) {
    } else if (OB_FAIL(databuff_printf(buf, buf_len, buf_pos, alloc, "`%.*s` = ",
                                       column_schema->get_column_name_str().length(),
                                       column_schema->get_column_name_str().ptr()))) {
    } else {
      ObObj obj;
      if (OB_FAIL(datum->to_obj(obj, row.at(slot)->obj_meta_))) {
      } else if (OB_FAIL(obj.print_sql_literal(buf, buf_len, buf_pos, alloc, print_params))) {
      }
    }
  }
  if (OB_SUCC(ret) && 0 == rowkey_info.get_size()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "ai column on a table without rowkey");
  }
  if (OB_SUCC(ret) && OB_FAIL(databuff_printf(buf, buf_len, buf_pos, alloc, "%s", ""))) {
  } else if (OB_SUCC(ret)) {
    if (OB_ISNULL(buf)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      buf[buf_pos++] = 0;
    }
  }
  return ret;
}

static int ai_column_find_hidden_column(const ObTableSchema &table_schema,
                                        const uint64_t ai_column_id,
                                        const bool find_state_column,
                                        const ObColumnSchemaV2 *&hidden_column)
{
  int ret = OB_SUCCESS;
  hidden_column = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && NULL == hidden_column && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: column schema is null", K(ret), K(i));
    } else if (0 == ai_column_id || column->get_ai_owner_column_id() != ai_column_id) {
      // not a hidden column of the given ai column
    } else if (find_state_column && column->is_ai_state_column()) {
      hidden_column = column;
    } else if (!find_state_column && column->is_ai_request_column()) {
      hidden_column = column;
    }
  }
  if (OB_SUCC(ret) && NULL == hidden_column) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai: hidden column of ai column not found", K(ret), K(ai_column_id), K(find_state_column));
  }
  return ret;
}

static int ai_column_collect_all_ai_columns(const ObTableSchema &table_schema,
                                            ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: column schema is null", K(ret), K(i));
    } else if (column->is_ai_column() && OB_FAIL(ai_columns.push_back(column))) {
      LOG_WARN("ai: push back ai column failed", K(ret));
    }
  }
  return ret;
}

static int ai_column_collect_changed_ai_columns(ObTableModifyOp &op,
                                                const ObTableSchema &table_schema,
                                                const ObDMLBaseCtDef &dml_ctdef,
                                                const ObExprPtrIArray &old_row,
                                                const ObExprPtrIArray &new_row,
                                                ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *ai_column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(ai_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: column schema is null", K(ret), K(i));
    } else if (!ai_column->is_ai_column()) {
      // not an ai column
    } else {
      const ObString &depend_columns = ai_column->get_ai_depend_columns();
      bool changed = false;
      int32_t start = 0;
      while (OB_SUCC(ret) && !changed && start < depend_columns.length()) {
        int32_t end = start;
        while (end < depend_columns.length() && ',' != depend_columns[end]) {
          ++end;
        }
        const ObString depend_name = depend_columns.substr(start, end - start).trim();
        const ObColumnSchemaV2 *depend_column = depend_name.empty()
            ? NULL : table_schema.get_column_schema(depend_name);
        int64_t slot = -1;
        if (NULL == depend_column) {
          // the dependent column is unknown, treat the ai column as affected
          changed = true;
        } else if (!ai_column_find_slot(dml_ctdef, depend_column->get_column_id(), slot)) {
          // the dependent column is not carried by the dml ctdef, treat it as changed
          changed = true;
        } else if (OB_FAIL(ai_column_value_changed(eval_ctx, old_row, new_row, slot, changed))) {
        }
        start = end + 1;
      }
      if (OB_SUCC(ret) && changed && OB_FAIL(ai_columns.push_back(ai_column))) {
        LOG_WARN("ai: push back ai column failed", K(ret));
      }
    }
  }
  return ret;
}

static int ai_column_append_sql_string(common::ObIAllocator &alloc,
                                       char *&buf,
                                       int64_t &buf_len,
                                       int64_t &pos,
                                       const char *str,
                                       const int64_t str_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(str) || str_len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < str_len; ++i) {
    if ('\'' == str[i]) {
      if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "''"))) {
      }
    } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "%c", str[i]))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "'"))) {
  }
  return ret;
}

static int ai_column_build_invalidate_sql(common::ObIAllocator &alloc,
                                          const ObColumnSchemaV2 &ai_column,
                                          const ObTableSchema &table_schema,
                                          const ObString &database_name,
                                          const ObString &table_name,
                                          const char *predicate,
                                          const int64_t predicate_len,
                                          const int64_t request_id,
                                          ObString &stmt)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *state_column = NULL;
  const ObColumnSchemaV2 *request_column = NULL;
  char *buf = NULL;
  int64_t buf_len = 0;
  int64_t pos = 0;
  const ObString ai_column_name = ai_column.get_column_name_str();
  if (OB_FAIL(ai_column_find_hidden_column(table_schema, ai_column.get_column_id(),
                                           true, state_column))) {
  } else if (OB_FAIL(ai_column_find_hidden_column(table_schema, ai_column.get_column_id(),
                                                  false, request_column))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc,
             "update `%.*s`.`%.*s` set `%.*s` = %ld, `%.*s` = %ld, `%.*s` = NULL where %.*s",
             database_name.length(), database_name.ptr(),
             table_name.length(), table_name.ptr(),
             state_column->get_column_name_str().length(),
             state_column->get_column_name_str().ptr(),
             share::schema::AI_COLUMN_STATE_PENDING,
             request_column->get_column_name_str().length(),
             request_column->get_column_name_str().ptr(),
             request_id,
             ai_column_name.length(), ai_column_name.ptr(),
             static_cast<int>(predicate_len), predicate))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "%s", ""))) {
  } else {
    buf[pos++] = 0;
    stmt.assign_ptr(buf, static_cast<int32_t>(pos - 1));
  }
  return ret;
}

static int ai_column_build_task_sql(common::ObIAllocator &alloc,
                                    const ObColumnSchemaV2 &ai_column,
                                    const uint64_t table_id,
                                    const char *predicate,
                                    const int64_t predicate_len,
                                    const int64_t request_id,
                                    ObString &stmt)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t buf_len = 0;
  int64_t pos = 0;
  const ObString normalized_expr = ai_column.get_ai_normalized_expr();
  if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc,
             "replace into oceanbase.__all_ai_column_task "
             "(table_id, column_id, rowkey_predicate, request_id, state, retry_count, "
             "eligible_ts, normalized_expr) values (%lu, %lu, ",
             table_id, ai_column.get_column_id()))) {
  } else if (OB_FAIL(ai_column_append_sql_string(alloc, buf, buf_len, pos,
                                                 predicate, predicate_len))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, ", %ld, %ld, %d, now(6), ",
                                     request_id, share::schema::AI_COLUMN_STATE_PENDING, 0))) {
  } else if (normalized_expr.empty()) {
    if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "NULL"))) {
    }
  } else if (OB_FAIL(ai_column_append_sql_string(alloc, buf, buf_len, pos,
                                                 normalized_expr.ptr(), normalized_expr.length()))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, ")"))) {
  } else if (OB_SUCC(ret) && OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "%s", ""))) {
  } else if (OB_SUCC(ret)) {
    buf[pos++] = 0;
    stmt.assign_ptr(buf, static_cast<int32_t>(pos - 1));
  }
  return ret;
}

static int ai_column_build_delete_tasks_sql(common::ObIAllocator &alloc,
                                            const uint64_t table_id,
                                            const char *predicate,
                                            const int64_t predicate_len,
                                            ObString &stmt)
{
  int ret = OB_SUCCESS;
  char *buf = NULL;
  int64_t buf_len = 0;
  int64_t pos = 0;
  if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc,
             "delete from oceanbase.__all_ai_column_task "
             "where table_id = %lu and rowkey_predicate = ", table_id))) {
  } else if (OB_FAIL(ai_column_append_sql_string(alloc, buf, buf_len, pos,
                                                 predicate, predicate_len))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, alloc, "%s", ""))) {
  } else {
    buf[pos++] = 0;
    stmt.assign_ptr(buf, static_cast<int32_t>(pos - 1));
  }
  return ret;
}

static int ai_column_handle_row(ObTableModifyOp &op,
                                const ObDMLBaseCtDef &dml_ctdef,
                                ObDMLBaseRtDef &dml_rtdef,
                                const ObDmlEventType dml_event)
{
  int ret = OB_SUCCESS;
  const ObDmlEventType t_insert = ObDmlEventType::DE_INSERTING;
  const ObDmlEventType t_update = ObDmlEventType::DE_UPDATING;
  const ObDmlEventType t_delete = ObDmlEventType::DE_DELETING;
  if (dml_event != t_insert && dml_event != t_update && dml_event != t_delete) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai: unexpected dml event", K(ret), K(dml_event));
  } else if (t_update == dml_event && !reinterpret_cast<ObUpdRtDef &>(dml_rtdef).is_row_changed_) {
    // no column of the row is changed
  } else if (OB_FAIL(op.get_exec_ctx().check_status())) {
    LOG_WARN("ai: check exec ctx status failed", K(ret));
  } else {
    const uint64_t data_table_id = dml_ctdef.das_base_ctdef_.index_tid_;
    ObSQLCtx *sql_ctx = op.get_exec_ctx().get_sql_ctx();
    ObSchemaGetterGuard *schema_guard = NULL == sql_ctx ? NULL : sql_ctx->schema_guard_;
    const ObTableSchema *table_schema = NULL;
    const ObDatabaseSchema *database_schema = NULL;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: schema guard is null", K(ret));
    } else if (OB_FAIL(schema_guard->get_table_schema( data_table_id, table_schema))) {
      LOG_WARN("ai: get table schema failed", K(ret), K(data_table_id));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: table schema is null", K(ret), K(data_table_id));
    } else if (OB_FAIL(schema_guard->get_database_schema(table_schema->get_database_id(),
                                                         database_schema))) {
      LOG_WARN("ai: get database schema failed", K(ret));
    } else if (OB_ISNULL(database_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: database schema is null", K(ret));
    } else {
      ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
      const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
      const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
      const ObString database_name = database_schema->get_database_name_str();
      const ObString table_name = table_schema->get_table_name_str();
      ObSEArray<const ObColumnSchemaV2 *, 4> invalidated_columns;
      char *old_predicate = NULL;
      int64_t old_predicate_len = 0;
      int64_t old_predicate_pos = 0;
      char *new_predicate = NULL;
      int64_t new_predicate_len = 0;
      int64_t new_predicate_pos = 0;
      bool delete_row_tasks = false;
      bool invalidate_columns = false;
      if (t_delete == dml_event) {
        delete_row_tasks = true;
        if (OB_FAIL(ai_column_build_rowkey_predicate(op, *table_schema, dml_ctdef, old_row, alloc,
                                                     old_predicate, old_predicate_len,
                                                     old_predicate_pos))) {
        }
      } else if (t_insert == dml_event) {
        invalidate_columns = true;
        if (OB_FAIL(ai_column_build_rowkey_predicate(op, *table_schema, dml_ctdef, new_row, alloc,
                                                     new_predicate, new_predicate_len,
                                                     new_predicate_pos))) {
        } else if (OB_FAIL(ai_column_collect_all_ai_columns(*table_schema, invalidated_columns))) {
        }
      } else {
        bool rowkey_changed = false;
        if (OB_FAIL(ai_column_check_rowkey_changed(op, *table_schema, dml_ctdef,
                                                   old_row, new_row, rowkey_changed))) {
        } else if (rowkey_changed) {
          // the row moves to a new rowkey: tasks of the old row are removed and
          // the ai columns of the new row are invalidated
          delete_row_tasks = true;
          invalidate_columns = true;
          if (OB_FAIL(ai_column_build_rowkey_predicate(op, *table_schema, dml_ctdef, old_row, alloc,
                                                       old_predicate, old_predicate_len,
                                                       old_predicate_pos))) {
          } else if (OB_FAIL(ai_column_build_rowkey_predicate(op, *table_schema, dml_ctdef, new_row, alloc,
                                                              new_predicate, new_predicate_len,
                                                              new_predicate_pos))) {
          } else if (OB_FAIL(ai_column_collect_all_ai_columns(*table_schema, invalidated_columns))) {
          }
        } else if (OB_FAIL(ai_column_collect_changed_ai_columns(op, *table_schema, dml_ctdef,
                                                                old_row, new_row,
                                                                invalidated_columns))) {
        } else if (invalidated_columns.count() > 0) {
          invalidate_columns = true;
          if (OB_FAIL(ai_column_build_rowkey_predicate(op, *table_schema, dml_ctdef, new_row, alloc,
                                                       new_predicate, new_predicate_len,
                                                       new_predicate_pos))) {
          }
        }
      }
      if (OB_SUCC(ret) && (delete_row_tasks || invalidate_columns)) {
        ObSEArray<ObString, 8> statements;
        const int64_t request_id = invalidate_columns ? ObDMLService::next_ai_column_request_id() : 0;
        ObString stmt;
        if (delete_row_tasks) {
          if (OB_FAIL(ai_column_build_delete_tasks_sql(alloc, table_schema->get_table_id(),
                                                       old_predicate, old_predicate_pos - 1,
                                                       stmt))) {
            LOG_WARN("ai: build delete task sql failed", K(ret));
          } else if (OB_FAIL(statements.push_back(stmt))) {
            LOG_WARN("ai: push back statement failed", K(ret));
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && invalidate_columns
             && i < invalidated_columns.count(); ++i) {
          const ObColumnSchemaV2 *ai_column = invalidated_columns.at(i);
          if (OB_ISNULL(ai_column)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai: ai column schema is null", K(ret));
          } else if (OB_FAIL(ai_column_build_invalidate_sql(alloc, *ai_column, *table_schema,
                                                             database_name, table_name,
                                                             new_predicate, new_predicate_pos - 1,
                                                             request_id, stmt))) {
            LOG_WARN("ai: build invalidate sql failed", K(ret));
          } else if (OB_FAIL(statements.push_back(stmt))) {
            LOG_WARN("ai: push back statement failed", K(ret));
          } else if (OB_FAIL(ai_column_build_task_sql(alloc, *ai_column,
                                                      table_schema->get_table_id(),
                                                      new_predicate, new_predicate_pos - 1,
                                                      request_id, stmt))) {
            LOG_WARN("ai: build task sql failed", K(ret));
          } else if (OB_FAIL(statements.push_back(stmt))) {
            LOG_WARN("ai: push back statement failed", K(ret));
          }
        }
        if (OB_SUCC(ret) && statements.count() > 0) {
          if (OB_FAIL(op.begin_nested_session(false))) {
            LOG_WARN("ai: begin nested session failed", K(ret));
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < statements.count(); ++i) {
              if (OB_FAIL(op.execute_write(statements.at(i).ptr()))) {
                LOG_WARN("ai: execute ai column sql failed", K(ret), "sql", statements.at(i));
              }
            }
            int end_ret = op.end_nested_session();
            if (OB_SUCCESS != end_ret) {
              LOG_WARN("ai: end nested session failed", K(end_ret));
              if (OB_SUCCESS == ret) {
                ret = end_ret;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////
// AI column background worker.
//
// The worker polls __all_ai_column_task for due tasks and groups them
// by request_id, so all ai columns invalidated by one statement become
// visible atomically. The ai value is produced by executing the
// normalized ai expression in an inner sql update guarded by the
// request id, which keeps stale results from overwriting newer ones.

static const int64_t AI_COLUMN_TASK_FETCH_LIMIT = 1024;
static const int64_t AI_COLUMN_TASK_RETRY_DELAY_US = 1000L * 1000L;

struct AiColumnTaskItem
{
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t request_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  ObString rowkey_predicate_;
  ObString column_name_;
  ObString database_name_;
  ObString table_name_;
  ObString normalized_expr_;
};

static int ai_column_task_get_int(common::sqlclient::ObMySQLResult *result,
                                  const int64_t idx,
                                  int64_t &val)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  val = 0;
  if (OB_ISNULL(result)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(result->get_obj(idx, obj))) {
    LOG_WARN("ai: get task column failed", K(ret), K(idx));
  } else if (obj.is_null()) {
    val = 0;
  } else if (obj.is_integer_type()) {
    val = obj.get_int();
  } else if (obj.is_number()) {
    number::ObNumber nmb;
    if (OB_FAIL(obj.get_number(nmb))) {
    } else if (OB_FAIL(nmb.cast_to_int64(val))) {
    }
  } else {
    ret = OB_OBJ_TYPE_ERROR;
    LOG_WARN("ai: unexpected task column type", K(ret), K(idx), K(obj.get_type()));
  }
  return ret;
}

static int ai_column_task_get_string(common::ObIAllocator &alloc,
                                     common::sqlclient::ObMySQLResult *result,
                                     const int64_t idx,
                                     ObString &val)
{
  int ret = OB_SUCCESS;
  ObObj obj;
  val.reset();
  if (OB_ISNULL(result)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(result->get_obj(idx, obj))) {
    LOG_WARN("ai: get task column failed", K(ret), K(idx));
  } else if (obj.is_null()) {
    // keep empty
  } else if (obj.is_string_type()) {
    if (OB_FAIL(ob_write_string(alloc, obj.get_string(), val))) {
      LOG_WARN("ai: copy task column failed", K(ret), K(idx));
    }
  } else {
    ret = OB_OBJ_TYPE_ERROR;
    LOG_WARN("ai: unexpected task column type", K(ret), K(idx), K(obj.get_type()));
  }
  return ret;
}

static int ai_column_fetch_tasks(common::ObISQLClient &sql_proxy,
                                 common::ObIAllocator &alloc,
                                 common::ObIArray<AiColumnTaskItem> &items)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt(
        "select task.table_id, task.column_id, task.request_id, task.retry_count, "
        "task.rowkey_predicate, aicol.column_name, aicol.max_retries, "
        "ifnull(task.normalized_expr, aicol.normalized_expr), d.database_name, t.table_name "
        "from oceanbase.__all_ai_column_task task "
        "join oceanbase.__all_table t on t.table_id = task.table_id "
        "join oceanbase.__all_database d on d.database_id = t.database_id "
        "join oceanbase.__all_ai_column aicol on aicol.table_id = task.table_id "
        "and aicol.column_id = task.column_id "
        "where task.state in (%ld, %ld) and task.eligible_ts <= now(6) "
        "order by task.request_id, task.table_id, task.column_id limit %ld",
        share::schema::AI_COLUMN_STATE_PENDING,
        share::schema::AI_COLUMN_STATE_FAILED,
        AI_COLUMN_TASK_FETCH_LIMIT))) {
    LOG_WARN("ai: build fetch task sql failed", K(ret));
  } else {
    ObISQLClient::ReadResult res;
    common::sqlclient::ObMySQLResult *result = NULL;
    if (OB_FAIL(sql_proxy.read(res, sql.ptr()))) {
      LOG_WARN("ai: read ai column tasks failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai: task result is null", K(ret));
    } else {
      while (OB_SUCC(ret) && OB_SUCC(result->next())) {
        AiColumnTaskItem item;
        item.table_id_ = OB_INVALID_ID;
        item.column_id_ = OB_INVALID_ID;
        item.request_id_ = 0;
        item.retry_count_ = 0;
        item.max_retries_ = 0;
        int64_t table_id = 0;
        int64_t column_id = 0;
        if (OB_FAIL(ai_column_task_get_int(result, 0, table_id))) {
        } else if (OB_FAIL(ai_column_task_get_int(result, 1, column_id))) {
        } else if (OB_FAIL(ai_column_task_get_int(result, 2, item.request_id_))) {
        } else if (OB_FAIL(ai_column_task_get_int(result, 3, item.retry_count_))) {
        } else if (OB_FAIL(ai_column_task_get_string(alloc, result, 4, item.rowkey_predicate_))) {
        } else if (OB_FAIL(ai_column_task_get_string(alloc, result, 5, item.column_name_))) {
        } else if (OB_FAIL(ai_column_task_get_int(result, 6, item.max_retries_))) {
        } else if (OB_FAIL(ai_column_task_get_string(alloc, result, 7, item.normalized_expr_))) {
        } else if (OB_FAIL(ai_column_task_get_string(alloc, result, 8, item.database_name_))) {
        } else if (OB_FAIL(ai_column_task_get_string(alloc, result, 9, item.table_name_))) {
        } else {
          item.table_id_ = static_cast<uint64_t>(table_id);
          item.column_id_ = static_cast<uint64_t>(column_id);
          if (OB_FAIL(items.push_back(item))) {
            LOG_WARN("ai: push back task item failed", K(ret));
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

static int ai_column_build_worker_update_sql(const AiColumnTaskItem &item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(item.normalized_expr_.empty())) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("ai: normalized expr of ai column is empty", K(ret),
             K(item.table_id_), K(item.column_id_));
  } else if (OB_FAIL(sql.append_fmt(
        "update `%.*s`.`%.*s` set `%.*s` = %.*s, `%s%.*s` = %ld where %.*s and `%s%.*s` = %ld",
        item.database_name_.length(), item.database_name_.ptr(),
        item.table_name_.length(), item.table_name_.ptr(),
        item.column_name_.length(), item.column_name_.ptr(),
        item.normalized_expr_.length(), item.normalized_expr_.ptr(),
        share::schema::AI_COLUMN_STATE_COLUMN_PREFIX,
        item.column_name_.length(), item.column_name_.ptr(),
        share::schema::AI_COLUMN_STATE_READY,
        item.rowkey_predicate_.length(), item.rowkey_predicate_.ptr(),
        share::schema::AI_COLUMN_REQUEST_COLUMN_PREFIX,
        item.column_name_.length(), item.column_name_.ptr(),
        item.request_id_))) {
    LOG_WARN("ai: build worker update sql failed", K(ret));
  }
  return ret;
}

static int ai_column_build_worker_failed_state_sql(const AiColumnTaskItem &item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(
        "update `%.*s`.`%.*s` set `%s%.*s` = %ld where %.*s and `%s%.*s` = %ld",
        item.database_name_.length(), item.database_name_.ptr(),
        item.table_name_.length(), item.table_name_.ptr(),
        share::schema::AI_COLUMN_STATE_COLUMN_PREFIX,
        item.column_name_.length(), item.column_name_.ptr(),
        share::schema::AI_COLUMN_STATE_FAILED,
        item.rowkey_predicate_.length(), item.rowkey_predicate_.ptr(),
        share::schema::AI_COLUMN_REQUEST_COLUMN_PREFIX,
        item.column_name_.length(), item.column_name_.ptr(),
        item.request_id_))) {
    LOG_WARN("ai: build worker failed state sql failed", K(ret));
  }
  return ret;
}

static int ai_column_build_worker_task_where(const AiColumnTaskItem &item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(
        "where table_id = %lu and column_id = %lu and rowkey_predicate = '",
        item.table_id_, item.column_id_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < item.rowkey_predicate_.length(); ++i) {
    if ('\'' == item.rowkey_predicate_[i]) {
      if (OB_FAIL(sql.append("''"))) {
      }
    } else if (OB_FAIL(sql.append(item.rowkey_predicate_.ptr() + i, 1))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("' and request_id = %ld", item.request_id_))) {
  }
  return ret;
}

static int ai_column_build_worker_task_retry_sql(const AiColumnTaskItem &item,
                                                 const int64_t error_code,
                                                 ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append_fmt(
        "update oceanbase.__all_ai_column_task set state = %ld, "
        "retry_count = retry_count + 1, error_code = %ld, "
        "eligible_ts = now(6) + interval %ld microsecond ",
        share::schema::AI_COLUMN_STATE_FAILED, error_code,
        AI_COLUMN_TASK_RETRY_DELAY_US))) {
  } else if (OB_FAIL(ai_column_build_worker_task_where(item, sql))) {
  }
  return ret;
}

static int ai_column_build_worker_task_delete_sql(const AiColumnTaskItem &item, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("delete from oceanbase.__all_ai_column_task "))) {
  } else if (OB_FAIL(ai_column_build_worker_task_where(item, sql))) {
  }
  return ret;
}

static int ai_column_worker_handle_group_failure(common::ObISQLClient &sql_proxy,
                                                 const common::ObIArray<AiColumnTaskItem> &items,
                                                 const int64_t start,
                                                 const int64_t end,
                                                 const int64_t error_code)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  if (OB_FAIL(trans.start(&sql_proxy))) {
    LOG_WARN("ai: start failure transaction failed", K(ret));
  } else {
    for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
      const AiColumnTaskItem &item = items.at(i);
      int64_t affected_rows = 0;
      ObSqlString state_sql;
      if (OB_FAIL(ai_column_build_worker_failed_state_sql(item, state_sql))) {
      } else if (OB_FAIL(trans.write(state_sql.ptr(), affected_rows))) {
        LOG_WARN("ai: mark ai column failed failed", K(ret), K(state_sql));
      } else if (item.retry_count_ < item.max_retries_) {
        ObSqlString retry_sql;
        if (OB_FAIL(ai_column_build_worker_task_retry_sql(item, error_code, retry_sql))) {
        } else if (OB_FAIL(trans.write(retry_sql.ptr(), affected_rows))) {
          LOG_WARN("ai: retry ai column task failed", K(ret), K(retry_sql));
        }
      } else {
        ObSqlString delete_sql;
        if (OB_FAIL(ai_column_build_worker_task_delete_sql(item, delete_sql))) {
        } else if (OB_FAIL(trans.write(delete_sql.ptr(), affected_rows))) {
          LOG_WARN("ai: remove terminal ai column task failed", K(ret), K(delete_sql));
        }
      }
    }
    int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret) && OB_FAIL(end_ret)) {
      ret = end_ret;
    }
  }
  return ret;
}

static int ai_column_worker_run_group(common::ObISQLClient &sql_proxy,
                                      const common::ObIArray<AiColumnTaskItem> &items,
                                      const int64_t start,
                                      const int64_t end)
{
  int ret = OB_SUCCESS;
  int64_t compute_ret = OB_SUCCESS;
  int64_t end_ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  if (OB_UNLIKELY(start < 0 || end > items.count() || start >= end)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("ai: invalid task group", K(ret), K(start), K(end), K(items.count()));
  } else if (OB_FAIL(trans.start(&sql_proxy))) {
    LOG_WARN("ai: start worker transaction failed", K(ret));
  } else {
    for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
      ObSqlString update_sql;
      int64_t affected_rows = 0;
      if (OB_FAIL(ai_column_build_worker_update_sql(items.at(i), update_sql))) {
        compute_ret = ret;
      } else if (OB_FAIL(trans.write(update_sql.ptr(), affected_rows))) {
        LOG_WARN("ai: compute ai column value failed", K(ret), K(update_sql));
        compute_ret = ret;
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
        ObSqlString delete_sql;
        int64_t affected_rows = 0;
        if (OB_FAIL(ai_column_build_worker_task_delete_sql(items.at(i), delete_sql))) {
        } else if (OB_FAIL(trans.write(delete_sql.ptr(), affected_rows))) {
          LOG_WARN("ai: remove finished task failed", K(ret), K(delete_sql));
        }
      }
    }
    end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret) && OB_FAIL(end_ret)) {
      ret = end_ret;
    }
    if (OB_FAIL(ret)) {
      // the failed transaction is rolled back, move the tasks to the
      // failed state in a separate transaction
      const int64_t error_code = OB_SUCCESS == compute_ret ? ret : compute_ret;
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = ai_column_worker_handle_group_failure(
                           sql_proxy, items, start, end, error_code))) {
        LOG_WARN("ai: handle ai column task failure failed", K(tmp_ret), K(error_code));
      }
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ObDMLService::run_ai_column_worker(common::ObISQLClient &sql_proxy)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR, OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<AiColumnTaskItem, 32> items;
  if (OB_FAIL(ai_column_fetch_tasks(sql_proxy, alloc, items))) {
    LOG_WARN("ai: fetch ai column tasks failed", K(ret));
  }
  int64_t start = 0;
  while (OB_SUCC(ret) && start < items.count()) {
    int64_t end = start + 1;
    while (end < items.count()
           && items.at(end).request_id_ == items.at(start).request_id_
           && items.at(end).table_id_ == items.at(start).table_id_) {
      ++end;
    }
    // the fetch may cut a request group in half, leave it to the next round
    if (end == items.count() && items.count() >= AI_COLUMN_TASK_FETCH_LIMIT) {
      LOG_INFO("ai: skip possibly partial task group", K(items.count()), K(start));
      break;
    }
    if (OB_FAIL(ai_column_worker_run_group(sql_proxy, items, start, end))) {
      LOG_WARN("ai: run ai column task group failed", K(ret), K(start), K(end));
    }
    start = end;
  }
  return ret;
}

int ObDMLService::handle_after_processing_multi_row(ObDMLModifyRowsList *dml_modify_rows,
                                                    ObTableModifyOp *dml_op)
{
  int ret = OB_SUCCESS;
  bool is_iter_end = true;
  const ObDmlEventType t_insert = ObDmlEventType::DE_INSERTING;
  const ObDmlEventType t_update = ObDmlEventType::DE_UPDATING;
  const ObDmlEventType t_delete = ObDmlEventType::DE_DELETING;
  if (OB_ISNULL(dml_modify_rows) || OB_ISNULL(dml_op) || OB_ISNULL(dml_op->get_child())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(is_iter_end = dml_op->iter_end_)) {
  } else if (!is_iter_end && OB_ISNULL(dml_op->last_store_row_.get_store_row()) &&
    OB_FAIL(dml_op->last_store_row_.init(dml_op->get_exec_ctx().get_allocator(), dml_op->get_child()->get_spec().output_.count()))) {
  } else if (!is_iter_end && OB_FAIL(dml_op->last_store_row_.shadow_copy(dml_op->get_child()->get_spec().output_, dml_op->get_eval_ctx()))) {
    LOG_ERROR("failed to backup the datum ptr of child operator", K(ret));
  } else {
    ObDMLModifyRowsList::iterator row_iter = dml_modify_rows->begin();
    for (; OB_SUCC(ret) && row_iter != dml_modify_rows->end(); row_iter++) {
      ObDMLModifyRowNode &modify_row = *row_iter;
      if (OB_ISNULL(modify_row.full_row_) && OB_ISNULL(modify_row.new_row_) && OB_ISNULL(modify_row.old_row_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(modify_row.dml_op_) || OB_ISNULL(modify_row.dml_ctdef_) || OB_ISNULL(modify_row.dml_rtdef_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObTableModifyOp &op = *modify_row.dml_op_;
        const ObDMLBaseCtDef &dml_ctdef = *modify_row.dml_ctdef_;
        ObDMLBaseRtDef &dml_rtdef = *modify_row.dml_rtdef_;
        const ObDmlEventType dml_event = modify_row.dml_event_;
        // process foreign key
        if (OB_NOT_NULL(modify_row.full_row_) && OB_FAIL(modify_row.full_row_->to_expr(modify_row.dml_ctdef_->full_row_, op.get_eval_ctx()))) {
        } else if (OB_NOT_NULL(modify_row.old_row_) && OB_FAIL(modify_row.old_row_->to_expr(dml_ctdef.old_row_, op.get_eval_ctx()))) {
        } else if (OB_NOT_NULL(modify_row.new_row_) && OB_FAIL(modify_row.new_row_->to_expr(dml_ctdef.new_row_, op.get_eval_ctx()))) {
        } else if (op.need_foreign_key_checks()) {
          if (t_update == dml_event && !reinterpret_cast<ObUpdRtDef &>(dml_rtdef).is_row_changed_) {
          } else {
            // if check foreign key in batch, build fk check tasks here
            if (dml_op->get_spec().check_fk_batch_) {
              if (OB_FAIL(build_batch_fk_check_tasks(dml_ctdef, dml_rtdef))) {
              }
            } else if (OB_FAIL(ForeignKeyHandle::do_handle(op, dml_ctdef, dml_rtdef))) {
            }
          }
        }
        // process after row trigger
        if (OB_SUCC(ret) && dml_ctdef.trig_ctdef_.all_tm_points_.has_after_row()) {
          ObEvalCtx &eval_ctx = op.get_eval_ctx();
          if (dml_event != t_insert && dml_event != t_update && dml_event != t_delete) {
            ret = OB_ERR_UNEXPECTED;
          } else if (t_insert == dml_event && OB_FAIL(TriggerHandle::init_param_new_row(
              eval_ctx, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_))) {
          } else if (t_delete == dml_event && OB_FAIL(TriggerHandle::init_param_old_row(
              eval_ctx, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_))) {
          } else if (t_update == dml_event && OB_FAIL(TriggerHandle::init_param_rows(
              eval_ctx, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_))) {
          } else if (OB_FAIL(TriggerHandle::do_handle_after_row(op, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_, dml_event))) {
          }
        }
        // process ai columns
        if (OB_SUCC(ret) && op.need_ai_column_process(dml_ctdef)
            && OB_FAIL(ai_column_handle_row(op, dml_ctdef, dml_rtdef, dml_event))) {
          LOG_WARN("ai: process ai column failed", K(ret));
        }
      }
    }

    // check the result of batch foreign key check results
    if (OB_SUCC(ret)) {
      if (dml_op->get_spec().check_fk_batch_ && OB_FAIL(dml_op->perform_batch_fk_check())) {
      } else if (OB_FAIL(!is_iter_end && dml_op->last_store_row_.restore(dml_op->get_child()->get_spec().output_, dml_op->get_eval_ctx()))) {
      }
    }
  }
  return ret;
}

int ObDMLService::handle_after_processing_single_row(ObDMLModifyRowNode &modify_row)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(modify_row.full_row_) && OB_ISNULL(modify_row.new_row_) && OB_ISNULL(modify_row.old_row_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(modify_row.dml_op_) || OB_ISNULL(modify_row.dml_ctdef_) || OB_ISNULL(modify_row.dml_rtdef_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObTableModifyOp &op = *modify_row.dml_op_;
    const ObDMLBaseCtDef &dml_ctdef = *modify_row.dml_ctdef_;
    ObDMLBaseRtDef &dml_rtdef = *modify_row.dml_rtdef_;
    const ObDmlEventType t_insert = ObDmlEventType::DE_INSERTING;
    const ObDmlEventType t_update = ObDmlEventType::DE_UPDATING;
    const ObDmlEventType t_delete = ObDmlEventType::DE_DELETING;
    const ObDmlEventType dml_event = modify_row.dml_event_;
    if (dml_event != t_insert && dml_event != t_update && dml_event != t_delete) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      if (t_update == dml_event) {
        // for update op, Foreign key checks need to be performed only if the value has changed
        if (reinterpret_cast<ObUpdRtDef &>(dml_rtdef).is_row_changed_ && OB_FAIL(ForeignKeyHandle::do_handle(op, dml_ctdef, dml_rtdef))) {
        }
      } else if (OB_FAIL(ForeignKeyHandle::do_handle(op, dml_ctdef, dml_rtdef))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(TriggerHandle::do_handle_after_row(op, dml_ctdef.trig_ctdef_, dml_rtdef.trig_rtdef_, dml_event))) {
    }
    // process ai columns
    if (OB_SUCC(ret) && op.need_ai_column_process(dml_ctdef)
        && OB_FAIL(ai_column_handle_row(op, dml_ctdef, dml_rtdef, dml_event))) {
      LOG_WARN("ai: process ai column failed", K(ret));
    }
  }
  return ret;
}

int ObDMLService::handle_after_processing_single_row(ObDMLModifyRowsList *dml_modify_rows)
{
  int ret = OB_SUCCESS;
  // for single-row processing, the expr defined in ctdef and trig parameters haven't been refreshed
  // case1: only one row added to the list of dml_modify_rows;
  // case2: for multi table dml stmt, each table adds a row to the list;
  // In these cases, there is no need to re-convert the rows to be modified into an expression and init trig parameters;
  ObDMLModifyRowsList::iterator row_iter = dml_modify_rows->begin();
  for (; OB_SUCC(ret) && row_iter != dml_modify_rows->end(); row_iter++) {
    ObDMLModifyRowNode &modify_row = *row_iter;
    if (OB_FAIL(handle_after_processing_single_row(modify_row))) {
    }
  }
  return ret;
}

int ObDMLService::handle_after_row_processing(ObTableModifyOp *op,
                                              ObDMLModifyRowsList *dml_modify_rows)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(op) || OB_ISNULL(dml_modify_rows)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (1 > dml_modify_rows->size()) {
    // after row processing list is empty, nothing to do
    #ifndef NDEBUG
      LOG_INFO("No row need to perform foreign key check or after row trigger");
    #endif
  } else if (op->execute_single_row_) {
    ret = handle_after_processing_single_row(dml_modify_rows);
  } else {
    ret = handle_after_processing_multi_row(dml_modify_rows, op);
  }
  return ret;
}

int ObDMLService::build_batch_fk_check_tasks(const ObDMLBaseCtDef &dml_ctdef,
                                             ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (dml_rtdef.fk_checker_array_.count() != dml_ctdef.fk_args_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int i = 0; OB_SUCC(ret) && i < dml_rtdef.fk_checker_array_.count(); ++i) {
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      bool need_check = true;
      if (OB_ISNULL(fk_checker)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!fk_arg.use_das_scan_) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(fk_checker->build_fk_check_das_task(fk_arg.columns_, dml_ctdef.new_row_, need_check))) {
      }
    }
  }
  return ret;
}

int ObDMLService::log_user_error_inner(
    int ret,
    int64_t row_num,
    common::ObString &column_name,
    ObExecContext &ctx,
    const ObObjType column_type)
{
  if (OB_DATA_OUT_OF_RANGE == ret) {
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_DATA_OUT_OF_RANGE, column_name.length(), column_name.ptr(), row_num);
  } else if (OB_ERR_DATA_TOO_LONG == ret) {
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_DATA_TOO_LONG, column_name.length(), column_name.ptr(), row_num);
  } else if (OB_ERR_VALUE_LARGER_THAN_ALLOWED == ret) {
    LOG_USER_ERROR(OB_ERR_VALUE_LARGER_THAN_ALLOWED);
  } else if (OB_INVALID_DATE_VALUE == ret || OB_INVALID_DATE_FORMAT == ret) {
    ret = OB_INVALID_DATE_VALUE;
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(
        OB_ERR_INVALID_DATE_MSG_FMT_V2,
        column_name.length(),
        column_name.ptr(),
        row_num);
  } else if (OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD == ret || OB_INVALID_NUMERIC == ret) {
    ret = OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD;
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    ObString decimal_type_str("decimal");
    ObString default_type_str("integer");
    ObString &type_str = ob_is_number_or_decimal_int_tc(column_type) ?
      decimal_type_str : default_type_str;
    LOG_USER_ERROR(
        OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD,
        type_str.length(),
        type_str.ptr(),
        column_name.length(),
        column_name.ptr(),
        row_num);
  } else if (OB_ERR_WARN_DATA_OUT_OF_RANGE == ret) {
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_WARN_DATA_OUT_OF_RANGE, column_name.length(), column_name.ptr(), row_num);
  } else if (OB_ERR_DATA_TRUNCATED == ret || OB_ERR_DOUBLE_TRUNCATED == ret) {
    ret = OB_ERR_DATA_TRUNCATED;
    ob_reset_tsi_warning_buffer();
    ObSQLUtils::copy_and_convert_string_charset(
        ctx.get_allocator(),
        column_name,
        column_name,
        CS_TYPE_UTF8MB4_BIN,
        ctx.get_my_session()->get_local_collation_connection());
    LOG_USER_ERROR(OB_ERR_DATA_TRUNCATED, column_name.length(), column_name.ptr(), row_num);
  } else {
  }
  return ret;
}
// Find the first ancestor exec context that initiates fk cascading.
// The exec_ctx can be used to create hash set to perform duplicate rowkey check,
// which is used to avoid delete or update same row mutiple times
int ObDMLService::get_root_exec_ctx_for_fk_cascading(ObExecContext *ctx, ObExecContext* &needed_ctx)
{
  int ret = OB_SUCCESS;
  ObExecContext *parent_ctx = nullptr;
  needed_ctx = nullptr;
  if (OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(parent_ctx = ctx->get_parent_ctx())) {
    // case1: current ctx is the root ctx, means current stmt is the root stmt,
    // create hash set at current ctx to perform duplicate rowkey check
    needed_ctx = ctx;
  } else if (!(parent_ctx->get_das_ctx().is_fk_cascading_)) {
    // case2: current stmt is not the root stmt, is not triggered by foreign key cascade operations
    // may be trigger or PL instead, create hash set at current ctx to perform duplicate rowkey check
    needed_ctx = ctx;
  } else if (OB_FAIL(SMART_CALL(get_root_exec_ctx_for_fk_cascading(parent_ctx, needed_ctx)))) {
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
