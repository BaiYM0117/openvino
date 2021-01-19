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

#include <functional>
#include <onnx/onnx_pb.h>
#include <stack>

#include "ngraph/check.hpp"
#include "onnx_import/editor/detail/subgraph_extraction.hpp"

using namespace ngraph::onnx_import;

namespace
{
    void validate_node_index(const ONNX_NAMESPACE::GraphProto& graph, const int node_idx)
    {
        NGRAPH_CHECK(
            node_idx >= 0 && node_idx < graph.node_size(),
            "The specified node index is out of range of nodes in the original model(idx: ",
            std::to_string(node_idx),
            "; nodes count in the model: ",
            std::to_string(graph.node_size()),
            ")");
    }

    struct CheckIfHasName
    {
    };

    template <typename T>
    std::function<bool(const T&)> name_equals(const std::string& name)
    {
        return [&name](const T& onnx_object) -> bool { return onnx_object.name() == name; };
    }

    template <typename T>
    std::function<bool(const T&)> name_equals(const std::string& name, CheckIfHasName)
    {
        return [&name](const T& onnx_object) -> bool {
            return onnx_object.has_name() && onnx_object.name() == name;
        };
    }

    std::function<bool(const std::string&)> is_equal_to(const std::string& other)
    {
        return [&other](const std::string& s) { return s == other; };
    }

    template <typename Container>
    bool already_exists(const Container& items, const std::string& name)
    {
        return std::any_of(
            items.begin(), items.end(), name_equals<typename Container::value_type>(name));
    }

    bool is_graph_input(const ONNX_NAMESPACE::GraphProto& graph, const std::string& name)
    {
        return already_exists(graph.input(), name);
    }

    bool is_graph_initializer(const ONNX_NAMESPACE::GraphProto& graph, const std::string& name)
    {
        return already_exists(graph.initializer(), name);
    }

    int find_source_node_idx(const ONNX_NAMESPACE::GraphProto& graph,
                             const int current_node_idx,
                             const std::string& input_name)
    {
        for (int i = current_node_idx - 1; i >= 0; --i)
        {
            const auto& outputs = graph.node(i).output();
            const auto output_found =
                std::any_of(outputs.begin(), outputs.end(), is_equal_to(input_name));

            if (output_found)
            {
                return i;
            }
        }

        return -1;
    }

    /// \brief Looks up a descriptor for a given tensor name. This descriptor contains inferred
    ///        shape information which is required to create new inputs and outputs in the graph.
    const ONNX_NAMESPACE::ValueInfoProto&
        find_tensor_descriptor(const ONNX_NAMESPACE::GraphProto& graph,
                               const std::string& tensor_name)
    {
        const auto it = std::find_if(
            graph.value_info().begin(),
            graph.value_info().end(),
            name_equals<ONNX_NAMESPACE::ValueInfoProto>(tensor_name, CheckIfHasName{}));

        NGRAPH_CHECK(it != graph.value_info().end(),
                     "Could not find a tensor descriptor for tensor '",
                     tensor_name,
                     "'. It's not possible to add a new input to the graph without the type and "
                     "shape information of the intermediate tensor.");

        return *it;
    }

    void replace_initializer_with_new_input(ONNX_NAMESPACE::GraphProto& graph,
                                            const InputEdge& edge,
                                            const std::string& new_input_name)
    {
        const auto it = std::find_if(graph.initializer().begin(),
                                     graph.initializer().end(),
                                     name_equals<ONNX_NAMESPACE::TensorProto>(edge.m_tensor_name));

        NGRAPH_CHECK(it != graph.initializer().end(),
                     "Could not find an initializer in the graph: '",
                     edge.m_tensor_name);

        const auto& initializer = *it;
        auto& new_input = *(graph.add_input());

        auto& new_input_tensor_type = *(new_input.mutable_type()->mutable_tensor_type());
        new_input_tensor_type.set_elem_type(initializer.data_type());

        auto& new_input_shape = *(new_input_tensor_type.mutable_shape());
        for (const auto initializer_dim : initializer.dims())
        {
            auto& new_dim = *(new_input_shape.add_dim());
            new_dim.set_dim_value(initializer_dim);
        }

        *(new_input.mutable_name()) = new_input_name;
    }

