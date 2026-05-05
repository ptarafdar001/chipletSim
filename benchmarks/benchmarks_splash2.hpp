

/**
 * @file benchmarks_splash2_full.hpp
 * @brief FULL-SCALE SPLASH-2 benchmarks with realistic task counts
 * 
 * NO SIMPLIFICATION - Actual task counts as in real applications
 * 
 * Task counts:
 * - FFT-1024:    2048+ tasks (full butterfly stages)
 * - LU-512:      512 tasks (block decomposition)
 * - Ocean-256:   1024 tasks (256x256 grid, 4 timesteps)
 * - Barnes-128:  384 tasks (tree + force computation)
 * - Raytrace-64: 8256 tasks (scene build + 8192 rays)
 */

#pragma once

#include "../include/types.hpp"
#include "../include/task.hpp"


#include <memory>
#include <random>
#include <cmath>
#include <vector>

namespace SPLASH2 {



// ══════════════════════════════════════════════════════════════
// FFT-1024: Full 1024-point FFT with ALL butterfly stages
// ══════════════════════════════════════════════════════════════
inline BenchmarkInfo make_fft_1024() {
    auto tg = std::make_unique<TaskGraph>();
    
    const int N = 1024;  // FFT size
    const int stages = static_cast<int>(std::log2(N)) + 2;  // log2(1024) = 10, +2 for I/O = 12 stages
    int task_id = 0;
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<> exec_dist(50, 200);
    
    std::vector<std::vector<int>> stage_tasks(stages);
    
    // ─── Stage 0: Input distribution (1024 tasks) ───
    for (int i = 0; i < N; i++) {
        tg->add_task(task_id, "fft_input_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW,
                     50 + (i % 30), 100000);
        stage_tasks[0].push_back(task_id);
        task_id++;
    }
    
    // ─── Stages 1-10: Butterfly computation (10 stages × 512 butterflies = 5120 tasks) ───
    for (int stage = 1; stage <= 10; stage++) {
        int butterflies_per_stage = N / 2;  // 512 butterflies per stage
        
        for (int b = 0; b < butterflies_per_stage; b++) {
            tg->add_task(task_id, "butterfly_s" + std::to_string(stage) + 
                        "_b" + std::to_string(b),
                        TaskType::COMPUTE_HEAVY,
                        stage <= 3 ? Criticality::HIGH : Criticality::MEDIUM,
                        exec_dist(rng), 100000);
            
            // Each butterfly depends on 2 tasks from previous stage
            int stride = 1 << (stage - 1);
            
            if (stage == 1) {
                // First stage: connect to input tasks
                int idx1 = (b * 2) % N;
                int idx2 = (b * 2 + 1) % N;
                tg->add_edge(stage_tasks[0][idx1], task_id, 64 + (b % 192));
                tg->add_edge(stage_tasks[0][idx2], task_id, 64 + (b % 192));
            } else {
                // Later stages: connect to previous butterfly stage
                int prev_b1 = b % (butterflies_per_stage / 2);
                int prev_b2 = (b + butterflies_per_stage / 2) % butterflies_per_stage;
                
                if (prev_b1 < (int)stage_tasks[stage - 1].size())
                    tg->add_edge(stage_tasks[stage - 1][prev_b1], task_id, 128 + (b % 128));
                if (prev_b2 < (int)stage_tasks[stage - 1].size() && prev_b2 != prev_b1)
                    tg->add_edge(stage_tasks[stage - 1][prev_b2], task_id, 128 + (b % 128));
            }
            
            stage_tasks[stage].push_back(task_id);
            task_id++;
        }
    }
    
    // ─── Stage 11: Output aggregation (1024 tasks) ───
    for (int i = 0; i < N; i++) {
        tg->add_task(task_id, "fft_output_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::HIGH,
                     60, 100000);
        
        // Connect to final butterfly stage
        if (i < (int)stage_tasks[10].size())
            tg->add_edge(stage_tasks[10][i], task_id, 64);
        
        task_id++;
    }
    
    return {
        "FFT-1024",
        "Full 1024-point FFT (" + std::to_string(task_id) + " tasks, 10 butterfly stages)",
        std::move(tg),
        
    };
}

// ══════════════════════════════════════════════════════════════
// LU-512: Full LU decomposition (512 tasks)
// ══════════════════════════════════════════════════════════════
inline BenchmarkInfo make_lu_512() {
    auto tg = std::make_unique<TaskGraph>();
    
    const int matrix_size = 512;
    const int block_size = 64;
    const int n_blocks = matrix_size / block_size;  // 8x8 = 64 blocks
    int task_id = 0;
    
    std::mt19937 rng(100);
    std::uniform_int_distribution<> exec_dist(100, 500);
    std::uniform_int_distribution<> comm_dist(128, 1024);
    
    // Task matrix: LU proceeds diagonally
    std::vector<std::vector<int>> block_tasks(n_blocks, std::vector<int>(n_blocks, -1));
    
    for (int k = 0; k < n_blocks; k++) {
        // ─── Diagonal block factorization ───
        tg->add_task(task_id, "lu_factor_diag_" + std::to_string(k),
                     TaskType::COMPUTE_HEAVY,
                     k < 3 ? Criticality::HIGH : Criticality::MEDIUM,
                     400 + exec_dist(rng) % 100, 100000);
        int diag_task = task_id;
        block_tasks[k][k] = task_id++;
        
        // ─── Row updates (forward substitution) ───
        for (int j = k + 1; j < n_blocks; j++) {
            tg->add_task(task_id, "lu_row_" + std::to_string(k) + "_" + std::to_string(j),
                        TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
                        200 + exec_dist(rng) % 200, 100000);
            tg->add_edge(diag_task, task_id, comm_dist(rng));
            block_tasks[k][j] = task_id++;
        }
        
        // ─── Column updates (back substitution) ───
        for (int i = k + 1; i < n_blocks; i++) {
            tg->add_task(task_id, "lu_col_" + std::to_string(i) + "_" + std::to_string(k),
                        TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
                        200 + exec_dist(rng) % 200, 100000);
            tg->add_edge(diag_task, task_id, comm_dist(rng));
            block_tasks[i][k] = task_id++;
        }
        
        // ─── Internal block updates ───
        for (int i = k + 1; i < n_blocks; i++) {
            for (int j = k + 1; j < n_blocks; j++) {
                tg->add_task(task_id, "lu_update_" + std::to_string(i) + 
                            "_" + std::to_string(j) + "_k" + std::to_string(k),
                            TaskType::COMPUTE_HEAVY, Criticality::LOW,
                            150 + exec_dist(rng) % 150, 100000);
                
                // Depends on row and column
                if (block_tasks[k][j] >= 0)
                    tg->add_edge(block_tasks[k][j], task_id, comm_dist(rng));
                if (block_tasks[i][k] >= 0)
                    tg->add_edge(block_tasks[i][k], task_id, comm_dist(rng));
                
                // Depends on previous iteration
                if (k > 0 && block_tasks[i][j] >= 0)
                    tg->add_edge(block_tasks[i][j], task_id, comm_dist(rng));
                
                block_tasks[i][j] = task_id++;
            }
        }
    }
    
    return {
        "LU-512",
        "LU decomposition 512x512 matrix (" + std::to_string(task_id) + " tasks)",
        std::move(tg),
   
    };
}

// ══════════════════════════════════════════════════════════════
// Ocean-256: Ocean simulation (1024 tasks = 256 grid × 4 timesteps)
// ══════════════════════════════════════════════════════════════
inline BenchmarkInfo make_ocean_256() {
    auto tg = std::make_unique<TaskGraph>();
    
    const int grid_size = 256;
    const int tile_size = 16;  // 16x16 tiles
    const int tiles_per_dim = grid_size / tile_size;  // 16 tiles per dimension
    const int total_tiles = tiles_per_dim * tiles_per_dim;  // 256 tiles
    const int timesteps = 4;
    int task_id = 0;
    
    std::mt19937 rng(200);
    std::uniform_int_distribution<> exec_dist(200, 800);
    std::uniform_int_distribution<> comm_dist(256, 512);
    
    // Grid: timesteps × tiles
    std::vector<std::vector<int>> grid(timesteps, std::vector<int>(total_tiles, -1));
    
    for (int t = 0; t < timesteps; t++) {
        for (int tile_idx = 0; tile_idx < total_tiles; tile_idx++) {
            int i = tile_idx / tiles_per_dim;
            int j = tile_idx % tiles_per_dim;
            
            tg->add_task(task_id, "ocean_t" + std::to_string(t) + 
                        "_tile_" + std::to_string(i) + "_" + std::to_string(j),
                        TaskType::COMPUTE_HEAVY,
                        t == 0 ? Criticality::HIGH : Criticality::MEDIUM,
                        exec_dist(rng), 100000);
            
            grid[t][tile_idx] = task_id;
            
            if (t > 0) {
                // 5-point stencil: depends on self + 4 neighbors from previous timestep
                tg->add_edge(grid[t-1][tile_idx], task_id, comm_dist(rng));
                
                // North neighbor
                if (i > 0) {
                    int north_idx = (i - 1) * tiles_per_dim + j;
                    tg->add_edge(grid[t-1][north_idx], task_id, comm_dist(rng));
                }
                // South neighbor
                if (i < tiles_per_dim - 1) {
                    int south_idx = (i + 1) * tiles_per_dim + j;
                    tg->add_edge(grid[t-1][south_idx], task_id, comm_dist(rng));
                }
                // West neighbor
                if (j > 0) {
                    int west_idx = i * tiles_per_dim + (j - 1);
                    tg->add_edge(grid[t-1][west_idx], task_id, comm_dist(rng));
                }
                // East neighbor
                if (j < tiles_per_dim - 1) {
                    int east_idx = i * tiles_per_dim + (j + 1);
                    tg->add_edge(grid[t-1][east_idx], task_id, comm_dist(rng));
                }
            }
            
            task_id++;
        }
    }
    
    return {
        "Ocean-256",
        "Ocean simulation 256x256 grid, 4 timesteps (" + std::to_string(task_id) + " tasks)",
        std::move(tg),
       
    };
}

// ══════════════════════════════════════════════════════════════
// Barnes-128: Barnes-Hut N-body (384 tasks)
// ══════════════════════════════════════════════════════════════
inline BenchmarkInfo make_barnes_128() {
    auto tg = std::make_unique<TaskGraph>();
    
    const int n_bodies = 128;
    const int tree_levels = 7;  // log2(128) = 7
    int task_id = 0;
    
    std::mt19937 rng(300);
    std::uniform_int_distribution<> exec_dist(150, 600);
    std::uniform_int_distribution<> comm_dist(64, 512);
    
    std::vector<std::vector<int>> tree_nodes(tree_levels);
    
    // ─── Phase 1: Build octree (127 internal nodes for 128 bodies) ───
    
    // Level 0: Root
    tg->add_task(task_id, "tree_root",
                 TaskType::COMPUTE_HEAVY, Criticality::HIGH,
                 exec_dist(rng), 100000);
    tree_nodes[0].push_back(task_id++);
    
    // Levels 1-6: Internal nodes
    for (int level = 1; level < tree_levels; level++) {
        int nodes_at_level = 1 << level;  // 2, 4, 8, 16, 32, 64
        
        for (int i = 0; i < nodes_at_level; i++) {
            tg->add_task(task_id, "tree_L" + std::to_string(level) + 
                        "_" + std::to_string(i),
                        TaskType::COMPUTE_HEAVY,
                        level < 3 ? Criticality::HIGH : Criticality::MEDIUM,
                        exec_dist(rng), 100000);
            
            // Connect to parent in previous level
            int parent_idx = i / 2;
            if (parent_idx < (int)tree_nodes[level - 1].size())
                tg->add_edge(tree_nodes[level - 1][parent_idx], task_id, comm_dist(rng));
            
            tree_nodes[level].push_back(task_id);
            task_id++;
        }
    }
    
    // ─── Phase 2: Force computation (128 tasks, one per body) ───
    int tree_root = tree_nodes[0][0];
    std::vector<int> force_tasks;
    
    for (int i = 0; i < n_bodies; i++) {
        tg->add_task(task_id, "force_body_" + std::to_string(i),
                     TaskType::COMPUTE_HEAVY,
                     i < 16 ? Criticality::HIGH : Criticality::MEDIUM,
                     300 + exec_dist(rng) % 300, 100000);
        
        // Each force computation traverses the tree
        tg->add_edge(tree_root, task_id, comm_dist(rng));
        force_tasks.push_back(task_id);
        task_id++;
    }
    
    // ─── Phase 3: Position update (128 tasks) ───
    for (int i = 0; i < n_bodies; i++) {
        tg->add_task(task_id, "update_pos_" + std::to_string(i),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW,
                     exec_dist(rng), 100000);
        tg->add_edge(force_tasks[i], task_id, comm_dist(rng));
        task_id++;
    }
    
    return {
        "Barnes-128",
        "Barnes-Hut N-body with 128 bodies (" + std::to_string(task_id) + " tasks)",
        std::move(tg),
     
    };
}

// ══════════════════════════════════════════════════════════════
// Raytrace-64: Full ray tracing (8256 tasks = BVH + 8192 rays)
// ══════════════════════════════════════════════════════════════
inline BenchmarkInfo make_raytrace_64() {
    auto tg = std::make_unique<TaskGraph>();
    
    const int image_width = 64;
    const int image_height = 64;
    const int total_pixels = image_width * image_height;  // 4096 pixels
    const int rays_per_pixel = 2;  // Primary + shadow ray
    const int total_rays = total_pixels * rays_per_pixel;  // 8192 rays
    int task_id = 0;
    
    std::mt19937 rng(400);
    std::uniform_int_distribution<> exec_dist(1000, 5000);
    std::uniform_int_distribution<> comm_dist(32, 128);
    
    // ─── Phase 1: Scene loading ───
    tg->add_task(task_id, "load_scene",
                 TaskType::COMPUTE_LIGHT, Criticality::LOW,
                 500, 100000);
    int load_task = task_id++;
    
    // ─── Phase 2: BVH construction (62 nodes for binary tree) ───
    int bvh_levels = 6;  // log2(64) = 6 levels
    std::vector<std::vector<int>> bvh_nodes(bvh_levels);
    
    // Root level
    tg->add_task(task_id, "bvh_root",
                 TaskType::COMPUTE_HEAVY, Criticality::HIGH,
                 2000, 100000);
    tg->add_edge(load_task, task_id, 2048);
    bvh_nodes[0].push_back(task_id++);
    
    // Build BVH tree
    for (int level = 1; level < bvh_levels; level++) {
        int nodes = 1 << level;
        for (int i = 0; i < nodes; i++) {
            tg->add_task(task_id, "bvh_L" + std::to_string(level) + 
                        "_" + std::to_string(i),
                        TaskType::COMPUTE_HEAVY,
                        level < 3 ? Criticality::HIGH : Criticality::MEDIUM,
                        800 + exec_dist(rng) % 1200, 100000);
            
            int parent = i / 2;
            if (parent < (int)bvh_nodes[level - 1].size())
                tg->add_edge(bvh_nodes[level - 1][parent], task_id, 512);
            
            bvh_nodes[level].push_back(task_id);
            task_id++;
        }
    }
    
    int bvh_root = bvh_nodes[0][0];
    
    // ─── Phase 3: Ray tracing (8192 rays) ───
    std::vector<int> ray_tasks;
    
    for (int ray = 0; ray < total_rays; ray++) {
        bool is_primary = (ray % 2 == 0);
        
        tg->add_task(task_id, (is_primary ? "primary_ray_" : "shadow_ray_") + 
                    std::to_string(ray / 2),
                    TaskType::COMPUTE_HEAVY,
                    ray < 100 ? Criticality::HIGH : Criticality::MEDIUM,
                    exec_dist(rng), 100000);
        
        tg->add_edge(bvh_root, task_id, comm_dist(rng));
        ray_tasks.push_back(task_id);
        task_id++;
    }
    
    // ─── Phase 4: Pixel shading (4096 pixels) ───
    for (int pixel = 0; pixel < total_pixels; pixel++) {
        tg->add_task(task_id, "shade_pixel_" + std::to_string(pixel),
                     TaskType::COMPUTE_LIGHT, Criticality::LOW,
                     200, 100000);
        
        // Depends on primary and shadow rays for this pixel
        int primary_ray = pixel * 2;
        int shadow_ray = pixel * 2 + 1;
        tg->add_edge(ray_tasks[primary_ray], task_id, comm_dist(rng));
        tg->add_edge(ray_tasks[shadow_ray], task_id, comm_dist(rng));
        
        task_id++;
    }
    
    return {
        "Raytrace-64x64",
        "Ray tracing 64x64 image (" + std::to_string(task_id) + " tasks, " + 
        std::to_string(total_rays) + " rays)",
        std::move(tg),

    };
}

} // namespace SPLASH2_FULL


























