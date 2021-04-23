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

#include <ngraph/opsets/opset6.hpp>
#include "squeeze.hpp"
#include <paddlepaddle_frontend/utility.hpp>

namespace ngraph {
namespace frontend {
namespace pdpd {
namespace op {

NamedOutputs squeeze (const NodeContext& node) {
    auto data = node.get_ng_input("X");
    auto axes = node.get_attribute<std::vector<int32_t>>("axes");
    PDPD_ASSERT(data.get_partial_shape().rank().is_static(), "squeeze: X rank must be static!");

    auto shape = data.get_partial_shape().to_shape();
    for (auto &&i : axes) {
        size_t idx = i;
        if (idx < 0) {
            idx = i + shape.size();
        }
        PDPD_ASSERT(idx < shape.size(), "squeeze: axes value must be < max_rank.");
        PDPD_ASSERT(shape[idx] == 1, "squeeze: the specified dimension is not equal to one.");
    }
    
    auto axesNode = ngraph::opset6::Constant::create(ngraph::element::i32, {axes.size()}, axes);
    return node.default_single_output_mapping({std::make_shared<ngraph::opset6::Squeeze>(data, axesNode)}, {"Out"});
}

}}}}