    std::pair<bool, InputEdge> append_new_graph_input(ONNX_NAMESPACE::GraphProto& graph,
                                                      const InputEdge& edge)
    {
        if (already_exists(graph.input(), edge.m_tensor_name) &&
            !is_graph_initializer(graph, edge.m_tensor_name))
        {
            // no need to append a new input if an edge points to an existing one in the model
            return {false, edge};
        }

        auto& target_node = *(graph.mutable_node(edge.m_node_idx));
        auto& node_inputs = *(target_node.mutable_input());
        auto target_input = std::find(node_inputs.begin(), node_inputs.end(), edge.m_tensor_name);

        NGRAPH_CHECK(target_input != node_inputs.end(),
                     "Input '",
                     edge.m_tensor_name,
                     "' not found in the inputs of node ",
                     edge.m_node_idx,
                     ". Cannot append a new graph input to this node.");

        const std::string new_input_name = target_node.output(0) + ":" + edge.m_tensor_name;

        if (is_graph_initializer(graph, edge.m_tensor_name))
        {
            replace_initializer_with_new_input(graph, edge, new_input_name);
        }
        else
        {
            auto& new_input = *(graph.add_input());
            // copy the intermediate tensor properties to the newly created input
            new_input.MergeFrom(find_tensor_descriptor(graph, edge.m_tensor_name));
            *(new_input.mutable_name()) = new_input_name;
        }

        // attach the new graph input to the target node's input
        *target_input = new_input_name;

        return {true, InputEdge{edge.m_node_idx, new_input_name}};
    }

    void append_new_graph_output(ONNX_NAMESPACE::GraphProto& graph, const OutputEdge& edge)
    {
        if (already_exists(graph.output(), edge.m_tensor_name))
        {
            return;
        }

        auto& target_node = *(graph.mutable_node(edge.m_node_idx));
        const auto& node_outputs = target_node.output();
        const auto target_output =
            std::find(node_outputs.begin(), node_outputs.end(), edge.m_tensor_name);

        NGRAPH_CHECK(target_output != node_outputs.end(),
                     "Output '",
                     edge.m_tensor_name,
                     "' not found in the outputs of node ",
                     edge.m_node_idx,
                     ". Cannot append a new graph output to this node.");

        auto& new_output = *(graph.add_output());
        // copy the intermediate tensor's properties to the newly created
        new_output.MergeFrom(find_tensor_descriptor(graph, edge.m_tensor_name));
        *(new_output.mutable_name()) = edge.m_tensor_name;
    }

    /// \brief Removes all items from a container except the ones whose names are in items_to_keep
    ///        It's intended to work with ONNX graph inputs and initializers only.
    template <typename Container>
    void discard_by_name(Container& all_items, const std::set<std::string>& items_to_keep)
    {
        static_assert(
            std::is_same<typename Container::value_type, ONNX_NAMESPACE::ValueInfoProto>::value ||
            std::is_same<typename Container::value_type, ONNX_NAMESPACE::TensorProto>::value);

        // The tested item can be discarded if its name is not found in the items_to_keep set
        const auto can_be_discarded = [&items_to_keep](const typename Container::value_type& item) {
            return items_to_keep.count(item.name()) == 0;
        };

        auto items_to_remove = all_items.size() - items_to_keep.size();

        // move the elements-to-discard to the end of the container
        auto new_end = all_items.end();
        while (items_to_remove > 0)
        {
            new_end = std::remove_if(all_items.begin(), new_end, can_be_discarded);

            --items_to_remove;
        }
        // erase all of the discarded elements past the new end of the container
        all_items.erase(new_end, all_items.end());
    }

    /// \brief Removes all nodes from a container keeping the ones whose index is in nodes_to_keep
    template <typename Container>
    void discard_nodes(Container& all_nodes, const std::set<int>& nodes_to_keep)
    {
        static_assert(
            std::is_same<typename Container::value_type, ONNX_NAMESPACE::NodeProto>::value);

        int idx = 0;
        const auto keep_node = [&nodes_to_keep, &idx](const typename Container::value_type&) {
            return nodes_to_keep.count(idx++) > 0;
        };

        // Stable partition rearranges the nodes keeping the relative order. This way the relative
        // ordering is preserved and all of the nodes to discard are moved after the returned iter.
        const auto new_end = std::stable_partition(all_nodes.begin(), all_nodes.end(), keep_node);
        all_nodes.erase(new_end, all_nodes.end());
    }
} // namespace

/* -----------------------------------------------------------------------------------------------*/

SubgraphExtractor::SubgraphExtractor(ONNX_NAMESPACE::GraphProto& graph)
    : m_onnx_graph{graph}
{
    for (int i = 0; i < graph.node_size(); ++i)
    {
        for (const auto& node_input : graph.node(i).input())
        {
            m_node_inputs.insert({i, node_input});
        }
    }
}

