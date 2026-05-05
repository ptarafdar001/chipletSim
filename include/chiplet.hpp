/**
 * @file chiplet.hpp
 * @brief Core, Chiplet, and HexaMesh topology definitions.
 *
 * Topology follows HexaMesh (Iff et al., 2023):
 *   - Chiplets arranged in concentric hexagonal rings.
 *   - Interior chiplets have up to 6 neighbours; ring-0 (central) has 6.
 *   - Each chiplet contains a MESH_DIM × MESH_DIM grid of processing cores.
 *
 * hex_ring:
 *   Old start = (radius, -radius) — wrong axial position.
 *   The walker immediately stepped through (0,0), overwriting
 *   coord_to_id for the center chiplet. Result: center lost its
 *   coordinate mapping, ring-1 nodes never connected to it,
 *   every chiplet ended up with degree=2 (a plain cycle).
 *
 *   start = (radius, 0) + correct ring-perimeter walk directions.
 *   Expected degrees after fix (rings=1, 7 chiplets):
 *     chiplet 0 (ring 0, center) : degree 6
 *     chiplets 1-6 (ring 1)      : degree 3
 */

#pragma once

#include "types.hpp"

#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <cmath>
#include <stdexcept>
#include <numeric>
#include <algorithm>
#include <sstream>

// ============================================================
//  Processing Core  (leaf node of the hierarchy)
// ============================================================
struct Core {
    int        global_id    {-1};
    int        chiplet_id   {-1};
    int        local_id     {-1};
    int        row          {-1};
    int        col          {-1};

    CoreState  state        {CoreState::IDLE};
    TaskType   last_task_type{TaskType::COMPUTE_LIGHT};

    uint64_t   tasks_executed   {0};
    uint64_t   cycles_busy      {0};
    uint64_t   busy_until       {0};
    int        current_task_id  {-1};

    bool is_available(uint64_t current_cycle = 0) const {
        return state == CoreState::IDLE && busy_until <= current_cycle;
    }
    bool is_faulty() const { return state == CoreState::FAULTY; }

    double utilization(uint64_t total_cycles) const {
        return (total_cycles > 0)
               ? static_cast<double>(cycles_busy) / total_cycles
               : 0.0;
    }
};

// ============================================================
//  Chiplet
// ============================================================
struct Chiplet {
    int         id          {-1};
    int         ring        {-1};
    int         position    {-1};

    ChipletState state      {ChipletState::ACTIVE};

    std::vector<Core> cores;
    std::vector<int>  neighbours;

    uint64_t    total_exec_cycles  {0};
    int         tasks_hosted       {0};
    int         clusters_hosted    {0};

    int core_count() const { return static_cast<int>(cores.size()); }
    int degree()     const { return static_cast<int>(neighbours.size()); }

    int available_core_count(uint64_t current_cycle = 0) const {
        int cnt = 0;
        for (auto& c : cores)
            if (c.is_available(current_cycle)) ++cnt;
        return cnt;
    }

    int faulty_core_count() const {
        int cnt = 0;
        for (auto& c : cores)
            if (c.is_faulty()) ++cnt;
        return cnt;
    }

    double load_fraction(uint64_t current_cycle = 0) const {
        int busy = core_count() - available_core_count(current_cycle);
        return static_cast<double>(busy) / core_count();
    }

    Core* next_available_core(uint64_t current_cycle = 0,
                               TaskType last_type     = TaskType::COMPUTE_LIGHT) {
        for (auto& c : cores) {
            if (!c.is_available(current_cycle)) continue;
            if (last_type == TaskType::COMPUTE_HEAVY &&
                c.last_task_type == TaskType::COMPUTE_HEAVY)
                continue;
            return &c;
        }
        for (auto& c : cores)
            if (c.is_available(current_cycle)) return &c;
        return nullptr;
    }

    Core* replica_core(int exclude_local_id, uint64_t current_cycle = 0) {
        int excl_row = (exclude_local_id >= 0)
                       ? exclude_local_id / Constants::MESH_DIM : -1;
        for (auto& c : cores) {
            if (c.local_id == exclude_local_id) continue;
            if (!c.is_available(current_cycle)) continue;
            if (c.row != excl_row) return &c;
        }
        for (auto& c : cores) {
            if (c.local_id == exclude_local_id) continue;
            if (c.is_available(current_cycle)) return &c;
        }
        return nullptr;
    }

