// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <algorithm>
#include <chrono>
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <utility>

#include <inference_engine.hpp>
#include <vpu/vpu_plugin_config.hpp>
#include <cldnn/cldnn_config.hpp>
#include <gna/gna_config.hpp>
#include <samples/common.hpp>
#include <samples/slog.hpp>
#include <samples/args_helper.hpp>

#include "framework.pb.h"

#include <ngraph/ngraph.hpp>
#include <ngraph/opsets/opset6.hpp>

using namespace InferenceEngine;

using namespace google;
using namespace paddle::framework;

std::map<paddle::framework::proto::VarType_Type, ngraph::element::Type> TYPE_MAP{
    {proto::VarType_Type::VarType_Type_BOOL, ngraph::element::boolean},
    {proto::VarType_Type::VarType_Type_INT16, ngraph::element::i16},
    {proto::VarType_Type::VarType_Type_INT32, ngraph::element::i32},
    {proto::VarType_Type::VarType_Type_INT64, ngraph::element::i64},
    {proto::VarType_Type::VarType_Type_FP16, ngraph::element::f16},
    {proto::VarType_Type::VarType_Type_FP32, ngraph::element::f32},
    {proto::VarType_Type::VarType_Type_FP64, ngraph::element::f64},
    {proto::VarType_Type::VarType_Type_UINT8, ngraph::element::u8},
    {proto::VarType_Type::VarType_Type_INT8, ngraph::element::i8},
    {proto::VarType_Type::VarType_Type_BF16, ngraph::element::bf16}
};

bool endsWith(std::string str, std::string suffix) {
    if (str.length() >= suffix.length()) {
        return (0 == str.compare(str.length() - suffix.length(), suffix.length(), suffix));
    }
    return false;
}

protobuf::RepeatedField<protobuf::int32> get_ints(proto::OpDesc op, std::string name,
    protobuf::RepeatedField<protobuf::int32> default = protobuf::RepeatedField<protobuf::int32>()) {
    std::vector<proto::OpDesc_Attr> attrs;
    for (const auto& attr : op.attrs()) {
        if (attr.name() == name)
            attrs.push_back(attr);
    }
    if (attrs.size() == 0) {
        return default;
    } else if (attrs.size() > 1) {
        // TODO: raise exception here
        return default;
    } else {
        return attrs[0].ints();
    }
}

int get_int(proto::OpDesc op, std::string name, int default = 0) {
    std::vector<proto::OpDesc_Attr> attrs;
    for (const auto& attr : op.attrs()) {
        if (attr.name() == name)
            attrs.push_back(attr);
    }
    if (attrs.size() == 0) {
        return default;
    } else if (attrs.size() > 1) {
        // TODO: raise exception here
        return default;
    } else {
        return attrs[0].i();
    }
}

float get_float(proto::OpDesc op, std::string name, float default = 0.) {
    std::vector<proto::OpDesc_Attr> attrs;
    for (const auto& attr : op.attrs()) {
        if (attr.name() == name)
            attrs.push_back(attr);
    }
    if (attrs.size() == 0) {
        return default;
    } else if (attrs.size() > 1) {
        // TODO: raise exception here
        return default;
    } else {
        return attrs[0].f();
    }
}

std::string get_str(proto::OpDesc op, std::string name, std::string default = "") {
    std::vector<proto::OpDesc_Attr> attrs;
    for (const auto& attr : op.attrs()) {
        if (attr.name() == name)
            attrs.push_back(attr);
    }
    if (attrs.size() == 0) {
        return default;
    } else if (attrs.size() > 1) {
        // TODO: raise exception here
        return default;
    } else {
        return attrs[0].s();
    }
}

bool get_bool(proto::OpDesc op, std::string name, bool default = false) {
    std::vector<proto::OpDesc_Attr> attrs;
    for (const auto& attr : op.attrs()) {
        if (attr.name() == name)
            attrs.push_back(attr);
    }
    if (attrs.size() == 0) {
        return default;
    } else if (attrs.size() > 1) {
        // TODO: raise exception here
        return default;
    } else {
        return attrs[0].b();
    }
}

