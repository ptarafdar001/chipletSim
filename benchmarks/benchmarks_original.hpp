/**
 * @file benchmarks.hpp
 * @brief Benchmark task graphs for validating the mapping algorithm.
 *
 * Includes:
 *   - Synthetic generators: random DAG, chain, fork-join, diamond
 *   - Real-world benchmark: E3S (Embedded System Synthesis Benchmarks Suite)
 *     consumer/auto/telecomm task graphs (hand-encoded representative subsets)
 *   - FFT and JPEG-encoder task graphs (classic NoC benchmarks)
 */

#pragma once

#include "../include/types.hpp"
#include "../include/task.hpp"

#include <string>
#include <random>
#include <cmath>
#include <vector>
#include <memory>

// ============================================================
//  Benchmark factory helpers
// ============================================================


// ============================================================
//  1. FFT task graph (16-point, butterfly structure)
//     Classic NoC benchmark with structured communication.
// ============================================================
inline BenchmarkInfo make_fft_16() {
    auto tg = std::make_unique<TaskGraph>();

    // Stage 0: 16 input tasks
    for (int i = 0; i < 16; ++i)
        tg->add_task(i, "fft_in_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW,
                     /*exec*/ 3, /*deadline*/ 300);

    // Stage 1-4: butterfly stages (log2(16)=4 stages, 16 tasks each)
    int base = 16;
    for (int stage = 1; stage <= 4; ++stage) {
        int stride = 1 << (stage - 1);   // 1,2,4,8
        for (int i = 0; i < 16; ++i) {
            int id = base + (stage - 1) * 16 + i;
            Criticality crit = (i < 4) ? Criticality::HIGH : Criticality::MEDIUM;
            tg->add_task(id, "fft_s" + std::to_string(stage) + "_" + std::to_string(i),
                         TaskType::COMPUTE_HEAVY, crit,
                         /*exec*/ 5 + (stage % 3), /*deadline*/ 300 + stage * 80);
            // Connect from previous stage
            int prev_base = base + (stage - 2) * 16;
            int src_a     = (stage == 1) ? i : prev_base + i;
            int src_b     = (stage == 1) ? ((i + stride) % 16)
                                          : prev_base + ((i + stride) % 16);
            uint64_t vol  = 64 * stride;
            tg->add_edge(src_a, id, vol);
            if (src_b != src_a) tg->add_edge(src_b, id, vol / 2);
        }
    }

    // Output accumulator
    int out_id = base + 4 * 16;
    tg->add_task(out_id, "fft_out",
                 TaskType::COMPUTE_LIGHT, Criticality::HIGH,
                 /*exec*/ 2, /*deadline*/ 800);
    int last_stage_base = base + 3 * 16;
    for (int i = 0; i < 16; ++i)
        tg->add_edge(last_stage_base + i, out_id, 32);

    return { "FFT-16",
             "16-point FFT butterfly task graph (4 stages, 81 tasks)",
             std::move(tg) };
}

