/**
 * @file benchmark_e3s.hpp
 * @brief E3S benchmark loader - handles actual E3S TGFF format
 */

#pragma once

#include "../include/types.hpp"
#include "../include/task.hpp"

#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <cmath>

namespace E3S {

// ══════════════════════════════════════════════════════════════════════════
// E3S TGFF Parser - Handles actual E3S format with named tasks
// ══════════════════════════════════════════════════════════════════════════

class E3STGFFParser {
public:
    struct TaskData {
        int graph_id;
        std::string name;
        int type;
    };
    
    struct ArcData {
        int graph_id;
        std::string from_name;
        std::string to_name;
        int comm_type;
    };
    
    static bool parse_file(const std::string& filepath,
                          std::vector<TaskData>& tasks,
                          std::vector<ArcData>& arcs,
                          std::unordered_map<int, uint64_t>& comm_volumes)
    {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "ERROR: Cannot open E3S file: " << filepath << std::endl;
            return false;
        }
        
        std::cout << "Parsing E3S file: " << filepath << std::endl;
        
        std::string line;
        int current_graph = -1;
        bool in_commun_table = false;
        
        while (std::getline(file, line)) {
            // Trim whitespace
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            
            // Skip empty lines and comments
            if (line.empty() || line[0] == '#') continue;
            
            // Convert to uppercase for case-insensitive matching
            std::string line_upper = line;
            std::transform(line_upper.begin(), line_upper.end(), line_upper.begin(), ::toupper);
            
            // Check for @TASK_GRAPH
            if (line_upper.find("@TASK_GRAPH") == 0) {
                std::istringstream iss(line);
                std::string tag;
                iss >> tag >> current_graph;
                std::cout << "  Found TASK_GRAPH " << current_graph << std::endl;
                continue;
            }
            
            // Check for @COMMUN_QUANT (E3S uses this instead of @COMMUN)
            if (line_upper.find("@COMMUN") == 0) {
                in_commun_table = true;
                std::cout << "  Entering @COMMUN table" << std::endl;
                continue;
            }
            
            // End of sections
            if (line[0] == '}') {
                in_commun_table = false;
                if (current_graph >= 0) {
                    std::cout << "  Completed TASK_GRAPH " << current_graph << std::endl;
                }
                current_graph = -1;
                continue;
            }
            
            // Parse TASK entries (format: TASK <name> TYPE <type>)
            if (current_graph >= 0 && line_upper.find("TASK ") == 0) {
                std::istringstream iss(line);
                std::string keyword, task_name, type_keyword;
                int task_type;
                
                iss >> keyword >> task_name >> type_keyword >> task_type;
                
                if (type_keyword == "TYPE" || type_keyword == "type") {
                    TaskData td;
                    td.graph_id = current_graph;
                    td.name = task_name;
                    td.type = task_type;
                    tasks.push_back(td);
                    
                    std::cout << "    Task: " << task_name << " (type " << task_type << ")" << std::endl;
                }
            }
            
            // Parse ARC entries (format: ARC <name> FROM <src> TO/to <dst> TYPE <type>)
            if (current_graph >= 0 && line_upper.find("ARC ") == 0) {
                std::istringstream iss(line);
                std::string keyword, arc_name, from_kw, from_task, to_kw, to_task, type_kw;
                int comm_type;
                
                iss >> keyword >> arc_name >> from_kw >> from_task >> to_kw >> to_task >> type_kw >> comm_type;
                
                // E3S sometimes uses lowercase "to" instead of "TO"
                std::transform(from_kw.begin(), from_kw.end(), from_kw.begin(), ::toupper);
                std::transform(to_kw.begin(), to_kw.end(), to_kw.begin(), ::toupper);
                std::transform(type_kw.begin(), type_kw.end(), type_kw.begin(), ::toupper);
                
                if (from_kw == "FROM" && to_kw == "TO" && type_kw == "TYPE") {
                    ArcData ad;
                    ad.graph_id = current_graph;
                    ad.from_name = from_task;
                    ad.to_name = to_task;
                    ad.comm_type = comm_type;
                    arcs.push_back(ad);
                    
                    std::cout << "    Arc: " << from_task << " -> " << to_task 
                              << " (type " << comm_type << ")" << std::endl;
                }
            }
            
            // Parse @COMMUN table rows
            if (in_commun_table && line[0] != '@' && line[0] != '}') {
                std::istringstream iss(line);
                int type;
                std::string volume_str;
                
                if (iss >> type >> volume_str) {
                    // E3S uses scientific notation like "4E3" for 4000
                    uint64_t volume = parse_scientific_notation(volume_str);
                    comm_volumes[type] = volume;
                    
                    std::cout << "    Comm type " << type << " = " << volume << " bytes" << std::endl;
                }
            }
        }
        
