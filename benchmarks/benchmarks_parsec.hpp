/**
 * @file benchmarks_parsec.hpp
 * @brief PARSEC benchmark suite – full task-graph implementations
 *
 * All 10 PARSEC apps derived from the official PARSEC GitHub source.
 * Run configurations and metadata are in benchmarks/parsec_benchmarks.json
 * (generated from the .runconf files in the parsec_apps source tree).
 *
 * Benchmarks implemented
 * ──────────────────────
 *  Original 5 (full-scale):
 *    Blackscholes-64   – financial option pricing      (fork-join)
 *    Bodytrack-32      – computer vision tracking      (pipeline + particle filter)
 *    Canneal-64        – chip placement via SA         (irregular)
 *    Streamcluster-128 – online k-means clustering     (streaming)
 *    Swaptions-32      – Monte Carlo swaption pricing  (fork-join)
 *
 *  Added from PARSEC source run configs:
 *    Ferret-50         – content-based image search    (6-stage pipeline)
 *    Fluidanimate-35K  – SPH fluid simulation          (iterative grid)
 *    Freqmine-250K     – frequent itemset mining       (irregular FP-tree)
 *    Raytrace-480x270  – 3-D ray tracing               (embarrassingly parallel)
 *    Vips-1600x1200    – image processing pipeline     (multi-stage tiled)
 *    X264-640x360      – H.264 video encoding          (frame-parallel pipeline)
 *    Facesim-sim       – physics-based face animation  (time-stepped FEM)
 */

#pragma once

#include "../include/types.hpp"
#include "../include/task.hpp"

#include <memory>
#include <random>
#include <string>
#include <vector>

namespace PARSEC {

// ══════════════════════════════════════════════════════════════════════════
// 1. Blackscholes-64
//    Source: parsec_apps/apps/blackscholes/parsec/simsmall.runconf
//    Args:   bin/blackscholes ${NTHREADS} in_4K.txt prices.txt
//    Pattern: fork-join  |  64 options × 64 MC paths = 4096 pricing tasks
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_blackscholes_64() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_options = 64, mc_paths = 64;
    int id = 0;

    std::mt19937 rng(100);
    std::uniform_int_distribution<> exec_d(2000, 4000); // micro second
    std::uniform_int_distribution<> comm_d(32, 128);

