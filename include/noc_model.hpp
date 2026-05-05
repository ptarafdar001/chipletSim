/**
 * @file noc_model.hpp
 * @brief Cycle-accurate NoC routing and latency model for HexaMesh.
 *
 * This is the central piece that makes the simulator cycle-accurate.
 * It computes the precise number of cycles required to transfer data
 * from a source core to a destination core, accounting for:
 *
 *   1. Intra-chiplet routing  (4×4 mesh, XY routing, 2 cycles/hop)
 *   2. Chiplet-to-chiplet routing (HexaMesh BFS shortest path, 27 cycles/hop)
 *   3. Serialization latency  (ceil(volume / PHIT_BYTES) cycles per link)
 *   4. Per-link bandwidth contention (link occupancy tracking)
 *
 * Usage:
 *   NoCModel noc(hex_mesh);
 *   uint64_t lat = noc.transfer_latency(src_chiplet, src_core_local,
 *                                        dst_chiplet, dst_core_local,
 *                                        volume_bytes, current_cycle);
 *
 * The returned latency is the number of cycles from when the first
 * flit is injected until the last flit is accepted by the destination.
 */

#pragma once

#include "types.hpp"
#include "chiplet.hpp"

#include <vector>
#include <queue>
#include <unordered_map>
#include <algorithm>
#include <cmath>

// ============================================================
//  Link occupancy tracker for bandwidth contention
//
//  A link (src_chiplet→dst_chiplet) can carry PHIT_BYTES per cycle.
//  We model it as "busy_until" — the cycle when it becomes free.
//  Serialization of a transfer of V bytes takes ceil(V/PHIT_BYTES) cycles.
// ============================================================
struct LinkState {
    uint64_t busy_until {0};   // link free after this cycle
};

// ============================================================
//  NoCModel
// ============================================================
class NoCModel {
public:
    explicit NoCModel(const HexaMesh& mesh) : mesh_(mesh) {
        build_shortest_paths();
    }

    // ── Primary API ───────────────────────────────────────────────────────
    /**
     * Compute the communication latency (cycles) for transferring `volume`
     * bytes from (src_chiplet, src_core) to (dst_chiplet, dst_core),
     * assuming the transfer starts at `inject_cycle`.
     *
     * Also books the links involved, advancing their busy_until for
     * subsequent transfers on the same links.
     *
     * @return  Number of cycles until last byte arrives at destination.
     *          This is added to `inject_cycle` by the caller to get the
     *          absolute cycle when the successor's input data is available.
     */
    uint64_t transfer_latency(int      src_chiplet,
                               int      src_core_local,
                               int      dst_chiplet,
                               int      dst_core_local,
                               uint64_t volume,
                               uint64_t inject_cycle)
    {
        if (volume == 0) return 0;

        uint64_t total_lat = 0;

        // ── Step 1: Intra-chiplet routing on source side ──────────────
        //   Source core → chiplet edge router.
        //   4×4 XY mesh: hop count = |Δrow| + |Δcol| to reach router (0,0).
        if (src_chiplet == dst_chiplet) {
            // Same chiplet: only intra-chiplet hops
            int src_r, src_c, dst_r, dst_c;
            local_core_coords(src_core_local, src_r, src_c);
            local_core_coords(dst_core_local, dst_r, dst_c);
            int hops = std::abs(src_r - dst_r) + std::abs(src_c - dst_c);
            uint64_t ser  = serialization_cycles(volume);
            // Each hop: INTRA_CHIPLET_HOP cycle pipeline + serialization
            total_lat = static_cast<uint64_t>(hops) * Constants::INTRA_CHIPLET_HOP + ser;
            return total_lat;
        }

        // ── Step 2: Intra-chiplet egress hops (src core → edge) ──────
        int sr, sc, dr, dc;
        local_core_coords(src_core_local, sr, sc);
        // Egress: go to top-left router (0,0) = worst-case XY distance
        int egress_hops = sr + sc;   // XY from (row,col) to (0,0)
        uint64_t ser = serialization_cycles(volume);
        total_lat += static_cast<uint64_t>(egress_hops) * Constants::INTRA_CHIPLET_HOP + ser;

        // ── Step 3: Inter-chiplet hops (chiplet BFS shortest path) ───
        int chiplet_hops = shortest_path_hops(src_chiplet, dst_chiplet);
        // Each inter-chiplet hop: INTER_CHIPLET_HOP + serialization per link
        // Model bandwidth contention on each link:
        uint64_t link_avail = inject_cycle + total_lat;
        for (int h = 0; h < chiplet_hops; ++h) {
            auto key = link_key(src_chiplet + h, dst_chiplet);  // simplified key
            auto& ls  = link_state_[key];
            // Must wait if link is busy
            link_avail = std::max(link_avail, ls.busy_until);
            // This transfer occupies the link for ser cycles
            ls.busy_until = link_avail + ser + Constants::INTER_CHIPLET_HOP;
            // Hop transit time
            link_avail += Constants::INTER_CHIPLET_HOP + ser;
        }
        // Inter-chiplet contribution:
        total_lat = link_avail - inject_cycle;

        // ── Step 4: Intra-chiplet ingress hops (edge → dst core) ─────
        local_core_coords(dst_core_local, dr, dc);
        int ingress_hops = dr + dc;
        total_lat += static_cast<uint64_t>(ingress_hops) * Constants::INTRA_CHIPLET_HOP;

        return total_lat;
    }

