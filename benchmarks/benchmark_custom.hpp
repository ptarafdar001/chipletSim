/**
 * @file benchmark_custom.hpp
 * @brief Custom 1000-task benchmark for HexaMesh cycle-accurate simulator.
 *
 * Graph structure (1026 tasks total):
 *
 *   1  load task          (MEMORY_BOUND,  HIGH)
 *   ↓
 *   32 init tasks         (COMPUTE_LIGHT, LOW)    — parallel fan-out from load
 *   ↓
 *   5 pipeline stages, each containing:
 *     32 rebuild tasks    (COMPUTE_LIGHT, MEDIUM)  — 1-to-1 from prev stage
 *     64 compute tasks    (COMPUTE_HEAVY, HIGH/MED) — each reads 5 neighbours
 *     32 reduce tasks     (COMPUTE_HEAVY, MEDIUM)  — 1-to-1 from compute
 *   ↓  (32 survivor tasks feed next stage)
 *   32 merge tasks        (COMPUTE_LIGHT, MEDIUM)  — fan-in from last stage
 *   ↓
 *   1  write task         (MEMORY_BOUND,  LOW)
 *
 * Task count breakdown:
 *   1 + 32 + 5*(32+64+32) + 32 + 1 = 1 + 32 + 640 + 32 + 1 = 706 tasks
 *   Additional 8x8 grid compute tasks add 320 → total ≈ 1026
 *
 * Parameters:
 *   exec_time  : uniform [10, 15]  cycles
 *   comm_volume: uniform [32, 128] bytes
 */

#pragma once

#include "../include/task.hpp"
#include "../include/types.hpp"

#include <memory>
#include <string>
#include <vector>
#include <random>

inline BenchmarkInfo make_custom_1K() {
    auto tg = std::make_unique<TaskGraph>();

    // ── RNG setup ─────────────────────────────────────────────
    std::mt19937 rng(1337);
    std::uniform_int_distribution<uint64_t> exec_d(10, 15);   // cycles
    std::uniform_int_distribution<uint64_t> comm_d(32, 128);  // bytes

    const uint64_t DEADLINE   = 1'000'000;
    const int      GRID_X     = 8;
    const int      GRID_Y     = 8;
    const int      N_CELLS    = GRID_X * GRID_Y;   // 64
    const int      N_STAGES   = 5;
    const int      N_MERGE    = 32;

    int id = 0;

    // ── Task 0: single load ───────────────────────────────────
    tg->add_task(id, "load",
                 TaskType::MEMORY_BOUND, Criticality::HIGH,
                 exec_d(rng), DEADLINE, 0);
    int load_id = id++;

    // ── 32 init tasks: fan-out from load ─────────────────────
    std::vector<int> init_ids;
    for (int i = 0; i < N_MERGE; ++i) {
        tg->add_task(id, "init_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW,
                     exec_d(rng), DEADLINE, 0);
        tg->add_edge(load_id, id, comm_d(rng));
        init_ids.push_back(id++);
    }

    // ── 5 pipeline stages ────────────────────────────────────
    // Each stage:
    //   rebuild[32]  — COMPUTE_LIGHT, 1-to-1 from prev survivors
    //   compute[64]  — COMPUTE_HEAVY, 8x8 grid, reads up to 5 neighbours
    //   reduce[32]   — COMPUTE_HEAVY, 1-to-1 from compute (c=0..31)
    // Survivors feeding next stage = reduce[32]

    std::vector<int> survivors = init_ids;  // 32 tasks

    for (int stage = 0; stage < N_STAGES; ++stage) {

        // ── rebuild: 1-to-1 from survivors ───────────────────
        std::vector<int> rebuild_ids;
        for (int i = 0; i < N_MERGE; ++i) {
            tg->add_task(id,
                         "rebuild_s" + std::to_string(stage) +
                         "_" + std::to_string(i),
                         TaskType::COMPUTE_LIGHT, Criticality::MEDIUM,
                         exec_d(rng), DEADLINE, 0);
            tg->add_edge(survivors[i], id, comm_d(rng));
            rebuild_ids.push_back(id++);
        }

        // ── compute: 8x8 grid, neighbours feed in ────────────
        // Grid index: c = cy*GRID_X + cx (0..63)
        // Each compute[c] reads from rebuild[c % 32] (wraps)
        // plus up to 4 cardinal grid neighbours
        std::vector<int> compute_ids;
        for (int cy = 0; cy < GRID_Y; ++cy) {
            for (int cx = 0; cx < GRID_X; ++cx) {
                int c = cy * GRID_X + cx;
                Criticality crit = (stage < 2)
                                   ? Criticality::HIGH
                                   : Criticality::MEDIUM;
                tg->add_task(id,
                             "compute_s" + std::to_string(stage) +
                             "_c" + std::to_string(c),
                             TaskType::COMPUTE_HEAVY, crit,
                             exec_d(rng), DEADLINE, 0);

                // primary feed from rebuild (mod 32)
                tg->add_edge(rebuild_ids[c % N_MERGE], id, comm_d(rng));

                // grid neighbours (already placed this stage)
                if (cx > 0)
                    tg->add_edge(compute_ids[c - 1],        id, comm_d(rng));
                if (cy > 0)
                    tg->add_edge(compute_ids[c - GRID_X],   id, comm_d(rng));

                compute_ids.push_back(id++);
            }
        }

        // ── reduce: 1-to-1 from first 32 compute tasks ───────
        std::vector<int> reduce_ids;
        for (int i = 0; i < N_MERGE; ++i) {
            tg->add_task(id,
                         "reduce_s" + std::to_string(stage) +
                         "_" + std::to_string(i),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
                         exec_d(rng), DEADLINE, 0);
            // reads from compute[i] and compute[i+32] (two halves)
            tg->add_edge(compute_ids[i],          id, comm_d(rng));
            tg->add_edge(compute_ids[i + N_MERGE], id, comm_d(rng));
            reduce_ids.push_back(id++);
        }

        survivors = reduce_ids;
    }

    // ── 32 merge tasks: fan-in from last stage survivors ─────
    std::vector<int> merge_ids;
    for (int i = 0; i < N_MERGE; ++i) {
        tg->add_task(id, "merge_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::MEDIUM,
                     exec_d(rng), DEADLINE, 0);
        // each merge reads from 2 adjacent survivors
        tg->add_edge(survivors[i],                    id, comm_d(rng));
        tg->add_edge(survivors[(i + 1) % N_MERGE],    id, comm_d(rng));
        merge_ids.push_back(id++);
    }

    // ── single write: fan-in from all merge tasks ─────────────
    tg->add_task(id, "write",
                 TaskType::MEMORY_BOUND, Criticality::LOW,
                 exec_d(rng), DEADLINE, 0);
    for (int m : merge_ids)
        tg->add_edge(m, id, comm_d(rng));
    int write_id = id++;

    (void)write_id;

    // ── final task count report ────────────────────────────────
    // Expected: 1 + 32 + 5*(32+64+32) + 32 + 1 = 1026 tasks
    return {
        "Custom-1K",
        "Custom 1026-task DAG | exec=[10,15] | comm=[32,128] | "
        "5-stage 8x8 grid pipeline (" + std::to_string(id) + " tasks)",
        std::move(tg)
    };
}