    tg->add_task(id, "load_market_data", TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
    int load = id++;

    std::vector<int> rng_tasks;
    for (int i = 0; i < mc_paths; ++i) {
        tg->add_task(id, "rng_stream_" + std::to_string(i), TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 300, 100000);
        tg->add_edge(load, id, 64);
        rng_tasks.push_back(id++);
    }

    std::vector<std::vector<int>> pricing(n_options);
    for (int opt = 0; opt < n_options; ++opt)
        for (int path = 0; path < mc_paths; ++path) {
            tg->add_task(id, "price_opt" + std::to_string(opt) + "_path" + std::to_string(path),
                         TaskType::COMPUTE_HEAVY, opt < 8 ? Criticality::HIGH : Criticality::MEDIUM,
                         exec_d(rng), 100000);
            tg->add_edge(rng_tasks[path], id, comm_d(rng));
            pricing[opt].push_back(id++);
        }

    std::vector<int> reduce;
    for (int opt = 0; opt < n_options; ++opt) {
        tg->add_task(id, "reduce_opt_" + std::to_string(opt), TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 100, 100000);
        for (int pt : pricing[opt]) 
            tg->add_edge(pt, id, 32);
        reduce.push_back(id++);
    }

    tg->add_task(id, "final_aggregate", TaskType::COMPUTE_LIGHT, Criticality::LOW, 150, 100000);
    for (int r : reduce) tg->add_edge(r, id, 64);
    ++id;

    return { "Blackscholes-64",
             "Black-Scholes 64 options, " + std::to_string(mc_paths) +
             " MC paths (" + std::to_string(id) + " tasks)", std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 2. Bodytrack-32
//    Source: parsec_apps/apps/bodytrack/parsec/simsmall.runconf
//    Args:   bin/bodytrack sequenceB_1 4 1 1000 5 0 ${NTHREADS}
//    Pattern: pipeline + particle filter  |  32 frames × 3 stages × 33 particles
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_bodytrack_32() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_frames = 32, n_particles = 33;
    int id = 0;

    std::mt19937 rng(200);
    std::uniform_int_distribution<> ex_det(800, 1500), ex_trk(500, 1200), ex_ref(600, 2000);
    std::uniform_int_distribution<> comm_d(256, 1024);

    tg->add_task(id, "init_camera_model", TaskType::COMPUTE_LIGHT, Criticality::LOW, 300, 100000);
    int init = id++;

    std::vector<std::vector<int>> prev_refine;

    for (int fr = 0; fr < n_frames; ++fr) {
        std::vector<int> det, trk, ref;
        for (int p = 0; p < n_particles; ++p) {
            tg->add_task(id, "detect_f" + std::to_string(fr) + "_p" + std::to_string(p),
                         TaskType::COMPUTE_HEAVY, fr < 4 ? Criticality::HIGH : Criticality::MEDIUM,
                         ex_det(rng), 100000);
            tg->add_edge(fr == 0 ? init : prev_refine[fr-1][p % n_particles], id,
                         fr == 0 ? 1024 : comm_d(rng));
            det.push_back(id++);
        }
        for (int p = 0; p < n_particles; ++p) {
            tg->add_task(id, "track_f" + std::to_string(fr) + "_p" + std::to_string(p),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, ex_trk(rng), 100000);
            tg->add_edge(det[p], id, comm_d(rng));
            trk.push_back(id++);
        }
        for (int p = 0; p < n_particles; ++p) {
            tg->add_task(id, "refine_f" + std::to_string(fr) + "_p" + std::to_string(p),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, ex_ref(rng), 100000);
            tg->add_edge(trk[p], id, comm_d(rng));
            ref.push_back(id++);
        }
        prev_refine.push_back(ref);

        tg->add_task(id, "resample_f" + std::to_string(fr),
                     TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 200, 100000);
        for (int r : ref) tg->add_edge(r, id, 64);
        ++id;
    }

    tg->add_task(id, "output_poses", TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
    for (auto& fp : prev_refine) for (int r : fp) tg->add_edge(r, id, 32);
    ++id;

    return { "Bodytrack-32",
             "Particle filter tracking " + std::to_string(n_frames) + " frames, " +
             std::to_string(n_particles) + " particles (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 3. Canneal-64
//    Source: parsec_apps/apps/canneal (no explicit simsmall.runconf – uses pthreads)
//    Pattern: irregular SA  |  64 elements × 320 SA iterations
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_canneal_64() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_elems = 64, iters = 320;
    int id = 0;

    std::mt19937 rng(300);
    std::uniform_int_distribution<> exec_d(1000, 10000);
    std::uniform_int_distribution<> comm_d(128, 512);

    tg->add_task(id, "load_netlist", TaskType::COMPUTE_LIGHT, Criticality::LOW, 500, 100000);
    int load = id++;

    std::vector<int> placement;
    for (int i = 0; i < n_elems; ++i) {
        tg->add_task(id, "init_place_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::HIGH, 300, 100000);
        tg->add_edge(load, id, 256);
        placement.push_back(id++);
    }

    for (int it = 0; it < iters; ++it) {
        std::vector<int> cur;
        for (int e = 0; e < n_elems; ++e) {
            tg->add_task(id, "anneal_i" + std::to_string(it) + "_e" + std::to_string(e),
                         TaskType::COMPUTE_HEAVY, it < 10 ? Criticality::HIGH : Criticality::MEDIUM,
                         exec_d(rng), 100000);
            int nd = 1 + (e % 5);
            for (int d = 0; d < nd; ++d)
                tg->add_edge(placement[(e + d * 13) % n_elems], id, comm_d(rng));
            cur.push_back(id++);
        }
        placement = cur;
    }

    tg->add_task(id, "eval_final_cost", TaskType::COMPUTE_HEAVY, Criticality::LOW, 1000, 100000);
    for (int e : placement) tg->add_edge(e, id, comm_d(rng));
    ++id;

    return { "Canneal-64",
             "Simulated annealing " + std::to_string(n_elems) + " elements, " +
             std::to_string(iters) + " iters (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 4. Streamcluster-128
//    Source: parsec_apps/apps/streamcluster/parsec/simsmall.runconf
//    Args:   bin/streamcluster 10 20 128 16384 16384 1000 none output.txt ${NTHREADS}
//    Pattern: streaming k-means  |  8 batches × 10 k-means iterations
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_streamcluster_128() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_pts = 128, batch_sz = 16, n_batches = n_pts / batch_sz, km_iters = 10;
    int id = 0;

    std::mt19937 rng(400);
    std::uniform_int_distribution<> exec_d(300, 1500);
    std::uniform_int_distribution<> comm_d(512, 2048);

    tg->add_task(id, "init_centers", TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
    int prev_merge = id++;

    for (int b = 0; b < n_batches; ++b) {
        std::vector<int> pts;
        for (int p = 0; p < batch_sz; ++p) {
            tg->add_task(id, "load_b" + std::to_string(b) + "_p" + std::to_string(p),
                         TaskType::COMPUTE_LIGHT, Criticality::LOW, 100, 100000);
            tg->add_edge(prev_merge, id, 256);
            pts.push_back(id++);
        }

        std::vector<int> prev = pts;
        for (int it = 0; it < km_iters; ++it) {
            std::vector<int> assign;
            for (int p = 0; p < batch_sz; ++p) {
                tg->add_task(id, "assign_b" + std::to_string(b) + "_i" + std::to_string(it) +
                             "_p" + std::to_string(p),
                             TaskType::COMPUTE_HEAVY, b < 2 ? Criticality::HIGH : Criticality::MEDIUM,
                             exec_d(rng), 100000);
                tg->add_edge(prev[p % prev.size()], id, comm_d(rng));
                assign.push_back(id++);
            }
            tg->add_task(id, "update_b" + std::to_string(b) + "_i" + std::to_string(it),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_d(rng), 100000);
            for (int a : assign) tg->add_edge(a, id, comm_d(rng));
            int upd = id++;

            tg->add_task(id, "check_conv_b" + std::to_string(b) + "_i" + std::to_string(it),
                         TaskType::COMPUTE_LIGHT, Criticality::LOW, 100, 100000);
            tg->add_edge(upd, id, 128);
            prev = { id++ };
        }

        tg->add_task(id, "merge_batch_" + std::to_string(b),
                     TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_d(rng), 100000);
        for (int p : prev) tg->add_edge(p, id, comm_d(rng));
        prev_merge = id++;
    }

    tg->add_task(id, "output_clusters", TaskType::COMPUTE_LIGHT, Criticality::LOW, 150, 100000);
    tg->add_edge(prev_merge, id, 512);
    ++id;

    return { "Streamcluster-128",
             "Online k-means " + std::to_string(n_pts) + " pts, " +
             std::to_string(n_batches) + " batches (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 5. Swaptions-32
//    Source: parsec_apps/apps/swaptions/parsec/simsmall.runconf
//    Args:   bin/swaptions -ns 16 -sm 10000 -nt ${NTHREADS}
//    Pattern: fork-join Monte Carlo  |  32 swaptions × 256 MC paths
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_swaptions_32() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_swap = 32, mc_paths = 256;
    int id = 0;

    std::mt19937 rng(500);
    std::uniform_int_distribution<> exec_d(3000, 8000);
    std::uniform_int_distribution<> comm_d(64, 256);

    tg->add_task(id, "load_market_data", TaskType::COMPUTE_LIGHT, Criticality::LOW, 400, 100000);
    int load = id++;
    tg->add_task(id, "build_yield_curve", TaskType::COMPUTE_HEAVY, Criticality::HIGH, 1000, 100000);
    tg->add_edge(load, id, 512);
    int yield = id++;

    std::vector<int> paths;
    for (int i = 0; i < mc_paths; ++i) {
        tg->add_task(id, "gen_path_" + std::to_string(i),
                     TaskType::COMPUTE_HEAVY, Criticality::HIGH, 1500, 100000);
        tg->add_edge(yield, id, 256);
        paths.push_back(id++);
    }

    std::vector<int> reduce;
    for (int s = 0; s < n_swap; ++s) {
        std::vector<int> priced;
        for (int p = 0; p < mc_paths; ++p) {
            tg->add_task(id, "price_swap" + std::to_string(s) + "_path" + std::to_string(p),
                         TaskType::COMPUTE_HEAVY, s < 4 ? Criticality::HIGH : Criticality::MEDIUM,
                         exec_d(rng), 100000);
            tg->add_edge(paths[p], id, comm_d(rng));
            priced.push_back(id++);
        }
        tg->add_task(id, "reduce_swap_" + std::to_string(s),
                     TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 200, 100000);
        for (int pt : priced) tg->add_edge(pt, id, 64);
        reduce.push_back(id++);
    }

    tg->add_task(id, "aggregate_portfolio", TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
    for (int r : reduce) tg->add_edge(r, id, 128);
    ++id;

    return { "Swaptions-32",
             "Monte Carlo swaptions " + std::to_string(n_swap) + " x " +
             std::to_string(mc_paths) + " paths (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 6. Ferret-50
//    Source: parsec_apps/apps/ferret/parsec/simsmall.runconf
//    Args:   bin/ferret corel lsh queries 10 20 ${NTHREADS} output.txt
//    Pattern: 6-stage pipeline  |  50 queries (segment→extract→vec→rank)
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_ferret_50() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_queries = 50;
    int id = 0;

    std::mt19937 rng(600);
    std::uniform_int_distribution<> exec_seg(400, 800);
    std::uniform_int_distribution<> exec_ext(600, 1200);
    std::uniform_int_distribution<> exec_vec(300, 700);
    std::uniform_int_distribution<> exec_rank(500, 1500);
    std::uniform_int_distribution<> comm_d(128, 512);

    tg->add_task(id, "load_db", TaskType::MEMORY_BOUND, Criticality::HIGH, 1000, 100000);
    int db = id++;
    tg->add_task(id, "build_lsh_index", TaskType::COMPUTE_HEAVY, Criticality::HIGH, 2000, 100000);
    tg->add_edge(db, id, 2048);
    int lsh = id++;

    std::vector<int> rank_tasks;
    for (int q = 0; q < n_queries; ++q) {
        tg->add_task(id, "segment_q" + std::to_string(q),
                     TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_seg(rng), 100000);
        tg->add_edge(lsh, id, comm_d(rng));
        int seg = id++;

        tg->add_task(id, "extract_q" + std::to_string(q),
                     TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_ext(rng), 100000);
        tg->add_edge(seg, id, comm_d(rng));
        int ext = id++;

        tg->add_task(id, "vectorize_q" + std::to_string(q),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, exec_vec(rng), 100000);
        tg->add_edge(ext, id, comm_d(rng));
        int vec = id++;

        tg->add_task(id, "rank_q" + std::to_string(q),
                     TaskType::COMPUTE_HEAVY, q < 5 ? Criticality::HIGH : Criticality::MEDIUM,
                     exec_rank(rng), 100000);
        tg->add_edge(vec, id, comm_d(rng));
        tg->add_edge(lsh, id, 256);
        rank_tasks.push_back(id++);
    }

    tg->add_task(id, "merge_results", TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
    for (int r : rank_tasks) tg->add_edge(r, id, 64);
    ++id;

    return { "Ferret-50",
             "Content-based image search " + std::to_string(n_queries) +
             " queries, 6-stage pipeline (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 7. Fluidanimate-35K
//    Source: parsec_apps/apps/fluidanimate/parsec/simsmall.runconf
//    Args:   bin/fluidanimate ${NTHREADS} 5 in_35K.fluid out.fluid
//    Pattern: iterative SPH grid  |  5 timesteps, 8x8 cell grid
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_fluidanimate_35K() {
    auto tg = std::make_unique<TaskGraph>();
    const int grid_x = 8, grid_y = 8, n_cells = grid_x * grid_y, n_steps = 5;
    int id = 0;

    std::mt19937 rng(700);
    std::uniform_int_distribution<> exec_d(50, 100);
    std::uniform_int_distribution<> comm_d(256, 1024);

    tg->add_task(id, "load_fluid", TaskType::MEMORY_BOUND, Criticality::HIGH, 500, 100000);
    int load = id++;

    std::vector<int> cells;
    for (int c = 0; c < n_cells; ++c) {
        tg->add_task(id, "init_cell_" + std::to_string(c),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
        tg->add_edge(load, id, 512);
        cells.push_back(id++);
    }

    for (int step = 0; step < n_steps; ++step) {
        std::vector<int> binned;
        for (int c = 0; c < n_cells; ++c) {
            tg->add_task(id, "rebuild_s" + std::to_string(step) + "_c" + std::to_string(c),
                         TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 200, 100000);
            tg->add_edge(cells[c], id, comm_d(rng));
            binned.push_back(id++);
        }

        std::vector<int> density;
        for (int cy = 0; cy < grid_y; ++cy) {
            for (int cx = 0; cx < grid_x; ++cx) {
                int c = cy * grid_x + cx;
                tg->add_task(id, "density_s" + std::to_string(step) + "_c" + std::to_string(c),
                             TaskType::COMPUTE_HEAVY, step < 2 ? Criticality::HIGH : Criticality::MEDIUM,
                             exec_d(rng), 100000);
                tg->add_edge(binned[c], id, comm_d(rng));
                if (cx > 0)        tg->add_edge(binned[c-1],       id, comm_d(rng));
                if (cx < grid_x-1) tg->add_edge(binned[c+1],       id, comm_d(rng));
                if (cy > 0)        tg->add_edge(binned[c-grid_x],  id, comm_d(rng));
                if (cy < grid_y-1) tg->add_edge(binned[c+grid_x],  id, comm_d(rng));
                density.push_back(id++);
            }
        }

        std::vector<int> next_cells;
        for (int c = 0; c < n_cells; ++c) {
            tg->add_task(id, "force_s" + std::to_string(step) + "_c" + std::to_string(c),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_d(rng), 100000);
            tg->add_edge(density[c], id, comm_d(rng));
            next_cells.push_back(id++);
        }
        cells = next_cells;
    }

    tg->add_task(id, "write_output", TaskType::MEMORY_BOUND, Criticality::LOW, 300, 100000);
    for (int c : cells) tg->add_edge(c, id, 256);
    ++id;

    return { "Fluidanimate-35K",
             "SPH fluid sim 35K particles, 8x8 grid, " + std::to_string(n_steps) +
             " timesteps (" + std::to_string(id) + " tasks)", std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 8. Freqmine-250K
//    Source: parsec_apps/apps/freqmine/parsec/simsmall.runconf
//    Args:   bin/freqmine kosarak_250k.dat 220
//    Pattern: irregular FP-tree  |  32 partitions, depth-4 conditional mining
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_freqmine_250K() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_parts = 32, mine_depth = 4;
    int id = 0;

    std::mt19937 rng(800);
    std::uniform_int_distribution<> exec_d(500, 3000);
    std::uniform_int_distribution<> comm_d(128, 1024);

    tg->add_task(id, "scan_db", TaskType::MEMORY_BOUND, Criticality::HIGH, 2000, 100000);
    int scan = id++;
    tg->add_task(id, "sort_freq_items", TaskType::COMPUTE_HEAVY, Criticality::HIGH, 800, 100000);
    tg->add_edge(scan, id, 1024);
    int sort_t = id++;

    std::vector<int> trees;
    for (int p = 0; p < n_parts; ++p) {
        tg->add_task(id, "build_fptree_p" + std::to_string(p),
                     TaskType::COMPUTE_HEAVY, p < 4 ? Criticality::HIGH : Criticality::MEDIUM,
                     exec_d(rng), 100000);
        tg->add_edge(sort_t, id, comm_d(rng));
        trees.push_back(id++);
    }

    std::vector<int> prev = trees;
    for (int depth = 0; depth < mine_depth; ++depth) {
        std::vector<int> mined;
        for (int p = 0; p < n_parts; ++p) {
            tg->add_task(id, "mine_d" + std::to_string(depth) + "_p" + std::to_string(p),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_d(rng), 100000);
            tg->add_edge(prev[p], id, comm_d(rng));
            if (p > 0 && (p % 4) == 0) tg->add_edge(prev[p-1], id, comm_d(rng));
            mined.push_back(id++);
        }
        prev = mined;
    }

    tg->add_task(id, "merge_itemsets", TaskType::COMPUTE_LIGHT, Criticality::LOW, 400, 100000);
    for (int p : prev) tg->add_edge(p, id, 256);
    int merge = id++;

    tg->add_task(id, "write_output", TaskType::MEMORY_BOUND, Criticality::LOW, 200, 100000);
    tg->add_edge(merge, id, 512);
    ++id;

    return { "Freqmine-250K",
             "FP-tree mining 250K transactions, " + std::to_string(n_parts) +
             " partitions, depth-" + std::to_string(mine_depth) +
             " (" + std::to_string(id) + " tasks)", std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 9. Raytrace-480x270
//    Source: parsec_apps/apps/raytrace/parsec/simsmall.runconf
//    Args:   bin/rtview happy_buddha.obj -automove -nthreads ${NTHREADS} -frames 3 -res 480 270
//    Pattern: embarrassingly parallel  |  3 frames, 40 tiles/frame
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_raytrace_480x270() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_frames = 3, tiles_x = 8, tiles_y = 5, n_tiles = tiles_x * tiles_y;
    int id = 0;

    std::mt19937 rng(900);
    std::uniform_int_distribution<> exec_d(1500, 5000);
    std::uniform_int_distribution<> comm_d(512, 2048);

    tg->add_task(id, "load_scene", TaskType::MEMORY_BOUND, Criticality::HIGH, 800, 100000);
    int scene = id++;
    tg->add_task(id, "build_bvh", TaskType::COMPUTE_HEAVY, Criticality::HIGH, 3000, 100000);
    tg->add_edge(scene, id, 4096);
    int bvh = id++;

    std::vector<int> frame_composites;
    for (int fr = 0; fr < n_frames; ++fr) {
        tg->add_task(id, "update_camera_f" + std::to_string(fr),
                     TaskType::COMPUTE_LIGHT, Criticality::HIGH, 100, 100000);
        tg->add_edge(bvh, id, 128);
        if (fr > 0) tg->add_edge(frame_composites.back(), id, 64);
        int cam = id++;

        std::vector<int> tiles;
        for (int t = 0; t < n_tiles; ++t) {
            tg->add_task(id, "render_f" + std::to_string(fr) + "_t" + std::to_string(t),
                         TaskType::COMPUTE_HEAVY, t < 8 ? Criticality::HIGH : Criticality::MEDIUM,
                         exec_d(rng), 100000);
            tg->add_edge(cam, id, 256);
            tg->add_edge(bvh, id, comm_d(rng));
            tiles.push_back(id++);
        }

        tg->add_task(id, "composite_f" + std::to_string(fr),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, 200, 100000);
        for (int t : tiles) tg->add_edge(t, id, comm_d(rng));
        frame_composites.push_back(id++);
    }

    tg->add_task(id, "write_frames", TaskType::MEMORY_BOUND, Criticality::LOW, 400, 100000);
    for (int f : frame_composites) tg->add_edge(f, id, 1024);
    ++id;

    return { "Raytrace-480x270",
             "PARSEC ray tracing " + std::to_string(n_frames) + " frames, 480x270, " +
             std::to_string(n_tiles) + " tiles/frame (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 10. Vips-1600x1200
//     Source: parsec_apps/apps/vips/parsec/simsmall.runconf
//     Args:   bin/vips im_benchmark pomegranate_1600x1200.v output.v
//     Pattern: tiled image pipeline  |  5 stages, 192 tiles
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_vips_1600x1200() {
    auto tg = std::make_unique<TaskGraph>();
    const int tiles_x = 16, tiles_y = 12, n_tiles = tiles_x * tiles_y;
    int id = 0;

    std::mt19937 rng(1000);
    std::uniform_int_distribution<> comm_d(256, 1024);

    tg->add_task(id, "load_image", TaskType::MEMORY_BOUND, Criticality::HIGH, 600, 100000);
    int load = id++;

    std::vector<int> prev;
    for (int t = 0; t < n_tiles; ++t) {
        tg->add_task(id, "tile_load_" + std::to_string(t),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, 100, 100000);
        tg->add_edge(load, id, 512);
        prev.push_back(id++);
    }

    struct Stage { std::string name; int lo, hi; Criticality crit; bool stencil; };
    std::vector<Stage> stages = {
        { "shrink", 200,  400, Criticality::HIGH,   false },
        { "affine",  400, 1000, Criticality::HIGH,   false },
        { "conv1",   200,  600, Criticality::MEDIUM, true  },
        { "gamma",   100,  300, Criticality::LOW,    false },
        { "conv2",   200,  600, Criticality::MEDIUM, true  },
    };

    for (auto& stg : stages) {
        std::uniform_int_distribution<> exec_s(stg.lo, stg.hi);
        std::vector<int> cur;
        for (int t = 0; t < n_tiles; ++t) {
            tg->add_task(id, stg.name + "_t" + std::to_string(t),
                         TaskType::COMPUTE_HEAVY, stg.crit, exec_s(rng), 100000);
            tg->add_edge(prev[t], id, comm_d(rng));
            if (stg.stencil) {
                int tx = t % tiles_x, ty = t / tiles_x;
                if (tx > 0)        tg->add_edge(prev[t-1],       id, 128);
                if (tx < tiles_x-1)tg->add_edge(prev[t+1],       id, 128);
                if (ty > 0)        tg->add_edge(prev[t-tiles_x], id, 128);
                if (ty < tiles_y-1)tg->add_edge(prev[t+tiles_x], id, 128);
            }
            cur.push_back(id++);
        }
        prev = cur;
    }

    tg->add_task(id, "write_image", TaskType::MEMORY_BOUND, Criticality::LOW, 400, 100000);
    for (int t : prev) tg->add_edge(t, id, comm_d(rng));
    ++id;

    return { "Vips-1600x1200",
             "VIPS image pipeline 1600x1200, " + std::to_string(stages.size()) +
             " stages, " + std::to_string(n_tiles) + " tiles (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 11. X264-640x360
//     Source: parsec_apps/apps/x264/parsec/simsmall.runconf
//     Args:   bin/x264 --threads ${NTHREADS} ... eledream_640x360_8.y4m
//     Pattern: frame-parallel H.264 pipeline  |  8 frames, 220 MBs/frame
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_x264_640x360() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_frames = 8, mb_x = 20, mb_y = 11, n_mbs = mb_x * mb_y;
    int id = 0;

    std::mt19937 rng(1100);
    std::uniform_int_distribution<> exec_intra(200, 500);
    std::uniform_int_distribution<> exec_me(400, 1200);
    std::uniform_int_distribution<> exec_dct(100, 300);
    std::uniform_int_distribution<> comm_d(64, 512);

    tg->add_task(id, "init_encoder", TaskType::COMPUTE_LIGHT, Criticality::HIGH, 200, 100000);
    int enc = id++;

    std::vector<int> prev_dct;

    for (int fr = 0; fr < n_frames; ++fr) {
        tg->add_task(id, "ref_list_f" + std::to_string(fr),
                     TaskType::COMPUTE_LIGHT, Criticality::HIGH, 100, 100000);
        tg->add_edge(enc, id, 128);
        if (fr > 0) tg->add_edge(prev_dct.back(), id, 256);
        int ref = id++;

        std::vector<int> intra;
        for (int mb = 0; mb < n_mbs; ++mb) {
            tg->add_task(id, "intra_f" + std::to_string(fr) + "_mb" + std::to_string(mb),
                         TaskType::COMPUTE_HEAVY, fr == 0 ? Criticality::HIGH : Criticality::MEDIUM,
                         exec_intra(rng), 100000);
            tg->add_edge(ref, id, 64);
            intra.push_back(id++);
        }

        std::vector<int> me_tasks;
        if (fr > 0) {
            for (int mb = 0; mb < n_mbs; ++mb) {
                tg->add_task(id, "me_f" + std::to_string(fr) + "_mb" + std::to_string(mb),
                             TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_me(rng), 100000);
                tg->add_edge(intra[mb], id, comm_d(rng));
                tg->add_edge(prev_dct[mb % prev_dct.size()], id, comm_d(rng));
                me_tasks.push_back(id++);
            }
        } else {
            me_tasks = intra;
        }

        std::vector<int> dct_tasks;
        for (int mb = 0; mb < n_mbs; ++mb) {
            tg->add_task(id, "dct_f" + std::to_string(fr) + "_mb" + std::to_string(mb),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_dct(rng), 100000);
            tg->add_edge(me_tasks[mb], id, comm_d(rng));
            dct_tasks.push_back(id++);
        }

        tg->add_task(id, "entropy_f" + std::to_string(fr),
                     TaskType::COMPUTE_HEAVY, Criticality::HIGH, 500, 100000);
        for (int dt : dct_tasks) tg->add_edge(dt, id, 64);
        int entropy = id++;

        tg->add_task(id, "deblock_f" + std::to_string(fr),
                     TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 300, 100000);
        tg->add_edge(entropy, id, 1024);
        prev_dct = dct_tasks;
        prev_dct.push_back(id++);
    }

    tg->add_task(id, "mux_output", TaskType::MEMORY_BOUND, Criticality::LOW, 200, 100000);
    for (int f : prev_dct) tg->add_edge(f, id, 512);
    ++id;

    return { "X264-640x360",
             "H.264 encode 640x360, " + std::to_string(n_frames) + " frames, " +
             std::to_string(n_mbs) + " MBs/frame (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

// ══════════════════════════════════════════════════════════════════════════
// 12. Facesim-sim
//     Source: parsec_apps/apps/facesim/parsec/simsmall.runconf
//     Args:   bin/facesim -timing -threads ${NTHREADS}
//     Pattern: time-stepped parallel FEM  |  64 elements, 10 timesteps
// ══════════════════════════════════════════════════════════════════════════
inline BenchmarkInfo make_facesim_sim() {
    auto tg = std::make_unique<TaskGraph>();
    const int n_elems = 64, n_steps = 10;
    int id = 0;

    std::mt19937 rng(1200);
    std::uniform_int_distribution<> exec_force(300, 800);
    std::uniform_int_distribution<> exec_cg(200, 600);
    std::uniform_int_distribution<> comm_d(64, 256);

    tg->add_task(id, "load_mesh", TaskType::MEMORY_BOUND, Criticality::HIGH, 600, 100000);
    int mesh = id++;
    tg->add_task(id, "init_muscle_model", TaskType::COMPUTE_HEAVY, Criticality::HIGH, 1000, 100000);
    tg->add_edge(mesh, id, 1024);
    int muscle = id++;

    std::vector<int> elem_state;
    for (int e = 0; e < n_elems; ++e) {
        tg->add_task(id, "init_elem_" + std::to_string(e),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW, 100, 100000);
        tg->add_edge(muscle, id, 128);
        elem_state.push_back(id++);
    }

    for (int step = 0; step < n_steps; ++step) {
        std::vector<int> forces;
        for (int e = 0; e < n_elems; ++e) {
            tg->add_task(id, "force_s" + std::to_string(step) + "_e" + std::to_string(e),
                         TaskType::COMPUTE_HEAVY, step < 3 ? Criticality::HIGH : Criticality::MEDIUM,
                         exec_force(rng), 100000);
            tg->add_edge(elem_state[e], id, comm_d(rng));
            if (e > 0) tg->add_edge(elem_state[e-1], id, comm_d(rng));
            forces.push_back(id++);
        }

        std::vector<int> cg_tasks;
        for (int e = 0; e < n_elems; ++e) {
            tg->add_task(id, "cg_s" + std::to_string(step) + "_e" + std::to_string(e),
                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM, exec_cg(rng), 100000);
            tg->add_edge(forces[e], id, comm_d(rng));
            cg_tasks.push_back(id++);
        }

        tg->add_task(id, "reduce_s" + std::to_string(step),
                     TaskType::COMPUTE_LIGHT, Criticality::MEDIUM, 200, 100000);
        for (int cg : cg_tasks) tg->add_edge(cg, id, 64);
        int reduce = id++;

        std::vector<int> next;
        for (int e = 0; e < n_elems; ++e) {
            tg->add_task(id, "update_s" + std::to_string(step) + "_e" + std::to_string(e),
                         TaskType::COMPUTE_LIGHT, Criticality::LOW, 100, 100000);
            tg->add_edge(reduce, id, comm_d(rng));
            next.push_back(id++);
        }
        elem_state = next;
    }

    tg->add_task(id, "write_animation", TaskType::MEMORY_BOUND, Criticality::LOW, 300, 100000);
    for (int e : elem_state) tg->add_edge(e, id, 128);
    ++id;

    return { "Facesim-sim",
             "Physics face sim " + std::to_string(n_elems) + " FEM elems, " +
             std::to_string(n_steps) + " timesteps (" + std::to_string(id) + " tasks)",
             std::move(tg) };
}

} // namespace PARSEC
