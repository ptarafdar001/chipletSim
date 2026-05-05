

/**
 * @file algorithms.hpp
 * @brief Cycle-accurate task-mapping algorithms for HexaMesh.
 *
 * Algorithms 1, 2, 3 remain structurally identical to the original.
 * Cycle-accuracy is introduced via:
 *
 *   Algorithm 3 (TaskToCoreMapping):
 *     – actual_duration   = exec_time + type-specific penalty
 *     – start_cycle       = max(data_ready_cycle, core.busy_until, task.release_time)
 *     – finish_cycle      = start_cycle + actual_duration
 *     – comm_latency_from[pred] is pre-filled by the engine before Algorithm 3
 *       is invoked (or is zero for the initial placement sweep; the drain
 *       simulation recomputes it once predecessors have real finish_cycles).
 *     – data_ready_cycle  = max over predecessors of
 *                           (pred.finish_cycle + comm_latency_from[pred.id])
 */

#pragma once

#include "types.hpp"
#include "task.hpp"
#include "cluster.hpp"
#include "chiplet.hpp"
#include "noc_model.hpp"

#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <regex>

// ============================================================
//  Event log and statistics
// ============================================================
struct MappingEvent {
    enum class Kind { TASK_PLACED, TASK_DEFERRED, TASK_FAILED,
                      REPLICA_PLACED, FAULT_DETECTED, FAULT_RECOVERED,
                      CLUSTER_FORMED, CLUSTER_PLACED, COMM_LATENCY };
    Kind        kind;
    uint64_t    cycle;
    int         task_id    {-1};
    int         cluster_id {-1};
    int         chiplet_id {-1};
    int         core_id    {-1};
    std::string note;
};

struct MappingStats {
    int      tasks_placed       {0};
    int      tasks_deferred     {0};
    int      tasks_failed       {0};
    int      replicas_created   {0};
    int      faults_detected    {0};
    int      faults_recovered   {0};
    int      clusters_formed    {0};
    int      spill_events       {0};

    double   avg_chiplet_utilization {0.0};
    double   avg_core_utilization    {0.0};
    double   load_imbalance_index    {0.0};

    // Cycle-accurate additions
    uint64_t total_comm_latency_cycles {0};
    uint64_t total_wait_cycles         {0};
    uint64_t critical_path_length      {0};
    double   avg_comm_latency          {0.0};
    uint64_t max_comm_latency          {0};
    uint64_t min_comm_latency          {0};
    int      total_comm_edges          {0};
    double   avg_task_wait_cycles      {0.0};
};

// ============================================================
//  Forward declarations
// ============================================================
class TaskToCoreMapping;
class ClusterToChipletMapping;

