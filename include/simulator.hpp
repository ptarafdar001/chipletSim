/**
 * @file simulator.hpp
 * @brief Cycle-accurate HexaMesh task-mapping simulator.
 *
 * What makes this cycle-accurate:
 *
 *  1. Communication latency on every DAG edge
 *     Each edge u→v contributes:
 *       intra-chiplet-egress + inter-chiplet-hops + serialization + intra-chiplet-ingress
 *     Computed by NoCModel for the exact (src_core,src_chiplet)→(dst_core,dst_chiplet) path.
 *
 *  2. Data-ready cycle for every successor
 *     data_ready(v) = max over all u in pred(v) of (u.finish_cycle + comm_latency(u,v))
 *     A task CANNOT start until ALL its input tokens arrive.
 *
 *  3. Core availability
 *     start(v) = max(data_ready(v), core.busy_until, v.release_time)
 *
 *  4. Memory/IO task penalties
 *     actual_duration = exec_time + MEM_ACCESS_PENALTY or IO_STALL_PENALTY
 *
 *  5. Bandwidth contention on inter-chiplet links
 *     NoCModel tracks per-link busy_until and serialises concurrent transfers.
 *
 *  6. Event-driven drain using a min-heap
 *     Clock jumps only to the next task-completion event.
 */

#pragma once

#include "types.hpp"
#include "task.hpp"
#include "cluster.hpp"
#include "chiplet.hpp"
#include "algorithms.hpp"
#include "noc_model.hpp"

#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <numeric>
#include <cassert>

class Simulator {
public:
    struct Config {
        int      rings           {Constants::DEFAULT_RINGS};
        int      mesh_dim        {Constants::MESH_DIM};
        int      cluster_size    {Constants::DEFAULT_CLUSTER_SZ};
        double   alpha           {Constants::ALPHA};
        double   beta            {Constants::BETA};
        double   fault_prob      {Constants::FAULT_PROBABILITY};
        bool     inject_faults   {true};
        bool     verbose         {false};
        unsigned fault_seed      {42};
    };

    explicit Simulator(const Config& cfg)
        : cfg_(cfg)
        , hex_mesh_(cfg.rings, cfg.mesh_dim)
        , noc_(hex_mesh_)
    {}

    HexaMesh&       mesh()       { return hex_mesh_; }
    const HexaMesh& mesh() const { return hex_mesh_; }
    const std::vector<MappingEvent>& event_log() const { return event_log_; }
    const MappingStats&              stats()     const { return stats_; }
    const std::vector<Cluster>&      clusters()  const { return clusters_; }

    uint64_t run(TaskGraph& tg) {
        event_log_.clear();
        stats_         = MappingStats{};
        current_cycle_ = 0;
        noc_.reset();

        if (cfg_.inject_faults)
            hex_mesh_.inject_faults(cfg_.fault_prob, cfg_.fault_seed);
        count_initial_faults();

        clusters_ = TaskClustering::run(tg, hex_mesh_,
                                         cfg_.cluster_size,
                                         cfg_.alpha, cfg_.beta,
                                         current_cycle_, &event_log_);
        stats_.clusters_formed = static_cast<int>(clusters_.size());

        drain_cycle_accurate(tg);
        compute_statistics(tg);
        if (cfg_.verbose) print_report(tg);
        return current_cycle_;
    }

private:
    Config                    cfg_;
    HexaMesh                  hex_mesh_;
    NoCModel                  noc_;
    std::vector<Cluster>      clusters_;
    std::vector<MappingEvent> event_log_;
    MappingStats              stats_;
    uint64_t                  current_cycle_{0};

    using PQ = std::priority_queue<
                    std::pair<uint64_t,int>,
                    std::vector<std::pair<uint64_t,int>>,
                    std::greater<std::pair<uint64_t,int>>>;

