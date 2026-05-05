/**
 * @file task.hpp
 * @brief Task and TaskGraph for the cycle-accurate HexaMesh simulator.
 */

#pragma once

#include "types.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>
#include <map>
#include <stdexcept>

// ============================================================
//  Task
// ============================================================
struct Task {
    // ── Identity ─────────────────────────────────────────────
    int         id          {-1};
    std::string name        {};

    // ── Classification ────────────────────────────────────────
    TaskType    type        {TaskType::COMPUTE_LIGHT};
    Criticality criticality {Criticality::LOW};

    // ── Static timing (from benchmark definition) ────────────
    uint64_t    exec_time     {1};              // abstract compute cycles
    uint64_t    deadline      {Constants::INF_TIME};
    uint64_t    release_time  {0};

    // ── Cycle-accurate timing (set by engine) ─────────────────
    uint64_t    actual_duration   {0};  // exec_time + memory/IO penalty
    uint64_t    data_ready_cycle  {0};  // when all input data has arrived
    uint64_t    wait_cycles       {0};  // stall waiting for data (data_ready − pred_finish)
    uint64_t    slack_time        {Constants::INF_TIME};

    // comm latency from each predecessor (chiplet-hop-dependent, set by engine)
    std::unordered_map<int, uint64_t> comm_latency_from; // pred_id → cycles

    // ── Runtime tracking ──────────────────────────────────────
    TaskState   state         {TaskState::PENDING};
    uint64_t    start_cycle   {0};   // cycle execution begins on core
    uint64_t    finish_cycle  {0};   // start_cycle + actual_duration

    // ── Placement ─────────────────────────────────────────────
    int         assigned_chiplet  {-1};
    int         assigned_core     {-1};
    int         cluster_id        {-1};
    int         replica_core      {-1};

    // ── DAG linkage ───────────────────────────────────────────
    std::vector<int> predecessors;
    std::vector<int> successors;

    // ── Helpers ───────────────────────────────────────────────
    bool is_high_criticality() const { return criticality == Criticality::HIGH; }
    bool has_deadline()        const { return deadline != Constants::INF_TIME; }

    // Actual execution duration including type-specific stall penalties
    static uint64_t compute_actual_duration(uint64_t exec_time, TaskType type) {
        uint64_t penalty = 0;
        switch (type) {
            case TaskType::MEMORY_BOUND: penalty = Constants::MEM_ACCESS_PENALTY; break;
            case TaskType::IO_BOUND:     penalty = Constants::IO_STALL_PENALTY;   break;
            default: break;
        }
        return exec_time + penalty;
    }

    // The earliest cycle this task can START on its assigned core:
    //   max(data_ready_cycle, core_free_at, release_time)
    uint64_t earliest_start(uint64_t core_free_at) const {
        uint64_t ready = std::max(data_ready_cycle, release_time);
        return std::max(ready, core_free_at);
    }

    void compute_slack(uint64_t current_cycle) {
        if (has_deadline()) {
            slack_time = (deadline > current_cycle + actual_duration)
                         ? deadline - current_cycle - actual_duration : 0;
        }
    }

    bool is_low_slack() const {
        return slack_time != Constants::INF_TIME && slack_time < actual_duration * 2;
    }
};

// ============================================================
//  Communication edge
// ============================================================
struct CommEdge {
    int      src_id   {-1};
    int      dst_id   {-1};
    uint64_t volume   {0};    // bytes
    double   score    {0.0};  // affinity score (set by Algorithm 1)
};

// ============================================================
//  TaskGraph  (application DAG)
// ============================================================
class TaskGraph {
public:

    Task& add_task(int id, std::string name,
                   TaskType type, Criticality crit,
                   uint64_t exec_time,
                   uint64_t deadline     = Constants::INF_TIME,
                   uint64_t release_time = 0)
    {
        if (tasks_.count(id))
            throw std::invalid_argument("Duplicate task id: " + std::to_string(id));
        Task t;
        t.id              = id;
        t.name            = std::move(name);
        t.type            = type;
        t.criticality     = crit;
        t.exec_time       = exec_time;
        t.actual_duration = Task::compute_actual_duration(exec_time, type);
        t.deadline        = deadline;
        t.release_time    = release_time;
        tasks_[id]        = std::move(t);
        return tasks_[id];
    }

    void add_edge(int src, int dst, uint64_t volume) {
        if (!tasks_.count(src) || !tasks_.count(dst))
            throw std::invalid_argument("add_edge: unknown task id");
        edges_.push_back({src, dst, volume, 0.0});
        tasks_[src].successors.push_back(dst);
        tasks_[dst].predecessors.push_back(src);
        adj_[src].push_back(dst);
        comm_[{src,dst}] = volume;
    }

    const std::vector<int>& predecessors(int id) const {
        return get_task(id).predecessors;
    }

    Task& get_task(int id)             { return tasks_.at(id); }
    const Task& get_task(int id) const { return tasks_.at(id); }

    uint64_t comm_volume(int u, int v) const {
        auto it = comm_.find({u,v});
        return (it != comm_.end()) ? it->second : 0;
    }

    const std::vector<CommEdge>&            edges()   const { return edges_; }
    const std::unordered_map<int,Task>&     tasks()   const { return tasks_; }
    const std::vector<int>& successors(int id)        const {
        static const std::vector<int> empty;
        auto it = adj_.find(id);
        return (it != adj_.end()) ? it->second : empty;
    }

    int task_count() const { return static_cast<int>(tasks_.size()); }
    int edge_count()  const { return static_cast<int>(edges_.size()); }

    std::vector<int> topological_order() const {
        std::unordered_map<int,int> in_deg;
        for (auto& [id,t] : tasks_) in_deg[id] = 0;
        for (auto& e : edges_)      in_deg[e.dst_id]++;

        std::vector<int> q, order;
        for (auto& [id,deg] : in_deg)
            if (deg == 0) q.push_back(id);

        while (!q.empty()) {
            int u = q.back(); q.pop_back();
            order.push_back(u);
            for (int v : successors(u))
                if (--in_deg[v] == 0) q.push_back(v);
        }
        if (static_cast<int>(order.size()) != task_count())
            throw std::runtime_error("TaskGraph has a cycle!");
        return order;
    }

    uint64_t max_exec_time() const {
        uint64_t m = 0;
        for (auto& [id,t] : tasks_) m = std::max(m, t.exec_time);
        return m;
    }
    uint64_t max_comm_volume() const {
        uint64_t m = 0;
        for (auto& e : edges_) m = std::max(m, e.volume);
        return m;
    }

private:
    std::unordered_map<int, Task>              tasks_;
    std::vector<CommEdge>                      edges_;
    std::unordered_map<int, std::vector<int>>  adj_;
    std::map<std::pair<int,int>, uint64_t>     comm_;
};