// ============================================================
//  Algorithm 3: Adaptive_Task_to_Core_Mapping(t, ci)
// ============================================================
class TaskToCoreMapping {
public:
    /**
     * Place `task` on an available core in `chiplet`.
     *
     * OPT-A: Speculative scheduling.
     *   Predecessors in RUNNING state are accepted — their finish_cycle is
     *   already committed.  data_ready_cycle is updated to reflect the maximum
     *   of all predecessor finish times (+ comm latency).  The task is placed
     *   now so the core is reserved; it will stall only until data arrives.
     *
     * start_cycle = max(data_ready_cycle, core.busy_until, task.release_time)
     * finish_cycle = start_cycle + task.actual_duration
     */
    static bool run(Task&                      task,
                    Chiplet&                   chiplet,
                    TaskGraph&                 task_graph,
                    uint64_t                   current_cycle,
                    std::vector<MappingEvent>* log = nullptr)
    {
        // ── OPT-A: Speculative dependency check ──────────────────────────────
        // Accept RUNNING predecessors with committed finish_cycle.
        // Reject only if any predecessor has not been placed yet.
        if (task.state == TaskState::PENDING ||
            task.state == TaskState::READY)
        {
            uint64_t speculative_ready = task.data_ready_cycle;
            bool     can_speculate     = true;

            for (int pid : task.predecessors) {
                Task& pred = task_graph.get_task(pid);

                if (pred.state == TaskState::COMPLETED) {
                    uint64_t lat = task.comm_latency_from.count(pid)
                                   ? task.comm_latency_from.at(pid) : 0;
                    speculative_ready = std::max(speculative_ready,
                                                  pred.finish_cycle + lat);
                }
                else if (pred.state == TaskState::RUNNING) {
                    // finish_cycle committed at placement time — safe to use.
                    uint64_t lat = task.comm_latency_from.count(pid)
                                   ? task.comm_latency_from.at(pid) : 0;
                    speculative_ready = std::max(speculative_ready,
                                                  pred.finish_cycle + lat);
                }
                else {
                    // PENDING or DEFERRED: not yet placed, finish unknown.
                    can_speculate = false;
                    break;
                }
            }

            if (!can_speculate) {
                task.state = TaskState::DEFERRED;
                if (log) log->push_back({MappingEvent::Kind::TASK_DEFERRED,
                    current_cycle, task.id, task.cluster_id, chiplet.id, -1,
                    "Predecessor not yet placed (speculative blocked)"});
                return false;
            }

            // Propagate speculative bound so start computation is correct.
            task.data_ready_cycle = speculative_ready;
            task.state            = TaskState::READY;
        }

        if (chiplet.available_core_count(current_cycle) == 0) {
            task.state = TaskState::DEFERRED;
            if (log) log->push_back({MappingEvent::Kind::TASK_DEFERRED,
                current_cycle, task.id, task.cluster_id, chiplet.id, -1,
                "No core available"});
            return false;
        }

        task.compute_slack(current_cycle);

        Core* core = chiplet.next_available_core(current_cycle, task.type);
        if (!core) {
            task.state = TaskState::DEFERRED;
            if (log) log->push_back({MappingEvent::Kind::TASK_DEFERRED,
                current_cycle, task.id, task.cluster_id, chiplet.id, -1,
                "No suitable core"});
            return false;
        }

        // OPT-A: all three timing constraints ──────────────────────
        uint64_t start  = std::max({task.data_ready_cycle,
                                    core->busy_until,
                                    task.release_time});
        uint64_t finish = start + task.actual_duration;
        task.wait_cycles = (start > current_cycle) ? start - current_cycle : 0;

        core->state           = CoreState::BUSY;
        core->current_task_id = task.id;
        core->last_task_type  = task.type;
        core->busy_until      = finish;
        core->cycles_busy    += task.actual_duration;
        ++core->tasks_executed;

        task.state            = TaskState::RUNNING;
        task.assigned_chiplet = chiplet.id;
        task.assigned_core    = core->local_id;
        task.start_cycle      = start;
        task.finish_cycle     = finish;   // committed; successors may read this

        ++chiplet.tasks_hosted;
        chiplet.total_exec_cycles += task.actual_duration;

        if (log) log->push_back({MappingEvent::Kind::TASK_PLACED,
            start, task.id, task.cluster_id, chiplet.id, core->local_id,
            "start=" + std::to_string(start) +
            " finish=" + std::to_string(finish) +
            " dur=" + std::to_string(task.actual_duration) +
            " wait=" + std::to_string(task.wait_cycles) +
            (task.data_ready_cycle > core->busy_until ? " [data-stall]"
                                                      : " [core-stall]")});

        // HIGH criticality → replica core
        if (task.criticality == Criticality::HIGH) {
            Core* rep = chiplet.replica_core(core->local_id, current_cycle);
            if (rep) {
                uint64_t rep_start = task.is_low_slack() ? start : finish;
                rep->state           = CoreState::BUSY;
                rep->current_task_id = task.id;
                rep->last_task_type  = task.type;
                rep->busy_until      = rep_start + task.actual_duration;
                rep->cycles_busy    += task.actual_duration;
                ++rep->tasks_executed;
                task.replica_core    = rep->local_id;

                if (log) log->push_back({MappingEvent::Kind::REPLICA_PLACED,
                    rep_start, task.id, task.cluster_id, chiplet.id, rep->local_id,
                    task.is_low_slack() ? "Parallel replica (low slack)"
                                        : "Deferred replica (high slack)"});
            }
        }

        return true;
    }

