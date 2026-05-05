/**
 * @file test_e3s.cpp
 * @brief Test E3S TGFF file parsing
 * 
 * Compile: g++ -std=c++17 test_e3s.cpp -o test_e3s
 * Run: ./test_e3s path/to/file.tgff
 */

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <tgff_file>" << std::endl;
        std::cout << "\nExample:" << std::endl;
        std::cout << "  " << argv[0] << " benchmarks/e3s/auto-indust-cords.tgff" << std::endl;
        return 1;
    }
    
    std::string filepath = argv[1];
    
    std::cout << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  E3S TGFF File Tester                                        ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n" << std::endl;
    
    std::cout << "Testing file: " << filepath << std::endl << std::endl;
    
    // Try to open file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "❌ ERROR: Cannot open file!" << std::endl;
        std::cerr << "\nPossible issues:" << std::endl;
        std::cerr << "  1. File doesn't exist at this path" << std::endl;
        std::cerr << "  2. No read permissions" << std::endl;
        std::cerr << "  3. Incorrect path" << std::endl;
        return 1;
    }
    
    std::cout << "✓ File opened successfully" << std::endl << std::endl;
    
    // Count key elements
    int task_graphs = 0;
    int tasks = 0;
    int arcs = 0;
    int task_table_rows = 0;
    int commun_table_rows = 0;
    bool has_task_table = false;
    bool has_commun_table = false;
    
    std::string line;
    int line_num = 0;
    
    std::cout << "Analyzing file contents...\n" << std::endl;
    
    while (std::getline(file, line)) {
        line_num++;
        
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        
        // Skip empty lines
        if (line.empty()) continue;
        
        // Check for key elements
        if (line.find("@TASK_GRAPH") == 0) {
            task_graphs++;
            std::cout << "Line " << line_num << ": " << line << std::endl;
        }
        else if (line.find("TASK ") == 0 && line.find(" TYPE ") != std::string::npos) {
            tasks++;
            if (tasks <= 5) {  // Show first 5 tasks
                std::cout << "Line " << line_num << ": " << line << std::endl;
            }
        }
        else if (line.find("ARC ") == 0) {
            arcs++;
            if (arcs <= 5) {  // Show first 5 arcs
                std::cout << "Line " << line_num << ": " << line << std::endl;
            }
        }
        else if (line.find("@TASK") == 0) {
            has_task_table = true;
            std::cout << "Line " << line_num << ": " << line << std::endl;
        }
        else if (line.find("@COMMUN") == 0) {
            has_commun_table = true;
            std::cout << "Line " << line_num << ": " << line << std::endl;
        }
        else if (has_task_table && line[0] != '@' && line[0] != '}' && line[0] != '#') {
            std::istringstream iss(line);
            int type;
            if (iss >> type) {
                task_table_rows++;
                if (task_table_rows <= 3) {
                    std::cout << "Line " << line_num << ": @TASK row: " << line << std::endl;
                }
            }
        }
        else if (has_commun_table && line[0] != '@' && line[0] != '}' && line[0] != '#') {
            std::istringstream iss(line);
            int type;
            if (iss >> type) {
                commun_table_rows++;
                if (commun_table_rows <= 3) {
                    std::cout << "Line " << line_num << ": @COMMUN row: " << line << std::endl;
                }
            }
        }
        
        if (line[0] == '}') {
            has_task_table = false;
            has_commun_table = false;
        }
    }
    
    std::cout << "\n" << std::string(66, '=') << std::endl;
    std::cout << "SUMMARY" << std::endl;
    std::cout << std::string(66, '=') << std::endl;
    
    std::cout << "Task Graphs Found:     " << task_graphs << std::endl;
    std::cout << "Tasks Found:           " << tasks << std::endl;
    std::cout << "Arcs Found:            " << arcs << std::endl;
    std::cout << "@TASK table rows:      " << task_table_rows << std::endl;
    std::cout << "@COMMUN table rows:    " << commun_table_rows << std::endl;
    
    std::cout << "\n" << std::string(66, '=') << std::endl;
    std::cout << "VALIDATION" << std::endl;
    std::cout << std::string(66, '=') << std::endl;
    
    bool valid = true;
    
    if (task_graphs == 0) {
        std::cout << "❌ No @TASK_GRAPH blocks found!" << std::endl;
        valid = false;
    } else {
        std::cout << "✓ Found " << task_graphs << " task graph(s)" << std::endl;
    }
    
    if (tasks == 0) {
        std::cout << "❌ No TASK entries found!" << std::endl;
        valid = false;
    } else {
        std::cout << "✓ Found " << tasks << " task(s)" << std::endl;
    }
    
    if (arcs == 0) {
        std::cout << "⚠️  No ARC entries found (graph might have isolated tasks)" << std::endl;
    } else {
        std::cout << "✓ Found " << arcs << " arc(s)" << std::endl;
    }
    
    if (task_table_rows == 0) {
        std::cout << "⚠️  No @TASK table found (will use default execution times)" << std::endl;
    } else {
        std::cout << "✓ Found @TASK table with " << task_table_rows << " type(s)" << std::endl;
    }
    
    if (commun_table_rows == 0) {
        std::cout << "⚠️  No @COMMUN table found (will use default communication volumes)" << std::endl;
    } else {
        std::cout << "✓ Found @COMMUN table with " << commun_table_rows << " type(s)" << std::endl;
    }
    
    std::cout << "\n" << std::string(66, '=') << std::endl;
    
    if (valid) {
        std::cout << "✓ FILE IS VALID - Ready to use with simulator!" << std::endl;
        std::cout << "\nExpected output when running simulator:" << std::endl;
        std::cout << "  Tasks: " << tasks << std::endl;
        std::cout << "  Edges: " << arcs << std::endl;
    } else {
        std::cout << "❌ FILE HAS ISSUES - See errors above" << std::endl;
    }
    
    std::cout << std::string(66, '=') << std::endl;
    
    return valid ? 0 : 1;
}