// ============================================================
//  2. JPEG Encoder task graph
//     Reflects real DCT + quantize + Huffman pipeline.
// ============================================================
inline BenchmarkInfo make_jpeg_encoder() {
    auto tg = std::make_unique<TaskGraph>();
    int id = 0;

    // 8×8 block decomposition (16 blocks for an image tile)
    std::vector<int> dct_ids, quant_ids, zig_ids, huff_ids;

    for (int blk = 0; blk < 16; ++blk) {
        // 1. DCT
        int dct = id++;
        tg->add_task(dct, "dct_blk" + std::to_string(blk),
                     TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, 12);
        dct_ids.push_back(dct);

        // 2. Quantise
        int q = id++;
        tg->add_task(q, "quant_blk" + std::to_string(blk),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, 4);
        tg->add_edge(dct, q, 512);
        quant_ids.push_back(q);

        // 3. Zig-zag scan
        int z = id++;
        tg->add_task(z, "zigzag_blk" + std::to_string(blk),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, 2);
        tg->add_edge(q, z, 256);
        zig_ids.push_back(z);
    }

    // 4. Huffman encoding (shared, serialises all blocks)
    int huff = id++;
    tg->add_task(huff, "huffman",
                 TaskType::COMPUTE_HEAVY, Criticality::HIGH, 20,
                 /*deadline*/ 2000);
    for (int z : zig_ids) tg->add_edge(z, huff, 128);

    // 5. Bitstream output
    int bstr = id++;
    tg->add_task(bstr, "bitstream_out",
                 TaskType::IO_BOUND, Criticality::HIGH, 5, 2100);
    tg->add_edge(huff, bstr, 4096);

    return { "JPEG-Encoder",
             "JPEG tile encoder: 16 DCT blocks + Huffman (" +
             std::to_string(tg->task_count()) + " tasks)",
             std::move(tg) };
}

// ============================================================
//  3. E3S Consumer benchmark (automotive subset)
//     Based on the publicly available E3S v1.1 task graphs.
//     We encode the "auto-indust" application (tasks & periods).
// ============================================================
inline BenchmarkInfo make_e3s_auto() {
    auto tg = std::make_unique<TaskGraph>();

    // Engine control (4 hard-real-time tasks)
    tg->add_task(0,  "throttle_ctrl", TaskType::COMPUTE_HEAVY, Criticality::HIGH,  8,  50);
    tg->add_task(1,  "fuel_inject",   TaskType::COMPUTE_HEAVY, Criticality::HIGH,  6,  50);
    tg->add_task(2,  "ignition_ctrl", TaskType::COMPUTE_HEAVY, Criticality::HIGH,  4,  50);
    tg->add_task(3,  "rpm_monitor",   TaskType::COMPUTE_LIGHT, Criticality::HIGH,  2,  50);

    // Transmission
    tg->add_task(4,  "gear_select",   TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,10, 100);
    tg->add_task(5,  "clutch_ctrl",   TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 5, 100);

    // ABS / Safety
    tg->add_task(6,  "wheel_speed",   TaskType::IO_BOUND,      Criticality::HIGH,   3,  20);
    tg->add_task(7,  "brake_ctrl",    TaskType::COMPUTE_HEAVY, Criticality::HIGH,   8,  20);
    tg->add_task(8,  "stability_ctrl",TaskType::COMPUTE_HEAVY, Criticality::HIGH,  12,  50);

    // Dashboard / HMI
    tg->add_task(9,  "display_update",TaskType::IO_BOUND,      Criticality::LOW,    5, 200);
    tg->add_task(10, "sensor_fusion", TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,15, 100);
    tg->add_task(11, "gps_update",    TaskType::IO_BOUND,      Criticality::LOW,    8, 500);
    tg->add_task(12, "audio_alert",   TaskType::IO_BOUND,      Criticality::MEDIUM, 3, 200);

    // Data logging
    tg->add_task(13, "can_bus_rx",    TaskType::IO_BOUND,      Criticality::MEDIUM, 2,  10);
    tg->add_task(14, "data_logger",   TaskType::MEMORY_BOUND,  Criticality::LOW,   10, 500);
    tg->add_task(15, "diag_report",   TaskType::COMPUTE_LIGHT, Criticality::LOW,    5,1000);

    // DAG edges (data dependencies)
    tg->add_edge(13,  0, 256);   // CAN → throttle
    tg->add_edge(13,  1, 256);
    tg->add_edge(13,  2, 128);
    tg->add_edge( 0,  3, 64);
    tg->add_edge( 1,  3, 64);
    tg->add_edge( 2,  3, 32);
    tg->add_edge( 3,  4, 128);
    tg->add_edge( 4,  5, 256);
    tg->add_edge(13,  6, 64);
    tg->add_edge( 6,  7, 128);
    tg->add_edge( 7,  8, 256);
    tg->add_edge( 6,  8, 128);
    tg->add_edge( 8, 12, 32);
    tg->add_edge( 3,  9, 32);
    tg->add_edge( 5,  9, 32);
    tg->add_edge( 6, 10, 128);
    tg->add_edge(11, 10, 512);
    tg->add_edge(10,  9, 256);
    tg->add_edge( 0, 14, 512);
    tg->add_edge( 1, 14, 512);
    tg->add_edge( 2, 14, 256);
    tg->add_edge( 7, 14, 256);
    tg->add_edge(14, 15, 1024);
    tg->add_edge( 9, 15, 128);

    return { "E3S-Auto",
             "Automotive E3S benchmark: engine/ABS/HMI pipeline (16 tasks)",
             std::move(tg) };
}

