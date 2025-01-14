/*
 * Copyright 2018- The Pixie Authors.
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
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "src/carnot/planner/objects/otel.h"
#include <absl/container/flat_hash_set.h>

#include <functional>
#include <utility>
#include <vector>

#include "src/carnot/planner/ir/otel_export_sink_ir.h"
#include "src/carnot/planner/objects/dataframe.h"
#include "src/carnot/planner/objects/dict_object.h"
#include "src/carnot/planner/objects/exporter.h"
#include "src/carnot/planner/objects/expr_object.h"
#include "src/carnot/planner/objects/funcobject.h"
#include "src/carnot/planner/objects/none_object.h"
#include "src/carnot/planpb/plan.pb.h"
#include "src/shared/types/typespb/types.pb.h"

namespace px {
namespace carnot {
namespace planner {
namespace compiler {

StatusOr<std::shared_ptr<OTelTraceModule>> OTelTraceModule::Create(ASTVisitor* ast_visitor) {
  auto trace_module = std::shared_ptr<OTelTraceModule>(new OTelTraceModule(ast_visitor));
  PL_RETURN_IF_ERROR(trace_module->Init());
  return trace_module;
}

StatusOr<std::shared_ptr<OTelMetricsModule>> OTelMetricsModule::Create(ASTVisitor* ast_visitor) {
  auto metrics_module = std::shared_ptr<OTelMetricsModule>(new OTelMetricsModule(ast_visitor));
  PL_RETURN_IF_ERROR(metrics_module->Init());
  return metrics_module;
}

StatusOr<std::shared_ptr<EndpointConfig>> EndpointConfig::Create(
    ASTVisitor* ast_visitor, std::string url,
    std::vector<EndpointConfig::ConnAttribute> attributes) {
  auto endpoint = std::shared_ptr<EndpointConfig>(
      new EndpointConfig(ast_visitor, std::move(url), std::move(attributes)));
  PL_RETURN_IF_ERROR(endpoint->Init());
  return endpoint;
}

Status ExportToOTel(planpb::OTelExportSinkOperator pb, std::vector<ExpectedColumn> columns,
                    const pypa::AstPtr& ast, Dataframe* df) {
  auto op = df->op();
  return op->graph()->CreateNode<OTelExportSinkIR>(ast, op, pb, columns).status();
}

StatusOr<std::string> GetArgAsString(const pypa::AstPtr& ast, const ParsedArgs& args,
                                     std::string_view arg_name) {
  PL_ASSIGN_OR_RETURN(StringIR * arg_ir, GetArgAs<StringIR>(ast, args, arg_name));
  return arg_ir->str();
}

Status ParseEndpointConfig(QLObjectPtr endpoint, planpb::OTelEndpointConfig* pb) {
  // TODO(philkuz) determine how to handle a default configuration based on the plugin.
  if (endpoint->type() != EndpointConfig::EndpointType.type()) {
    return endpoint->CreateError("expected Endpoint type for 'endpoint' arg, received $0",
                                 endpoint->name());
  }

  return static_cast<EndpointConfig*>(endpoint.get())->ToProto(pb);
}

StatusOr<QLObjectPtr> OTelSpanDefinition(const pypa::AstPtr& ast, const ParsedArgs& args,
                                         ASTVisitor* visitor) {
  planpb::OTelExportSinkOperator pb;
  PL_RETURN_IF_ERROR(ParseEndpointConfig(args.GetArg("endpoint"), pb.mutable_endpoint_config()));
  std::vector<ExpectedColumn> columns;
  auto span = pb.mutable_span();
  PL_ASSIGN_OR_RETURN(auto name, GetArgAsString(ast, args, "name"));
  span->set_name(name);
  PL_ASSIGN_OR_RETURN(auto span_id_column, GetArgAs<StringIR>(ast, args, "span_id"));
  span->set_span_id_column(span_id_column->str());
  columns.push_back(ExpectedColumn{
      span_id_column,
      "span_id",
      span_id_column->str(),
      {types::STRING},
  });

  PL_ASSIGN_OR_RETURN(auto parent_span_id_column, GetArgAs<StringIR>(ast, args, "parent_span_id"));
  span->set_parent_span_id_column(parent_span_id_column->str());
  columns.push_back(ExpectedColumn{
      parent_span_id_column,
      "parent_span_id",
      parent_span_id_column->str(),
      {types::STRING},
  });

  PL_ASSIGN_OR_RETURN(auto trace_id_column, GetArgAs<StringIR>(ast, args, "trace_id"));
  span->set_trace_id_column(trace_id_column->str());
  columns.push_back(ExpectedColumn{
      trace_id_column,
      "trace_id",
      trace_id_column->str(),
      {types::STRING},
  });

  PL_ASSIGN_OR_RETURN(auto status_column, GetArgAs<StringIR>(ast, args, "status"));
  span->set_status_column(status_column->str());
  columns.push_back(ExpectedColumn{
      status_column,
      "status",
      status_column->str(),
      {types::INT64},
  });

  // Time columns.
  PL_ASSIGN_OR_RETURN(auto start_time_unix_nano_column,
                      GetArgAs<StringIR>(ast, args, "start_time_unix_nano"));
  span->set_start_time_unix_nano_column(start_time_unix_nano_column->str());
  columns.push_back(ExpectedColumn{
      start_time_unix_nano_column,
      "start_time_unix_nano",
      start_time_unix_nano_column->str(),
      {types::TIME64NS},
  });

  PL_ASSIGN_OR_RETURN(auto end_time_unix_nano_column,
                      GetArgAs<StringIR>(ast, args, "end_time_unix_nano"));
  columns.push_back(ExpectedColumn{
      end_time_unix_nano_column,
      "end_time_unix_nano",
      end_time_unix_nano_column->str(),
      {types::TIME64NS},
  });
  span->set_end_time_unix_nano_column(end_time_unix_nano_column->str());

  // Kind column.
  PL_ASSIGN_OR_RETURN(auto kind, GetArgAs<IntIR>(ast, args, "kind"));
  if (!planpb::OTelSpanKind_IsValid(kind->val())) {
    return kind->CreateIRNodeError("Kind value '$0' is not a valid option", kind->val());
  }
  span->set_kind(static_cast<planpb::OTelSpanKind>(kind->val()));

  // Process attributes.
  QLObjectPtr attributes = args.GetArg("attributes");
  if (!DictObject::IsDict(attributes)) {
    return attributes->CreateError("Expected attributes to be a dictionary, received $0",
                                   attributes->name());
  }
  auto dict = static_cast<DictObject*>(attributes.get());
  auto values = dict->values();
  auto keys = dict->keys();
  CHECK_EQ(values.size(), keys.size());
  for (const auto& [idx, keyobj] : Enumerate(keys)) {
    auto attr = span->add_attributes();
    PL_ASSIGN_OR_RETURN(auto key, GetArgAs<StringIR>(keyobj, "attribute"));
    attr->set_name(key->str());
    PL_ASSIGN_OR_RETURN(auto val, GetArgAs<StringIR>(values[idx], "attribute value column"));
    attr->set_value_column(val->str());
    columns.push_back(ExpectedColumn{
        val,
        "attribute",
        val->str(),
        {types::STRING},
    });
  }

  return Exporter::Create(visitor, [pb, columns](auto&& ast, auto&& df) -> Status {
    return ExportToOTel(pb, columns, std::forward<decltype(ast)>(ast),
                        std::forward<decltype(df)>(df));
  });
}

StatusOr<QLObjectPtr> OTelMetricDefinition(const pypa::AstPtr& ast, const ParsedArgs& args,
                                           ASTVisitor* visitor) {
  auto data = args.GetArg("data");
  if (!OTelMetricData::IsType(data)) {
    return data->CreateError("Expected an OTelMetricData type. Received $0", data->name());
  }

  auto summary = static_cast<OTelMetricData*>(data.get());

  planpb::OTelExportSinkOperator pb;
  auto metric = pb.mutable_metric();
  *metric = summary->ToProto();
  auto columns = summary->columns();
  PL_RETURN_IF_ERROR(ParseEndpointConfig(args.GetArg("endpoint"), pb.mutable_endpoint_config()));
  PL_ASSIGN_OR_RETURN(auto name, GetArgAsString(ast, args, "name"));
  metric->set_name(name);
  PL_ASSIGN_OR_RETURN(auto description, GetArgAsString(ast, args, "description"));
  metric->set_description(description);

  // Process attributes.
  QLObjectPtr attributes = args.GetArg("attributes");
  if (!DictObject::IsDict(attributes)) {
    return attributes->CreateError("Expected attributes to be a dictionary, received $0",
                                   attributes->name());
  }
  auto dict = static_cast<DictObject*>(attributes.get());
  auto values = dict->values();
  auto keys = dict->keys();
  CHECK_EQ(values.size(), keys.size());
  for (const auto& [idx, keyobj] : Enumerate(keys)) {
    auto attr = metric->add_attributes();
    PL_ASSIGN_OR_RETURN(auto key, GetArgAs<StringIR>(keyobj, "attribute"));
    attr->set_name(key->str());
    PL_ASSIGN_OR_RETURN(auto val, GetArgAs<StringIR>(values[idx], "attribute value column"));
    attr->set_value_column(val->str());
    columns.push_back(ExpectedColumn{
        val,
        "attribute",
        val->str(),
        {types::STRING},
    });
  }

  return Exporter::Create(visitor, [pb, columns](auto&& ast, auto&& df) -> Status {
    return ExportToOTel(pb, columns, std::forward<decltype(ast)>(ast),
                        std::forward<decltype(df)>(df));
  });
}

StatusOr<QLObjectPtr> OTelGaugeDefinition(const pypa::AstPtr& ast, const ParsedArgs& args,
                                          ASTVisitor* visitor) {
  planpb::OTelMetric metric;
  std::vector<ExpectedColumn> columns;
  PL_ASSIGN_OR_RETURN(auto value_column, GetArgAs<StringIR>(ast, args, "value"));
  metric.mutable_gauge()->set_value_column(value_column->str());
  columns.push_back(ExpectedColumn{
      value_column,
      "value",
      value_column->str(),
      {types::INT64, types::FLOAT64},
  });

  // Time columns.
  PL_ASSIGN_OR_RETURN(auto start_time_unix_nano_column,
                      GetArgAs<StringIR>(ast, args, "start_time_unix_nano"));
  metric.set_start_time_unix_nano_column(start_time_unix_nano_column->str());
  columns.push_back(ExpectedColumn{
      start_time_unix_nano_column,
      "start_time_unix_nano",
      start_time_unix_nano_column->str(),
      {types::TIME64NS},
  });

  PL_ASSIGN_OR_RETURN(auto time_unix_nano_column, GetArgAs<StringIR>(ast, args, "time_unix_nano"));
  columns.push_back(ExpectedColumn{
      time_unix_nano_column,
      "time_unix_nano",
      time_unix_nano_column->str(),
      {types::TIME64NS},
  });
  metric.set_time_unix_nano_column(time_unix_nano_column->str());
  return OTelMetricData::Create(visitor, metric, columns);
}

StatusOr<QLObjectPtr> OTelSummaryDefinition(const pypa::AstPtr& ast, const ParsedArgs& args,
                                            ASTVisitor* visitor) {
  planpb::OTelMetric metric;
  auto summary = metric.mutable_summary();

  std::vector<ExpectedColumn> columns;
  PL_ASSIGN_OR_RETURN(auto count_column, GetArgAs<StringIR>(ast, args, "count"));
  summary->set_count_column(count_column->str());
  columns.push_back(ExpectedColumn{
      count_column,
      "count",
      count_column->str(),
      {types::FLOAT64},
  });

  PL_ASSIGN_OR_RETURN(auto sum_column, GetArgAs<StringIR>(ast, args, "sum"));
  summary->set_sum_column(sum_column->str());
  columns.push_back(ExpectedColumn{
      sum_column,
      "sum",
      sum_column->str(),
      {types::FLOAT64},
  });

  auto qvs = args.GetArg("quantile_values");
  if (!DictObject::IsDict(qvs)) {
    return qvs->CreateError("Expected quantile_values to be a dictionary, received $0",
                            qvs->name());
  }
  auto dict = static_cast<DictObject*>(qvs.get());
  auto values = dict->values();
  auto keys = dict->keys();
  CHECK_EQ(values.size(), keys.size());
  for (const auto& [idx, keyobj] : Enumerate(keys)) {
    auto qv = summary->add_quantile_values();
    PL_ASSIGN_OR_RETURN(auto key, GetArgAs<FloatIR>(keyobj, "quantile"));
    qv->set_quantile(key->val());
    PL_ASSIGN_OR_RETURN(auto val, GetArgAs<StringIR>(values[idx], "quantile value column"));
    qv->set_value_column(val->str());
    columns.push_back(ExpectedColumn{
        val,
        absl::Substitute("$0", key->val()),
        val->str(),
        {types::FLOAT64},
    });
  }

  // Time columns.
  PL_ASSIGN_OR_RETURN(auto start_time_unix_nano_column,
                      GetArgAs<StringIR>(ast, args, "start_time_unix_nano"));
  metric.set_start_time_unix_nano_column(start_time_unix_nano_column->str());
  columns.push_back(ExpectedColumn{
      start_time_unix_nano_column,
      "start_time_unix_nano",
      start_time_unix_nano_column->str(),
      {types::TIME64NS},
  });

  PL_ASSIGN_OR_RETURN(auto time_unix_nano_column, GetArgAs<StringIR>(ast, args, "time_unix_nano"));
  columns.push_back(ExpectedColumn{
      time_unix_nano_column,
      "time_unix_nano",
      time_unix_nano_column->str(),
      {types::TIME64NS},
  });
  metric.set_time_unix_nano_column(time_unix_nano_column->str());
  return OTelMetricData::Create(visitor, metric, columns);
}

StatusOr<std::shared_ptr<OTelMetricData>> OTelMetricData::Create(
    ASTVisitor* ast_visitor, planpb::OTelMetric pb, std::vector<ExpectedColumn> columns) {
  return std::shared_ptr<OTelMetricData>(
      new OTelMetricData(ast_visitor, std::move(pb), std::move(columns)));
}

Status OTelTraceModule::Init() {
  // Setup methods.
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> span_fn,
      FuncObject::Create(kSpanOpID,
                         {"name", "start_time_unix_nano", "end_time_unix_nano", "span_id",
                          "parent_span_id", "trace_id", "status", "kind", "attributes", "endpoint"},
                         {{"span_id", "''"},
                          {"parent_span_id", "''"},
                          {"trace_id", "''"},
                          {"status", "''"},
                          {"kind", "2"},
                          {"attributes", "{}"},
                          {"endpoint", "None"}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&OTelSpanDefinition, std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));

  PL_RETURN_IF_ERROR(span_fn->SetDocString(kSpanOpDocstring));
  AddMethod(kSpanOpID, span_fn);
  return Status::OK();
}

Status OTelMetricsModule::Init() {
  // Setup methods.
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> metric_fn,
      FuncObject::Create(kMetricOpID, {"name", "description", "data", "attributes", "endpoint"},
                         {{"attributes", "{}"}, {"endpoint", "None"}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&OTelMetricDefinition, std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));
  PL_RETURN_IF_ERROR(metric_fn->SetDocString(kMetricOpDocstring));
  AddMethod(kMetricOpID, metric_fn);

  PL_ASSIGN_OR_RETURN(std::shared_ptr<FuncObject> gauge_fn,
                      FuncObject::Create(kGaugeOpID,
                                         {
                                             "start_time_unix_nano",
                                             "time_unix_nano",
                                             "value",
                                         },
                                         {},
                                         /* has_variable_len_args */ false,
                                         /* has_variable_len_kwargs */ false,
                                         std::bind(&OTelGaugeDefinition, std::placeholders::_1,
                                                   std::placeholders::_2, std::placeholders::_3),
                                         ast_visitor()));

  PL_RETURN_IF_ERROR(gauge_fn->SetDocString(kGaugeOpDocstring));
  AddMethod(kGaugeOpID, gauge_fn);

  PL_ASSIGN_OR_RETURN(std::shared_ptr<FuncObject> summary_fn,
                      FuncObject::Create(kSummaryOpID,
                                         {
                                             "start_time_unix_nano",
                                             "time_unix_nano",
                                             "count",
                                             "sum",
                                             "quantile_values",
                                         },
                                         {},
                                         /* has_variable_len_args */ false,
                                         /* has_variable_len_kwargs */ false,
                                         std::bind(&OTelSummaryDefinition, std::placeholders::_1,
                                                   std::placeholders::_2, std::placeholders::_3),
                                         ast_visitor()));

  PL_RETURN_IF_ERROR(summary_fn->SetDocString(kSummaryOpDocstring));
  AddMethod(kSummaryOpID, summary_fn);
  return Status::OK();
}