        std::cout << "\nParsing complete:" << std::endl;
        std::cout << "  Total tasks: " << tasks.size() << std::endl;
        std::cout << "  Total arcs: " << arcs.size() << std::endl;
        std::cout << "  Comm types: " << comm_volumes.size() << std::endl;
        
        return !tasks.empty();
    }

private:
    static uint64_t parse_scientific_notation(const std::string& str) {
        // Handle E3S scientific notation: "4E3" = 4000, "8E3" = 8000
        size_t e_pos = str.find_first_of("Ee");
        if (e_pos != std::string::npos) {
            double base = std::stod(str.substr(0, e_pos));
            int exp = std::stoi(str.substr(e_pos + 1));
            return static_cast<uint64_t>(base * std::pow(10, exp));
        }
        return std::stoull(str);
    }
};

// ══════════════════════════════════════════════════════════════════════════
// E3S Mapper - Convert E3S data to simulator Task attributes
// ══════════════════════════════════════════════════════════════════════════

class E3SMapper {
public:
    static TaskType classify_task_type(int task_type_id) {
        // E3S task types vary by benchmark
        // Use type ID to determine computational intensity
        if (task_type_id < 15) return TaskType::COMPUTE_LIGHT;
        if (task_type_id < 30) return TaskType::COMPUTE_HEAVY;
        if (task_type_id < 45) return TaskType::MEMORY_BOUND;
        return TaskType::IO_BOUND;
    }
    
    static Criticality classify_criticality(int num_successors, bool is_source, bool is_sink) {
        if (is_source || is_sink) return Criticality::HIGH;
        if (num_successors > 3) return Criticality::HIGH;
        if (num_successors > 0) return Criticality::MEDIUM;
        return Criticality::LOW;
    }
    
    static uint64_t default_exec_time(int task_type_id) {
        // E3S doesn't provide @TASK table, so use type-based defaults
        // Realistic embedded task execution times
        if (task_type_id < 15) return 1000 + task_type_id * 100;   // 1000-2400 cycles
        if (task_type_id < 30) return 2500 + task_type_id * 200;   // 2700-8500 cycles
        if (task_type_id < 45) return 5000 + task_type_id * 300;   // 5300-18500 cycles
        return 10000 + task_type_id * 500;                          // 10000+ cycles
    }
    
    static uint64_t scale_comm_volume(uint64_t e3s_volume) {
        // E3S volumes are already in bytes (4E3 = 4000 bytes)
        // Just ensure minimum
        return std::max(static_cast<uint64_t>(32), e3s_volume);
    }
};

// ══════════════════════════════════════════════════════════════════════════
// Build TaskGraph from E3S data
// ══════════════════════════════════════════════════════════════════════════

