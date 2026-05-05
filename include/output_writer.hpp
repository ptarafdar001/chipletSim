/**
 * @file output_writer.hpp
 * @brief Writes cycle-accurate simulation results to JSON and CSV.
 *
 * New outputs vs. original:
 *   - summary.json          : now includes comm_latency and wait statistics
 *   - task_placement.csv    : now includes actual_duration, data_ready_cycle,
 *                             wait_cycles, comm latencies from each predecessor
 *   - comm_latency.csv      : per-edge communication latency breakdown
 *   - core_utilization.csv  : unchanged but uses actual_duration
 *   - event_log.csv         : now includes COMM_LATENCY events
 *   - cluster_report.csv    : unchanged
 */

#pragma once

#include "types.hpp"
#include "task.hpp"
#include "cluster.hpp"
#include "chiplet.hpp"
#include "algorithms.hpp"
#include "simulator.hpp"

#include <fstream>
#include <string>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>

class OutputWriter {
public:
    static void write_all(const std::string&               output_dir,
                          const TaskGraph&                 tg,
                          const HexaMesh&                  hm,
                          const std::vector<Cluster>&      clusters,
                          const std::vector<MappingEvent>& event_log,
                          const MappingStats&              stats,
                          uint64_t                         total_cycles)
    {
        mkdir_p(output_dir);
        write_summary_json  (output_dir, tg, hm, stats, total_cycles);
        write_task_csv       (output_dir, tg);
        write_comm_latency_csv(output_dir, tg);
        write_core_util_csv  (output_dir, hm, total_cycles);
        write_event_log_csv  (output_dir, event_log);
        write_cluster_csv    (output_dir, clusters);
    }

private:
    static void mkdir_p(const std::string& path) {
        // Create directory and parents
        std::string cmd = "mkdir -p \"" + path + "\"";
        (void)system(cmd.c_str());
    }

    static std::string ts() {
        std::time_t t = std::time(nullptr);
        std::ostringstream o;
        o << std::put_time(std::localtime(&t), "%Y-%m-%dT%H:%M:%S");
        return o.str();
    }