StatusOr<QLObjectPtr> EndpointConfigConstructor(const pypa::AstPtr& ast, const ParsedArgs& args,
                                                ASTVisitor* visitor) {
  PL_ASSIGN_OR_RETURN(auto url, GetArgAsString(ast, args, "url"));

  std::vector<EndpointConfig::ConnAttribute> attributes;
  auto attr = args.GetArg("attributes");
  if (!DictObject::IsDict(attr)) {
    return attr->CreateError("expected dict() for 'attributes' arg, received $0", attr->name());
  }
  auto attr_dict = static_cast<DictObject*>(attr.get());
  for (const auto& [i, key] : Enumerate(attr_dict->keys())) {
    PL_ASSIGN_OR_RETURN(StringIR * key_ir, GetArgAs<StringIR>(ast, key, "attribute key"));
    PL_ASSIGN_OR_RETURN(StringIR * val_ir,
                        GetArgAs<StringIR>(ast, attr_dict->values()[i], "attribute value"));
    attributes.push_back(EndpointConfig::ConnAttribute{key_ir->str(), val_ir->str()});
  }
  return EndpointConfig::Create(visitor, url, attributes);
}

Status EndpointConfig::Init() {
  // Setup methods.
  PL_ASSIGN_OR_RETURN(
      std::shared_ptr<FuncObject> config_fn,
      FuncObject::Create(kOtelEndpointOpID, {"url", "attributes"}, {{"attributes", "{}"}},
                         /* has_variable_len_args */ false,
                         /* has_variable_len_kwargs */ false,
                         std::bind(&EndpointConfigConstructor, std::placeholders::_1,
                                   std::placeholders::_2, std::placeholders::_3),
                         ast_visitor()));

  AddCallMethod(config_fn);
  PL_RETURN_IF_ERROR(config_fn->SetDocString(kOtelEndpointOpDocstring));
  return Status::OK();
}
Status EndpointConfig::ToProto(planpb::OTelEndpointConfig* pb) {
  pb->set_url(url_);
  for (const auto& attr : attributes_) {
    (*pb->mutable_attributes())[attr.name] = attr.value;
  }
  return Status::OK();
}

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace px