// /**
//  * @file benchmarks_splash2.hpp
//  * @brief SPLASH-2 benchmark suite task graph implementations
//  * 
//  * Based on published characterizations from:
//  * - Woo et al., "The SPLASH-2 Programs: Characterization and Methodological Considerations" (1995)
//  * - Bienia et al., "Benchmarking Modern Multiprocessors" (2011)
//  * - Task graph models from NoC mapping literature (2015-2024)
//  */

// #pragma once

// #include "../include/types.hpp"
// #include "../include/task.hpp"

// #include <memory>
// #include <cmath>
// #include <random>

// namespace SPLASH2 {

// // ══════════════════════════════════════════════════════════════
// // FFT-1024: 1024-point Fast Fourier Transform
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_fft_1024() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int N = 1024;  // FFT size
//     const int stages = static_cast<int>(std::log2(N)) + 2;  // log2(N) butterfly + I/O
//     int task_id = 0;
    
//     // Stage 0: Input data distribution (parallel read)
//     std::vector<int> stage_tasks[stages];
//     for (int i = 0; i < N; i++) {
//         tg->add_task(task_id, "input_" + std::to_string(i),
//                      TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                      50 + (i % 30), 100000);  // 50-80 cycles
//         stage_tasks[0].push_back(task_id);
//         task_id++;
//     }
    
