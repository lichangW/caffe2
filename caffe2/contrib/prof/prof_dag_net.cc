/**
 * Copyright (c) 2016-present, Facebook, Inc.
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

#include "prof_dag_net.h"

#include <cmath>

#include "caffe2/core/operator.h"
#include "caffe2/core/timer.h"

namespace caffe2 {

ProfDAGNet::ProfDAGNet(
    const std::shared_ptr<const NetDef>& net_def,
    Workspace* ws)
    : DAGNetBase(net_def, ws), time_per_op_(operator_nodes_.size()) {
  VLOG(1) << "Constructing ProfDAGNet " << name_;
}

ProfDAGNet::~ProfDAGNet() {
  VLOG(1) << "Closing ProfDAGNet " << name_;
  if (runs_ <= 1) {
    LOG(INFO) << "Insufficient runs to produce meaningful data.";
    return;
  }
  PrintStats();
}

void ProfDAGNet::ValidateOpTensorDevices() {
  bool had_mismatches = false;
  for (int idx = 0; idx < operator_nodes_.size(); idx++) {
    const auto& node = operator_nodes_[idx];
    auto mismatches =
        ValidateTensorDevices(*node.operator_, node.operator_->debug_def());
    for (auto& mismatch : mismatches) {
      had_mismatches = true;
      LOG(INFO) << "== PERFORMANCE WARNING == \n"
                << " Operator " << node.operator_->debug_def().type()
                << " expects GPU " << mismatch.second.first.cuda_gpu_id()
                << " but tensor [" << mismatch.first << "] is on GPU "
                << mismatch.second.second.cuda_gpu_id();
    }
  }
  if (!had_mismatches) {
    LOG(INFO) << "Analyzed operator & blob GPU assignments -- no mismatches";
  }
}

bool ProfDAGNet::DoRunAsync() {
  runs_++;

  // don't collect statistics from first run
  if (runs_ <= 1) {
    bool success = DAGNetBase::DoRunAsync();
    ValidateOpTensorDevices();
    return success;
  }

  CAFFE_ENFORCE(
      time_per_op_.size() == operator_nodes_.size(),
      "Data collected for ",
      time_per_op_.size(),
      " ops, expected ",
      operator_nodes_.size(),
      " ops.");

  // create a copy and later collect the differences
  vector<Stats> time_per_op_run(time_per_op_);
  bool success = DAGNetBase::DoRunAsync();

  // aggregate this run's stats per operator type
  CaffeMap<string, float> time_per_op_type_run;
  for (int idx = 0; idx < operator_nodes_.size(); idx++) {
    const auto& node = operator_nodes_[idx];
    const string& op_type = node.operator_->debug_def().type();
    time_per_op_type_run[op_type] +=
        time_per_op_[idx].sum - time_per_op_run[idx].sum;
    time_per_op_type_[op_type].cnt += 1;
  }

  for (const auto& item : time_per_op_type_run) {
    time_per_op_type_[item.first].sum += item.second;
    time_per_op_type_[item.first].sqrsum += item.second * item.second;
  }

  return success;
}

ProfDAGProto ProfDAGNet::ProtoMsg(std::pair<std::string, Stats> op_stat) const {
  ProfDAGProto message;
  float mean = op_stat.second.sum / (runs_ - 1);
  float stddev = std::sqrt(op_stat.second.sqrsum / (runs_ - 1) - mean * mean);
  message.set_mean(mean);
  message.set_stddev(stddev);
  message.set_name(op_stat.first);
  return message;
}

ProfDAGProtos ProfDAGNet::GetOperatorStats() {
  ProfDAGProtos prof_dag_protos;
  for (auto& item : time_per_op_type_) {
    auto buf = prof_dag_protos.add_stats();
    buf->CopyFrom(ProtoMsg(item));
  }
  return prof_dag_protos;
}

// GetPerOperatorCost collects the execution time of each operator, the output
// is formatted as a map: (netName__opIndex__opType, cost)
ProfDAGProtos ProfDAGNet::GetPerOperatorCost() {
  CAFFE_ENFORCE(
      time_per_op_.size() == operator_nodes_.size(),
      "Data collected for ",
      time_per_op_.size(),
      " ops, expected ",
      operator_nodes_.size(),
      " ops.");

  ProfDAGProtos prof_dag_protos;
  for (int idx = 0; idx < operator_nodes_.size(); idx++) {
    const auto& op = operator_nodes_[idx].operator_;
    const auto& def = op->debug_def();
    const string& op_type = def.type();

    auto buf = prof_dag_protos.add_stats();
    std::string op_output_name =
        name_ + "___" + to_string(idx) + "___" + op_type;
    std::pair<std::string, Stats> op_stat =
        std::pair<std::string, Stats>(op_output_name, time_per_op_[idx]);
    buf->CopyFrom(ProtoMsg(op_stat));
  }
  return prof_dag_protos;
}

bool ProfDAGNet::RunAt(int /* unused */, const std::vector<int>& chain) {
  bool success = true;
  Timer timer;
  for (const auto idx : chain) {
    // don't collect metrics from first run
    if (runs_ <= 1) {
      success &= operator_nodes_[idx].operator_->Run();

    } else {
      timer.Start();
      success &= operator_nodes_[idx].operator_->Run();
      float spent = timer.MilliSeconds();

      CAFFE_ENFORCE(
          time_per_op_.size() > idx,
          "Expecting ",
          time_per_op_.size(),
          " ops, but op #",
          idx,
          " was given.");
      time_per_op_[idx].sum += spent;
      time_per_op_[idx].sqrsum += spent * spent;
    }
  }
  return success;
}

void ProfDAGNet::PrintStats() {
  CAFFE_ENFORCE(
      time_per_op_.size() == operator_nodes_.size(),
      "Data collected for ",
      time_per_op_.size(),
      " ops, expected ",
      operator_nodes_.size(),
      " ops.");

  CAFFE_ENFORCE(runs_ > 1, "# of runs: ", runs_, ", expected > 1.");
  int measured_runs = runs_ - 1;

  for (int idx = 0; idx < operator_nodes_.size(); idx++) {
    const auto& op = operator_nodes_[idx].operator_;
    const auto& def = op->debug_def();
    const string& op_type = def.type();
    const string& print_name = def.name().size()
        ? def.name()
        : (op->OutputSize() ? def.output(0) : "NO_OUTPUT");

    float mean = time_per_op_[idx].sum / measured_runs;
    float stddev =
        std::sqrt(time_per_op_[idx].sqrsum / measured_runs - mean * mean);
    VLOG(1) << "Op #" << idx << " (" << print_name << ", " << op_type << ") "
            << mean << " ms/iter (" << stddev << " ms/iter)";
  }

  LOG(INFO) << "Time per operator type:";
  for (const auto& item : time_per_op_type_) {
    float mean = item.second.sum / measured_runs;
    float stddev = std::sqrt(item.second.sqrsum / measured_runs - mean * mean);
    LOG(INFO) << std::setw(10) << std::setfill(' ') << mean << " ms/iter ("
              << std::setw(10) << std::setfill(' ') << stddev << " ms/iter) "
              << " Count per iter: " << (item.second.cnt / measured_runs)
              << "  " << item.first;
  }
}

namespace {

REGISTER_NET(prof_dag, ProfDAGNet);
}

} // namespace caffe2
