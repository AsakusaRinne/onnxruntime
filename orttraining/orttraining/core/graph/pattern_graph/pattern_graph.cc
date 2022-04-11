// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/core/graph/pattern_graph/pattern_graph.h"

using namespace ONNX_NAMESPACE;

namespace onnxruntime {
namespace training {

Node* PatternMatchResult::GetNodeByName(std::string const& node_name) const {
  auto res = matched_node_groups_.find(node_name);
  ORT_ENFORCE(res != matched_node_groups_.end(),
              "No target node has cooresponding name %s in pattern graph", node_name);
  return res->second.matched_node;
}

NodeArg* PatternMatchResult::GetInputByName(std::string const& arg_name) const {
  auto res = matched_input_groups_.find(arg_name);
  ORT_ENFORCE(res != matched_input_groups_.end(),
              "No args has cooresponding name %s in pattern graph", arg_name);
  return res->second.matched_input_arg;
}

Status PatternMatchResult::GetNodesWithCondition(InlinedVector<std::reference_wrapper<Node>>& filtered_nodes,
                                                 std::function<bool(std::string const&, MatchedNodeGroup&)> filter_func) {
  for (auto iter = matched_node_groups_.begin(); iter != matched_node_groups_.end(); ++iter) {
    if (filter_func(iter->first, iter->second)) {
      filtered_nodes.push_back(*GetNodeByName(iter->first));
    }
  }

  return Status::OK();
}

Status PatternGraph::TryMatch(Graph& target_graph, PatternMatchResult& res, const std::string& root_node) {
  Graph& pattern_graph = GetGraph();
  GraphViewer graph_viewer(target_graph);
  GraphViewer pattern_viewer(pattern_graph);
  const auto& graph_topology_list = graph_viewer.GetNodesInTopologicalOrder();
  const auto& pattern_topology_list = pattern_viewer.GetNodesInTopologicalOrder();

  Node* pattern_root = nullptr;  // Not real root, only a root specified by user
  if (pattern_topology_list.size() && root_node.empty()) {
    pattern_root = pattern_graph.GetNode(0);
  }
  for (auto node_index : pattern_topology_list) {
    auto* node = pattern_graph.GetNode(node_index);
    if (strcmp(node->Name().c_str(), root_node.c_str()) == 0) {
      pattern_root = node;
      break;
    }
  }
  if (!pattern_root) {
    return Status(common::ONNXRUNTIME, common::FAIL, "Pattern root was not found.");
  }

  for (auto node_index : graph_topology_list) {
    auto* node = target_graph.GetNode(node_index);
    res.Clear();
    std::unordered_set<const Node*> graph_path, pattern_path;
    std::unordered_map<const Node*, const Node*> path_map;
    if (FindMatchRecursively(node, pattern_root, graph_path, pattern_path, path_map, res, target_graph)) {
      std::unordered_map<std::string, MatchedNodeGroup> matches;
      matches.emplace(pattern_root->Name(), MatchedNodeGroup(target_graph.GetNode(node_index), name_pnode_mapping_[pattern_root->Name()]));
      res.AppendToNodeGroups(matches);
      return Status::OK();
    }
  }

  return Status(common::ONNXRUNTIME, common::FAIL, "No match for the target graph.");
}

/*
Try to find a match for all the args of a node. We just use brute force here to iterate all the possible cases.
Let's assume that args of the node in pattern graph are a1, a2, ..., ai, ... an and that of target graph is b1, b2, ..., bj, ... bn
Then we keep the order of {ai} fixed and try to match it with {bj}.
We first choose b1, then choose an arg in {bj | j != 1, j <= n} as next arg and the recurse it.
"visited" is used to record visited args to avoid duplicated conditions.
*/
bool FindMatchForArgs(const Graph& graph, PatternGraph& pattern_graph,
                      const std::unordered_map<std::string, const PGraphInput*>& name_pargs_mapping,
                      ConstPointerContainer<std::vector<NodeArg*>>& p_args,
                      ConstPointerContainer<std::vector<NodeArg*>>& t_args,
                      const std::unordered_map<std::string, std::unique_ptr<ArgCompareFunc>>& arg_constraints,
                      const std::unique_ptr<ArgCompareFunc>& arg_default_constraint,
                      size_t p_arg_idx,
                      std::unordered_set<const NodeArg*>& visited) {
  // TODO: constraint the order of the args
  if (p_arg_idx >= p_args.size()) return true;
  auto p_arg = p_args[p_arg_idx];
  PARSER_LOG << "Try to find match for arg " << p_arg->Name();
  auto p_arg_name = p_arg->Name();
  auto find_defined_arg = name_pargs_mapping.find(p_arg_name);
  if (find_defined_arg == name_pargs_mapping.end()) return FindMatchForArgs(graph, pattern_graph, name_pargs_mapping, p_args, t_args, arg_constraints, arg_default_constraint, p_arg_idx + 1, visited);
  const PGraphInput* p_arg_define = find_defined_arg->second;
  auto func = arg_constraints.count(p_arg_name) > 0 ? arg_constraints.find(p_arg_name)->second.get() : arg_default_constraint.get();
  for (auto t_arg : t_args) {
    if (!visited.count(t_arg)) {
      visited.insert(t_arg);
      if ((*func)(graph, t_arg, pattern_graph, p_arg_define) &&
          FindMatchForArgs(graph, pattern_graph, name_pargs_mapping, p_args, t_args, arg_constraints, arg_default_constraint, p_arg_idx + 1, visited)) {
        return true;
      } else {
        visited.erase(t_arg);
        continue;
      }
    }
  }
  return false;
};

/*
This function searches in the graph for a match with two given nodes as start.
"g" is the node we want to start from in target graph and "p" is that of pattern graph.
"graph_path", "pattern_path" are two set to record the visited nodes in target graph and pattern graph respectively. They are used to avoid duplicated
match.
"path_map" is a mapping from nodes in graph_path and its corresponding matched node in pattern graph. It's used to look ahead when the match encounters
visited nodes to avoid wrong match. We give a simple example below to indicate the condition and effect of "look ahead".

C <----------- B
|              ^
v              |
D ---> E1 <--- A
|
v
E2
|
v
F

We start search from A, then B, C and D and all these nodes find a match. Then we want to find match for E1 and E2, which are all the same except the position.
If we do not look ahead, we find E1 reach the end of recursion (A has been visited) and return true. Then We may actually match E1 with a node which should match E2.
So we need to look ahead to make sure that the visited node (A) E1 finds is exactly the matched nodes for that of target graph. In this way, we can make sure that
E1 and E2 are correctly matched.

"target" is the target graph which should be passed by the user.

The main idea of the algorithm is that if we regard node T (of target graph) and node P (of pattern graph) as a match, they must satisfy two requiments.

1. T and P have same properties, including optype, version, domain and so on.
2. All the neighbor nodes of P could find a match among neighbor nodes of T.

*/
bool PatternGraph::FindMatchRecursively(const Node* g, const Node* p,
                                        std::unordered_set<const Node*>& graph_path, std::unordered_set<const Node*>& pattern_path,
                                        std::unordered_map<const Node*, const Node*>& path_map,
                                        PatternMatchResult& matched, Graph& target) {
  PARSER_LOG << "jump in. g: " << g->Name() << "  p: " << p->Name();

  const std::string pnode_name = p->Name();
  const PGraphNode* pnode = name_pnode_mapping_[pnode_name];
  // Load customized function if there is one for the current node.
  auto func = custom_node_constraints_.count(pnode_name) > 0 ? custom_node_constraints_[pnode_name].get() : default_node_compare_func_.get();
  // Ensure that the two nodes have same properties.
  if (!(*func)(target, g, *this, pnode)) {
    PARSER_LOG << "return not matched properties.";
    return false;
  }

  // verify args
  auto p_args = p->InputDefs();
  auto t_args = g->InputDefs();
  std::unordered_set<const NodeArg*> visited_args;
  if (!FindMatchForArgs(target, *this, name_parg_mapping_, p_args, t_args, custom_arg_constraints_, default_arg_compare_func_, 0, visited_args)) {
    PARSER_LOG << "return not matched args.";
    return false;
  }

  // These two are used to record the path in the recursion to avoid repeated visit of a node.
  graph_path.insert(g);
  pattern_path.insert(p);
  // The map is just a mapping from nodes in "pattern_path" to nodes in "graph_path", which is used for "look_ahead".
  path_map[p] = g;

  auto look_ahead = [&path_map](const Node* cur_gnode, const Node* next_pnode, bool verify_input) {
    auto next_gnode = path_map[next_pnode];
    if (verify_input) {
      for (auto iter = cur_gnode->InputNodesBegin(); iter != cur_gnode->InputNodesEnd(); ++iter) {
        if (&(*iter) == next_gnode) return true;
      }
    } else {
      for (auto iter = cur_gnode->OutputNodesBegin(); iter != cur_gnode->OutputNodesEnd(); ++iter) {
        if (&(*iter) == next_gnode) return true;
      }
    }
    return false;
  };

  // A container to temporarily save matched results. If the total match succeed, it will be fully inserted into matched.
  // If not, it will be dropped.
  std::unordered_map<std::string, MatchedNodeGroup> matches_if_success;

  // Different from the two set above, these two are used to record the visited nodes in current process instead of nodes in recursion.
  std::unordered_set<const Node*> visited_graph_nodes;
  std::unordered_set<const Node*> visited_pattern_nodes;

  // try to find a full match for all the input nodes of the current node p in pattern graph
  // TODO: constraint the order of the input nodes?
  for (auto pin_iter = p->InputNodesBegin(); pin_iter != p->InputNodesEnd(); ++pin_iter) {
    const Node& cur = *pin_iter;  // one of the input node of p
    if (visited_pattern_nodes.count(&cur))
      continue;
    if (pattern_path.count(&cur)) {
      // look ahead when encounter a visited node
      if (look_ahead(g, &cur, true)) {
        continue;
      }
      pattern_path.erase(p);
      graph_path.erase(g);
      path_map.erase(p);
      return false;
    }
    bool has_matched_branch = false;
    // iterate all input nodes of g to find a match for cur
    for (auto gin_iter = g->InputNodesBegin(); gin_iter != g->InputNodesEnd(); ++gin_iter) {
      const Node& tar = *gin_iter;
      if (!graph_path.count(&tar) && !visited_graph_nodes.count(&tar) && FindMatchRecursively(&tar, &cur, graph_path, pattern_path, path_map, matched, target)) {
        has_matched_branch = true;
        matches_if_success.emplace(cur.Name(), MatchedNodeGroup(target.GetNode(tar.Index()), name_pnode_mapping_[cur.Name()]));
        visited_graph_nodes.insert(&tar);
        visited_pattern_nodes.insert(&cur);
        break;
      }
    }
    // If we cannot find match for every input nodes of p, we need to restore the scene
    if (!has_matched_branch) {
      PARSER_LOG << "return false because of no match input node match for " << cur.Name();
      pattern_path.erase(p);
      graph_path.erase(g);
      path_map.erase(p);
      return false;
    }
  }
  // try to find a full match for all the input nodes of the current node p in pattern graph, which is like a mirror symmetry of inputs matching.
  for (auto pout_iter = p->OutputNodesBegin(); pout_iter != p->OutputNodesEnd(); ++pout_iter) {
    const Node& cur = *pout_iter;
    if (visited_pattern_nodes.count(&cur))
      continue;
    if (pattern_path.count(&cur)) {
      if (look_ahead(g, &cur, false)) {
        continue;
      }
      pattern_path.erase(p);
      graph_path.erase(g);
      path_map.erase(p);
      return false;
    }
    bool has_matched_branch = false;
    for (auto gout_iter = g->OutputNodesBegin(); gout_iter != g->OutputNodesEnd(); ++gout_iter) {
      const Node& tar = *gout_iter;
      if (!graph_path.count(&tar) && !visited_graph_nodes.count(&tar) && FindMatchRecursively(&tar, &cur, graph_path, pattern_path, path_map, matched, target)) {
        has_matched_branch = true;
        matches_if_success.emplace(cur.Name(), MatchedNodeGroup(target.GetNode(tar.Index()), name_pnode_mapping_[cur.Name()]));
        visited_graph_nodes.insert(&tar);
        visited_pattern_nodes.insert(&cur);
        break;
      }
      graph_path.erase(&tar);
    }
    if (!has_matched_branch) {
      PARSER_LOG << "return false because of no match output node match for " << cur.Name();
      pattern_path.erase(p);
      graph_path.erase(g);
      path_map.erase(p);
      return false;
    }
  }

  // Reaching here means it has been matched successfully, the nodes will be collcted into result.
  matched.AppendToNodeGroups(matches_if_success);

  PARSER_LOG << "return true.";
  return true;
}

bool DefaultNodeCompareFunc::operator()(const Graph&, const Node* target_node,
                                        const PatternGraph& pattern_graph, const PGraphNode* pattern_node) const {
  if (!target_node && !pattern_node)
    return true;
  if (!target_node && pattern_node || !pattern_node && target_node)
    return false;
  if (!skip_op_type_ && !pattern_node->MatchesOpType(target_node->OpType())) {
    PARSER_LOG << "OpType mismatch, "
               << "target_node is: " << target_node->OpType();
    return false;
  }
  if (!skip_domain_and_version_ && !pattern_node->MatchesDomainVersion(target_node->Domain(), target_node->SinceVersion())) {
    PARSER_LOG << "Domain or Version mismatch, "
               << "target_node's domain is: " << target_node->Domain()
               << "target_node's version is: " << target_node->SinceVersion();
    return false;
  }

  if (pattern_node->output_edges_count_ == 0 && target_node->GetOutputEdgesCount() != pattern_graph.GetPatternGraphNode(pattern_node->node_name_)->GetOutputEdgesCount()) {
    PARSER_LOG << "Output edges count mismatch, "
               << "target_node is: " << target_node->SinceVersion();
    return false;
  } else if (pattern_node->output_edges_count_ > 0 && target_node->GetOutputEdgesCount() != static_cast<size_t>(pattern_node->output_edges_count_)) {
    PARSER_LOG << "Output edges count mismatch, "
               << "target_node is: " << target_node->SinceVersion();
    return false;
  }
  return true;
}

}  // namespace training
}  // namespace onnxruntime