inline BenchmarkInfo build_from_tgff(const std::string& filepath,
                                     const std::string& name,
                                     const std::string& description)
{
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  Loading E3S Benchmark: " << name << std::string(39 - name.length(), ' ') << "║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;
    
    // Parse TGFF file
    std::vector<E3STGFFParser::TaskData> tasks;
    std::vector<E3STGFFParser::ArcData> arcs;
    std::unordered_map<int, uint64_t> comm_volumes;
    
    bool success = E3STGFFParser::parse_file(filepath, tasks, arcs, comm_volumes);
    
    if (!success || tasks.empty()) {
        std::cerr << "\nERROR: Failed to load E3S benchmark!" << std::endl;
        auto tg = std::make_unique<TaskGraph>();
        return BenchmarkInfo(name, description + " (FAILED)", std::move(tg));
    }
    
    // Create TaskGraph
    auto tg = std::make_unique<TaskGraph>();
    
    // Map task name -> global ID
    std::unordered_map<std::string, int> name_to_global;
    std::unordered_map<int, std::string> global_to_name;
    
    int global_id = 0;
    
    // Count successors for criticality
    std::unordered_map<std::string, int> successor_counts;
    std::unordered_map<std::string, bool> has_predecessor;
    
    for (const auto& arc : arcs) {
        successor_counts[arc.from_name]++;
        has_predecessor[arc.to_name] = true;
    }
    
    std::cout << "\nCreating tasks..." << std::endl;
    
    // First pass: Create all tasks
    for (const auto& task : tasks) {
        // Get execution time
        uint64_t exec_time = E3SMapper::default_exec_time(task.type);
        
        // Classify task type
        TaskType type = E3SMapper::classify_task_type(task.type);
        
        // Count successors
        int num_succ = successor_counts[task.name];
        bool is_source = !has_predecessor[task.name];
        bool is_sink = (num_succ == 0);
        
        // Determine criticality
        Criticality crit = E3SMapper::classify_criticality(num_succ, is_source, is_sink);
        
        // Create readable task name
        std::string task_name = "g" + std::to_string(task.graph_id) + 
                               "_" + task.name +
                               "_t" + std::to_string(task.type);
        
        // Add task
        tg->add_task(global_id, task_name, type, crit, exec_time, Constants::INF_TIME);
        
        std::cout << "  [" << global_id << "] " << task.name 
                  << " (type:" << task.type 
                  << ", " << to_string(type) 
                  << ", " << to_string(crit) 
                  << ", " << exec_time << " cycles)" << std::endl;
        
        // name_to_global[task.name] = global_id;
        // global_to_name[global_id] = task.name;

        // Use graph-qualified unique key to avoid name collisions
        std::string unique_key = "g" + std::to_string(task.graph_id) + "_" + task.name;
        name_to_global[unique_key] = global_id;
        global_to_name[global_id] = task.name;
        global_id++;
    }
    
    std::cout << "\nCreating edges..." << std::endl;
    
    // Second pass: Add edges
    for (const auto& arc : arcs) {
        // Use graph-qualified names to look up correct global IDs
        std::string from_unique = "g" + std::to_string(arc.graph_id) + "_" + arc.from_name;
        std::string to_unique = "g" + std::to_string(arc.graph_id) + "_" + arc.to_name;
        
        if (!name_to_global.count(from_unique) || !name_to_global.count(to_unique)) {
            std::cerr << "  WARNING: Arc references unknown task: " 
                      << arc.from_name << " -> " << arc.to_name << std::endl;
            continue;
        }
        
        int from_global = name_to_global[from_unique];
        int to_global = name_to_global[to_unique];
 
        
        // Get communication volume
        uint64_t volume = 64;  // default
        if (comm_volumes.count(arc.comm_type)) {
            volume = E3SMapper::scale_comm_volume(comm_volumes[arc.comm_type]);
        }
        
        tg->add_edge(from_global, to_global, volume);
        
        std::cout << "  " << arc.from_name << " -> " << arc.to_name 
                  << " (" << volume << " bytes)" << std::endl;
    }
    
    std::string full_desc = description + " (" + std::to_string(tg->task_count()) + " tasks from E3S)";
    
    std::cout << "\n✓ E3S benchmark loaded successfully!" << std::endl;
    std::cout << "  Total tasks: " << tg->task_count() << std::endl;
    std::cout << "  Total edges: " << tg->edge_count() << std::endl;
    std::cout << std::endl;
    
    return BenchmarkInfo(name, full_desc, std::move(tg));
}

// ══════════════════════════════════════════════════════════════════════════
// Convenience functions
// ══════════════════════════════════════════════════════════════════════════

inline BenchmarkInfo make_e3s_from_tgff(const std::string& path) {
    size_t last_slash = path.find_last_of("/\\");
    std::string name = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    
    return build_from_tgff(path, name, "E3S benchmark");
}

inline BenchmarkInfo make_auto_indust_e3s(const std::string& base = "benchmarks/e3s") {
    return build_from_tgff(
        base + "/auto-indust-cords.tgff",
        "E3S-AutoIndust",
        "Automotive/Industrial control (engine, ABS, transmission)"
    );
}

inline BenchmarkInfo make_consumer_e3s(const std::string& base = "benchmarks/e3s") {
    return build_from_tgff(
        base + "/consumer-cords.tgff",
        "E3S-Consumer",
        "Consumer electronics (JPEG, MP3, video)"
    );
}

inline BenchmarkInfo make_networking_e3s(const std::string& base = "benchmarks/e3s") {
    return build_from_tgff(
        base + "/networking-cords.tgff",
        "E3S-Networking",
        "Network processing (routing, firewall, NAT)"
    );
}

inline BenchmarkInfo make_telecom_e3s(const std::string& base = "benchmarks/e3s") {
    return build_from_tgff(
        base + "/telecom-cords.tgff",
        "E3S-Telecom",
        "Telecommunications (GSM, voice, signal processing)"
    );
}

inline BenchmarkInfo make_office_e3s(const std::string& base = "benchmarks/e3s") {
    return build_from_tgff(
        base + "/office-automation-cords.tgff",
        "E3S-Office",
        "Office automation (document processing)"
    );
}

} // namespace E3S








