/**
 * @file types.hpp
 * @brief Core types, enumerations, and constants for the
 *        Cycle-Accurate HexaMesh Chiplet-Based NoC/NoI Simulator.
 *
 * Topology: HexaMesh (Iff et al., arXiv:2211.13989v4)
 *
 * Cycle-accuracy additions:
 *   - Distinct intra-chiplet (core-mesh) and inter-chiplet (UCIe) hop costs
 *   - Memory-access penalty cycles by task type
 *   - Per-link bandwidth contention model constants
 *   - NoC flit size and phit width for volume-to-latency conversion
 */

#pragma once

#include <cstdint>
#include <string>
#include <limits>
#include <memory>

class TaskGraph;

// ============================================================
//  Benchmark factory helper
// ============================================================
struct BenchmarkInfo {
    std::string                name;
    std::string                description;
    std::unique_ptr<TaskGraph> graph;

    BenchmarkInfo(std::string n, std::string d, std::unique_ptr<TaskGraph> g)
        : name(std::move(n)), description(std::move(d)), graph(std::move(g)) {}
};

// ============================================================
//  Simulation constants
// ============================================================
namespace Constants {

    // ── HexaMesh topology ─────────────────────────────────────
    constexpr int  DEFAULT_RINGS        = 1;    // r=2 → 1+3*2*3 = 19 chiplets
    constexpr int  CORES_PER_CHIPLET    = 4;   // 2×2 intra-chiplet mesh
    constexpr int  MESH_DIM             = 2;

    // ── NoC hop latencies (cycles) ────────────────────────────
    //   Intra-chiplet:  2-cycle router pipeline per hop in the 4×4 mesh.
    //   Inter-chiplet:  27-cycle PHY latency per chiplet hop (UCIe spec).
    constexpr uint64_t INTRA_CHIPLET_HOP   =  2;
    constexpr uint64_t INTER_CHIPLET_HOP   = 27;

    // ── NoC bandwidth / flit model ────────────────────────────
    //   A flit is 64 bytes; a phit (physical transfer unit) is 16 bytes.
    //   Serialization latency = ceil(volume / PHIT_BYTES) cycles per link.
    constexpr uint64_t FLIT_BYTES          = 64;
    constexpr uint64_t PHIT_BYTES          = 16;   // UCIe 64-bit/cycle @ 4 GHz

    // ── Memory / IO overhead (cycles added to exec_time) ─────
    //   Applied in the cycle-accurate engine when computing actual duration.
    constexpr uint64_t MEM_ACCESS_PENALTY  = 10;   // extra cycles for MEMORY_BOUND
    constexpr uint64_t IO_STALL_PENALTY    =  5;   // extra cycles for IO_BOUND

    // ── Fault simulation ──────────────────────────────────────
    constexpr double   FAULT_PROBABILITY   = 0.02;

    // ── Task clustering weights ───────────────────────────────
    constexpr double   ALPHA               = 0.7;
    constexpr double   BETA                = 0.3;
    constexpr int      DEFAULT_CLUSTER_SZ  = 4;
    constexpr double   THRESHOLD_PERCENTILE = 0.40;

    // ── Sentinel value for "infinite" time ───────────────────
    constexpr uint64_t INF_TIME = std::numeric_limits<uint64_t>::max();

    // ── Load-balance threshold ────────────────────────────────
    constexpr double   LOAD_IMBALANCE_THR  = 0.80;
}

// ============================================================
//  Task classification enums
// ============================================================
enum class Criticality  : uint8_t { LOW = 0, MEDIUM = 1, HIGH = 2 };
enum class TaskType     : uint8_t { COMPUTE_HEAVY = 0, COMPUTE_LIGHT = 1,
                                     MEMORY_BOUND  = 2, IO_BOUND     = 3 };
enum class TaskState    : uint8_t { PENDING = 0, READY = 1, RUNNING = 2,
                                     COMPLETED = 3, FAILED = 4, DEFERRED = 5 };

// ============================================================
//  Infrastructure classification enums
// ============================================================
enum class ClusterType  : uint8_t { COMM_HEAVY = 0, COMM_LESS = 1 };
enum class CoreState    : uint8_t { IDLE = 0, BUSY = 1, FAULTY = 2 };
enum class ChipletState : uint8_t { ACTIVE = 0, DEGRADED = 1, OFFLINE = 2 };

// ============================================================
//  String converters
// ============================================================
inline std::string to_string(Criticality c) {
    switch(c) { case Criticality::LOW: return "LOW"; case Criticality::MEDIUM: return "MEDIUM"; case Criticality::HIGH: return "HIGH"; }
    return "UNKNOWN";
}
inline std::string to_string(TaskType t) {
    switch(t) { case TaskType::COMPUTE_HEAVY: return "COMPUTE_HEAVY"; case TaskType::COMPUTE_LIGHT: return "COMPUTE_LIGHT"; case TaskType::MEMORY_BOUND: return "MEMORY_BOUND"; case TaskType::IO_BOUND: return "IO_BOUND"; }
    return "UNKNOWN";
}
inline std::string to_string(TaskState s) {
    switch(s) { case TaskState::PENDING: return "PENDING"; case TaskState::READY: return "READY"; case TaskState::RUNNING: return "RUNNING"; case TaskState::COMPLETED: return "COMPLETED"; case TaskState::FAILED: return "FAILED"; case TaskState::DEFERRED: return "DEFERRED"; }
    return "UNKNOWN";
}
inline std::string to_string(ClusterType ct) {
    return (ct == ClusterType::COMM_HEAVY) ? "COMM_HEAVY" : "COMM_LESS";
}
inline std::string to_string(CoreState cs) {
    switch(cs) { case CoreState::IDLE: return "IDLE"; case CoreState::BUSY: return "BUSY"; case CoreState::FAULTY: return "FAULTY"; }
    return "UNKNOWN";
}
