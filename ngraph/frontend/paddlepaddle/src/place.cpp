//*****************************************************************************
// Copyright 2021 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************


#include <paddlepaddle_frontend/place.hpp>
#include "framework.pb.h"
#include "decoder.hpp"

using namespace ngraph;
using namespace frontend;

bool PlacePDPD::isInput() const {
    const auto &model_ins = m_input_model.getInputs();

    const auto cmp = [this](const Place::Ptr &p) {
        return p.get() == this;
    };
    return std::find_if(model_ins.begin(), model_ins.end(), cmp) != model_ins.end();
}

bool PlacePDPD::isOutput() const {
    const auto &model_outs = m_input_model.getOutputs();
    const auto cmp = [this](const Place::Ptr &p) {
        return p.get() == this;
    };
    return std::find_if(model_outs.begin(), model_outs.end(), cmp) != model_outs.end();
}

OpPlacePDPD::OpPlacePDPD(const InputModel &input_model, const std::vector<std::string> &names,
                         const std::shared_ptr<paddle::framework::proto::OpDesc> &op_desc)
        : PlacePDPD(input_model, names),
          m_op_desc(op_desc) {

}

OpPlacePDPD::OpPlacePDPD(const InputModel &input_model,
                         const std::shared_ptr<paddle::framework::proto::OpDesc> &op_desc)
        : OpPlacePDPD(input_model, {}, op_desc) {
}

TensorPlacePDPD::TensorPlacePDPD(const InputModel &input_model, const std::vector<std::string> &names,
                                 const std::shared_ptr<paddle::framework::proto::VarDesc> &var_desc)
        : PlacePDPD(input_model, names),
          m_var_desc(var_desc) {
    const auto& var_type = var_desc->type();
    if (var_type.type() == paddle::framework::proto::VarType::LOD_TENSOR) {
        const auto& tensor_desc = var_type.lod_tensor().tensor();
        m_type = TYPE_MAP[tensor_desc.data_type()];
        m_pshape = PartialShape(std::vector<Dimension>(tensor_desc.dims().begin(), tensor_desc.dims().end()));
    }
}

TensorPlacePDPD::TensorPlacePDPD(const InputModel &input_model,
                                 const std::shared_ptr<paddle::framework::proto::VarDesc> &var_desc)
        : TensorPlacePDPD(input_model, {var_desc->name()}, var_desc) {
}

std::vector<Place::Ptr> TensorPlacePDPD::getConsumingPorts() const {
    std::vector<Place::Ptr> consuming_ports;
    for (const auto & consuming_port: m_consuming_ports) {
        if (const auto& locked = consuming_port.lock()) {
            consuming_ports.push_back(locked);
        } else {
            MY_ASSERT(false, "Consuming Port has expired.");
        }
    }
    return consuming_ports;
}

Place::Ptr TensorPlacePDPD::getProducingPort() const {
    MY_ASSERT(m_producing_ports.size() > 1, "Only one producing port is supported.");
    if (const auto& producing_port = m_producing_ports[0].lock()) {
        return producing_port;
    }
    MY_ASSERT(false, "Producing Port has expired.");
}

std::shared_ptr<Place> InPortPlacePDPD::getSourceTensor(int idx) const {
    if (const auto& tensor = m_source_tensors[idx].lock()) {
        return tensor;
    }
    MY_ASSERT(false, "Source Tensor has expired.");
}

std::shared_ptr<TensorPlacePDPD> InPortPlacePDPD::getSourceTensorPDPD(int idx) const {
    if (const auto& tensor = m_source_tensors[idx].lock()) {
        return tensor;
    }
    MY_ASSERT(false, "Source Tensor has expired.");
}

std::shared_ptr<OpPlacePDPD> InPortPlacePDPD::getOp() {
    if (const auto& op = m_op.lock()) {
        return op;
    }
    MY_ASSERT(false, "Operation has expired.");
}

std::vector<std::shared_ptr<TensorPlacePDPD>> InPortPlacePDPD::getSourceTensors() const {
    std::vector<std::shared_ptr<TensorPlacePDPD>> source_tensors;
    for (const auto & tensor: m_source_tensors) {
        if (const auto& locked = tensor.lock()) {
            source_tensors.push_back(locked);
        } else {
            MY_ASSERT(false, "Source Tensor has expired.");
        }
    }
    return source_tensors;
}

std::shared_ptr<Place> OutPortPlacePDPD::getTargetTensor(int idx) const {
    if (const auto& target_tensor = m_target_tensors.at(idx).lock()) {
        return target_tensor;
    }
    MY_ASSERT(false, "Target Tensor has expired.");
}

std::shared_ptr<TensorPlacePDPD> OutPortPlacePDPD::getTargetTensorPDPD(int idx) const {
    if (const auto& target_tensor = m_target_tensors.at(idx).lock()) {
        return target_tensor;
    }
    MY_ASSERT(false, "Target Tensor has expired.");
}

std::vector<std::shared_ptr<TensorPlacePDPD>> OutPortPlacePDPD::getTargetTensors() const {
    std::vector<std::shared_ptr<TensorPlacePDPD>> target_tensors;
    for (const auto & tensor: m_target_tensors) {
        if (const auto& locked = tensor.lock()) {
            target_tensors.push_back(locked);
        } else {
            MY_ASSERT(false, "Target Tensor has expired.");
        }
    }
    return target_tensors;
}