typedef std::shared_ptr<ngraph::Node>(*CreatorFunction)(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block);

std::shared_ptr<ngraph::Node> conv2d_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["Input"].size() == 1);
    auto data = inputs["Input"][0];
    assert(inputs["Filter"].size() == 1);
    auto filter = inputs["Filter"][0];
    assert(inputs["Bias"].size() == 0);
    assert(inputs["ResidualData"].size() == 0);
    // TODO: resolve padding according to spec
    auto strides = get_ints(op, "strides");
    auto paddings = get_ints(op, "paddings");
    auto dilations = get_ints(op, "dilations");
    return std::make_shared<ngraph::opset6::Convolution>(data,
        filter,
        ngraph::Strides(strides.begin(), strides.end()),
        ngraph::CoordinateDiff(paddings.begin(), paddings.end()),
        ngraph::CoordinateDiff(paddings.begin(), paddings.end()),
        ngraph::Strides(dilations.begin(), dilations.end()));
}


std::shared_ptr<ngraph::Node> batch_norm_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["X"].size() == 1);
    assert(inputs["Scale"].size() == 1);
    assert(inputs["Bias"].size() == 1);
    assert(inputs["Mean"].size() == 1);
    assert(inputs["Variance"].size() == 1);
    auto data = inputs["X"][0];
    auto gamma = inputs["Scale"][0];
    auto beta = inputs["Bias"][0];
    auto mean = inputs["Mean"][0];
    auto variance = inputs["Variance"][0];
    return std::make_shared<ngraph::opset6::BatchNormInference>(data, gamma, beta, mean, variance, get_float(op, "epsilon"));
}


std::shared_ptr<ngraph::Node> relu_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["X"].size() == 1);
    auto data = inputs["X"][0];
    return std::make_shared<ngraph::opset6::Relu>(data);
}

std::shared_ptr<ngraph::Node> pool2d_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["X"].size() == 1);
    auto data = inputs["X"][0];
    // TODO : resolve padding according to spec
    auto pooling_type = get_str(op, "pooling_type");
    auto global_pooling = get_bool(op, "global_pooling");
    if (pooling_type == "max" && !global_pooling) {
        auto strides = get_ints(op, "strides");
        auto paddings = get_ints(op, "paddings");
        auto kernel_shape = get_ints(op, "ksize");
        return std::make_shared<ngraph::opset6::MaxPool>(data,
            ngraph::Strides(strides.begin(), strides.end()),
            ngraph::Shape(paddings.begin(), paddings.end()),
            ngraph::Shape(paddings.begin(), paddings.end()),
            ngraph::Shape(kernel_shape.begin(), kernel_shape.end()));
    } else if (pooling_type == "avg" && global_pooling) {
        // TODO : resolve axes according to rank
        auto axes = ngraph::opset6::Constant::create(ngraph::element::i64, { 2 }, { 2, 3 });
        return std::make_shared<ngraph::opset6::ReduceMean>(data, axes, true);
    } else {
        throw std::exception("Unsupported pooling type");
    }
}

std::shared_ptr<ngraph::Node> elementwise_add_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["X"].size() == 1);
    assert(inputs["Y"].size() == 1);
    auto x = inputs["X"][0];
    auto y = inputs["Y"][0];
    // TODO : resolve broadcast
    return std::make_shared<ngraph::opset6::Add>(x, y);
}