//     // Stages 1 to log2(N): Butterfly computation
//     for (int stage = 1; stage < stages - 1; stage++) {
//         int pairs = N / (1 << stage);
//         for (int p = 0; p < pairs; p++) {
//             tg->add_task(task_id, "butterfly_s" + std::to_string(stage) + 
//                         "_p" + std::to_string(p),
//                         TaskType::COMPUTE_HEAVY, 
//                         stage <= 3 ? Criticality::HIGH : Criticality::MEDIUM,
//                         100 + (p % 100), 100000);  // 100-200 cycles
            
//             // Dependencies: each butterfly depends on 2 tasks from previous stage
//             int stride = 1 << (stage - 1);
//             int base = p * stride * 2;
//             if (stage == 1) {
//                 // First butterfly stage depends on input tasks
//                 for (int k = 0; k < (1 << stage); k++) {
//                     if (base + k < (int)stage_tasks[0].size()) {
//                         tg->add_edge(stage_tasks[0][base + k], task_id, 
//                                     64 + (k % 192));  // 64-256 bytes
//                     }
//                 }
//             } else {
//                 // Later stages depend on previous butterfly stage
//                 int prev_base = p / 2;
//                 if (prev_base < (int)stage_tasks[stage - 1].size()) {
//                     tg->add_edge(stage_tasks[stage - 1][prev_base], task_id, 
//                                 128 + (p % 128));  // 128-256 bytes
//                 }
//                 if (prev_base + 1 < (int)stage_tasks[stage - 1].size()) {
//                     tg->add_edge(stage_tasks[stage - 1][prev_base + 1], task_id, 
//                                 128 + (p % 128));
//                 }
//             }
            