    /**
     * Convenience: compute latency for a transfer where src/dst chiplets and
     * cores are already known, without booking links (read-only query).
     */
    uint64_t transfer_latency_no_contention(int      src_chiplet,
                                             int      src_core_local,
                                             int      dst_chiplet,
                                             int      dst_core_local,
                                             uint64_t volume) const
    {
        if (volume == 0) return 0;

        if (src_chiplet == dst_chiplet) {
            int sr, sc, dr, dc;
            local_core_coords(src_core_local, sr, sc);
            local_core_coords(dst_core_local, dr, dc);
            int hops = std::abs(sr - dr) + std::abs(sc - dc);
            return static_cast<uint64_t>(hops) * Constants::INTRA_CHIPLET_HOP
                   + serialization_cycles(volume);
        }

        int sr, sc, dr, dc;
        local_core_coords(src_core_local, sr, sc);
        local_core_coords(dst_core_local, dr, dc);

        int egress_hops  = sr + sc;
        int chiplet_hops = shortest_path_hops(src_chiplet, dst_chiplet);
        int ingress_hops = dr + dc;
        uint64_t ser     = serialization_cycles(volume);

        return static_cast<uint64_t>(egress_hops)  * Constants::INTRA_CHIPLET_HOP
             + static_cast<uint64_t>(chiplet_hops) * (Constants::INTER_CHIPLET_HOP + ser)
             + static_cast<uint64_t>(ingress_hops) * Constants::INTRA_CHIPLET_HOP;
    }

    // ── Query helpers ─────────────────────────────────────────────────────
    int shortest_path_hops(int src, int dst) const {
        if (src == dst) return 0;
        auto it = dist_.find(src);
        if (it == dist_.end()) return 99;
        auto jt = it->second.find(dst);
        return (jt != it->second.end()) ? jt->second : 99;
    }

    // Reset link contention state (call once per benchmark run)
    void reset() { link_state_.clear(); }

private:
    const HexaMesh& mesh_;

    // BFS shortest-path distances between every chiplet pair
    std::unordered_map<int, std::unordered_map<int,int>> dist_;

    // Link contention: key = (src_chiplet * 1000 + dst_chiplet), directional
    std::unordered_map<int64_t, LinkState> link_state_;

    // ── Build all-pairs BFS distances ────────────────────────────────────
    void build_shortest_paths() {
        int N = mesh_.chiplet_count();
        for (int s = 0; s < N; ++s) {
            auto& d = dist_[s];
            d[s] = 0;
            std::queue<int> q;
            q.push(s);
            while (!q.empty()) {
                int u = q.front(); q.pop();
                for (int v : mesh_.chiplet(u).neighbours) {
                    if (!d.count(v)) {
                        d[v] = d[u] + 1;
                        q.push(v);
                    }
                }
            }
        }
    }

    // Convert local core id → (row, col) inside the MESH_DIM×MESH_DIM grid
    static void local_core_coords(int local_id, int& row, int& col) {
        row = local_id / Constants::MESH_DIM;
        col = local_id % Constants::MESH_DIM;
    }

    // Serialization: ceil(volume / PHIT_BYTES) cycles
    static uint64_t serialization_cycles(uint64_t volume) {
        return (volume + Constants::PHIT_BYTES - 1) / Constants::PHIT_BYTES;
    }

    // Directional link key (ordered pair of chiplet ids)
    static int64_t link_key(int src, int dst) {
        return static_cast<int64_t>(src) * 100000LL + dst;
    }
};