std::shared_ptr<ngraph::Node> mul_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["X"].size() == 1);
    assert(inputs["Y"].size() == 1);
    auto x = inputs["X"][0];
    auto y = inputs["Y"][0];
    assert(x->output(0).get_partial_shape().rank().is_static());
    int64_t x_rank = x->output(0).get_partial_shape().rank().get_length();
    assert(y->output(0).get_partial_shape().rank().is_static() && y->output(0).get_partial_shape().rank().get_length() == 2);
    if (x_rank > 2) {
        auto shape = std::make_shared<ngraph::opset6::ShapeOf>(x);
        int64_t x_num_col_dims = get_int(op, "x_num_col_dims");
        auto axis = ngraph::opset6::Constant::create(ngraph::element::i64, {}, { 0 });
        auto split_lengths = ngraph::opset6::Constant::create(ngraph::element::i64, {2}, { x_num_col_dims, x_rank - x_num_col_dims });
        auto split = std::make_shared<ngraph::opset6::VariadicSplit>(shape, axis, split_lengths);
        auto f_dim_red_axis = ngraph::opset6::Constant::create(ngraph::element::i64, {}, { 0 });
        auto first_dim_reduce = std::make_shared<ngraph::opset6::ReduceProd>(split->output(0), f_dim_red_axis);
        auto f_dim_shape = ngraph::opset6::Constant::create(ngraph::element::i64, { 1 }, { 1 });
        auto first_dim = std::make_shared<ngraph::opset6::Reshape>(first_dim_reduce, f_dim_shape, false);
        auto s_dim_red_axis = ngraph::opset6::Constant::create(ngraph::element::i64, {}, { 0 });
        auto second_dim_reduce = std::make_shared<ngraph::opset6::ReduceProd>(split->output(1), s_dim_red_axis);
        auto s_dim_shape = ngraph::opset6::Constant::create(ngraph::element::i64, { 1 }, { 1 });
        auto second_dim = std::make_shared<ngraph::opset6::Reshape>(second_dim_reduce, s_dim_shape, false);
        auto out_shape = std::make_shared<ngraph::opset6::Concat>(ngraph::NodeVector{ first_dim, second_dim }, 0);
        auto x_reshaped = std::make_shared<ngraph::opset6::Reshape>(x, out_shape, false);
        return std::make_shared<ngraph::opset6::MatMul>(x_reshaped, y);
    }
    return std::make_shared<ngraph::opset6::MatMul>(x, y);
}

std::shared_ptr<ngraph::Node> scale_creator(std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs,
    proto::OpDesc op, proto::BlockDesc block) {
    assert(inputs["X"].size() == 1);
    auto data = inputs["X"][0];
    auto scale = ngraph::opset6::Constant::create(ngraph::element::f32, { 1 }, { get_float(op, "scale") });
    return std::make_shared<ngraph::opset6::Multiply>(data, scale);
}

std::shared_ptr<ngraph::Node> make_ng_node(std::map<std::string, google::protobuf::RepeatedPtrField<std::string>> inputs,
    std::map<std::string, std::shared_ptr<ngraph::Node>> nodes,
    paddle::framework::proto::OpDesc op,
    paddle::framework::proto::BlockDesc block) {
    std::map<std::string, CreatorFunction> CREATORS_MAP{
        {"conv2d", conv2d_creator},
        {"batch_norm", batch_norm_creator},
        {"relu", relu_creator},
        {"pool2d", pool2d_creator},
        {"elementwise_add", elementwise_add_creator},
        {"mul", mul_creator},
        {"scale", scale_creator}
    };
    assert(CREATORS_MAP.find(op.type()) != CREATORS_MAP.end());
    std::map<std::string, std::vector<std::shared_ptr<ngraph::Node>>> inputs_preproc;
    for (const auto& item : inputs) {
        inputs_preproc[item.first] = std::vector<std::shared_ptr<ngraph::Node>>();
        for (const auto& input_name : item.second) {
            // TODO: refactor to not search every time
            inputs_preproc[item.first].push_back(nodes[input_name]);
        }
    }
    return CREATORS_MAP[op.type()](inputs_preproc, op, block);
}