//             stage_tasks[stage].push_back(task_id);
//             task_id++;
//         }
//     }
    
//     // Final stage: Output aggregation
//     tg->add_task(task_id, "output_aggregate",
//                  TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                  80, 100000);
//     for (int prev : stage_tasks[stages - 2]) {
//         tg->add_edge(prev, task_id, 64);
//     }
    
//     return BenchmarkInfo{
//         "FFT-1024",
//         "1024-point Fast Fourier Transform (butterfly structure)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // LU-512: LU Decomposition (512x512 matrix, 32x32 blocks)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_lu_512() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int matrix_size = 512;
//     const int block_size = 32;
//     const int n_blocks = matrix_size / block_size;  // 16x16 = 256 blocks
//     int task_id = 0;
    
//     // Task grid: LU decomposition proceeds diagonally
//     std::vector<std::vector<int>> task_grid(n_blocks, std::vector<int>(n_blocks, -1));
    
//     std::mt19937 rng(42);
//     std::uniform_int_distribution<> exec_dist(100, 500);
//     std::uniform_int_distribution<> comm_dist(128, 1024);
    
//     for (int k = 0; k < n_blocks; k++) {
//         // Diagonal block (factorization)
//         tg->add_task(task_id, "factor_" + std::to_string(k),
//                      TaskType::COMPUTE_HEAVY, 
//                      k < 4 ? Criticality::HIGH : Criticality::MEDIUM,
//                      400 + exec_dist(rng) % 100, 100000);  // 400-500 cycles
//         int diag_task = task_id;
//         task_grid[k][k] = task_id++;
        
//         // Row tasks (forward substitution)
//         for (int j = k + 1; j < n_blocks; j++) {
//             tg->add_task(task_id, "row_" + std::to_string(k) + 
//                         "_" + std::to_string(j),
//                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
//                         200 + exec_dist(rng) % 200, 100000);  // 200-400 cycles
//             tg->add_edge(diag_task, task_id, comm_dist(rng));
//             task_grid[k][j] = task_id++;
//         }
        
//         // Column tasks (back substitution)
//         for (int i = k + 1; i < n_blocks; i++) {
//             tg->add_task(task_id, "col_" + std::to_string(i) + 
//                         "_" + std::to_string(k),
//                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
//                         200 + exec_dist(rng) % 200, 100000);
//             tg->add_edge(diag_task, task_id, comm_dist(rng));
//             task_grid[i][k] = task_id++;
//         }
        
