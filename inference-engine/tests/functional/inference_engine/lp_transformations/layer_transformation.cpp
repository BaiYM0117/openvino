// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "layer_transformation.hpp"

#include "generic_ie.hpp"
#include <transformations/common_optimizations/common_optimizations.hpp>
#include <transformations/convert_opset1_to_legacy/convert_opset1_to_legacy.hpp>
#include <transformations/convert_opset2_to_opset1/convert_opset2_to_opset1.hpp>
#include <transformations/convert_opset3_to_opset2/convert_opset3_to_opset2.hpp>
#include <ngraph/opsets/opset1.hpp>
#include <ngraph/opsets/opset2.hpp>
#include <ngraph/opsets/opset3.hpp>

#include "simple_low_precision_transformer.hpp"

using namespace testing;
using namespace ngraph::pass;

ngraph::pass::low_precision::LayerTransformation::Params LayerTransformation::createParamsU8I8() {
    return low_precision::LayerTransformation::Params(
        true,
        true,
        true,
        low_precision::LayerTransformation::QuantizedTensorAlignment::UpdateLevel,
        low_precision::LayerTransformation::QuantizedTensorAlignment::None,
        true,
        true,
        true,
        { ngraph::element::u8 },
        { ngraph::element::i8 });
}

ngraph::pass::low_precision::LayerTransformation::Params LayerTransformation::createParamsI8I8() {
    return low_precision::LayerTransformation::Params(
        true,
        true,
        true,
        low_precision::LayerTransformation::QuantizedTensorAlignment::UpdateLevel,
        low_precision::LayerTransformation::QuantizedTensorAlignment::None,
        true,
        true,
        true,
        { ngraph::element::i8 },
        { ngraph::element::i8 });
}

ngraph::pass::low_precision::LayerTransformation::Params LayerTransformation::createParamsU8I8AndI8() {
    return low_precision::LayerTransformation::Params(
        true,
        true,
        true,
        low_precision::LayerTransformation::QuantizedTensorAlignment::UpdateLevel,
        low_precision::LayerTransformation::QuantizedTensorAlignment::None,
        true,
        true,
        true,
        { ngraph::element::u8, ngraph::element::i8 },
        { ngraph::element::i8 });
}

std::string LayerTransformation::toString(const ngraph::pass::low_precision::LayerTransformation::Params& params) {
    std::ostringstream result;
    result <<
        (params.supportAsymmetricQuantization ? "asymmetric_" : "symmetric_") <<
        (params.updatePrecisions ? "" : "notUpdatePrecisions_") <<
        params.precisionsOnActivations[0] << "_" <<
        params.precisionsOnWeights[0] << "_" <<
        params.quantizedTensorAlignmentOnActivations;

    return result.str();
}

void LayerTransformation::transform(std::shared_ptr<ngraph::Function> function) {
    ngraph::pass::low_precision::LowPrecisionTransformations transformations = ngraph::pass::low_precision::LowPrecisionTransformer::getAllTransformations();
    ngraph::pass::low_precision::LowPrecisionTransformer transformer(transformations);
    transformer.transform(function);
}

std::string LayerTransformation::getTestCaseNameByParams(
    const ngraph::element::Type& type,
    const ngraph::Shape& shape,
    const ngraph::pass::low_precision::LayerTransformation::Params& params) {
    std::ostringstream result;
    result << type << "_" << shape << "_" << toString(params);
    return result.str();
}
