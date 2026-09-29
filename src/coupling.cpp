// Copyright 2026 Summon Software Labs
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

#include "thermal_zone_manager/coupling.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "thermal_zone_manager/limits.hpp"
#include "validation.hpp"

namespace thermal_zone_manager {
namespace {

const CouplingGraph::EdgeList& empty_edges() {
  static const CouplingGraph::EdgeList instance;
  return instance;
}

}  // namespace

namespace detail {

Status validate_coupling_numeric(const std::vector<CouplingEdge>& edges) {
  for (const CouplingEdge& edge : edges) {
    if (edge.coefficient.value < 0 || edge.coefficient.value > kPpmScale) {
      return Error(ErrorCode::InvalidCoefficient,
                   "a coupling coefficient is outside the closed unit interval")
          .with("source", edge.source.raw())
          .with("sink", edge.sink.raw())
          .with("coefficient_ppm", static_cast<std::int64_t>(edge.coefficient.value))
          .with("max", static_cast<std::int64_t>(kPpmScale));
    }
  }
  return Status();
}

}  // namespace detail

std::vector<ZoneId> canonical_zone_ids(std::vector<ZoneId> zones) {
  std::sort(zones.begin(), zones.end());
  zones.erase(std::unique(zones.begin(), zones.end()), zones.end());
  return zones;
}

Result<CouplingGraph> CouplingGraph::create(EdgeList edges, const std::vector<ZoneId>& zones) {
  if (edges.size() > kMaxCouplingEdges) {
    return Error(ErrorCode::TooManyCouplingEdges, "the coupling graph exceeds its edge ceiling")
        .with("edges", static_cast<std::uint64_t>(edges.size()))
        .with("limit", static_cast<std::uint64_t>(kMaxCouplingEdges));
  }

  const auto known = [&zones](ZoneId id) {
    return std::binary_search(zones.begin(), zones.end(), id);
  };

  // Every coefficient is checked before the graph is ordered, so a malformed
  // coefficient is reported as a numeric fault rather than as an ordering
  // artefact.
  for (const CouplingEdge& edge : edges) {
    if (edge.coefficient.value < 0 || edge.coefficient.value > kPpmScale) {
      return Error(ErrorCode::InvalidCoefficient,
                   "a coupling coefficient is outside the closed unit interval")
          .with("coefficient_ppm", static_cast<std::int64_t>(edge.coefficient.value))
          .with("max", static_cast<std::int64_t>(kPpmScale));
    }
  }
  for (const CouplingEdge& edge : edges) {
    if (!known(edge.source) || !known(edge.sink)) {
      return Error(ErrorCode::UnknownCouplingEndpoint,
                   "a coupling edge names a zone that is not declared")
          .with("source", edge.source.raw())
          .with("sink", edge.sink.raw());
    }
    if (edge.source == edge.sink) {
      return Error(ErrorCode::SelfCoupling, "a zone may not couple to itself")
          .with("zone", edge.source.raw());
    }
  }

  std::sort(edges.begin(), edges.end(), [](const CouplingEdge& a, const CouplingEdge& b) {
    if (a.source != b.source) {
      return a.source < b.source;
    }
    return a.sink < b.sink;
  });
  for (std::size_t index = 1; index < edges.size(); ++index) {
    if (edges[index - 1].source == edges[index].source &&
        edges[index - 1].sink == edges[index].sink) {
      return Error(ErrorCode::DuplicateCouplingEdge,
                   "a coupling pair may only be declared once")
          .with("source", edges[index].source.raw())
          .with("sink", edges[index].sink.raw());
    }
  }

  // Outgoing budget: a zone cannot hand more than all of its heat to its
  // neighbours.
  for (std::size_t index = 0; index < edges.size();) {
    const ZoneId source = edges[index].source;
    std::int64_t total = 0;
    while (index < edges.size() && edges[index].source == source) {
      total += static_cast<std::int64_t>(edges[index].coefficient.value);
      ++index;
    }
    if (total > kMaxOutgoingCouplingPpm) {
      return Error(ErrorCode::CouplingBudgetExceeded,
                   "the outgoing coupling of a zone exceeds unity")
          .with("source", source.raw())
          .with("outgoing_ppm", total)
          .with("limit", kMaxOutgoingCouplingPpm);
    }
  }

  CouplingGraph graph;
  graph.edges_ = std::move(edges);

  // Build the incoming index in ascending (sink, source) order. The canonical
  // edge list is ordered by source, so a second ordering is required here.
  {
    EdgeList by_sink = graph.edges_;
    std::sort(by_sink.begin(), by_sink.end(), [](const CouplingEdge& a, const CouplingEdge& b) {
      if (a.sink != b.sink) {
        return a.sink < b.sink;
      }
      return a.source < b.source;
    });
    for (const CouplingEdge& edge : by_sink) {
      if (graph.incoming_.empty() || graph.incoming_.back().sink != edge.sink) {
        graph.incoming_.push_back(SinkEdges{edge.sink, EdgeList()});
      }
      graph.incoming_.back().from.push_back(edge);
    }
  }

  graph.contraction_ppm_ = 0;
  for (const SinkEdges& entry : graph.incoming_) {
    std::int64_t total = 0;
    bool carries_heat = false;
    for (const CouplingEdge& edge : entry.from) {
      total += static_cast<std::int64_t>(edge.coefficient.value);
      carries_heat = carries_heat || edge.coefficient.value > 0;
    }
    if (total > kMaxIncomingCouplingPpm) {
      return Error(ErrorCode::CouplingBudgetExceeded,
                   "the incoming coupling of a zone exceeds its ceiling")
          .with("sink", entry.sink.raw())
          .with("incoming_ppm", total)
          .with("limit", kMaxIncomingCouplingPpm);
    }
    if (total > graph.contraction_ppm_) {
      graph.contraction_ppm_ = total;
    }
    if (carries_heat) {
      graph.coupled_sinks_.push_back(entry.sink);
    }
  }

  return graph;
}

const CouplingGraph::EdgeList& CouplingGraph::incoming(ZoneId sink) const noexcept {
  const auto found = std::lower_bound(
      incoming_.begin(), incoming_.end(), sink,
      [](const SinkEdges& entry, ZoneId value) { return entry.sink < value; });
  if (found == incoming_.end() || found->sink != sink) {
    return empty_edges();
  }
  return found->from;
}

std::int64_t CouplingGraph::outgoing_total_ppm(ZoneId source) const noexcept {
  std::int64_t total = 0;
  for (const CouplingEdge& edge : edges_) {
    if (edge.source == source) {
      total += static_cast<std::int64_t>(edge.coefficient.value);
    } else if (edge.source > source) {
      break;
    }
  }
  return total;
}

std::int64_t CouplingGraph::incoming_total_ppm(ZoneId sink) const noexcept {
  const EdgeList& list = incoming(sink);
  std::int64_t total = 0;
  for (const CouplingEdge& edge : list) {
    total += static_cast<std::int64_t>(edge.coefficient.value);
  }
  return total;
}

bool CouplingGraph::has_edge(ZoneId source, ZoneId sink) const noexcept {
  const EdgeList& list = incoming(sink);
  return std::any_of(list.begin(), list.end(), [source](const CouplingEdge& edge) {
    return edge.source == source && edge.coefficient.value > 0;
  });
}

bool CouplingGraph::operator==(const CouplingGraph& other) const {
  if (edges_.size() != other.edges_.size()) {
    return false;
  }
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    if (!(edges_[index] == other.edges_[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace thermal_zone_manager