    void drain_cycle_accurate(TaskGraph& tg) {
        PQ pq;
        for (auto& [id,t] : tg.tasks())
            if (t.state == TaskState::RUNNING)
                pq.push({t.finish_cycle, t.id});

        std::vector<int> deferred;
        for (auto& [id,t] : tg.tasks())
            if (t.state == TaskState::DEFERRED || t.state == TaskState::PENDING)
                deferred.push_back(id);

        while (!pq.empty()) {
            auto [finish, tid] = pq.top(); pq.pop();
            Task& t = tg.get_task(tid);

            if (t.state != TaskState::RUNNING)   continue;
            if (t.finish_cycle != finish)         continue;

            t.state        = TaskState::COMPLETED;
            current_cycle_ = std::max(current_cycle_, finish);
            ++stats_.tasks_placed;

            if (t.assigned_chiplet >= 0 && t.assigned_core >= 0) {
                Core& co = hex_mesh_.chiplet(t.assigned_chiplet)
                                    .cores[t.assigned_core];
                if (co.state != CoreState::FAULTY) co.state = CoreState::IDLE;
                co.current_task_id = -1;
            }

            for (int succ_id : tg.successors(tid)) {
                Task& succ = tg.get_task(succ_id);
                if (succ.state == TaskState::COMPLETED) continue;

                uint64_t vol  = tg.comm_volume(tid, succ_id);
                uint64_t comm = compute_comm_latency(t, succ, vol, finish, tg);
                succ.comm_latency_from[tid] = comm;

                stats_.total_comm_latency_cycles += comm;
                ++stats_.total_comm_edges;
                stats_.max_comm_latency = std::max(stats_.max_comm_latency, comm);
                if (stats_.min_comm_latency == 0 || comm < stats_.min_comm_latency)
                    stats_.min_comm_latency = comm;

                bool all_done = true;
                for (int pid : succ.predecessors)
                    if (tg.get_task(pid).state != TaskState::COMPLETED) { all_done=false; break; }
                if (!all_done) continue;

                uint64_t data_ready = 0;
                for (int pid : succ.predecessors) {
                    const Task& pred = tg.get_task(pid);
                    uint64_t lat = succ.comm_latency_from.count(pid)
                                   ? succ.comm_latency_from.at(pid)
                                   : compute_comm_latency(pred, succ,
                                         tg.comm_volume(pid, succ_id),
                                         pred.finish_cycle, tg);
                    data_ready = std::max(data_ready, pred.finish_cycle + lat);
                }
                succ.data_ready_cycle = data_ready;
                uint64_t stall = (data_ready > finish) ? data_ready - finish : 0;
                succ.wait_cycles = stall;
                stats_.total_wait_cycles += stall;

                bool placed = schedule_ready_task(succ, tg, data_ready);
                if (placed) {
                    pq.push({succ.finish_cycle, succ.id});
                } else {
                    bool dup = false;
                    for (int d : deferred) if (d==succ_id){dup=true;break;}
                    if (!dup) deferred.push_back(succ_id);
                }
            }

            retry_deferred(tg, deferred, pq);
        }

        for (auto& [id,t] : tg.tasks()) {
            if (t.state==TaskState::DEFERRED || t.state==TaskState::PENDING)
                ++stats_.tasks_deferred;
            else if (t.state==TaskState::FAILED)
                ++stats_.tasks_failed;
        }
    }