    static void handle_fault(Chiplet&                   chiplet,
                              int                        local_core_id,
                              TaskGraph&                 task_graph,
                              std::vector<int>&          ready_queue,
                              uint64_t                   current_cycle,
                              std::vector<MappingEvent>* log = nullptr)
    {
        if (local_core_id < 0 || local_core_id >= chiplet.core_count()) return;
        Core& core = chiplet.cores[local_core_id];

        if (log) log->push_back({MappingEvent::Kind::FAULT_DETECTED,
            current_cycle, core.current_task_id, -1, chiplet.id, local_core_id,
            "Core FAULTY"});

        int evicted = core.current_task_id;
        chiplet.mark_core_faulty(local_core_id);
        core.current_task_id = -1;

        if (evicted >= 0) {
            Task& task      = task_graph.get_task(evicted);
            bool replica_ok = (task.replica_core >= 0 &&
                               task.replica_core != local_core_id);
            if (!replica_ok) {
                task.state            = TaskState::READY;
                task.assigned_chiplet = -1;
                task.assigned_core    = -1;
                ready_queue.push_back(evicted);
                if (log) log->push_back({MappingEvent::Kind::FAULT_RECOVERED,
                    current_cycle, evicted, task.cluster_id, chiplet.id,
                    local_core_id, "No replica — task requeued"});
            } else {
                task.state = TaskState::RUNNING;
                if (log) log->push_back({MappingEvent::Kind::FAULT_RECOVERED,
                    current_cycle, evicted, task.cluster_id, chiplet.id,
                    local_core_id, "Replica covers fault"});
            }
        }
    }
};

