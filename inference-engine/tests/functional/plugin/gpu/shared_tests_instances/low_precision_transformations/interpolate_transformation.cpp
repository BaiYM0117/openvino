// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "low_precision_transformations/interpolate_transformation.hpp"

using namespace LayerTestsDefinitions;
using namespace InferenceEngine::details;

namespace {
const std::vector<ngraph::element::Type> precisions = {
    ngraph::element::f32
};

const std::vector<std::pair<ngraph::Shape, ngraph::Shape>> shapes = {
    {{1, 4, 16, 16}, {32, 32}},
    {{1, 2, 48, 80}, {50, 60}},
};

const std::vector<LayerTestsUtils::LayerTransformation::LptVersion> versions = {
    LayerTestsUtils::LayerTransformation::LptVersion::cnnNetwork,
    LayerTestsUtils::LayerTransformation::LptVersion::nGraph
};

const std::vector<ngraph::op::InterpolateAttrs> interpAttrs = {
    {
        ngraph::AxisSet{2, 3},
        "nearest",
        false,
        false,
        {0},
        {0}
    },
    {
        ngraph::AxisSet{2, 3},
        "nearest",
        false,
        true,
        {0},
        {0}
    },
};

const auto combineValues = ::testing::Combine(
    ::testing::ValuesIn(precisions),
    ::testing::ValuesIn(shapes),
    ::testing::Values(CommonTestUtils::DEVICE_GPU),
    ::testing::ValuesIn(interpAttrs),
    ::testing::ValuesIn(versions)
);

INSTANTIATE_TEST_CASE_P(LPT, InterpolateTransformation, combineValues, InterpolateTransformation::getTestCaseName);
}  // namespace