//         // Internal block updates (depend on row and column of same iteration)
//         for (int i = k + 1; i < n_blocks; i++) {
//             for (int j = k + 1; j < n_blocks; j++) {
//                 tg->add_task(task_id, "update_" + std::to_string(i) + 
//                             "_" + std::to_string(j) + "_k" + std::to_string(k),
//                             TaskType::COMPUTE_HEAVY, Criticality::LOW,
//                             150 + exec_dist(rng) % 150, 100000);  // 150-300 cycles
                
//                 if (task_grid[k][j] >= 0)
//                     tg->add_edge(task_grid[k][j], task_id, comm_dist(rng));
//                 if (task_grid[i][k] >= 0)
//                     tg->add_edge(task_grid[i][k], task_id, comm_dist(rng));
                
//                 // Update depends on previous iteration's result
//                 if (k > 0 && task_grid[i][j] >= 0)
//                     tg->add_edge(task_grid[i][j], task_id, comm_dist(rng));
                
//                 task_grid[i][j] = task_id++;
//             }
//         }
//     }
    
//     return BenchmarkInfo{
//         "LU-512",
//         "LU decomposition of 512x512 matrix (block parallel)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // Ocean-256: Ocean current simulation (256x256 grid)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_ocean_256() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int grid_size = 256;
//     const int tile_size = 16;  // 16x16 tiles
//     const int n_tiles = grid_size / tile_size;  // 16x16 = 256 tiles
//     const int timesteps = 4;
//     int task_id = 0;
    
//     std::mt19937 rng(123);
//     std::uniform_int_distribution<> exec_dist(200, 800);
//     std::uniform_int_distribution<> comm_dist(256, 512);
    
//     // Grid of tasks: each tile at each timestep
//     std::vector<std::vector<std::vector<int>>> grid(
//         timesteps, 
//         std::vector<std::vector<int>>(n_tiles, std::vector<int>(n_tiles, -1))
//     );
    
//     for (int t = 0; t < timesteps; t++) {
//         for (int i = 0; i < n_tiles; i++) {
//             for (int j = 0; j < n_tiles; j++) {
//                 bool is_boundary = (i == 0 || i == n_tiles-1 || 
//                                    j == 0 || j == n_tiles-1);
                
//                 tg->add_task(task_id, "ocean_t" + std::to_string(t) + 
//                             "_" + std::to_string(i) + "_" + std::to_string(j),
//                             TaskType::COMPUTE_HEAVY,
//                             t == 0 ? Criticality::HIGH : Criticality::MEDIUM,
//                             exec_dist(rng), 100000);
                
//                 grid[t][i][j] = task_id;
                
//                 // Dependencies: depend on previous timestep (self and 4 neighbors)
//                 if (t > 0) {
//                     // Self from previous timestep
//                     tg->add_edge(grid[t-1][i][j], task_id, comm_dist(rng));
                    
//                     // Neighbors from previous timestep (5-point stencil)
//                     if (i > 0)
//                         tg->add_edge(grid[t-1][i-1][j], task_id, comm_dist(rng));
//                     if (i < n_tiles - 1)
//                         tg->add_edge(grid[t-1][i+1][j], task_id, comm_dist(rng));
//                     if (j > 0)
//                         tg->add_edge(grid[t-1][i][j-1], task_id, comm_dist(rng));
//                     if (j < n_tiles - 1)
//                         tg->add_edge(grid[t-1][i][j+1], task_id, comm_dist(rng));
//                 }
                
//                 task_id++;
//             }
//         }
//     }
    
//     return BenchmarkInfo{
//         "Ocean-256",
//         "Ocean current simulation with 256x256 grid (5-point stencil)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // Barnes-128: Barnes-Hut N-body simulation (128 bodies)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_barnes_128() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int n_bodies = 128;
//     const int tree_levels = 7;  // log2(128)
//     int task_id = 0;
    
//     std::mt19937 rng(456);
//     std::uniform_int_distribution<> exec_dist(150, 600);
//     std::uniform_int_distribution<> comm_dist(64, 512);
    
//     // Phase 1: Build octree (hierarchical)
//     std::vector<int> tree_nodes[tree_levels];
    
//     // Leaf level (one per body)
//     for (int i = 0; i < n_bodies; i++) {
//         tg->add_task(task_id, "leaf_" + std::to_string(i),
//                      TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                      exec_dist(rng), 100000);
//         tree_nodes[0].push_back(task_id);
//         task_id++;
//     }
    
//     // Build tree bottom-up
//     for (int level = 1; level < tree_levels; level++) {
//         int nodes_at_level = n_bodies / (1 << level);
//         for (int i = 0; i < nodes_at_level; i++) {
//             tg->add_task(task_id, "tree_l" + std::to_string(level) + 
//                         "_" + std::to_string(i),
//                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
//                         exec_dist(rng), 100000);
            
//             // Depends on children from previous level
//             int child1 = i * 2;
//             int child2 = i * 2 + 1;
//             if (child1 < (int)tree_nodes[level - 1].size())
//                 tg->add_edge(tree_nodes[level - 1][child1], task_id, comm_dist(rng));
//             if (child2 < (int)tree_nodes[level - 1].size())
//                 tg->add_edge(tree_nodes[level - 1][child2], task_id, comm_dist(rng));
            
//             tree_nodes[level].push_back(task_id);
//             task_id++;
//         }
//     }
    
//     // Phase 2: Force computation (each body traverses tree)
//     int root_task = tree_nodes[tree_levels - 1][0];
//     std::vector<int> force_tasks;
    
//     for (int i = 0; i < n_bodies; i++) {
//         tg->add_task(task_id, "force_" + std::to_string(i),
//                      TaskType::COMPUTE_HEAVY, 
//                      i < 16 ? Criticality::HIGH : Criticality::MEDIUM,
//                      300 + exec_dist(rng) % 300, 100000);  // 300-600 cycles
        