// ============================================================
//  Algorithm 2: Dynamic_Cluster_to_Chiplet_Mapping(Ω, Gc)
// ============================================================
class ClusterToChipletMapping {
public:
    static void run(Cluster&                   cluster,
                    HexaMesh&                  hex_mesh,
                    TaskGraph&                 task_graph,
                    uint64_t                   current_cycle,
                    std::vector<MappingEvent>* log = nullptr)
    {
        // ── OPT-B: chiplets where this cluster's predecessors are placed ──────
        // Returns predecessor chiplets (most-used first) plus their neighbours.
        auto predecessor_chiplets = [&]() -> std::vector<int> {
            std::unordered_map<int, int> freq;
            for (int tid : cluster.task_ids) {
                Task& t = task_graph.get_task(tid);
                for (int pid : t.predecessors) {
                    Task& pred = task_graph.get_task(pid);
                    if (pred.assigned_chiplet >= 0)
                        freq[pred.assigned_chiplet]++;
                }
            }
            if (freq.empty()) return {};

            std::vector<int> sorted;
            sorted.reserve(freq.size());
            for (auto& [cid, _] : freq) sorted.push_back(cid);
            std::sort(sorted.begin(), sorted.end(),
                      [&](int a, int b){ return freq[a] > freq[b]; });

            // Expand to direct neighbours for spill headroom.
            std::unordered_set<int> seen(sorted.begin(), sorted.end());
            std::vector<int> expanded = sorted;
            for (int cid : sorted) {
                for (int nid : hex_mesh.chiplet(cid).neighbours) {
                    if (!seen.count(nid) &&
                        hex_mesh.chiplet(nid).state != ChipletState::OFFLINE) {
                        expanded.push_back(nid);
                        seen.insert(nid);
                    }
                }
            }
            return expanded;
        };

        // Least-loaded chiplet among candidates that still has free cores.
        auto best_by_load = [&](const std::vector<int>& cands) -> int {
            if (cands.empty()) return -1;
            int min_load = INT32_MAX;
            for (int cid : cands) {
                auto& ch = hex_mesh.chiplet(cid);
                if (ch.state != ChipletState::OFFLINE)
                    min_load = std::min(min_load, ch.tasks_hosted);
            }
            std::vector<int> tied;
            for (int cid : cands) {
                auto& ch = hex_mesh.chiplet(cid);
                if (ch.state != ChipletState::OFFLINE &&
                    ch.tasks_hosted == min_load &&
                    ch.available_core_count(current_cycle) > 0)
                    tied.push_back(cid);
            }
            if (tied.empty()) return -1;
            return tied[cluster.id % tied.size()];
        };

        // OPT-B: prefer locality over pure load balance.
        int init_chiplet = best_by_load(predecessor_chiplets());

        if (init_chiplet < 0) {
            if (cluster.type == ClusterType::COMM_HEAVY)
                init_chiplet = best_by_load(hex_mesh.high_connectivity_chiplets());
            else
                init_chiplet = best_by_load(hex_mesh.edge_chiplets());
        }

        if (init_chiplet < 0) {
            std::vector<int> all;
            for (auto& ch : hex_mesh.chiplets())
                if (ch.state != ChipletState::OFFLINE) all.push_back(ch.id);
            init_chiplet = best_by_load(all);
        }
        if (init_chiplet < 0) {
            if (log) log->push_back({MappingEvent::Kind::TASK_DEFERRED,
                current_cycle, -1, cluster.id, -1, -1, "No chiplet available"});
            return;
        }
        cluster.primary_chiplet = init_chiplet;

        std::vector<int> ready_list = cluster.task_ids;

        Chiplet& primary = hex_mesh.chiplet(init_chiplet);
        if (static_cast<int>(ready_list.size()) <=
            primary.available_core_count(current_cycle))
        {
            for (int t_id : ready_list)
                TaskToCoreMapping::run(task_graph.get_task(t_id), primary,
                                       task_graph, current_cycle, log);
            if (log) log->push_back({MappingEvent::Kind::CLUSTER_PLACED,
                current_cycle, -1, cluster.id, init_chiplet, -1,
                "Single-chiplet placement"});
            return;
        }

        // ── BFS spill-over ────────────────────────────────────────────────────
        // OPT-D: collect predecessor chiplet IDs for proximity scoring.
        std::unordered_set<int> pred_chiplet_set;
        for (int tid : cluster.task_ids) {
            Task& t = task_graph.get_task(tid);
            for (int pid : t.predecessors) {
                Task& pred = task_graph.get_task(pid);
                if (pred.assigned_chiplet >= 0)
                    pred_chiplet_set.insert(pred.assigned_chiplet);
            }
        }

        // Proximity score: 2 = IS a predecessor chiplet,
        //                  1 = adjacent to one,  0 = unrelated.
        auto proximity_score = [&](int cid) -> int {
            if (pred_chiplet_set.count(cid)) return 2;
            for (int nid : hex_mesh.chiplet(cid).neighbours)
                if (pred_chiplet_set.count(nid)) return 1;
            return 0;
        };

        std::queue<int>         Q;
        std::unordered_set<int> visited;

        visited.insert(init_chiplet);  // mark at push-time
        Q.push(init_chiplet);

        while (!ready_list.empty()) {
            if (Q.empty()) {
                if (log) log->push_back({MappingEvent::Kind::TASK_DEFERRED,
                    current_cycle, -1, cluster.id, -1, -1,
                    "Spill-over: no chiplets left"});
                break;
            }
            int cid = Q.front(); Q.pop();

            Chiplet& ci = hex_mesh.chiplet(cid);
            if (ci.state == ChipletState::OFFLINE) continue;
            if (cid != init_chiplet) cluster.spill_chiplets.push_back(cid);

            // cache and decrement locally.
            int avail = ci.available_core_count(current_cycle);

            std::vector<int> remaining;
            for (int t_id : ready_list) {
                if (avail <= 0) { remaining.push_back(t_id); continue; }
                Task& task  = task_graph.get_task(t_id);
                bool placed = TaskToCoreMapping::run(task, ci, task_graph,
                                                     current_cycle, log);
                if (!placed) remaining.push_back(t_id);
                else         --avail;
            }
            ready_list = remaining;

            // OPT-D: sort neighbours by proximity before pushing.
            std::vector<int> nbrs;
            for (int nid : ci.neighbours) {
                if (visited.count(nid)) continue;
                Chiplet& cj = hex_mesh.chiplet(nid);
                if (cj.state != ChipletState::OFFLINE &&
                    cj.available_core_count(current_cycle) > 0) {
                    nbrs.push_back(nid);
                    visited.insert(nid);   // mark before push
                }
            }
            std::stable_sort(nbrs.begin(), nbrs.end(),
                [&](int a, int b){
                    return proximity_score(a) > proximity_score(b);
                });
            for (int nid : nbrs) Q.push(nid);
        }

        if (log) log->push_back({MappingEvent::Kind::CLUSTER_PLACED,
            current_cycle, -1, cluster.id, init_chiplet, -1,
            "Spill-over: used " +
            std::to_string(cluster.spill_chiplets.size()) + " extra chiplet(s)"});
    }
};

