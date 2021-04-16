//*****************************************************************************
// Copyright 2017-2021 Intel Corporation
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

#pragma once

#include <frontend_manager/frontend_manager.hpp>

#include "place.hpp"

namespace ngraph {
namespace frontend {

class NGRAPH_API InputModelPDPD : public InputModel
{
    friend class FrontEndPDPD;
    class InputModelPDPDImpl;
    std::shared_ptr<InputModelPDPDImpl> _impl;

    std::vector<float> readWeight(const std::string& name, int64_t tensor_length);
    std::vector<std::shared_ptr<OpPlacePDPD>> getOpPlaces(int i) const;
    std::map<std::string, std::shared_ptr<TensorPlacePDPD>> getVarPlaces(int i) const;
    size_t getBlockNumber() const;

public:
    explicit InputModelPDPD (const std::string& _path);
    std::vector<Place::Ptr> getInputs () const override;
    std::vector<Place::Ptr> getOutputs () const override;
    Place::Ptr getPlaceByTensorName (const std::string& tensorName) const override;
    void overrideAllOutputs (const std::vector<Place::Ptr>& outputs) override;
    void overrideAllInputs (const std::vector<Place::Ptr>& inputs) override;
    void extractSubgraph (const std::vector<Place::Ptr>& inputs, const std::vector<Place::Ptr>& outputs) override;
    void setDefaultShape (Place::Ptr place, const ngraph::Shape&) override;
    void setPartialShape (Place::Ptr place, const ngraph::PartialShape&) override;
};

} // namespace frontend
} // namespace ngraph
