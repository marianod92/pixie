#include "src/carnot/compiler/objects/dataframe.h"
#include "src/carnot/compiler/ir/ast_utils.h"
#include "src/carnot/compiler/objects/metadata_object.h"
#include "src/carnot/compiler/objects/none_object.h"

namespace pl {
namespace carnot {
namespace compiler {
Dataframe::Dataframe(OperatorIR* op) : QLObject(DataframeType, op), op_(op) {
  CHECK(op != nullptr) << "Bad argument in Dataframe constructor.";
  /**
   * # Equivalent to the python method method syntax:
   * def merge(self, right, how, left_on, right_on, suffixes=('_x', '_y')):
   *     ...
   */
  std::shared_ptr<FuncObject> mergefn(new FuncObject(
      kMergeOpId, {"right", "how", "left_on", "right_on", "suffixes"},
      {{"suffixes", "('_x', '_y')"}},
      /* has_variable_len_kwargs */ false,
      std::bind(&JoinHandler::Eval, this, std::placeholders::_1, std::placeholders::_2)));
  AddMethod(kMergeOpId, mergefn);

  /**
   * # Equivalent to the python method method syntax:
   * def agg(self, **kwargs):
   *     ...
   */
  std::shared_ptr<FuncObject> aggfn(new FuncObject(
      kBlockingAggOpId, {}, {},
      /* has_variable_len_kwargs */ true,
      std::bind(&AggHandler::Eval, this, std::placeholders::_1, std::placeholders::_2)));
  AddMethod(kBlockingAggOpId, aggfn);

  /**
   * # Equivalent to the python method method syntax:
   * def drop(self, fn):
   *     ...
   */
  std::shared_ptr<FuncObject> dropfn(new FuncObject(
      kDropOpId, {"columns"}, {}, /* has_kwargs */ false,
      std::bind(&DropHandler::Eval, this, std::placeholders::_1, std::placeholders::_2)));
  AddMethod(kDropOpId, dropfn);

  /**
   * # Equivalent to the python method method syntax:
   * def head(self, n=5):
   *     ...
   */
  std::shared_ptr<FuncObject> limitfn(new FuncObject(
      kLimitOpId, {"n"}, {{"n", "5"}}, /* has_variable_len_kwargs */ false,
      std::bind(&LimitHandler::Eval, this, std::placeholders::_1, std::placeholders::_2)));
  AddMethod(kLimitOpId, limitfn);

  /**
   *
   * # Equivalent to the python method method syntax:
   * def __getitem__(self, key):
   *     ...
   *
   * # It's important to note that this is added as a subscript method instead.
   */
  std::shared_ptr<FuncObject> subscript_fn(new FuncObject(
      kSubscriptMethodName, {"key"}, {},
      /* has_variable_len_kwargs */ false,
      std::bind(&SubscriptHandler::Eval, this, std::placeholders::_1, std::placeholders::_2)));
  AddSubscriptMethod(subscript_fn);

  std::shared_ptr<FuncObject> group_by_fn(new FuncObject(
      kGroupByOpId, {"by"}, {},
      /* has_variable_len_kwargs */ false,
      std::bind(&GroupByHandler::Eval, this, std::placeholders::_1, std::placeholders::_2)));
  AddMethod(kGroupByOpId, group_by_fn);

  attributes_.emplace(kMetadataAttrName);
}

StatusOr<QLObjectPtr> Dataframe::GetAttributeImpl(const pypa::AstPtr& ast,
                                                  const std::string& name) const {
  // If this gets to this point, should fail here.
  DCHECK(HasNonMethodAttribute(name));

  if (name == kMetadataAttrName) {
    return MetadataObject::Create(op());
  }

  // Shouldn't ever be hit, but will appear here anyways.
  return CreateAstError(ast, "'$0' object has no attribute '$1'", name);
}

StatusOr<QLObjectPtr> JoinHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                        const ParsedArgs& args) {
  // GetArg returns non-nullptr or errors out in Debug mode. No need
  // to check again.
  IRNode* right_node = args.GetArg("right");
  IRNode* how_node = args.GetArg("how");
  IRNode* left_on_node = args.GetArg("left_on");
  IRNode* right_on_node = args.GetArg("right_on");
  IRNode* suffixes_node = args.GetArg("suffixes");
  if (!Match(right_node, Operator())) {
    return right_node->CreateIRNodeError("'right' must be an operator, got $0",
                                         right_node->type_string());
  }
  OperatorIR* right = static_cast<OperatorIR*>(right_node);

  if (!Match(how_node, String())) {
    return how_node->CreateIRNodeError("'how' must be a string, got $0", how_node->type_string());
  }
  std::string how_type = static_cast<StringIR*>(how_node)->str();

  PL_ASSIGN_OR_RETURN(std::vector<ColumnIR*> left_on_cols, ProcessCols(left_on_node, "left_on", 0));
  PL_ASSIGN_OR_RETURN(std::vector<ColumnIR*> right_on_cols,
                      ProcessCols(right_on_node, "right_on", 1));

  // TODO(philkuz) consider using a struct instead of a vector because it's a fixed size.
  if (!Match(suffixes_node, CollectionWithChildren(String()))) {
    return suffixes_node->CreateIRNodeError(
        "'suffixes' must be a tuple with 2 strings for the left and right suffixes. Received $0",
        suffixes_node->type_string());
  }

  PL_ASSIGN_OR_RETURN(std::vector<std::string> suffix_strs,
                      ParseStringsFromCollection(static_cast<ListIR*>(suffixes_node)));
  if (suffix_strs.size() != 2) {
    return suffixes_node->CreateIRNodeError(
        "'suffixes' must be a tuple with 2 elements. Received $0", suffix_strs.size());
  }

  PL_ASSIGN_OR_RETURN(JoinIR * join_op, df->graph()->CreateNode<JoinIR>(
                                            ast, std::vector<OperatorIR*>{df->op(), right},
                                            how_type, left_on_cols, right_on_cols, suffix_strs));
  return StatusOr(std::make_shared<Dataframe>(join_op));
}

StatusOr<std::vector<ColumnIR*>> JoinHandler::ProcessCols(IRNode* node, std::string arg_name,
                                                          int64_t parent_index) {
  DCHECK(node != nullptr);
  IR* graph = node->graph_ptr();
  if (Match(node, ListWithChildren(String()))) {
    auto list = static_cast<ListIR*>(node);
    std::vector<ColumnIR*> columns(list->children().size());
    for (const auto& [idx, node] : Enumerate(list->children())) {
      StringIR* str = static_cast<StringIR*>(node);
      PL_ASSIGN_OR_RETURN(ColumnIR * col,
                          graph->CreateNode<ColumnIR>(str->ast_node(), str->str(), parent_index));
      columns[idx] = col;
    }
    return columns;
  } else if (!Match(node, String())) {
    return node->CreateIRNodeError("'$0' must be a label or a list of labels", arg_name);
  }
  StringIR* str = static_cast<StringIR*>(node);
  PL_ASSIGN_OR_RETURN(ColumnIR * col,
                      graph->CreateNode<ColumnIR>(str->ast_node(), str->str(), parent_index));
  return std::vector<ColumnIR*>{col};
}

StatusOr<QLObjectPtr> AggHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                       const ParsedArgs& args) {
  // converts the mapping of args.kwargs into ColExpressionvector
  ColExpressionVector aggregate_expressions;
  for (const auto& [name, expr] : args.kwargs()) {
    if (!Match(expr, Tuple())) {
      return expr->CreateIRNodeError("Expected '$0' kwarg argument to be a tuple, not $1",
                                     Dataframe::kBlockingAggOpId, expr->type_string());
    }
    PL_ASSIGN_OR_RETURN(FuncIR * parsed_expr,
                        ParseNameTuple(df->graph(), static_cast<TupleIR*>(expr)));
    aggregate_expressions.push_back({name, parsed_expr});
  }

  PL_ASSIGN_OR_RETURN(BlockingAggIR * agg_op,
                      df->graph()->CreateNode<BlockingAggIR>(
                          ast, df->op(), std::vector<ColumnIR*>{}, aggregate_expressions));
  return StatusOr(std::make_shared<Dataframe>(agg_op));
}

StatusOr<FuncIR*> AggHandler::ParseNameTuple(IR* ir, TupleIR* tuple) {
  DCHECK_EQ(tuple->children().size(), 2UL);
  IRNode* childone = tuple->children()[0];
  IRNode* childtwo = tuple->children()[1];
  if (!Match(childone, String())) {
    return childone->CreateIRNodeError("Expected 'str' for first tuple argument. Received '$0'",
                                       childone->type_string());
  }

  if (!Match(childtwo, Func())) {
    return childtwo->CreateIRNodeError("Expected 'func' for second tuple argument. Received '$0'",
                                       childtwo->type_string());
  }

  std::string argcol_name = static_cast<StringIR*>(childone)->str();
  FuncIR* func = static_cast<FuncIR*>(childtwo);
  // The function should be specified as a single function by itself.
  // This could change in the future.
  if (func->args().size() != 0) {
    return func->CreateIRNodeError("Unexpected aggregate function");
  }
  // parent_op_idx is 0 because we only have one parent for an aggregate.
  PL_ASSIGN_OR_RETURN(ColumnIR * argcol, ir->CreateNode<ColumnIR>(childone->ast_node(), argcol_name,
                                                                  /* parent_op_idx */ 0));
  PL_RETURN_IF_ERROR(func->AddArg(argcol));

  // Delete tuple id.
  PL_RETURN_IF_ERROR(ir->DeleteNode(tuple->id()));
  return func;
}

StatusOr<QLObjectPtr> DropHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                        const ParsedArgs& args) {
  IRNode* columns_arg = args.GetArg("columns");
  if (!Match(columns_arg, List())) {
    return columns_arg->CreateIRNodeError(
        "Expected '$0' kwarg argument 'columns' to be a list, not $1", Dataframe::kDropOpId,
        columns_arg->type_string());
  }
  ListIR* columns_list = static_cast<ListIR*>(columns_arg);
  PL_ASSIGN_OR_RETURN(std::vector<std::string> columns, ParseStringsFromCollection(columns_list));

  PL_ASSIGN_OR_RETURN(DropIR * drop_op, df->graph()->CreateNode<DropIR>(ast, df->op(), columns));
  PL_RETURN_IF_ERROR(df->graph()->DeleteNodeAndChildren(columns_list->id()));
  return StatusOr(std::make_shared<Dataframe>(drop_op));
}

