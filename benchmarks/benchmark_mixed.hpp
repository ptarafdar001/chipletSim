/**
 * @file benchmark_mixed.hpp
 * @brief Stratified 1021-task benchmark for HexaMesh cycle-accurate simulator.
 *
 * Task composition (of the 1019 non-load/write tasks):
 *   25% COMM_HEAVY   (~255 tasks): MEMORY_BOUND / IO_BOUND,  comm=[512,1024]
 *   40% COMP_HEAVY   (~408 tasks): COMPUTE_HEAVY,            exec=[800,2000], comm=[32,64]
 *   35% MIXED        (~357 tasks): COMPUTE_LIGHT/HEAVY,      exec=[200,600],  comm=[128,512]
 *
 * Graph topology (4-layer pipeline):
 *
 *   [load]
 *      ↓  (fan-out)
 *   [Layer 1: 255 COMM_HEAVY tasks]   ← high comm volume from load
 *      ↓  (each feeds 1-2 compute)
 *   [Layer 2: 408 COMP_HEAVY tasks]   ← heavy execution, light comm
 *      ↓  (pairs reduce into mixed)
 *   [Layer 3: 357 MIXED tasks]        ← moderate exec + moderate comm
 *      ↓  (fan-in)
 *   [write]
 *
 * Edges between layers also carry realistic comm volumes matching the
 * source task's communication profile so Algorithm 1 clustering
 * correctly identifies the COMM_HEAVY→COMP_HEAVY boundary as high-affinity.
 */

#pragma once

#include "../include/task.hpp"
#include "../include/types.hpp"

#include <memory>
#include <string>
#include <vector>
#include <random>
#include <algorithm>