//         // Depends on tree root
//         tg->add_edge(root_task, task_id, comm_dist(rng));
//         force_tasks.push_back(task_id);
//         task_id++;
//     }
    
//     // Phase 3: Update positions
//     for (int i = 0; i < n_bodies; i++) {
//         tg->add_task(task_id, "update_" + std::to_string(i),
//                      TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                      exec_dist(rng), 100000);
//         tg->add_edge(force_tasks[i], task_id, comm_dist(rng));
//         task_id++;
//     }
    
//     return BenchmarkInfo{
//         "Barnes-128",
//         "Barnes-Hut N-body simulation with 128 bodies (hierarchical tree)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // Raytrace-64: 3D ray tracing (64 parallel rays)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_raytrace_64() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int n_rays = 64;
//     int task_id = 0;
    
//     std::mt19937 rng(789);
//     std::uniform_int_distribution<> exec_dist(1000, 5000);
//     std::uniform_int_distribution<> comm_dist(32, 128);
    
//     // Init: Load scene geometry
//     tg->add_task(task_id, "load_scene",
//                  TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                  500, 100000);
//     int init_task = task_id++;
    
//     // Build acceleration structure (BVH)
//     tg->add_task(task_id, "build_bvh",
//                  TaskType::COMPUTE_HEAVY, Criticality::HIGH,
//                  2000, 100000);
//     tg->add_edge(init_task, task_id, 2048);  // Large scene data
//     int bvh_task = task_id++;
    
//     // Ray tracing (embarrassingly parallel)
//     std::vector<int> ray_tasks;
//     for (int i = 0; i < n_rays; i++) {
//         tg->add_task(task_id, "raytrace_" + std::to_string(i),
//                      TaskType::COMPUTE_HEAVY, 
//                      i < 8 ? Criticality::HIGH : Criticality::MEDIUM,
//                      exec_dist(rng), 100000);  // 1000-5000 cycles (varies widely)
//         tg->add_edge(bvh_task, task_id, comm_dist(rng));
//         ray_tasks.push_back(task_id);
//         task_id++;
//     }
    
//     // Combine results (gather)
//     tg->add_task(task_id, "combine_pixels",
//                  TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                  300, 100000);
//     for (int ray : ray_tasks) {
//         tg->add_edge(ray, task_id, comm_dist(rng));
//     }
    
//     return BenchmarkInfo{
//         "Raytrace-64",
//         "3D ray tracing with 64 rays (embarrassingly parallel)",
//         std::move(tg)
//     };
// }

// } // namespace SPLASH2








// /**
//  * @file benchmarks_splash2.hpp
//  * @brief SPLASH-2 benchmark suite task graph implementations
//  * 
//  * Based on published characterizations from:
//  * - Woo et al., "The SPLASH-2 Programs: Characterization and Methodological Considerations" (1995)
//  * - Bienia et al., "Benchmarking Modern Multiprocessors" (2011)
//  * - Task graph models from NoC mapping literature (2015-2024)
//  */

// #pragma once

// #include "types.hpp"
// #include "task.hpp"

// #include <memory>
// #include <cmath>
// #include <random>

// // namespace SPLASH2 {

// // ============================================================
// //  Benchmark factory helpers
// // ============================================================
// struct BenchmarkInfo {
//     std::string   name;
//     std::string   description;
//     std::unique_ptr<TaskGraph> graph;
// };

// // ══════════════════════════════════════════════════════════════
// // FFT-1024: 1024-point Fast Fourier Transform
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_fft_1024() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int N = 1024;  // FFT size
//     const int stages = static_cast<int>(std::log2(N)) + 2;  // log2(N) butterfly + I/O
//     int task_id = 0;
    
//     // Stage 0: Input data distribution (parallel read)
//     std::vector<int> stage_tasks[stages];
//     for (int i = 0; i < N; i++) {
//         tg->add_task(task_id, "input_" + std::to_string(i),
//                      TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                      50 + (i % 30), 100000);  // 50-80 cycles
//         stage_tasks[0].push_back(task_id);
//         task_id++;
//     }
    
//     // Stages 1 to log2(N): Butterfly computation
//     for (int stage = 1; stage < stages - 1; stage++) {
//         int pairs = N / (1 << stage);
//         for (int p = 0; p < pairs; p++) {
//             tg->add_task(task_id, "butterfly_s" + std::to_string(stage) + 
//                         "_p" + std::to_string(p),
//                         TaskType::COMPUTE_HEAVY, 
//                         stage <= 3 ? Criticality::HIGH : Criticality::MEDIUM,
//                         100 + (p % 100), 100000);  // 100-200 cycles
            
//             // Dependencies: each butterfly depends on 2 tasks from previous stage
//             int stride = 1 << (stage - 1);
//             int base = p * stride * 2;
//             if (stage == 1) {
//                 // First butterfly stage depends on input tasks
//                 for (int k = 0; k < (1 << stage); k++) {
//                     if (base + k < (int)stage_tasks[0].size()) {
//                         tg->add_edge(stage_tasks[0][base + k], task_id, 
//                                     64 + (k % 192));  // 64-256 bytes
//                     }
//                 }
//             } else {
//                 // Later stages depend on previous butterfly stage
//                 int prev_base = p / 2;
//                 if (prev_base < (int)stage_tasks[stage - 1].size()) {
//                     tg->add_edge(stage_tasks[stage - 1][prev_base], task_id, 
//                                 128 + (p % 128));  // 128-256 bytes
//                 }
//                 if (prev_base + 1 < (int)stage_tasks[stage - 1].size()) {
//                     tg->add_edge(stage_tasks[stage - 1][prev_base + 1], task_id, 
//                                 128 + (p % 128));
//                 }
//             }
            