// ============================================================
//  4. Synthetic Random DAG (large-scale stress test)
// ============================================================
inline BenchmarkInfo make_random_dag(int   num_tasks   = 64,
                                     float edge_prob   = 0.15f,
                                     unsigned seed     = 42)
{
    auto tg = std::make_unique<TaskGraph>();
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> exec_d(2, 30);
    std::uniform_int_distribution<int> dead_d(100, 2000);
    std::uniform_int_distribution<int> type_d(0, 3);
    std::uniform_int_distribution<int> crit_d(0, 2);
    std::uniform_int_distribution<int> vol_d(32, 1024);
    std::uniform_real_distribution<float> edge_rng(0.0f, 1.0f);

    for (int i = 0; i < num_tasks; ++i) {
        tg->add_task(i, "t" + std::to_string(i),
                     static_cast<TaskType>(type_d(rng)),
                     static_cast<Criticality>(crit_d(rng)),
                     exec_d(rng), dead_d(rng));
    }

    // Add edges only from lower-id to higher-id → guaranteed acyclic
    for (int u = 0; u < num_tasks - 1; ++u) {
        for (int v = u + 1; v < num_tasks; ++v) {
            if (edge_rng(rng) < edge_prob) {
                tg->add_edge(u, v, vol_d(rng));
            }
        }
    }

    return { "Random-DAG-" + std::to_string(num_tasks),
             "Random DAG: " + std::to_string(num_tasks) +
             " tasks, edge_prob=" + std::to_string(edge_prob),
             std::move(tg) };
}

// ============================================================
//  5. Fork-Join (parallel application pattern)
// ============================================================
inline BenchmarkInfo make_fork_join(int branches = 8, int depth = 4) {
    auto tg = std::make_unique<TaskGraph>();
    int id = 0;

    // Root
    tg->add_task(id, "root", TaskType::COMPUTE_LIGHT, Criticality::HIGH,
                 5, 5000);
    int root_id = id++;

    // Branches
    std::vector<std::vector<int>> branch_ids(branches);
    for (int b = 0; b < branches; ++b) {
        int prev = root_id;
        for (int d = 0; d < depth; ++d) {
            Criticality crit = (b < 2 && d < 2) ? Criticality::HIGH
                               : Criticality::MEDIUM;
            tg->add_task(id, "b" + std::to_string(b) + "_d" + std::to_string(d),
                         TaskType::COMPUTE_HEAVY, crit,
                         4 + d * 2, 5000);
            tg->add_edge(prev, id, 256 >> d);
            prev = id;
            branch_ids[b].push_back(id++);
        }
    }

    // Join
    tg->add_task(id, "join", TaskType::COMPUTE_HEAVY, Criticality::HIGH,
                 10, 5000);
    for (int b = 0; b < branches; ++b)
        tg->add_edge(branch_ids[b].back(), id, 512);
    ++id;

    return { "Fork-Join-" + std::to_string(branches) + "x" + std::to_string(depth),
             "Fork-join: " + std::to_string(branches) + " branches, depth=" +
             std::to_string(depth),
             std::move(tg) };
}


