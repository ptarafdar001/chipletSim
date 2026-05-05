/**
 * @file cluster.hpp
 * @brief Task Cluster definition (output of Algorithm 1).
 *
 * A Cluster groups strongly-communicating tasks that should ideally
 * be co-located on the same or nearby chiplets.
 */

#pragma once

#include "types.hpp"
#include "task.hpp"

#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <sstream>

// ============================================================
//  Cluster
// ============================================================
struct Cluster {
    int         id           {-1};
    ClusterType type         {ClusterType::COMM_LESS};

    std::vector<int> task_ids;   // tasks in this cluster (topological order)

    // ---- Aggregate statistics (computed after formation) ----
    uint64_t    total_exec_cost     {0};
    uint64_t    total_comm_volume   {0};
    double      avg_affinity_score  {0.0};
    Criticality max_criticality     {Criticality::LOW};

    // ---- Placement result (set by Algorithm 2) ----
    int primary_chiplet          {-1};    // main chiplet
    std::vector<int> spill_chiplets;      // additional chiplets used

    int task_count() const { return static_cast<int>(task_ids.size()); }

    bool is_fully_placed() const { return primary_chiplet >= 0; }

    bool contains(int task_id) const {
        return std::find(task_ids.begin(), task_ids.end(), task_id)
               != task_ids.end();
    }

    void update_max_criticality(Criticality c) {
        if (static_cast<int>(c) > static_cast<int>(max_criticality))
            max_criticality = c;
    }

    std::string summary() const {
        std::ostringstream oss;
        oss << "Cluster[" << id << "] type=" << to_string(type)
            << " tasks=" << task_count()
            << " exec_cost=" << total_exec_cost
            << " comm_vol=" << total_comm_volume
            << " max_crit=" << to_string(max_criticality)
            << " primary_chiplet=" << primary_chiplet;
        if (!spill_chiplets.empty()) {
            oss << " spills=[";
            for (int i = 0; i < static_cast<int>(spill_chiplets.size()); ++i)
                oss << spill_chiplets[i] << (i+1<static_cast<int>(spill_chiplets.size()) ? "," : "]");
        }
        return oss.str();
    }
};
