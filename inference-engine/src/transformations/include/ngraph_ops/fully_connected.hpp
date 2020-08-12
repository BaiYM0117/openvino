// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <memory>

#include <transformations_visibility.hpp>

#include "ngraph/node.hpp"
#include "ngraph/op/op.hpp"
#include "ngraph/op/util/fused_op.hpp"

namespace ngraph {
namespace op {

/// \brief Operator performing Matrix Multiplication.
class TRANSFORMATIONS_API FullyConnected : public Op {
public:
    static constexpr NodeTypeInfo type_info{"FullyConnected", 0};
    const NodeTypeInfo& get_type_info() const override { return type_info; }
    FullyConnected() = default;
    /// \brief Constructs an FullyConnected operation.
    ///
    /// \param A Matrix A
    /// \param B Matrix B
    /// \param C Matrix C
#ifdef LPT_SUPPORT
    FullyConnected(const Output<Node> & A,
                   const Output<Node> & B,
                   const Output<Node> & C,
                   const Shape & output_shape,
                   const element::Type output_type = element::undefined);
#else
    FullyConnected(const Output<Node> & A,
                   const Output<Node> & B,
                   const Output<Node> & C,
                   const Shape & output_shape);
#endif


    void validate_and_infer_types() override;

    std::shared_ptr<Node> clone_with_new_inputs(const OutputVector& new_args) const override;

    size_t get_out_size() { return m_output_size; }

#ifdef LPT_SUPPORT
    void set_output_type(size_t i,
        const element::Type& element_type,
        const PartialShape& pshape) override;
#endif

private:
    size_t m_output_size = 0;
    Shape m_output_shape = {};
#ifdef LPT_SUPPORT
    element::Type m_output_type;
#endif
};

}  // namespace op
}  // namespace ngraph
