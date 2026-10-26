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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    res.set_null();
  } else if (OB_NOT_NULL(arg_dim) && !arg_dim->is_null() && arg_dim->get_int() <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id;
    ObString content;
    int64_t dim = (OB_NOT_NULL(arg_dim) && !arg_dim->is_null()) ? arg_dim->get_int() : 0;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
                 *arg_model_id, expr.args_[MODEL_IDX]->datum_meta_,
                 expr.args_[MODEL_IDX]->obj_meta_.has_lob_header(), model_id))) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
                 *arg_content, expr.args_[CONTENT_IDX]->datum_meta_,
                 expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
    } else if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    } else if (OB_FAIL(build_embed_config(temp_allocator, dim, config))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObDatumVector res_datum = expr.locate_expr_datumvector(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  if (OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (expr.arg_cnt_ > DIM_IDX && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    common::ObArenaAllocator &tmp_allocator = tmp_alloc_g.get_allocator();
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObDatumVector model_array = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector content_array = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
    ObDatumVector dim_array = expr.arg_cnt_ > DIM_IDX
        ? expr.args_[DIM_IDX]->locate_expr_datumvector(ctx) : ObDatumVector();
    ObArray<int64_t> row_indexes;
    ObArray<ObString> row_models;
    ObArray<ObString> row_contents;
    ObArray<int64_t> row_dims;
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; ++j) {
      ObDatum *model_datum = nullptr;
      ObDatum *content_datum = nullptr;
      if (skip.at(j) || eval_flags.at(j)) {
        continue;
      }
      eval_flags.set(j);
      model_datum = model_array.at(j);
      content_datum = content_array.at(j);
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (model_datum->is_null()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
      } else if (content_datum->is_null()) {
        res_datum.at(j)->set_null();
        continue;
      }
      int64_t dim = 0;
      ObDatum *dim_datum = expr.arg_cnt_ > DIM_IDX ? dim_array.at(j) : nullptr;
      if (OB_FAIL(ret)) {
      } else if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
        dim = dim_datum->get_int();
        if (dim <= 0) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        }
      }
      ObString model_id;
      ObString content;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, tmp_allocator,
                   *model_datum, expr.args_[MODEL_IDX]->datum_meta_,
                   expr.args_[MODEL_IDX]->obj_meta_.has_lob_header(), model_id))) {
      } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, tmp_allocator,
                   *content_datum, expr.args_[CONTENT_IDX]->datum_meta_,
                   expr.args_[CONTENT_IDX]->obj_meta_.has_lob_header(), content))) {
      } else if (model_id.empty() || content.empty()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      } else if (OB_FAIL(row_indexes.push_back(j))
                 || OB_FAIL(row_models.push_back(model_id))
                 || OB_FAIL(row_contents.push_back(content))
                 || OB_FAIL(row_dims.push_back(dim))) {
      }
    }

    const int64_t row_count = row_indexes.count();
    ObArray<bool> consumed;
    if (OB_FAIL(ret)) {
    } else if (row_count > 0 && OB_FAIL(consumed.prepare_allocate(row_count))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < row_count; ++i) {
      if (consumed[i]) {
        continue;
      }
      consumed[i] = true;
      ObArray<int64_t> group_rows;
      ObArray<ObString> group_contents;
      if (OB_FAIL(group_rows.push_back(row_indexes[i]))
          || OB_FAIL(group_contents.push_back(row_contents[i]))) {
        break;
      }
      for (int64_t k = i + 1; OB_SUCC(ret) && k < row_count; ++k) {
        if (consumed[k]) {
          continue;
        }
        if (row_dims[k] != row_dims[i] || 0 != row_models[k].compare(row_models[i])) {
          continue;
        }
        consumed[k] = true;
        if (OB_FAIL(group_rows.push_back(row_indexes[k]))
            || OB_FAIL(group_contents.push_back(row_contents[k]))) {
        }
      }
      if (OB_FAIL(ret)) {
        break;
      }
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      ObArray<ObString> results;
      if (OB_FAIL(build_embed_config(tmp_allocator, row_dims[i], config))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(tmp_allocator, row_models[i], info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     row_models[i], tmp_allocator, resolved_endpoint))) {
      } else {
        ObAIFuncModel model(tmp_allocator, *info, resolved_endpoint);
        if (OB_FAIL(model.call_dense_embedding_vector_v2(group_contents, config, results))) {
        } else if (results.count() != group_contents.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t k = 0; OB_SUCC(ret) && k < group_rows.count(); ++k) {
            if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res_datum.at(group_rows[k]),
                                                         results[k], group_rows[k]))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::build_embed_config(common::ObIAllocator &allocator, const int64_t dim,
                                      common::ObJsonObject *&config)
{
  int ret = OB_SUCCESS;
  config = nullptr;
  if (dim > 0) {
    ObJsonInt *dim_json = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, dim, dim_json))) {
    } else if (OB_FAIL(config->add("dimensions", dim_json))) {
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