// ============================================================
//  Algorithm 1: Task_Clustering
// ============================================================
class TaskClustering {
public:
    static std::vector<Cluster> run(
            TaskGraph&                 task_graph,
            HexaMesh&                  hex_mesh,
            int                        cluster_size  = Constants::DEFAULT_CLUSTER_SZ,
            double                     alpha         = Constants::ALPHA,
            double                     beta          = Constants::BETA,
            uint64_t                   current_cycle = 0,
            std::vector<MappingEvent>* log           = nullptr)
    {
        const auto topo_order = task_graph.topological_order();

        // ═══════════════════════════════════════════════════════════════
        // ADD THIS SECTION AFTER LINE ~465 (after topo_order)
        // ═══════════════════════════════════════════════════════════════
        
        // GRAPH-AWARE CLUSTERING: Identify separate application graphs
        std::unordered_map<int, std::vector<int>> app_graphs;
        std::cout << "\n[GRAPH-AWARE CLUSTERING] Detecting application graphs..." << std::endl;
        
        for (auto& [id, task] : task_graph.tasks()) {
            // Extract graph ID from task name: "g0_src_t45" → graph_id = 0
            int gid = -1;
            
            // Try to extract graph ID
            std::regex g_regex("g(\\d+)_.*");
            std::smatch m;
            if (std::regex_match(task.name, m, g_regex)) {
                gid = std::stoi(m[1].str());
            }
            
            app_graphs[gid].push_back(id);
        }
        
        std::cout << "  Found " << app_graphs.size() << " application graphs:" << std::endl;
        for (auto& [gid, tasks] : app_graphs) {
            std::cout << "    Graph " << (gid >= 0 ? std::to_string(gid) : "unknown") 
                    << ": " << tasks.size() << " tasks" << std::endl;
        }
        

        uint64_t max_comm = task_graph.max_comm_volume();
        uint64_t max_exec = task_graph.max_exec_time();

        std::unordered_map<int, std::unordered_map<int,double>> score_map;
        double sum_score = 0.0; int edge_count = 0;

        // Single pass: build score_map.
        for (auto& e : task_graph.edges()) {
            double C_hat = max_comm > 0
                           ? static_cast<double>(e.volume) / max_comm : 0.0;
            double L_hat = max_exec > 0
                           ? static_cast<double>(
                               task_graph.get_task(e.src_id).exec_time +
                               task_graph.get_task(e.dst_id).exec_time) /
                             (2.0 * max_exec) : 0.0;
            double S = alpha * C_hat + beta * L_hat;
            score_map[e.src_id][e.dst_id] = S;
            sum_score += S; ++edge_count;
        }

        const double threshold = edge_count > 0 ? sum_score / edge_count : 0.0;

        // build has_high_comm from score_map (no second edge-list scan).
        std::unordered_set<int> unassigned;
        for (auto& [id,t] : task_graph.tasks()) unassigned.insert(id);

        std::unordered_set<int> has_high_comm;
        for (auto& [src, dsts] : score_map)
            for (auto& [dst, S] : dsts)
                if (S >= threshold) { has_high_comm.insert(src);
                                      has_high_comm.insert(dst); }

        
        
        // ═══════════════════════════════════════════════════════════════
        // REPLACE THE EXISTING CLUSTERING LOOP WITH THIS:
        // ═══════════════════════════════════════════════════════════════
        
        std::vector<Cluster> all_clusters;
        int cluster_id = 0;
        
        // Process each application graph separately
        for (auto& [graph_id, graph_task_list] : app_graphs) {
            
            std::cout << "\n[CLUSTERING] Processing graph " 
                    << (graph_id >= 0 ? std::to_string(graph_id) : "unknown") 
                    << " (" << graph_task_list.size() << " tasks)" << std::endl;
            
            // Create topological order for THIS graph only
            std::unordered_set<int> graph_task_set(graph_task_list.begin(), graph_task_list.end());
            std::vector<int> graph_topo_order;
            for (int tid : topo_order) {
                if (graph_task_set.count(tid)) {
                    graph_topo_order.push_back(tid);
                }
            }
            
            // Keep track of unassigned tasks for THIS graph
            std::unordered_set<int> unassigned(graph_task_set.begin(), graph_task_set.end());
            
            // Identify high-communication tasks within THIS graph only
            std::unordered_set<int> has_high_comm;
            for (auto& [src, dsts] : score_map) {
                if (!graph_task_set.count(src)) continue;
                for (auto& [dst, S] : dsts) {
                    if (graph_task_set.count(dst) && S >= threshold) {
                        has_high_comm.insert(src);
                        has_high_comm.insert(dst);
                    }
                }
            }
            
            // ── Phase 1: COMM_HEAVY clusters (WITHIN THIS GRAPH) ──
            for (int t_id : graph_topo_order) {
                if (!unassigned.count(t_id)) continue;
                if (!has_high_comm.count(t_id)) continue;
                
                Cluster omega; omega.id = cluster_id++;
                
                auto absorb = [&](int tid) {
                    Task& t = task_graph.get_task(tid);
                    omega.task_ids.push_back(tid);
                    omega.total_exec_cost += t.exec_time;
                    omega.update_max_criticality(t.criticality);
                    t.cluster_id = omega.id;
                    unassigned.erase(tid);
                };
                absorb(t_id);
                
                std::queue<int> q; q.push(t_id);
                while (!q.empty()) {
                    int u = q.front(); q.pop();
                    
                    // OPT-C: absorb high-affinity successors (WITHIN THIS GRAPH)
                    for (int v : task_graph.successors(u)) {
                        if (!unassigned.count(v)) continue;
                        if (!graph_task_set.count(v)) continue; // ← KEY: Stay within graph
                        
                        double S = 0.0;
                        if (score_map.count(u) && score_map.at(u).count(v))
                            S = score_map.at(u).at(v);
                        if (S >= threshold &&
                            omega.task_count() + 1 <= cluster_size) {
                            omega.total_comm_volume += task_graph.comm_volume(u, v);
                            absorb(v); q.push(v);
                        }
                    }
                    
                    // OPT-C: also absorb high-affinity predecessors (WITHIN THIS GRAPH)
                    for (int p : task_graph.predecessors(u)) {
                        if (!unassigned.count(p)) continue;
                        if (!graph_task_set.count(p)) continue; // ← KEY: Stay within graph
                        
                        double S = 0.0;
                        if (score_map.count(p) && score_map.at(p).count(u))
                            S = score_map.at(p).at(u);
                        if (S >= threshold &&
                            omega.task_count() + 1 <= cluster_size) {
                            omega.total_comm_volume += task_graph.comm_volume(p, u);
                            absorb(p); q.push(p);
                        }
                    }
                }
                
                omega.type = ClusterType::COMM_HEAVY;
                if (omega.task_count() > 1) {
                    double sc = 0.0; int n = 0;
                    for (int a : omega.task_ids)
                        for (int b : omega.task_ids) {
                            if (a == b) continue;
                            if (score_map.count(a) && score_map.at(a).count(b))
                            { sc += score_map.at(a).at(b); ++n; }
                        }
                    omega.avg_affinity_score = n > 0 ? sc / n : 0.0;
                }
                
                if (log) log->push_back({MappingEvent::Kind::CLUSTER_FORMED,
                    current_cycle, -1, omega.id, -1, -1,
                    "COMM_HEAVY (graph_aware) tasks=" + std::to_string(omega.task_count())});
                
                ClusterToChipletMapping::run(omega, hex_mesh, task_graph,
                                            current_cycle, log);
                all_clusters.push_back(std::move(omega));
            }
            
            // ── Phase 2: COMM_LESS clusters (WITHIN THIS GRAPH) ──
            std::vector<int> unassigned_topo;
            unassigned_topo.reserve(unassigned.size());
            for (int x : graph_topo_order)
                if (unassigned.count(x)) unassigned_topo.push_back(x);
            
            size_t cursor = 0;
            while (cursor < unassigned_topo.size()) {
                Cluster omega; omega.id = cluster_id++;
                omega.type = ClusterType::COMM_LESS;
                
                while (cursor < unassigned_topo.size() &&
                    omega.task_count() < cluster_size) {
                    int x = unassigned_topo[cursor++];
                    if (!unassigned.count(x)) continue;
                    Task& t = task_graph.get_task(x);
                    omega.task_ids.push_back(x);
                    omega.total_exec_cost += t.exec_time;
                    omega.update_max_criticality(t.criticality);
                    t.cluster_id = omega.id;
                    unassigned.erase(x);
                }
                
                if (omega.task_count() == 0) break;
                
                if (log) log->push_back({MappingEvent::Kind::CLUSTER_FORMED,
                    current_cycle, -1, omega.id, -1, -1,
                    "COMM_LESS (graph_aware) tasks=" + std::to_string(omega.task_count())});
                
                ClusterToChipletMapping::run(omega, hex_mesh, task_graph,
                                            current_cycle, log);
                all_clusters.push_back(std::move(omega));
            }
            
            std::cout << "  Graph " << (graph_id >= 0 ? std::to_string(graph_id) : "unknown")
                    << " clustered into " << (cluster_id - (all_clusters.size() - graph_task_list.size()))
                    << " clusters" << std::endl;
        }
        
        std::cout << "\n[CLUSTERING] Total clusters: " << all_clusters.size() << "\n" << std::endl;
        
        return all_clusters;

        // std::vector<Cluster> all_clusters;
        // int cluster_id = 0;

        // // ── Phase 1: COMM_HEAVY clusters ──────────────────────────────
        // for (int t_id : topo_order) {
        //     if (!unassigned.count(t_id)) continue;
        //     if (!has_high_comm.count(t_id)) continue;

        //     Cluster omega; omega.id = cluster_id++;

        //     auto absorb = [&](int tid) {
        //         Task& t = task_graph.get_task(tid);
        //         omega.task_ids.push_back(tid);
        //         omega.total_exec_cost += t.exec_time;
        //         omega.update_max_criticality(t.criticality);
        //         t.cluster_id = omega.id;
        //         unassigned.erase(tid);
        //     };
        //     absorb(t_id);

        //     std::queue<int> q; q.push(t_id);
        //     while (!q.empty()) {
        //         int u = q.front(); q.pop();

        //         // OPT-C: absorb high-affinity successors (original direction).
        //         for (int v : task_graph.successors(u)) {
        //             if (!unassigned.count(v)) continue;
        //             double S = 0.0;
        //             if (score_map.count(u) && score_map.at(u).count(v))
        //                 S = score_map.at(u).at(v);
        //             if (S >= threshold &&
        //                 omega.task_count() + 1 <= cluster_size) {
        //                 omega.total_comm_volume += task_graph.comm_volume(u, v);
        //                 absorb(v); q.push(v);
        //             }
        //         }

        //         // OPT-C: also absorb high-affinity predecessors.
        //         // Ensures tightly coupled (pred→u) pairs land in the same
        //         // cluster even when the predecessor wasn't a BFS root.
        //         for (int p : task_graph.predecessors(u)) {
        //             if (!unassigned.count(p)) continue;
        //             double S = 0.0;
        //             if (score_map.count(p) && score_map.at(p).count(u))
        //                 S = score_map.at(p).at(u);
        //             if (S >= threshold &&
        //                 omega.task_count() + 1 <= cluster_size) {
        //                 omega.total_comm_volume += task_graph.comm_volume(p, u);
        //                 absorb(p); q.push(p);
        //             }
        //         }
        //     }

        //     omega.type = ClusterType::COMM_HEAVY;
        //     if (omega.task_count() > 1) {
        //         double sc = 0.0; int n = 0;
        //         // skip self-pairs; only real intra-cluster edges.
        //         for (int a : omega.task_ids)
        //             for (int b : omega.task_ids) {
        //                 if (a == b) continue;
        //                 if (score_map.count(a) && score_map.at(a).count(b))
        //                 { sc += score_map.at(a).at(b); ++n; }
        //             }
        //         omega.avg_affinity_score = n > 0 ? sc / n : 0.0;
        //     }

        //     if (log) log->push_back({MappingEvent::Kind::CLUSTER_FORMED,
        //         current_cycle, -1, omega.id, -1, -1,
        //         "Phase1 COMM_HEAVY tasks=" + std::to_string(omega.task_count())});

        //     ClusterToChipletMapping::run(omega, hex_mesh, task_graph,
        //                                   current_cycle, log);
        //     all_clusters.push_back(std::move(omega));
        // }

        // // ── Phase 2: COMM_LESS clusters ───────────────────────────────
        // std::vector<int> unassigned_topo;
        // unassigned_topo.reserve(unassigned.size());
        // for (int x : topo_order)
        //     if (unassigned.count(x)) unassigned_topo.push_back(x);

        // size_t cursor = 0;
        // while (cursor < unassigned_topo.size()) {
        //     Cluster omega; omega.id = cluster_id++;
        //     omega.type = ClusterType::COMM_LESS;

        //     while (cursor < unassigned_topo.size() &&
        //            omega.task_count() < cluster_size) {
        //         int x = unassigned_topo[cursor++];
        //         if (!unassigned.count(x)) continue;
        //         Task& t = task_graph.get_task(x);
        //         omega.task_ids.push_back(x);
        //         omega.total_exec_cost += t.exec_time;
        //         omega.update_max_criticality(t.criticality);
        //         t.cluster_id = omega.id;
        //         unassigned.erase(x);
        //     }

        //     if (omega.task_count() == 0) break;

        //     if (log) log->push_back({MappingEvent::Kind::CLUSTER_FORMED,
        //         current_cycle, -1, omega.id, -1, -1,
        //         "Phase2 COMM_LESS tasks=" + std::to_string(omega.task_count())});

        //     ClusterToChipletMapping::run(omega, hex_mesh, task_graph,
        //                                   current_cycle, log);
        //     all_clusters.push_back(std::move(omega));
        // }

        // return all_clusters;
    }
};