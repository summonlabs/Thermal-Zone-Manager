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

#ifndef THERMAL_ZONE_MANAGER_COUPLING_HPP
#define THERMAL_ZONE_MANAGER_COUPLING_HPP

#include <cstdint>
#include <vector>

#include "thermal_zone_manager/errors.hpp"
#include "thermal_zone_manager/export.hpp"
#include "thermal_zone_manager/ids.hpp"
#include "thermal_zone_manager/limits.hpp"
#include "thermal_zone_manager/units.hpp"

namespace thermal_zone_manager {

// One directed thermal coupling: the fraction of the heat dissipated in
// source that is transported into sink. The coefficient is a bounded ratio in
// parts per million and is exact; there is no floating-point representation of
// a coupling anywhere in this runtime.
//
// The graph is a relation, not a hierarchy. Cycles are legal configuration and
// are handled by the bounded evaluation below rather than by rejection.
struct CouplingEdge {
  ZoneId source;
  ZoneId sink;
  PartsPerMillion coefficient;

  friend bool operator==(const CouplingEdge& a, const CouplingEdge& b) {
    return a.source == b.source && a.sink == b.sink && a.coefficient == b.coefficient;
  }
};

// A validated coupling graph over a fixed zone set.
//
// Invariants established by create() and never violated afterwards:
//   * every endpoint names a declared zone;
//   * no self coupling;
//   * no duplicate (source, sink) pair;
//   * every coefficient is inside [0, 1000000];
//   * the outgoing coefficients of one zone sum to at most 1000000;
//   * the incoming coefficients of one zone sum to at most 4000000;
//   * edges() is in ascending (source, sink) order and incoming() is in
//     ascending source order, so iteration is a total order.
class CouplingGraph {
 public:
  using EdgeList = std::vector<CouplingEdge>;

  // An empty graph over an empty zone set.
  CouplingGraph() = default;

  // Validates and canonicalises. zones must be sorted ascending and free of
  // duplicates; use make_zone_ids() or sort before calling.
  static Result<CouplingGraph> create(EdgeList edges, const std::vector<ZoneId>& zones);

  bool empty() const noexcept { return edges_.empty(); }
  std::size_t edge_count() const noexcept { return edges_.size(); }

  // Canonical ascending (source, sink) order.
  const EdgeList& edges() const noexcept { return edges_; }

  // Incoming edges of a zone in ascending source order. An unknown zone yields
  // the empty list rather than an invented edge.
  const EdgeList& incoming(ZoneId sink) const noexcept;

  // Sum of the coefficients leaving a zone, in parts per million.
  std::int64_t outgoing_total_ppm(ZoneId source) const noexcept;

  // Sum of the coefficients entering a zone, in parts per million.
  std::int64_t incoming_total_ppm(ZoneId sink) const noexcept;

  // The largest incoming total over all zones, in parts per million. This is
  // the contraction factor of the one-step propagation operator.
  std::int64_t contraction_ppm() const noexcept { return contraction_ppm_; }

  // True when every zone receives strictly less than unity of heat from its
  // neighbours, which is exactly the condition under which the propagation
  // series is bounded by a convergent geometric series.
  bool is_contractive() const noexcept { return contraction_ppm_ < kMaxOutgoingCouplingPpm; }

  // True when an edge with a non-zero coefficient runs from source to sink.
  bool has_edge(ZoneId source, ZoneId sink) const noexcept;

  // Zones with at least one incoming edge carrying a non-zero coefficient, in
  // ascending zone order.
  const std::vector<ZoneId>& coupled_sinks() const noexcept { return coupled_sinks_; }

  bool operator==(const CouplingGraph& other) const;

 private:
  struct SinkEdges {
    ZoneId sink;
    EdgeList from;
  };

  EdgeList edges_;
  std::vector<SinkEdges> incoming_;
  std::vector<ZoneId> coupled_sinks_;
  std::int64_t contraction_ppm_ = 0;
};

// Deterministic helper: sorts and removes duplicate zone handles.
TZM_API std::vector<ZoneId> canonical_zone_ids(std::vector<ZoneId> zones);

}  // namespace thermal_zone_manager

#endif  // THERMAL_ZONE_MANAGER_COUPLING_HPP