std::shared_ptr<ngraph::opset6::Constant> read_tensor(paddle::framework::proto::VarDesc var, std::string model_dir) {
    assert(var.type().type() == paddle::framework::proto::VarType::LOD_TENSOR);
    auto tensor = var.type().lod_tensor().tensor();

    std::ifstream is;
    is.open(model_dir + "\\" + var.name(), std::ios::binary);
    // get length of file:
    is.seekg(0, std::ios::end);
    auto length = is.tellg();
    auto tensor_length = std::accumulate(tensor.dims().cbegin(), tensor.dims().cend(), 1, std::multiplies<int64_t>()) * 4;
    is.seekg((size_t)length - tensor_length, std::ios::beg);

    std::vector<float> tensor_data(tensor_length);
    is.read(reinterpret_cast<char*>(&tensor_data[0]), tensor_length);
    auto shape = std::vector<size_t>(tensor.dims().cbegin(), tensor.dims().cend());
    return ngraph::opset6::Constant::create(ngraph::element::f32, ngraph::Shape(shape), tensor_data);
}

std::shared_ptr<ngraph::Function> convert_model(const std::string & model_dir) {
    paddle::framework::proto::ProgramDesc fw_model;
    std::ifstream pb_stream(model_dir + "\\model", std::ios::binary);
    fw_model.ParseFromIstream(&pb_stream);

    std::map<std::string, std::shared_ptr<ngraph::Node>> nodes_dict;
    ngraph::ParameterVector parameter_nodes;
    ngraph::ResultVector result_nodes;

    const auto& global_block = fw_model.blocks()[0];
    for (const auto& var : global_block.vars()) {
        if (endsWith(var.name(), "feed") || endsWith(var.name(), "fetch"))
            continue;
        if (!var.persistable())
            continue;
        nodes_dict[var.name()] = read_tensor(var, model_dir);
    }

    for (const auto& block : fw_model.blocks()) {
        std::map<std::string, paddle::framework::proto::VarType> vars_dict;
        for (const auto& var : block.vars()) {
            vars_dict[var.name()] = var.type();
        }
        for (int i = 0; i < block.ops_size(); i++) {
            const auto& op = block.ops()[i];
            std::map<std::string, google::protobuf::RepeatedPtrField<std::string>> outputs_dict;
            for (const auto& output : op.outputs()) {
                outputs_dict[output.parameter()] = output.arguments();
            }
            std::map<std::string, google::protobuf::RepeatedPtrField<std::string>> inputs_dict;
            for (const auto& input : op.inputs()) {
                outputs_dict[input.parameter()] = input.arguments();
            }
            if (op.type() == "feed") {
                auto layer_name = outputs_dict["Out"][0];
                auto var = vars_dict[layer_name];
                assert(var.type() == paddle::framework::proto::VarType::LOD_TENSOR);
                auto tensor_desc = var.lod_tensor().tensor();
                auto dtype = tensor_desc.data_type();
                auto shape = std::vector<size_t>(tensor_desc.dims().cbegin(), tensor_desc.dims().cend());
                auto param = std::make_shared<ngraph::opset6::Parameter>(TYPE_MAP[dtype], ngraph::Shape(shape));
                nodes_dict[layer_name] = param;
                parameter_nodes.push_back(param);
            } else if (op.type() == "fetch") {
                auto input_node = inputs_dict["X"][0];
                assert(nodes_dict.find(input_node) != nodes_dict.end());
                result_nodes.push_back(std::make_shared<ngraph::opset6::Result>(nodes_dict[input_node]));
            } else {
                auto node = make_ng_node(inputs_dict, nodes_dict, op, block);
                for (const auto& item : outputs_dict) {
                    assert(item.second.size() <= 1);
                    if (item.second.size() == 1) {
                        nodes_dict[item.second[0]] = node;
                    }
                }
            }
        }
    }
    return std::make_shared<ngraph::Function>(result_nodes, parameter_nodes);
}

int main(int argc, char* argv[]) {
    try {
        std::string model_path = "C:\\Dev\\resnet_v2_50_imagenet\\model";
        auto func = convert_model(model_path);
    }
    catch (const std::exception& ex) {
        slog::err << ex.what() << slog::endl;

        return 3;
    }

    return 0;
}