    // ── summary.json ─────────────────────────────────────────────────
    static void write_summary_json(const std::string& dir, const TaskGraph& tg,
                                    const HexaMesh& hm, const MappingStats& s,
                                    uint64_t cycles)
    {
        std::ofstream f(dir + "/summary.json");
        f << "{\n";
        f << "  \"timestamp\": \"" << ts() << "\",\n";
        f << "  \"topology\": {\n";
        f << "    \"chiplet_count\": "     << hm.chiplet_count()  << ",\n";
        f << "    \"rings\": "             << hm.rings()          << ",\n";
        f << "    \"cores_per_chiplet\": " << Constants::CORES_PER_CHIPLET << ",\n";
        f << "    \"total_cores\": "
          << hm.chiplet_count() * Constants::CORES_PER_CHIPLET   << ",\n";
        f << "    \"theoretical_diameter\": " << hm.theoretical_diameter() << ",\n";
        f << "    \"intra_chiplet_hop_cycles\": " << Constants::INTRA_CHIPLET_HOP << ",\n";
        f << "    \"inter_chiplet_hop_cycles\": " << Constants::INTER_CHIPLET_HOP << ",\n";
        f << "    \"phit_bytes\": "        << Constants::PHIT_BYTES << "\n";
        f << "  },\n";
        f << "  \"application\": {\n";
        f << "    \"task_count\": " << tg.task_count() << ",\n";
        f << "    \"edge_count\": " << tg.edge_count() << "\n";
        f << "  },\n";
        f << "  \"results\": {\n";
        f << "    \"makespan_cycles\": "        << cycles  << ",\n";
        f << "    \"clusters_formed\": "        << s.clusters_formed << ",\n";
        f << "    \"tasks_placed\": "           << s.tasks_placed    << ",\n";
        f << "    \"tasks_deferred\": "         << s.tasks_deferred  << ",\n";
        f << "    \"tasks_failed\": "           << s.tasks_failed    << ",\n";
        f << "    \"replicas_created\": "       << s.replicas_created<< ",\n";
        f << "    \"faults_detected\": "        << s.faults_detected << ",\n";
        f << "    \"faults_recovered\": "       << s.faults_recovered<< ",\n";
        f << "    \"avg_chiplet_utilization\": "<< s.avg_chiplet_utilization << ",\n";
        f << "    \"avg_core_utilization\": "   << s.avg_core_utilization    << ",\n";
        f << "    \"load_imbalance_stddev\": "  << s.load_imbalance_index    << ",\n";
        // Cycle-accurate fields
        f << "    \"total_comm_edges\": "         << s.total_comm_edges          << ",\n";
        f << "    \"total_comm_latency_cycles\": " << s.total_comm_latency_cycles << ",\n";
        f << "    \"avg_comm_latency_cycles\": "  << s.avg_comm_latency          << ",\n";
        f << "    \"max_comm_latency_cycles\": "  << s.max_comm_latency          << ",\n";
        f << "    \"min_comm_latency_cycles\": "  << s.min_comm_latency          << ",\n";
        f << "    \"total_wait_cycles\": "        << s.total_wait_cycles         << ",\n";
        f << "    \"avg_task_wait_cycles\": "     << s.avg_task_wait_cycles      << ",\n";
        f << "    \"critical_path_length\": "     << s.critical_path_length      << "\n";
        f << "  },\n";
        f << "  \"noc_params\": {\n";
        f << "    \"intra_hop_cycles\": "    << Constants::INTRA_CHIPLET_HOP << ",\n";
        f << "    \"inter_hop_cycles\": "    << Constants::INTER_CHIPLET_HOP << ",\n";
        f << "    \"phit_bytes\": "          << Constants::PHIT_BYTES        << ",\n";
        f << "    \"flit_bytes\": "          << Constants::FLIT_BYTES        << ",\n";
        f << "    \"mem_access_penalty\": "  << Constants::MEM_ACCESS_PENALTY<< ",\n";
        f << "    \"io_stall_penalty\": "    << Constants::IO_STALL_PENALTY  << "\n";
        f << "  },\n";
        f << "  \"chiplets\": [\n";
        bool first = true;
        for (auto& ch : hm.chiplets()) {
            if (!first) f << ",\n"; first = false;
            f << "    {\"id\":" << ch.id
              << ",\"ring\":" << ch.ring
              << ",\"degree\":" << ch.degree()
              << ",\"tasks_hosted\":" << ch.tasks_hosted
              << ",\"total_exec_cycles\":" << ch.total_exec_cycles
              << ",\"faulty_cores\":" << ch.faulty_core_count()
              << ",\"state\":\"";
            switch (ch.state) {
                case ChipletState::ACTIVE:   f << "ACTIVE";   break;
                case ChipletState::DEGRADED: f << "DEGRADED"; break;
                case ChipletState::OFFLINE:  f << "OFFLINE";  break;
            }
            f << "\"}";
        }
        f << "\n  ]\n}\n";
    }