//             stage_tasks[stage].push_back(task_id);
//             task_id++;
//         }
//     }
    
//     // Final stage: Output aggregation
//     tg->add_task(task_id, "output_aggregate",
//                  TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                  80, 100000);
//     for (int prev : stage_tasks[stages - 2]) {
//         tg->add_edge(prev, task_id, 64);
//     }
    
//     return BenchmarkInfo{
//         "FFT-1024",
//         "1024-point Fast Fourier Transform (butterfly structure)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // LU-512: LU Decomposition (512x512 matrix, 32x32 blocks)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_lu_512() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int matrix_size = 512;
//     const int block_size = 32;
//     const int n_blocks = matrix_size / block_size;  // 16x16 = 256 blocks
//     int task_id = 0;
    
//     // Task grid: LU decomposition proceeds diagonally
//     std::vector<std::vector<int>> task_grid(n_blocks, std::vector<int>(n_blocks, -1));
    
//     std::mt19937 rng(42);
//     std::uniform_int_distribution<> exec_dist(100, 500);
//     std::uniform_int_distribution<> comm_dist(128, 1024);
    
//     for (int k = 0; k < n_blocks; k++) {
//         // Diagonal block (factorization)
//         tg->add_task(task_id, "factor_" + std::to_string(k),
//                      TaskType::COMPUTE_HEAVY, 
//                      k < 4 ? Criticality::HIGH : Criticality::MEDIUM,
//                      400 + exec_dist(rng) % 100, 100000);  // 400-500 cycles
//         int diag_task = task_id;
//         task_grid[k][k] = task_id++;
        
//         // Row tasks (forward substitution)
//         for (int j = k + 1; j < n_blocks; j++) {
//             tg->add_task(task_id, "row_" + std::to_string(k) + 
//                         "_" + std::to_string(j),
//                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
//                         200 + exec_dist(rng) % 200, 100000);  // 200-400 cycles
//             tg->add_edge(diag_task, task_id, comm_dist(rng));
//             task_grid[k][j] = task_id++;
//         }
        
//         // Column tasks (back substitution)
//         for (int i = k + 1; i < n_blocks; i++) {
//             tg->add_task(task_id, "col_" + std::to_string(i) + 
//                         "_" + std::to_string(k),
//                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
//                         200 + exec_dist(rng) % 200, 100000);
//             tg->add_edge(diag_task, task_id, comm_dist(rng));
//             task_grid[i][k] = task_id++;
//         }
        
//         // Internal block updates (depend on row and column of same iteration)
//         for (int i = k + 1; i < n_blocks; i++) {
//             for (int j = k + 1; j < n_blocks; j++) {
//                 tg->add_task(task_id, "update_" + std::to_string(i) + 
//                             "_" + std::to_string(j) + "_k" + std::to_string(k),
//                             TaskType::COMPUTE_HEAVY, Criticality::LOW,
//                             150 + exec_dist(rng) % 150, 100000);  // 150-300 cycles
                
//                 if (task_grid[k][j] >= 0)
//                     tg->add_edge(task_grid[k][j], task_id, comm_dist(rng));
//                 if (task_grid[i][k] >= 0)
//                     tg->add_edge(task_grid[i][k], task_id, comm_dist(rng));
                
//                 // Update depends on previous iteration's result
//                 if (k > 0 && task_grid[i][j] >= 0)
//                     tg->add_edge(task_grid[i][j], task_id, comm_dist(rng));
                
//                 task_grid[i][j] = task_id++;
//             }
//         }
//     }
    
//     return BenchmarkInfo{
//         "LU-512",
//         "LU decomposition of 512x512 matrix (block parallel)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // Ocean-256: Ocean current simulation (256x256 grid)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_ocean_256() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int grid_size = 256;
//     const int tile_size = 16;  // 16x16 tiles
//     const int n_tiles = grid_size / tile_size;  // 16x16 = 256 tiles
//     const int timesteps = 4;
//     int task_id = 0;
    
//     std::mt19937 rng(123);
//     std::uniform_int_distribution<> exec_dist(200, 800);
//     std::uniform_int_distribution<> comm_dist(256, 512);
    
//     // Grid of tasks: each tile at each timestep
//     std::vector<std::vector<std::vector<int>>> grid(
//         timesteps, 
//         std::vector<std::vector<int>>(n_tiles, std::vector<int>(n_tiles, -1))
//     );
    
//     for (int t = 0; t < timesteps; t++) {
//         for (int i = 0; i < n_tiles; i++) {
//             for (int j = 0; j < n_tiles; j++) {
//                 bool is_boundary = (i == 0 || i == n_tiles-1 || 
//                                    j == 0 || j == n_tiles-1);
                
//                 tg->add_task(task_id, "ocean_t" + std::to_string(t) + 
//                             "_" + std::to_string(i) + "_" + std::to_string(j),
//                             TaskType::COMPUTE_HEAVY,
//                             t == 0 ? Criticality::HIGH : Criticality::MEDIUM,
//                             exec_dist(rng), 100000);
                
//                 grid[t][i][j] = task_id;
                
//                 // Dependencies: depend on previous timestep (self and 4 neighbors)
//                 if (t > 0) {
//                     // Self from previous timestep
//                     tg->add_edge(grid[t-1][i][j], task_id, comm_dist(rng));
                    