StatusOr<QLObjectPtr> RangeHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                         const ParsedArgs& args) {
  IRNode* start_repr = args.GetArg("start");
  IRNode* stop_repr = args.GetArg("stop");
  if (!Match(start_repr, Expression())) {
    return start_repr->CreateIRNodeError("'start' must be an expression");
  }

  if (!Match(stop_repr, Expression())) {
    return stop_repr->CreateIRNodeError("'stop' must be an expression");
  }

  ExpressionIR* start_expr = static_cast<ExpressionIR*>(start_repr);
  ExpressionIR* stop_expr = static_cast<ExpressionIR*>(stop_repr);

  PL_ASSIGN_OR_RETURN(RangeIR * range_op,
                      df->graph()->CreateNode<RangeIR>(ast, df->op(), start_expr, stop_expr));
  return StatusOr(std::make_shared<Dataframe>(range_op));
}

StatusOr<QLObjectPtr> LimitHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                         const ParsedArgs& args) {
  // TODO(philkuz) (PL-1161) Add support for compile time evaluation of Limit argument.
  IRNode* rows_node = args.GetArg("n");
  if (!Match(rows_node, Int())) {
    return rows_node->CreateIRNodeError("'n' must be an int");
  }
  int64_t limit_value = static_cast<IntIR*>(rows_node)->val();

  PL_ASSIGN_OR_RETURN(LimitIR * limit_op,
                      df->graph()->CreateNode<LimitIR>(ast, df->op(), limit_value));
  // Delete the integer node.
  PL_RETURN_IF_ERROR(df->graph()->DeleteNode(rows_node->id()));
  return StatusOr(std::make_shared<Dataframe>(limit_op));
}