    // ── task_placement.csv ────────────────────────────────────────────
    static void write_task_csv(const std::string& dir, const TaskGraph& tg) {
        std::ofstream f(dir + "/task_placement.csv");
        f << "task_id,name,type,criticality,"
             "exec_time,actual_duration,deadline,release_time,"
             "mem_io_penalty,"
             "data_ready_cycle,wait_cycles,slack_time,"
             "cluster_id,assigned_chiplet,assigned_core,replica_core,"
             "start_cycle,finish_cycle,state\n";

        std::vector<const Task*> ordered;
        for (auto& [id,t] : tg.tasks()) ordered.push_back(&t);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Task* a, const Task* b){ return a->id < b->id; });

        for (auto* tp : ordered) {
            auto& t = *tp;
            uint64_t penalty = t.actual_duration - t.exec_time;
            f << t.id << ","
              << t.name << ","
              << to_string(t.type) << ","
              << to_string(t.criticality) << ","
              << t.exec_time << ","
              << t.actual_duration << ","
              << (t.deadline == Constants::INF_TIME ? -1 : (int64_t)t.deadline) << ","
              << t.release_time << ","
              << penalty << ","
              << t.data_ready_cycle << ","
              << t.wait_cycles << ","
              << (t.slack_time == Constants::INF_TIME ? -1 : (int64_t)t.slack_time) << ","
              << t.cluster_id << ","
              << t.assigned_chiplet << ","
              << t.assigned_core << ","
              << t.replica_core << ","
              << t.start_cycle << ","
              << t.finish_cycle << ","
              << to_string(t.state) << "\n";
        }
    }

    // ── comm_latency.csv ──────────────────────────────────────────────
    // New file: per-edge communication latency breakdown
    static void write_comm_latency_csv(const std::string& dir, const TaskGraph& tg) {
        std::ofstream f(dir + "/comm_latency.csv");
        f << "src_task_id,src_name,src_chiplet,src_core,"
             "dst_task_id,dst_name,dst_chiplet,dst_core,"
             "volume_bytes,comm_latency_cycles,inject_cycle\n";

        std::vector<const Task*> ordered;
        for (auto& [id,t] : tg.tasks()) ordered.push_back(&t);
        std::sort(ordered.begin(), ordered.end(),
                  [](const Task* a, const Task* b){ return a->id < b->id; });

        for (auto* tp : ordered) {
            const Task& dst = *tp;
            for (auto& [pred_id, lat] : dst.comm_latency_from) {
                const Task& src = tg.get_task(pred_id);
                uint64_t vol    = tg.comm_volume(pred_id, dst.id);
                f << src.id << "," << src.name << ","
                  << src.assigned_chiplet << "," << src.assigned_core << ","
                  << dst.id << "," << dst.name << ","
                  << dst.assigned_chiplet << "," << dst.assigned_core << ","
                  << vol << "," << lat << ","
                  << src.finish_cycle << "\n";
            }
        }
    }

    // ── core_utilization.csv ──────────────────────────────────────────
    static void write_core_util_csv(const std::string& dir, const HexaMesh& hm,
                                     uint64_t total_cycles)
    {
        std::ofstream f(dir + "/core_utilization.csv");
        f << "global_core_id,chiplet_id,ring,local_core_id,row,col,"
             "state,tasks_executed,cycles_busy,utilization_pct\n";
        for (auto& ch : hm.chiplets()) {
            for (auto& co : ch.cores) {
                double util = total_cycles > 0
                              ? co.utilization(total_cycles) * 100.0 : 0.0;
                f << co.global_id << ","
                  << co.chiplet_id << ","
                  << ch.ring << ","
                  << co.local_id << ","
                  << co.row << ","
                  << co.col << ","
                  << to_string(co.state) << ","
                  << co.tasks_executed << ","
                  << co.cycles_busy << ","
                  << std::fixed << std::setprecision(2) << util << "\n";
            }
        }
    }

    // ── event_log.csv ─────────────────────────────────────────────────
    static void write_event_log_csv(const std::string& dir,
                                     const std::vector<MappingEvent>& log)
    {
        std::ofstream f(dir + "/event_log.csv");
        f << "cycle,event_kind,task_id,cluster_id,chiplet_id,core_id,note\n";
        for (auto& e : log) {
            std::string kind;
            switch (e.kind) {
                case MappingEvent::Kind::TASK_PLACED:     kind="TASK_PLACED";     break;
                case MappingEvent::Kind::TASK_DEFERRED:   kind="TASK_DEFERRED";   break;
                case MappingEvent::Kind::TASK_FAILED:     kind="TASK_FAILED";     break;
                case MappingEvent::Kind::REPLICA_PLACED:  kind="REPLICA_PLACED";  break;
                case MappingEvent::Kind::FAULT_DETECTED:  kind="FAULT_DETECTED";  break;
                case MappingEvent::Kind::FAULT_RECOVERED: kind="FAULT_RECOVERED"; break;
                case MappingEvent::Kind::CLUSTER_FORMED:  kind="CLUSTER_FORMED";  break;
                case MappingEvent::Kind::CLUSTER_PLACED:  kind="CLUSTER_PLACED";  break;
                case MappingEvent::Kind::COMM_LATENCY:    kind="COMM_LATENCY";    break;
            }
            f << e.cycle << "," << kind << ","
              << e.task_id << "," << e.cluster_id << ","
              << e.chiplet_id << "," << e.core_id << ","
              << "\"" << e.note << "\"\n";
        }
    }

    // ── cluster_report.csv ────────────────────────────────────────────
    static void write_cluster_csv(const std::string& dir,
                                   const std::vector<Cluster>& clusters)
    {
        std::ofstream f(dir + "/cluster_report.csv");
        f << "cluster_id,type,task_count,total_exec_cost,total_comm_volume,"
             "avg_affinity,max_criticality,primary_chiplet,spill_count\n";
        for (auto& cl : clusters)
            f << cl.id << ","
              << to_string(cl.type) << ","
              << cl.task_count() << ","
              << cl.total_exec_cost << ","
              << cl.total_comm_volume << ","
              << std::fixed << std::setprecision(4) << cl.avg_affinity_score << ","
              << to_string(cl.max_criticality) << ","
              << cl.primary_chiplet << ","
              << cl.spill_chiplets.size() << "\n";
    }
};