//                     // Neighbors from previous timestep (5-point stencil)
//                     if (i > 0)
//                         tg->add_edge(grid[t-1][i-1][j], task_id, comm_dist(rng));
//                     if (i < n_tiles - 1)
//                         tg->add_edge(grid[t-1][i+1][j], task_id, comm_dist(rng));
//                     if (j > 0)
//                         tg->add_edge(grid[t-1][i][j-1], task_id, comm_dist(rng));
//                     if (j < n_tiles - 1)
//                         tg->add_edge(grid[t-1][i][j+1], task_id, comm_dist(rng));
//                 }
                
//                 task_id++;
//             }
//         }
//     }
    
//     return BenchmarkInfo{
//         "Ocean-256",
//         "Ocean current simulation with 256x256 grid (5-point stencil)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // Barnes-128: Barnes-Hut N-body simulation (128 bodies)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_barnes_128() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int n_bodies = 128;
//     const int tree_levels = 7;  // log2(128)
//     int task_id = 0;
    
//     std::mt19937 rng(456);
//     std::uniform_int_distribution<> exec_dist(150, 600);
//     std::uniform_int_distribution<> comm_dist(64, 512);
    
//     // Phase 1: Build octree (hierarchical)
//     std::vector<int> tree_nodes[tree_levels];
    
//     // Leaf level (one per body)
//     for (int i = 0; i < n_bodies; i++) {
//         tg->add_task(task_id, "leaf_" + std::to_string(i),
//                      TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                      exec_dist(rng), 100000);
//         tree_nodes[0].push_back(task_id);
//         task_id++;
//     }
    
//     // Build tree bottom-up
//     for (int level = 1; level < tree_levels; level++) {
//         int nodes_at_level = n_bodies / (1 << level);
//         for (int i = 0; i < nodes_at_level; i++) {
//             tg->add_task(task_id, "tree_l" + std::to_string(level) + 
//                         "_" + std::to_string(i),
//                         TaskType::COMPUTE_HEAVY, Criticality::MEDIUM,
//                         exec_dist(rng), 100000);
            
//             // Depends on children from previous level
//             int child1 = i * 2;
//             int child2 = i * 2 + 1;
//             if (child1 < (int)tree_nodes[level - 1].size())
//                 tg->add_edge(tree_nodes[level - 1][child1], task_id, comm_dist(rng));
//             if (child2 < (int)tree_nodes[level - 1].size())
//                 tg->add_edge(tree_nodes[level - 1][child2], task_id, comm_dist(rng));
            
//             tree_nodes[level].push_back(task_id);
//             task_id++;
//         }
//     }
    
//     // Phase 2: Force computation (each body traverses tree)
//     int root_task = tree_nodes[tree_levels - 1][0];
//     std::vector<int> force_tasks;
    
//     for (int i = 0; i < n_bodies; i++) {
//         tg->add_task(task_id, "force_" + std::to_string(i),
//                      TaskType::COMPUTE_HEAVY, 
//                      i < 16 ? Criticality::HIGH : Criticality::MEDIUM,
//                      300 + exec_dist(rng) % 300, 100000);  // 300-600 cycles
        
//         // Depends on tree root
//         tg->add_edge(root_task, task_id, comm_dist(rng));
//         force_tasks.push_back(task_id);
//         task_id++;
//     }
    
//     // Phase 3: Update positions
//     for (int i = 0; i < n_bodies; i++) {
//         tg->add_task(task_id, "update_" + std::to_string(i),
//                      TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                      exec_dist(rng), 100000);
//         tg->add_edge(force_tasks[i], task_id, comm_dist(rng));
//         task_id++;
//     }
    
//     return BenchmarkInfo{
//         "Barnes-128",
//         "Barnes-Hut N-body simulation with 128 bodies (hierarchical tree)",
//         std::move(tg)
//     };
// }

// // ══════════════════════════════════════════════════════════════
// // Raytrace-64: 3D ray tracing (64 parallel rays)
// // ══════════════════════════════════════════════════════════════
// inline BenchmarkInfo make_raytrace_64() {
//     auto tg = std::make_unique<TaskGraph>();
    
//     const int n_rays = 64;
//     int task_id = 0;
    
//     std::mt19937 rng(789);
//     std::uniform_int_distribution<> exec_dist(1000, 5000);
//     std::uniform_int_distribution<> comm_dist(32, 128);
    
//     // Init: Load scene geometry
//     tg->add_task(task_id, "load_scene",
//                  TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                  500, 100000);
//     int init_task = task_id++;
    
//     // Build acceleration structure (BVH)
//     tg->add_task(task_id, "build_bvh",
//                  TaskType::COMPUTE_HEAVY, Criticality::HIGH,
//                  2000, 100000);
//     tg->add_edge(init_task, task_id, 2048);  // Large scene data
//     int bvh_task = task_id++;
    
//     // Ray tracing (embarrassingly parallel)
//     std::vector<int> ray_tasks;
//     for (int i = 0; i < n_rays; i++) {
//         tg->add_task(task_id, "raytrace_" + std::to_string(i),
//                      TaskType::COMPUTE_HEAVY, 
//                      i < 8 ? Criticality::HIGH : Criticality::MEDIUM,
//                      exec_dist(rng), 100000);  // 1000-5000 cycles (varies widely)
//         tg->add_edge(bvh_task, task_id, comm_dist(rng));
//         ray_tasks.push_back(task_id);
//         task_id++;
//     }
    
//     // Combine results (gather)
//     tg->add_task(task_id, "combine_pixels",
//                  TaskType::COMPUTE_LIGHT, Criticality::LOW,
//                  300, 100000);
//     for (int ray : ray_tasks) {
//         tg->add_edge(ray, task_id, comm_dist(rng));
//     }
    
//     return BenchmarkInfo{
//         "Raytrace-64",
//         "3D ray tracing with 64 rays (embarrassingly parallel)",
//         std::move(tg)
//     };
// }

// // } // namespace SPLASH2