StatusOr<QLObjectPtr> SubscriptHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                             const ParsedArgs& args) {
  IRNode* key = args.GetArg("key");
  if (!key->IsExpression()) {
    return key->CreateIRNodeError("subscript argument must have an expression. '$0' not allowed",
                                  key->type_string());
  }
  if (Match(key, List())) {
    return EvalKeep(df, ast, static_cast<ListIR*>(key));
  }
  return EvalFilter(df, ast, static_cast<ExpressionIR*>(key));
}

StatusOr<QLObjectPtr> SubscriptHandler::EvalFilter(Dataframe* df, const pypa::AstPtr& ast,
                                                   ExpressionIR* expr) {
  PL_ASSIGN_OR_RETURN(FilterIR * filter_op, df->graph()->CreateNode<FilterIR>(ast, df->op(), expr));
  return StatusOr(std::make_shared<Dataframe>(filter_op));
}

StatusOr<QLObjectPtr> SubscriptHandler::EvalKeep(Dataframe* df, const pypa::AstPtr& ast,
                                                 ListIR* key) {
  PL_ASSIGN_OR_RETURN(std::vector<std::string> keep_column_names, ParseStringsFromCollection(key));

  ColExpressionVector keep_exprs;
  for (const auto& col_name : keep_column_names) {
    // parent_op_idx is 0 because we only have one parent for a map.
    PL_ASSIGN_OR_RETURN(ColumnIR * keep_col,
                        df->graph()->CreateNode<ColumnIR>(ast, col_name, /* parent_op_idx */ 0));
    keep_exprs.emplace_back(col_name, keep_col);
  }

  PL_ASSIGN_OR_RETURN(
      MapIR * map_op,
      df->graph()->CreateNode<MapIR>(ast, df->op(), keep_exprs, /* keep_input_columns */ false));
  return StatusOr(std::make_shared<Dataframe>(map_op));
}