inline BenchmarkInfo make_stratified_1K() {
    auto tg = std::make_unique<TaskGraph>();

    // ── RNGs per tier (separate seeds for reproducibility) ────
    std::mt19937 rng_comm(1001);
    std::mt19937 rng_comp(2002);
    std::mt19937 rng_mix (3003);
    std::mt19937 rng_edge(4004);

    // COMM_HEAVY tier: low exec, very high comm volume
    std::uniform_int_distribution<uint64_t> comm_exec(50,  150);    // short exec
    std::uniform_int_distribution<uint64_t> comm_vol (512, 1024);   // large xfer

    // COMP_HEAVY tier: long exec, minimal comm
    std::uniform_int_distribution<uint64_t> comp_exec(800, 2000);   // heavy exec
    std::uniform_int_distribution<uint64_t> comp_vol (32,  64);     // tiny xfer

    // MIXED tier: moderate exec + moderate comm
    std::uniform_int_distribution<uint64_t> mix_exec (200, 600);    // medium exec
    std::uniform_int_distribution<uint64_t> mix_vol  (128, 512);    // medium xfer

    // Cross-layer edge volumes (match source tier's comm profile)
    std::uniform_int_distribution<uint64_t> edge_comm_to_comp(512, 1024); // comm→comp
    std::uniform_int_distribution<uint64_t> edge_comp_to_mix (32,  64);   // comp→mix
    std::uniform_int_distribution<uint64_t> edge_mix_to_write(128, 512);  // mix→write

    const uint64_t DEADLINE = 10'000'000;
    int id = 0;

    // ── Target counts ─────────────────────────────────────────
    const int N_COMM = 255;   // 25%
    const int N_COMP = 408;   // 40%
    const int N_MIX  = 357;   // 35%
    // Total payload tasks: 1020, + load + write = 1022

    // ─────────────────────────────────────────────────────────
    // Task 0: Load
    // ─────────────────────────────────────────────────────────
    tg->add_task(id, "load",
                 TaskType::MEMORY_BOUND, Criticality::HIGH,
                 100, DEADLINE, 0);
    int load_id = id++;

    // ─────────────────────────────────────────────────────────
    // Layer 1: COMM_HEAVY tasks (25%)
    // Type:  MEMORY_BOUND (60%) + IO_BOUND (40%)
    // Crit:  HIGH (50%) + MEDIUM (50%)
    // Comm:  [512, 1024] bytes   ← makes C_hat high → COMM_HEAVY cluster
    // ─────────────────────────────────────────────────────────
    std::vector<int> comm_ids;
    for (int i = 0; i < N_COMM; ++i) {
        TaskType  ty   = (i % 5 < 3) ? TaskType::MEMORY_BOUND : TaskType::IO_BOUND;
        Criticality cr = (i % 2 == 0) ? Criticality::HIGH : Criticality::MEDIUM;

        tg->add_task(id,
                     "comm_" + std::to_string(i),
                     ty, cr, comm_exec(rng_comm), DEADLINE, 0);

        // Fan-out from load with high comm volume
        tg->add_edge(load_id, id, comm_vol(rng_comm));

        // Lateral edges: each comm task also reads from previous 2
        // (simulates streaming pipeline / ring buffer dependencies)
        if (i >= 1) tg->add_edge(comm_ids[i-1], id, comm_vol(rng_comm));
        if (i >= 2) tg->add_edge(comm_ids[i-2], id, comm_vol(rng_comm));

        comm_ids.push_back(id++);
    }

    // ─────────────────────────────────────────────────────────
    // Layer 2: COMP_HEAVY tasks (40%)
    // Type:  COMPUTE_HEAVY (100%)
    // Crit:  HIGH (30%) + MEDIUM (50%) + LOW (20%)
    // Comm:  [32, 64] bytes    ← low C_hat, high L_hat → pure compute cluster
    // Each comp task reads from ~2 comm tasks
    // ─────────────────────────────────────────────────────────
    std::vector<int> comp_ids;
    for (int i = 0; i < N_COMP; ++i) {
        Criticality cr;
        if      (i % 10 < 3) cr = Criticality::HIGH;
        else if (i % 10 < 8) cr = Criticality::MEDIUM;
        else                  cr = Criticality::LOW;

        tg->add_task(id,
                     "comp_" + std::to_string(i),
                     TaskType::COMPUTE_HEAVY, cr,
                     comp_exec(rng_comp), DEADLINE, 0);

        // Each comp reads from 2 comm tasks (strided mapping)
        int src0 = comm_ids[ i       % N_COMM];
        int src1 = comm_ids[(i + 13) % N_COMM];  // prime stride avoids hotspots
        tg->add_edge(src0, id, edge_comm_to_comp(rng_edge));
        if (src1 != src0)
            tg->add_edge(src1, id, edge_comm_to_comp(rng_edge));

        // Intra-layer dependency: every 4th task depends on previous
        // (simulates data-flow through compute pipeline stages)
        if (i > 0 && i % 4 == 0)
            tg->add_edge(comp_ids[i-1], id, comp_vol(rng_comp));

        comp_ids.push_back(id++);
    }

    // ─────────────────────────────────────────────────────────
    // Layer 3: MIXED tasks (35%)
    // Type:  COMPUTE_LIGHT (50%) + COMPUTE_HEAVY (50%)
    // Crit:  MEDIUM (70%) + HIGH (30%)
    // Comm:  [128, 512] bytes  ← moderate C_hat + L_hat → mixed cluster
    // Each mixed task reduces from ~2 comp tasks
    // ─────────────────────────────────────────────────────────
    std::vector<int> mix_ids;
    for (int i = 0; i < N_MIX; ++i) {
        TaskType    ty = (i % 2 == 0) ? TaskType::COMPUTE_LIGHT
                                       : TaskType::COMPUTE_HEAVY;
        Criticality cr = (i % 10 < 3) ? Criticality::HIGH : Criticality::MEDIUM;

        tg->add_task(id,
                     "mix_" + std::to_string(i),
                     ty, cr, mix_exec(rng_mix), DEADLINE, 0);

        // Each mixed task reads from 2 comp tasks (reduction pattern)
        int src0 = comp_ids[ i       % N_COMP];
        int src1 = comp_ids[(i + 17) % N_COMP];  // prime stride
        tg->add_edge(src0, id, edge_comp_to_mix(rng_edge));
        if (src1 != src0)
            tg->add_edge(src1, id, edge_comp_to_mix(rng_edge));

        // Lateral: mixed tasks form small reduction trees of depth 3
        if (i >= 3 && i % 3 == 0)
            tg->add_edge(mix_ids[i-3], id, mix_vol(rng_mix));

        mix_ids.push_back(id++);
    }

    // ─────────────────────────────────────────────────────────
    // Task N: Write (fan-in from all mixed tasks)
    // ─────────────────────────────────────────────────────────
    tg->add_task(id, "write",
                 TaskType::MEMORY_BOUND, Criticality::LOW,
                 100, DEADLINE, 0);
    for (int m : mix_ids)
        tg->add_edge(m, id, edge_mix_to_write(rng_edge));
    int write_id = id++;
    (void)write_id;

    // ── Summary ───────────────────────────────────────────────
    // id now = 1 (load) + 255 (comm) + 408 (comp) + 357 (mix) + 1 (write)
    //        = 1022 tasks
    int total = id;
    int pct_comm = (N_COMM * 100) / (N_COMM + N_COMP + N_MIX);
    int pct_comp = (N_COMP * 100) / (N_COMM + N_COMP + N_MIX);
    int pct_mix  = (N_MIX  * 100) / (N_COMM + N_COMP + N_MIX);

    return {
        "Stratified-1K",
        "1022-task stratified DAG | "
        "COMM=" + std::to_string(pct_comm) + "% "
        "COMP=" + std::to_string(pct_comp) + "% "
        "MIX="  + std::to_string(pct_mix)  + "% "
        "(" + std::to_string(total) + " tasks)",
        std::move(tg)
    };
}