    void mark_core_faulty(int local_id) {
        if (local_id >= 0 && local_id < core_count()) {
            cores[local_id].state = CoreState::FAULTY;
            if (faulty_core_count() == core_count())
                state = ChipletState::OFFLINE;
            else
                state = ChipletState::DEGRADED;
        }
    }

    std::string summary() const {
        std::ostringstream oss;
        oss << "Chiplet[" << id << "] ring=" << ring
            << " deg=" << degree()
            << " cores=" << core_count()
            << " faulty=" << faulty_core_count()
            << " state=";
        switch(state){
            case ChipletState::ACTIVE:   oss<<"ACTIVE";   break;
            case ChipletState::DEGRADED: oss<<"DEGRADED"; break;
            case ChipletState::OFFLINE:  oss<<"OFFLINE";  break;
        }
        return oss.str();
    }
};

// ============================================================
//  HexaMesh Graph
// ============================================================
class HexaMesh {
public:
    explicit HexaMesh(int rings    = Constants::DEFAULT_RINGS,
                      int mesh_dim = Constants::MESH_DIM)
        : rings_(rings), mesh_dim_(mesh_dim)
    {
        build_topology();
    }

    int chiplet_count()  const { return static_cast<int>(chiplets_.size()); }
    int rings()          const { return rings_; }

    Chiplet& chiplet(int id)             { return chiplets_.at(id); }
    const Chiplet& chiplet(int id) const { return chiplets_.at(id); }

    std::vector<Chiplet>& chiplets()             { return chiplets_; }
    const std::vector<Chiplet>& chiplets() const { return chiplets_; }

    std::vector<int> chiplets_by_degree() const {
        std::vector<int> ids(chiplets_.size());
        std::iota(ids.begin(), ids.end(), 0);
        std::sort(ids.begin(), ids.end(), [&](int a, int b){
            return chiplets_[a].degree() > chiplets_[b].degree();
        });
        return ids;
    }

    std::vector<int> high_connectivity_chiplets() const {
        std::vector<int> out;
        for (auto& c : chiplets_)
            if (c.degree() == 6 && c.degree() == 4 && c.state != ChipletState::OFFLINE)
                out.push_back(c.id);
        return out;
    }

    std::vector<int> edge_chiplets() const {
        std::vector<int> out;
        for (auto& c : chiplets_)
            if (c.degree() == 3 && c.state != ChipletState::OFFLINE)
                out.push_back(c.id);
        if (out.empty()) {
            int min_deg = 9999;
            for (auto& c : chiplets_)
                if (c.state != ChipletState::OFFLINE)
                    min_deg = std::min(min_deg, c.degree());
            for (auto& c : chiplets_)
                if (c.degree() == min_deg && c.state != ChipletState::OFFLINE)
                    out.push_back(c.id);
        }
        return out;
    }

    int total_available_cores(uint64_t current_cycle = 0) const {
        int cnt = 0;
        for (auto& c : chiplets_)
            if (c.state != ChipletState::OFFLINE)
                cnt += c.available_core_count(current_cycle);
        return cnt;
    }

    double theoretical_diameter() const {
        int N = chiplet_count();
        return (1.0/3.0) * std::sqrt(12.0*N - 3.0) - 1.0;
    }

    void inject_faults(double prob, unsigned seed = 42) {
        srand(seed);
        for (auto& ch : chiplets_)
            for (auto& co : ch.cores)
                if (co.state != CoreState::FAULTY &&
                    (static_cast<double>(rand()) / RAND_MAX) < prob)
                    ch.mark_core_faulty(co.local_id);
    }

    void print_summary() const;

private:
    int rings_;
    int mesh_dim_;
    std::vector<Chiplet> chiplets_;
    int next_global_core_id_ {0};

    using AxialCoord = std::pair<int,int>;