StatusOr<QLObjectPtr> GroupByHandler::Eval(Dataframe* df, const pypa::AstPtr& ast,
                                           const ParsedArgs& args) {
  IRNode* by = args.GetArg("by");

  PL_ASSIGN_OR_RETURN(std::vector<ColumnIR*> groups, ParseByFunction(by));
  PL_ASSIGN_OR_RETURN(GroupByIR * group_by_op,
                      df->graph()->CreateNode<GroupByIR>(ast, df->op(), groups));
  return StatusOr(std::make_shared<Dataframe>(group_by_op));
}

StatusOr<std::vector<ColumnIR*>> GroupByHandler::ParseByFunction(IRNode* by) {
  if (!Match(by, ListWithChildren(String())) && !Match(by, String())) {
    return by->CreateIRNodeError("'by' expected string or list of strings");
  } else if (Match(by, String())) {
    PL_ASSIGN_OR_RETURN(ColumnIR * col,
                        by->graph_ptr()->CreateNode<ColumnIR>(
                            by->ast_node(), static_cast<StringIR*>(by)->str(), /* parent_idx */ 0));
    return std::vector<ColumnIR*>{col};
  }

  PL_ASSIGN_OR_RETURN(std::vector<std::string> column_names,
                      ParseStringsFromCollection(static_cast<ListIR*>(by)));
  std::vector<ColumnIR*> columns;
  for (const auto& col_name : column_names) {
    PL_ASSIGN_OR_RETURN(ColumnIR * col,
                        by->graph_ptr()->CreateNode<ColumnIR>(by->ast_node(), col_name,
                                                              /* parent_idx */ 0));
    columns.push_back(col);
  }
  return columns;
}

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