    bool schedule_ready_task(Task& task, TaskGraph& tg, uint64_t min_start) {
        task.state = TaskState::READY;

        auto try_place = [&](int chiplet_id) -> bool {
            if (chiplet_id < 0) return false;
            Chiplet& ch = hex_mesh_.chiplet(chiplet_id);
            if (ch.state == ChipletState::OFFLINE) return false;

            Core* best = nullptr;
            uint64_t best_free = Constants::INF_TIME;
            for (auto& co : ch.cores) {
                if (co.is_faulty()) continue;
                if (!best || co.busy_until < best_free) {
                    best      = &co;
                    best_free = co.busy_until;
                }
            }
            if (!best) return false;

            uint64_t start  = task.earliest_start(best->busy_until);
            uint64_t finish = start + task.actual_duration;

            best->state           = CoreState::BUSY;
            best->current_task_id = task.id;
            best->last_task_type  = task.type;
            best->busy_until      = finish;
            best->cycles_busy    += task.actual_duration;
            ++best->tasks_executed;

            task.state            = TaskState::RUNNING;
            task.assigned_chiplet = ch.id;
            task.assigned_core    = best->local_id;
            task.start_cycle      = start;
            task.finish_cycle     = finish;
            uint64_t wt           = (start > min_start) ? start - min_start : 0;
            task.wait_cycles     += wt;
            stats_.total_wait_cycles += wt;

            ++ch.tasks_hosted;
            ch.total_exec_cycles += task.actual_duration;

            event_log_.push_back({MappingEvent::Kind::TASK_PLACED,
                start, task.id, task.cluster_id, ch.id, best->local_id,
                "CA start=" + std::to_string(start) +
                " finish=" + std::to_string(finish) +
                " data_ready=" + std::to_string(task.data_ready_cycle)});

            if (task.criticality == Criticality::HIGH) {
                Core* rep = ch.replica_core(best->local_id, start);
                if (rep) {
                    uint64_t rs = task.is_low_slack() ? start : finish;
                    rep->state = CoreState::BUSY;
                    rep->current_task_id = task.id;
                    rep->last_task_type  = task.type;
                    rep->busy_until      = rs + task.actual_duration;
                    rep->cycles_busy    += task.actual_duration;
                    ++rep->tasks_executed;
                    task.replica_core    = rep->local_id;
                    event_log_.push_back({MappingEvent::Kind::REPLICA_PLACED,
                        rs, task.id, task.cluster_id, ch.id, rep->local_id,
                        task.is_low_slack() ? "Parallel replica" : "Deferred replica"});
                }
            }
            return true;
        };

        int primary = -1; std::vector<int> spills;
        for (auto& cl : clusters_) {
            if (cl.id == task.cluster_id) {
                primary = cl.primary_chiplet;
                spills  = cl.spill_chiplets;
                break;
            }
        }
        if (try_place(primary)) return true;
        for (int s : spills) if (try_place(s)) return true;
        for (auto& ch : hex_mesh_.chiplets())
            if (ch.state != ChipletState::OFFLINE)
                if (try_place(ch.id)) return true;
        return false;
    }

    uint64_t compute_comm_latency(const Task& src, const Task& dst,
                                   uint64_t volume, uint64_t inject_cycle,
                                   TaskGraph&)
    {
        if (volume == 0) return 0;
        int sc=src.assigned_chiplet, dc=dst.assigned_chiplet;
        int sk=src.assigned_core,    dk=dst.assigned_core;
        if (sc>=0 && dc>=0 && sk>=0 && dk>=0)
            return noc_.transfer_latency(sc, sk, dc, dk, volume, inject_cycle);
        if (sc>=0 && dc>=0) {
            int hops = noc_.shortest_path_hops(sc, dc);
            uint64_t ser = (volume+Constants::PHIT_BYTES-1)/Constants::PHIT_BYTES;
            return static_cast<uint64_t>(hops)*(Constants::INTER_CHIPLET_HOP+ser);
        }
        uint64_t ser = (volume+Constants::PHIT_BYTES-1)/Constants::PHIT_BYTES;
        return Constants::INTER_CHIPLET_HOP + ser;
    }

    void retry_deferred(TaskGraph& tg, std::vector<int>& deferred, PQ& pq) {
        std::vector<int> still;
        for (int tid : deferred) {
            Task& task = tg.get_task(tid);
            if (task.state==TaskState::COMPLETED||task.state==TaskState::RUNNING) continue;

            bool all_done = true;
            for (int pid : task.predecessors)
                if (tg.get_task(pid).state!=TaskState::COMPLETED){all_done=false;break;}
            if (!all_done) { still.push_back(tid); continue; }

            uint64_t data_ready = 0;
            for (int pid : task.predecessors) {
                const Task& pred = tg.get_task(pid);
                uint64_t vol = tg.comm_volume(pid, tid);
                uint64_t lat = task.comm_latency_from.count(pid)
                               ? task.comm_latency_from.at(pid)
                               : compute_comm_latency(pred, task, vol,
                                     pred.finish_cycle, tg);
                data_ready = std::max(data_ready, pred.finish_cycle + lat);
            }
            task.data_ready_cycle = data_ready;

            bool placed = schedule_ready_task(task, tg, data_ready);
            if (placed) pq.push({task.finish_cycle, task.id});
            else        still.push_back(tid);
        }
        deferred = still;
    }

