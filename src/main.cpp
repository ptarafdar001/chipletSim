
/**
 * @file main.cpp
 * @brief HexaMesh Chiplet Task Mapping Simulator with E3S benchmarks
 */

#include "../benchmarks/benchmark_e3s.hpp"
#include "../include/simulator.hpp"
#include "../include/output_writer.hpp"

#include <iostream>
#include <string>
#include <vector>

void print_usage() {
    std::cout << "Usage: ./simulator [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --benchmark <name>   Benchmark to run (default: E3S-Networking)\n";
    std::cout << "  --rings <n>          HexaMesh rings (default: 1)\n";
    std::cout << "  --mesh-dim <n>       Cores per chiplet mesh dimension (default: 2)\n";
    std::cout << "  --verbose            Print detailed report\n";
    std::cout << "  --list-benchmarks    List all available benchmarks\n\n";
    
    std::cout << "Available Benchmarks:\n\n";
    
    std::cout << "E3S (Industry-Standard):\n";
    std::cout << "  E3S-AutoIndust       - Automotive/industrial control\n";
    std::cout << "  E3S-Consumer         - Consumer electronics (JPEG, MP3)\n";
    std::cout << "  E3S-Networking       - Network processing (routing, firewall)\n";
    std::cout << "  E3S-Telecom          - Telecommunications (GSM, voice)\n";
    std::cout << "  E3S-Office           - Office automation (document processing)\n\n";
}

int main(int argc, char** argv) {
    
    // Parse command-line arguments
    std::string benchmark_name = "E3S-Networking";
    int rings = 1;
    int mesh_dim = 2;
    bool verbose = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        else if (arg == "--list-benchmarks") {
            print_usage();
            return 0;
        }
        else if (arg == "--benchmark" && i + 1 < argc) {
            benchmark_name = argv[++i];
        }
        else if (arg == "--rings" && i + 1 < argc) {
            rings = std::stoi(argv[++i]);
        }
        else if (arg == "--mesh-dim" && i + 1 < argc) {
            mesh_dim = std::stoi(argv[++i]);
        }
        else if (arg == "--verbose") {
            verbose = true;
        }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage();
            return 1;
        }
    }
    
    // Select benchmark
    BenchmarkInfo bench_info = [&]() -> BenchmarkInfo {
        
        // E3S Benchmarks
        if (benchmark_name == "E3S-AutoIndust")    return E3S::make_auto_indust_e3s();
        if (benchmark_name == "E3S-Consumer")      return E3S::make_consumer_e3s();
        if (benchmark_name == "E3S-Networking")    return E3S::make_networking_e3s();
        if (benchmark_name == "E3S-Telecom")       return E3S::make_telecom_e3s();
        if (benchmark_name == "E3S-Office")        return E3S::make_office_e3s();
        
        std::cerr << "Unknown benchmark: " << benchmark_name << "\n";
        std::cerr << "Run with --list-benchmarks to see available options\n";
        std::exit(1);
    }();
    
    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout <<   "║  HexaMesh Chiplet Task Mapping Simulator                     ║\n";
    std::cout <<   "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << "Benchmark: " << bench_info.name << "\n";
    std::cout << "  " << bench_info.description << "\n\n";
    
    // Configure simulator
    Simulator::Config config;
    config.rings = rings;
    config.mesh_dim = mesh_dim;
    config.verbose = verbose;
    
    std::cout << "Configuration:\n";
    std::cout << "  Rings: " << config.rings << " (";
    int chiplet_count = 1 + 3 * config.rings * (config.rings + 1);
    std::cout << chiplet_count << " chiplets)\n";
    std::cout << "  Cores per chiplet: " << (config.mesh_dim * config.mesh_dim) << "\n";
    std::cout << "  Total cores: " << (chiplet_count * config.mesh_dim * config.mesh_dim) << "\n";
    std::cout << "  Clustering alpha: " << config.alpha << "\n";
    std::cout << "  Clustering beta: " << config.beta << "\n\n";
    
    // Run simulation
    Simulator sim(config);
    
    std::cout << "Running simulation...\n\n";
    uint64_t makespan = sim.run(*bench_info.graph);
    
    // ═══════════════════════════════════════════════════════════════
    // Write output files using OutputWriter
    // ═══════════════════════════════════════════════════════════════
    std::string output_dir = "output/" + bench_info.name;
    
    std::cout << "Writing output files to: " << output_dir << "\n";
    
    OutputWriter::write_all(
        output_dir,
        *bench_info.graph,
        sim.mesh(),
        sim.clusters(),
        sim.event_log(),
        sim.stats(),
        makespan
    );
    
    std::cout << "✓ Output files created:\n";
    std::cout << "  - summary.json (topology, results, per-chiplet stats)\n";
    std::cout << "  - task_placement.csv (task details, placement, timing)\n";
    std::cout << "  - comm_latency.csv (per-edge communication latency)\n";
    std::cout << "  - core_utilization.csv (per-core statistics)\n";
    std::cout << "  - event_log.csv (simulation events timeline)\n";
    std::cout << "  - cluster_report.csv (cluster formation details)\n\n";
    
    // ═══════════════════════════════════════════════════════════════
    // Print results summary
    // ═══════════════════════════════════════════════════════════════
    const auto& stats = sim.stats();
    
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  SIMULATION RESULTS                                          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    std::cout << "Execution:\n";
    std::cout << "  Makespan: " << makespan << " cycles\n";
    std::cout << "  Tasks placed: " << stats.tasks_placed << " / " 
              << bench_info.graph->task_count() << "\n";
    std::cout << "  Tasks deferred: " << stats.tasks_deferred << "\n";
    std::cout << "  Clusters formed: " << stats.clusters_formed << "\n\n";
    
    std::cout << "Resource Utilization:\n";
    std::cout << "  Average chiplet utilization: " 
              << (stats.avg_chiplet_utilization * 100.0) << "%\n";
    std::cout << "  Average core utilization: " 
              << (stats.avg_core_utilization * 100.0) << "%\n";
    std::cout << "  Load imbalance (σ): " << stats.load_imbalance_index << "\n\n";
    
    std::cout << "Communication:\n";
    std::cout << "  Total communication edges: " << stats.total_comm_edges << "\n";
    std::cout << "  Average communication latency: " << stats.avg_comm_latency << " cycles\n";
    std::cout << "  Min communication latency: " << stats.min_comm_latency << " cycles\n";
    std::cout << "  Max communication latency: " << stats.max_comm_latency << " cycles\n\n";
    
    std::cout << "Fault Tolerance:\n";
    std::cout << "  Faults detected: " << stats.faults_detected << "\n";
    std::cout << "  Replicas created: " << stats.replicas_created << "\n\n";
    
    std::cout << "Performance:\n";
    std::cout << "  Total wait cycles: " << stats.total_wait_cycles << "\n";
    std::cout << "  Average task wait: " << stats.avg_task_wait_cycles << " cycles\n\n";
    
    std::cout << "✓ Simulation complete!\n";
    std::cout << "  Results saved to: ./" << output_dir << "/\n\n";
    
    return 0;
}