    // ----------------------------------------------------------------
    // hex_ring
    //    dq[] = {-1, 0, 1, 1, 0,-1}
    //    dr[] = { 1, 1, 0,-1,-1, 0}
    //    → Side 0 step 0: pushes (r,-r), then moves to (r-1, -r+1)
    //      For r=1: pushes (1,-1), moves to (0,0)  ← CENTER COORD STOLEN
    //      coord_to_id[0][0] overwritten with ring-1 chiplet id.
    //      Center chiplet can no longer be found in neighbour wiring.
    //      All center↔ring connections missing → degree=2 for everyone.
    //
    //    Start at (radius, 0) — the easternmost spoke tip in axial coords.
    //    Walk directions traverse the ring perimeter without ever
    //    entering the interior. Verified for r=1,2,3.
    //
    //  Degree check (rings=1, 7 chiplets):
    //    chiplet 0 (center, ring 0) : degree 6  ✓
    //    chiplets 1-6 (ring 1)      : degree 3  ✓
    // ----------------------------------------------------------------
    static std::vector<AxialCoord> hex_ring(int radius) {
        std::vector<AxialCoord> ring_coords;
        if (radius == 0) { ring_coords.push_back({0,0}); return ring_coords; }

        // correct start point
        int q = radius, r = 0;

        // ring-perimeter walk directions (axial coords).
        // Each entry moves one step along one of the 6 ring sides.
        // These are the 6 axial neighbor directions, ordered so the
        // walk goes counterclockwise around the ring without stepping
        // inward toward (0,0).
        const int dq[] = {-1, -1,  0,  1,  1,  0};
        const int dr[] = { 1,  0, -1, -1,  0,  1};

        for (int side = 0; side < 6; ++side) {
            for (int step = 0; step < radius; ++step) {
                ring_coords.push_back({q, r});
                q += dq[side];
                r += dr[side];
            }
        }
        return ring_coords;
    }

    static double axial_distance(AxialCoord a, AxialCoord b) {
        int dq = a.first - b.first;
        int dr = a.second - b.second;
        return (std::abs(dq) + std::abs(dq+dr) + std::abs(dr)) / 2.0;
    }

    static const std::vector<AxialCoord>& hex_neighbours_delta() {
        static std::vector<AxialCoord> deltas = {
            {1,0},{-1,0},{0,1},{0,-1},{1,-1},{-1,1}
        };
        return deltas;
    }

    void build_topology() {
        std::unordered_map<int, std::unordered_map<int,int>> coord_to_id;

        int id = 0;
        for (int ring = 0; ring <= rings_; ++ring) {
            auto coords = hex_ring(ring);
            for (auto [q, r] : coords) {
                Chiplet ch;
                ch.id       = id;
                ch.ring     = ring;
                ch.position = static_cast<int>(chiplets_.size()) -
                              (ring == 0 ? 0 : id - (1 + 3*(ring-1)*ring));
                build_cores(ch);
                chiplets_.push_back(std::move(ch));
                coord_to_id[q][r] = id;
                ++id;
            }
        }

        // Connect neighbours using the now-correct coord_to_id map
        int cid = 0;
        for (int ring = 0; ring <= rings_; ++ring) {
            auto coords = hex_ring(ring);
            for (auto [q, r] : coords) {
                for (auto [dq, dr] : hex_neighbours_delta()) {
                    int nq = q + dq, nr = r + dr;
                    auto oit = coord_to_id.find(nq);
                    if (oit == coord_to_id.end()) continue;
                    auto iit = oit->second.find(nr);
                    if (iit == oit->second.end()) continue;
                    int nid = iit->second;
                    auto& nb = chiplets_[cid].neighbours;
                    if (std::find(nb.begin(), nb.end(), nid) == nb.end())
                        nb.push_back(nid);
                }
                ++cid;
            }
        }
    }

    void build_cores(Chiplet& ch) {
        int num_cores = mesh_dim_ * mesh_dim_;
        ch.cores.resize(num_cores);
        for (int i = 0; i < num_cores; ++i) {
            Core& co      = ch.cores[i];
            co.global_id  = next_global_core_id_++;
            co.chiplet_id = ch.id;
            co.local_id   = i;
            co.row        = i / mesh_dim_;
            co.col        = i % mesh_dim_;
            co.state      = CoreState::IDLE;
        }
    }
};

inline void HexaMesh::print_summary() const {
    printf("=== HexaMesh Topology ===\n");
    printf("  Rings         : %d\n", rings_);
    printf("  Chiplets      : %d\n", chiplet_count());
    printf("  Cores/chiplet : %d\n", mesh_dim_ * mesh_dim_);
    printf("  Total cores   : %d\n", chiplet_count() * mesh_dim_ * mesh_dim_);
    printf("  Theo. diameter: %.2f hops\n", theoretical_diameter());
    printf("\n  Chiplet details:\n");
    for (auto& ch : chiplets_) {
        printf("    %s\n", ch.summary().c_str());
    }
}