    void count_initial_faults() {
        for (auto& ch : hex_mesh_.chiplets())
            for (auto& co : ch.cores)
                if (co.is_faulty()) ++stats_.faults_detected;
    }

    void compute_statistics(const TaskGraph& tg) {
        double total_util=0.0; std::vector<double> utils;
        for (auto& ch : hex_mesh_.chiplets()) {
            if (current_cycle_==0){utils.push_back(0.0);continue;}
            double u=std::min(static_cast<double>(ch.total_exec_cycles)/
                              (current_cycle_*ch.core_count()),1.0);
            utils.push_back(u); total_util+=u;
        }
        stats_.avg_chiplet_utilization = total_util/hex_mesh_.chiplet_count();
        double mean=stats_.avg_chiplet_utilization, var=0.0;
        for (double u:utils) var+=(u-mean)*(u-mean);
        stats_.load_imbalance_index=std::sqrt(var/utils.size());

        double cu=0.0; int cc=0;
        for (auto& ch:hex_mesh_.chiplets())
            for (auto& co:ch.cores){
                if(co.is_faulty()) continue;
                ++cc;
                if(current_cycle_>0) cu+=co.utilization(current_cycle_);
            }
        stats_.avg_core_utilization = cc>0?cu/cc:0.0;

        stats_.replicas_created=0;
        for (auto& e:event_log_)
            if(e.kind==MappingEvent::Kind::REPLICA_PLACED) ++stats_.replicas_created;

        if (stats_.total_comm_edges>0)
            stats_.avg_comm_latency=
                static_cast<double>(stats_.total_comm_latency_cycles)/
                stats_.total_comm_edges;

        int placed=0;
        for (auto& [id,t]:tg.tasks()) if(t.state==TaskState::COMPLETED)++placed;
        if (placed>0)
            stats_.avg_task_wait_cycles=
                static_cast<double>(stats_.total_wait_cycles)/placed;

        stats_.critical_path_length=current_cycle_;
    }

    void print_report(const TaskGraph& tg) const {
        printf("\n╔══════════════════════════════════════════════════════════════╗\n");
        printf(  "║  Cycle-Accurate HexaMesh Simulation Report                   ║\n");
        printf(  "╚══════════════════════════════════════════════════════════════╝\n");
        printf("  Tasks              : %d\n",   tg.task_count());
        printf("  Makespan (cycles)  : %llu\n", (unsigned long long)current_cycle_);
        printf("  Clusters formed    : %d\n",   stats_.clusters_formed);
        printf("  Tasks placed       : %d\n",   stats_.tasks_placed);
        printf("  Tasks deferred     : %d\n",   stats_.tasks_deferred);
        printf("  Replicas created   : %d\n",   stats_.replicas_created);
        printf("  Faults detected    : %d\n",   stats_.faults_detected);
        printf("  Avg chiplet util   : %.2f%%\n",stats_.avg_chiplet_utilization*100.0);
        printf("  Avg core util      : %.2f%%\n",stats_.avg_core_utilization*100.0);
        printf("  Load imbalance (σ) : %.4f\n", stats_.load_imbalance_index);
        printf("  Total comm edges   : %d\n",   stats_.total_comm_edges);
        printf("  Avg comm latency   : %.2f cycles\n",stats_.avg_comm_latency);
        printf("  Max comm latency   : %llu cycles\n",(unsigned long long)stats_.max_comm_latency);
        printf("  Total wait cycles  : %llu\n", (unsigned long long)stats_.total_wait_cycles);
        printf("  Avg task wait      : %.2f cycles\n",stats_.avg_task_wait_cycles);
        printf("\n");
    }
};
