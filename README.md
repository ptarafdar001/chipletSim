# ChipletSim: Task Mapping simulation for chiplet based NoC/NoI


## Quick Start

### 1. Clone the project from here or download the zip

```bash
    git clone https://github.com/ptarafdar001/Chiplet_task_mapper_sim.git

    cd chipletsim
```

### 2. Compile

```bash
    make
    -o hexamesh_mapper src/main.cpp
```

### 3. Run

```bash
./hexamesh_mapper
```

**Output:**
```
╔══════════════════════════════════════════════════════════════╗
║  Cycle-Accurate HexaMesh Task Mapping Simulator              ║
║  Full Suite: 21 Benchmarks                                   ║
╚══════════════════════════════════════════════════════════════╝

Simulation Configuration:
  Rings                    : 1
  Intra-Chiplet Mesh Dim   : 2 x 2
  Cluster Size             : 4
  Alpha / Beta             : 0.70 / 0.30
  Fault Injection          : Yes (p=0.0200, seed=42)

NoC Model:
  Intra-chiplet hop        : 2 cycles
  Inter-chiplet hop (UCIe) : 27 cycles
  Phit width               : 16 bytes
  Memory access penalty    : 10 cycles
  I/O stall penalty        : 5 cycles

Loading benchmarks...
Total: 4 benchmarks

#   Benchmark                      Tasks    Cycles   Util%     Imbal CommEdge  AvgComm  AvgWait Deferred   Faults
--------------------------------------------------------------------------------------------------------------
1   Stratified-1K                   1022     86989    30.5    0.1287     2868     58.0    144.3        0        0
2   Blackscholes-64                 4226    485959    90.3    0.1311     8320     30.7 216076.5        0        0
3   Canneal-64                     20546   4835164    83.0    0.0253    60922     47.5   3916.1        0        0
4   X264-640x360                    5086    123693    63.9    0.1147     8604     39.9   2788.5        0        0
--------------------------------------------------------------------------------------------------------------

✓ All simulations complete!
  Results saved to : ./output/<benchmark>/
  New files        : comm_latency.csv (per-edge latency breakdown)
  Visualize with   : python visualize.py --all
   
```