void SubgraphExtractor::add_new_inputs(const std::vector<InputEdge>& new_inputs)
{
    for (const auto& edge_to_replace : new_inputs)
    {
        validate_node_index(m_onnx_graph, edge_to_replace.m_node_idx);

        const auto& new_edge = append_new_graph_input(m_onnx_graph, edge_to_replace);
        if (new_edge.first)
        {
            // TODO: all nodes with this input should be updated? additional edges creation uc
            replace_input_edge(edge_to_replace, new_edge.second);
        }
    }
}

void SubgraphExtractor::add_new_outputs(const std::vector<OutputEdge>& new_outputs)
{
    for (const auto& new_output : new_outputs)
    {
        validate_node_index(m_onnx_graph, new_output.m_node_idx);

        append_new_graph_output(m_onnx_graph, new_output);
    }
}

void SubgraphExtractor::replace_input_edge(const InputEdge& old_edge, const InputEdge& new_edge)
{
    const auto node_inputs = m_node_inputs.equal_range(old_edge.m_node_idx);
    auto old_input_name = node_inputs.first;

    while (old_input_name->second != old_edge.m_tensor_name && old_input_name != node_inputs.second)
    {
        ++old_input_name;
    }

    m_node_inputs.erase(old_input_name);
    m_node_inputs.insert({new_edge.m_node_idx, new_edge.m_tensor_name});
}

void SubgraphExtractor::extract_subgraph(std::vector<OutputEdge> subgraph_outputs)
{
    if (subgraph_outputs.empty())
    {
        subgraph_outputs = all_output_edges();
    }

    SubgraphComponents subgraph;

    for (const auto& output_edge : subgraph_outputs)
    {
        subgraph += discover_output_contributors(output_edge, subgraph);
    }

    extract_subgraph_from_onnx_model(subgraph);
}

SubgraphExtractor::SubgraphComponents SubgraphExtractor::discover_output_contributors(
    const OutputEdge& output_edge, const SubgraphComponents& already_collected) const
{
    const auto already_visited = [&already_collected](const int node_index) {
        return already_collected.nodes.count(node_index) > 0;
    };

    SubgraphComponents output_contributors;
    output_contributors.outputs.insert(output_edge.m_tensor_name);

    // reverse DFS graph traversal
    std::stack<int> nodes_to_visit;
    nodes_to_visit.push(output_edge.m_node_idx);

    while (!nodes_to_visit.empty())
    {
        const auto n = nodes_to_visit.top();
        nodes_to_visit.pop();

        if (already_visited(n))
        {
            continue;
        }

        output_contributors.nodes.insert(n);

        // check if the visitor reached any of the graph inputs
        // and/or keep looking for more contributors further up in the graph
        const auto n_inputs = m_node_inputs.equal_range(n);
        for (auto input_name = n_inputs.first; input_name != n_inputs.second; ++input_name)
        {
            if (is_graph_input(m_onnx_graph, input_name->second))
            {
                output_contributors.inputs.insert(input_name->second);
                // when an initializer has a matching graph input
                if (is_graph_initializer(m_onnx_graph, input_name->second))
                {
                    output_contributors.initializers.insert(input_name->second);
                }
            }
            else if (is_graph_initializer(m_onnx_graph, input_name->second))
            {
                // when an initializer doesn't have a corresponding input
                output_contributors.initializers.insert(input_name->second);
            }
            else
            {
                nodes_to_visit.push(find_source_node_idx(m_onnx_graph, n, input_name->second));
            }
        }
    }

    return output_contributors;
}

void SubgraphExtractor::extract_subgraph_from_onnx_model(const SubgraphComponents& subgraph)
{
    discard_by_name(*(m_onnx_graph.mutable_input()), subgraph.inputs);
    discard_by_name(*(m_onnx_graph.mutable_initializer()), subgraph.initializers);
    discard_by_name(*(m_onnx_graph.mutable_output()), subgraph.outputs);
    discard_nodes(*(m_onnx_graph.mutable_node()), subgraph.nodes);
    m_onnx_graph.clear_value_info();
}

std::vector<OutputEdge> SubgraphExtractor::all_output_edges() const
{
    std::vector<OutputEdge> all_outputs;

    for (const auto& graph_output : m_onnx_graph.output())
    {
        all_outputs.emplace_back(
            find_source_node_idx(m_onnx_graph, m_onnx_graph.node_size(), graph_output.name()),
            graph_output.name());
    }

    return all_outputs;
}
