﻿// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/low_precision/multiply.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cassert>

#include "transformations/low_precision/common/ie_lpt_exception.hpp"
#include "transformations/low_precision/network_helper.hpp"

// TODO: remove after debugging
#include <ngraph/pass/visualize_tree.hpp>

namespace ngraph {
namespace pass {
namespace low_precision {

void MultiplyTransformation::registerMatcherIn(GraphRewrite &pass, TransformationContext &context) const {
    addSingleNodePattern<opset1::Multiply>(pass, context);
}

void MultiplyTransformation::transform(TransformationContext& context, ngraph::pattern::Matcher &m) const {
    auto multiply = m.get_match_root();

    multiply = separateInStandaloneBranch(multiply);
    auto newMultiply = multiply;

    const int fullPathIndex = getNotEmpty(multiply);

    if (fullPathIndex == -1) {
        std::shared_ptr<Node> parent1 = multiply->get_input_node_shared_ptr(0);
        std::shared_ptr<Node> parent2 = multiply->get_input_node_shared_ptr(1);

        std::shared_ptr<opset1::Constant> constParent = as_type_ptr<opset1::Constant>(parent1);
        std::shared_ptr<opset1::Multiply> multiplyParent = as_type_ptr<opset1::Multiply>(parent2);

        if (constParent == nullptr || multiplyParent == nullptr) {
            constParent = as_type_ptr<opset1::Constant>(parent2);
            multiplyParent = as_type_ptr<opset1::Multiply>(parent1);
        }

        if (constParent == nullptr || multiplyParent == nullptr) {
            return;
        }

        auto multiplyParentParent1 = multiplyParent->get_input_node_shared_ptr(0);
        auto multiplyParentParent2 = multiplyParent->get_input_node_shared_ptr(1);

        auto multiplyParentParent = as_type_ptr<opset1::Multiply>(multiplyParentParent1);
        auto multiplyParentConst = as_type_ptr<opset1::Constant>(multiplyParentParent2);

        if (multiplyParentConst == nullptr) {
            multiplyParentParent = as_type_ptr<opset1::Multiply>(multiplyParentParent2);
            multiplyParentConst = as_type_ptr<opset1::Constant>(multiplyParentParent1);
        }

        if (multiplyParentConst == nullptr) {
            return;
        }

        newMultiply = std::make_shared<opset1::Multiply>(
            multiplyParentParent,
            fold<opset1::Multiply>(multiplyParentConst, constParent));
    } else {
        const int emptyPathIndex = fullPathIndex == 0 ? 1 : 0;

        FakeQuantizeDequantization dequantizationEmptyPath = NetworkHelper::getDequantization(multiply, emptyPathIndex);
        if (dequantizationEmptyPath.multiply == nullptr && dequantizationEmptyPath.subtract == nullptr) {
            return;
        }

        std::shared_ptr<Node> subtractValuesEmptyPath;
        std::shared_ptr<Node> multiplyValuesEmptyPath;
        std::tie(subtractValuesEmptyPath, multiplyValuesEmptyPath) = NetworkHelper::createEmptyValues(dequantizationEmptyPath);


        // check if empty path shifts are not zero
        std::shared_ptr<opset1::Constant> subtract1ValuesConst = as_type_ptr<opset1::Constant>(subtractValuesEmptyPath);
        if (NetworkHelper::isScalarLike(subtract1ValuesConst)) {
            auto scalar = NetworkHelper::distillToScalar(subtract1ValuesConst);
            if (!op::util::constantIsEqualTo(scalar, 0)) {
                return;
            }
        }

        FakeQuantizeDequantization dequantizationFullPath = NetworkHelper::getDequantization(multiply, fullPathIndex);
        std::shared_ptr<Node> subtractValuesFullPath;
        std::shared_ptr<Node> multiplyValuesFullPath;
        std::tie(subtractValuesFullPath, multiplyValuesFullPath) = NetworkHelper::createEmptyValues(dequantizationFullPath);

        std::shared_ptr<Node> newMultiplyValuesFullPath = fold<opset1::Multiply>(multiplyValuesEmptyPath, multiplyValuesFullPath);
        std::vector<std::shared_ptr<Node>> inputs{ {}, {} };
        inputs[emptyPathIndex] = dequantizationEmptyPath.subtract == nullptr ?
            dequantizationEmptyPath.multiply->get_input_node_shared_ptr(0) :
            dequantizationEmptyPath.subtract->get_input_node_shared_ptr(0);
        inputs[fullPathIndex] = std::make_shared<opset1::Multiply>(
            dequantizationFullPath.subtract == nullptr ?
                (dequantizationFullPath.convert == nullptr ?
                    dequantizationFullPath.data : dequantizationFullPath.convert) :
                dequantizationFullPath.subtract,
            newMultiplyValuesFullPath);

        newMultiply = std::make_shared<opset1::Multiply>(inputs[0], inputs[1]);
    }

    replace_node(multiply, newMultiply);
    updateOutput(context, newMultiply, multiply);
}

} // namespace low_precision
} // namespace pass
} // namespace ngraph
