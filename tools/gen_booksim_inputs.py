#!/usr/bin/env python3
"""
gen_booksim_inputs.py
─────────────────────
Reads  : summary.json  +  comm_latency.csv  (from the HexaMesh mapper)
Writes : trace.txt     –  BookSim2 4-column trace file
         config.txt    –  BookSim2 anynet + trace-mode config
         <anynet_file> –  anynet topology file (path set in config.txt)

Usage
─────
    python3 gen_booksim_inputs.py \
        --summary  summary.json        \
        --csv      comm_latency.csv    \
        --out_dir  .                       # directory for all outputs
        --name     sim                     # prefix: sim_trace.txt, sim_config.txt …
        --anynet_dir examples/anynet       # where BookSim will look for topology

All flags have defaults; run with --help for details.
"""

import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict


# ─────────────────────────────────────────────────────────────────────────────
# Argument parsing
# ─────────────────────────────────────────────────────────────────────────────

def parse_args():
    p = argparse.ArgumentParser(
        description="Generate BookSim2 trace + config + anynet from mapper outputs."
    )
    p.add_argument("--summary",     default="summary.json",     help="Path to summary.json")
    p.add_argument("--csv",         default="comm_latency.csv", help="Path to comm_latency.csv")
    p.add_argument("--out_dir",     default=".",                help="Output directory")
    p.add_argument("--name",        default="sim",              help="Output file prefix")
    p.add_argument("--anynet_dir",  default="examples/anynet",
                   help="Directory (relative to booksim src/) where anynet file will live")
    p.add_argument("--no_anynet",   action="store_true",
                   help="Skip writing the anynet topology file")
    return p.parse_args()


# ─────────────────────────────────────────────────────────────────────────────
# Topology helpers
# ─────────────────────────────────────────────────────────────────────────────

def node_id(chiplet: int, core: int, cores_per_chiplet: int) -> int:
    """Flat node index used by BookSim: chiplet * N + core."""
    return chiplet * cores_per_chiplet + core


def size_flits(volume_bytes: int, phit_bytes: int) -> int:
    """Round up byte volume to whole flits; minimum 1."""
    return max(1, math.ceil(volume_bytes / phit_bytes))


# ─────────────────────────────────────────────────────────────────────────────
# Trace file generator
# ─────────────────────────────────────────────────────────────────────────────

def build_trace(rows, cores_per_chiplet: int, phit_bytes: int):
    """
    Returns a list of (inject_cycle, src_node, dst_node, size_flits) tuples,
    sorted by (inject_cycle, src_node).  Self-sends are silently dropped.
    """
    events = []
    dropped = 0

    for r in rows:
        src_chip = int(r["src_chiplet"])
        src_core = int(r["src_core"])
        dst_chip = int(r["dst_chiplet"])
        dst_core = int(r["dst_core"])
        volume   = int(r["volume_bytes"])
        cycle    = int(r["inject_cycle"])

        src = node_id(src_chip, src_core, cores_per_chiplet)
        dst = node_id(dst_chip, dst_core, cores_per_chiplet)

        if src == dst:
            dropped += 1
            continue

        flits = size_flits(volume, phit_bytes)
        events.append((cycle, src, dst, flits))

    events.sort(key=lambda e: (e[0], e[1]))
    return events, dropped


def write_trace(events, path: str):
    """Write the 4-column trace file.  No comments — BookSim parser uses >>."""
    with open(path, "w") as f:
        for cycle, src, dst, flits in events:
            f.write(f"{cycle:8d}  {src:4d}  {dst:4d}  {flits:4d}\n")


# ─────────────────────────────────────────────────────────────────────────────
# AnyNet topology generator
# ─────────────────────────────────────────────────────────────────────────────

def hexamesh_chiplet_connections(num_chiplets: int, rings: int):
    """
    Return inter-chiplet edges as a set of frozensets {a, b}.

    HexaMesh wiring (rings=1):
      Chiplet 0 is the centre.  Chiplets 1..6 sit on ring 1.
      Ring-1 chiplets connect to centre (0) AND to their two ring-1 neighbours.

    For rings > 1 only ring-1 is implemented here; extend as needed.
    """
    edges = set()
    if num_chiplets == 1:
        return edges

    # Centre ↔ each ring-1 chiplet
    ring1 = list(range(1, min(7, num_chiplets)))
    for c in ring1:
        edges.add(frozenset([0, c]))

    # Ring-1 neighbours (circular)
    n = len(ring1)
    for i in range(n):
        a = ring1[i]
        b = ring1[(i + 1) % n]
        edges.add(frozenset([a, b]))

    return edges


def mesh2d_intra_edges(chiplet: int, cores_per_chiplet: int):
    """
    Return intra-chiplet 2-D mesh edges as list of (node_a, node_b).
    mesh_dim = sqrt(cores_per_chiplet).  Assumes square mesh.
    """
    dim = int(math.isqrt(cores_per_chiplet))
    assert dim * dim == cores_per_chiplet, \
        f"cores_per_chiplet={cores_per_chiplet} is not a perfect square"

    base = chiplet * cores_per_chiplet
    edges = []
    for r in range(dim):
        for c in range(dim):
            n = base + r * dim + c
            if c + 1 < dim:
                edges.append((n, n + 1))          # horizontal
            if r + 1 < dim:
                edges.append((n, n + dim))        # vertical
    return edges


def build_adjacency(num_chiplets, cores_per_chiplet, rings,
                    intra_latency, inter_latency):
    """
    Build adjacency list: adj[node] = [(neighbor_node, latency), ...]
    """
    total_nodes = num_chiplets * cores_per_chiplet
    adj = defaultdict(list)

    # Intra-chiplet edges (2-D mesh)
    for chip in range(num_chiplets):
        for a, b in mesh2d_intra_edges(chip, cores_per_chiplet):
            adj[a].append((b, intra_latency))
            adj[b].append((a, intra_latency))

    # Inter-chiplet edges: connect the "gateway" core of each chiplet.
    # Gateway = core 0 of each chiplet (lowest-index core).
    inter_edges = hexamesh_chiplet_connections(num_chiplets, rings)
    for edge in inter_edges:
        a_chip, b_chip = tuple(edge)
        a_node = node_id(a_chip, 0, cores_per_chiplet)
        b_node = node_id(b_chip, 0, cores_per_chiplet)
        adj[a_node].append((b_node, inter_latency))
        adj[b_node].append((a_node, inter_latency))

    return adj, total_nodes


def write_anynet(adj, total_nodes, path: str):
    """
    Write BookSim2 anynet topology file.
    Format per router:
        router <id> node <id> router <nb1> <lat1> router <nb2> <lat2> ...
    """
    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else ".", exist_ok=True)
    with open(path, "w") as f:
        for r in range(total_nodes):
            neighbors = " ".join(f"router {nb} {lat}" for nb, lat in sorted(adj[r]))
            f.write(f"router {r} node {r} {neighbors}\n")


# ─────────────────────────────────────────────────────────────────────────────
# Config file generator
# ─────────────────────────────────────────────────────────────────────────────

def write_config(path: str, topo: dict, trace_path_in_config: str,
                 anynet_path_in_config: str, max_flits: int,
                 last_cycle: int, total_nodes: int):
    """
    Write BookSim2 config that exactly matches the uploaded template,
    with fields filled from summary.json.
    """
    mesh_dim   = int(math.isqrt(topo["cores_per_chiplet"]))
    chiplets   = topo["chiplet_count"]
    rings      = topo["rings"]
    intra_lat  = topo["intra_chiplet_hop_cycles"]
    inter_lat  = topo["inter_chiplet_hop_cycles"]

    # sample_period must be > last inject cycle so the warmup phase covers
    # the entire trace.  Add a 100-cycle margin (min 500).
    sample_period = max(500, last_cycle + 100)

    lines = [
        f"// BookSim2 config — HexaMesh anynet + trace-driven injection",
        f"// rings={rings}, mesh_dim={mesh_dim}, chiplets={chiplets}, total_nodes={total_nodes}",
        f"// intra_latency={intra_lat} cycles, inter_latency={inter_lat} cycles (UCIe PHY)",
        f"",
        f"// ── Topology ────────────────────────────────────────────────────",
        f"topology     = anynet;",
        f"network_file = {anynet_path_in_config};",
        f"",
        f"// ── Routing ─────────────────────────────────────────────────────",
        f'// "min" + "_" + "anynet" = "min_anynet" (Dijkstra shortest-path)',
        f"routing_function = min;",
        f"",
        f"// ── Flow control ─────────────────────────────────────────────────",
        f"num_vcs          = 8;",
        f"vc_buf_size      = 8;",
        f"wait_for_tail_credit = 1;",
        f"vc_allocator     = islip;",
        f"sw_allocator     = islip;",
        f"alloc_iters      = 2;",
        f"credit_delay     = 2;",
        f"routing_delay    = 0;",
        f"vc_alloc_delay   = 1;",
        f"sw_alloc_delay   = 1;",
        f"st_final_delay   = 1;",
        f"input_speedup    = 1;",
        f"output_speedup   = 1;",
        f"internal_speedup = 1.0;",
        f"",
        f"// ── Trace-driven injection ───────────────────────────────────────",
        f'// Both traffic AND injection_process must be set to "trace".',
        f"// trace_file: 4-column text file, no comments, one packet per line:",
        f"//   <inject_cycle>  <src_node_id>  <dst_node_id>  <packet_size_flits>",
        f"traffic           = trace;",
        f"injection_process = trace;",
        f"trace_file        = {trace_path_in_config};",
        f"",
        f"// packet_size: upper bound only — actual size comes from trace column 4.",
        f"// Set this to your largest packet size in the trace.",
        f"packet_size       = {max_flits};",
        f"",
        f"use_read_write    = 0;",
        f"",
        f"// ── Simulation control ───────────────────────────────────────────",
        f"// sim_type=latency: tracks per-packet end-to-end latency.",
        f"// warmup_periods=0: convergence-based warmup (correct for trace mode —",
        f"//   the modified trafficmanager exits as soon as all trace events are",
        f"//   injected and queues drain, printing stats before any reset).",
        f"// sample_period: must be >= last_trace_inject_cycle + ~100 cycles margin",
        f"//   so the warmup phase spans the entire trace. Use:",
        f"//   sample_period = <last_event_cycle_in_your_trace> + 100",
        f"sim_type          = latency;",
        f"warmup_periods    = 0;",
        f"sample_period     = {sample_period};",
        f"sim_count         = 1;",
        f"",
        f"// ── Optional: write Matlab-format stats to a file ────────────────",
        f"// stats_out = results.m;",
    ]

    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


# ─────────────────────────────────────────────────────────────────────────────
# Summary printer
# ─────────────────────────────────────────────────────────────────────────────

def print_summary(events, dropped, topo, out_trace, out_config, out_anynet):
    total      = len(events)
    total_flits = sum(e[3] for e in events)
    cpk        = topo["cores_per_chiplet"]
    cycles     = [e[0] for e in events]
    flits      = [e[3] for e in events]

    intra = sum(1 for e in events
                if e[1] // cpk == e[2] // cpk)

    print()
    print("══════════════════════════════════════════════════")
    print("  BookSim2 input generation — summary")
    print("══════════════════════════════════════════════════")
    print(f"  Topology : {topo['chiplet_count']} chiplets, "
          f"{cpk} cores/chiplet, "
          f"{topo['chiplet_count'] * cpk} total nodes")
    print(f"  Phit     : {topo['phit_bytes']} bytes/flit")
    print(f"  Latency  : intra={topo['intra_chiplet_hop_cycles']} cyc, "
          f"inter={topo['inter_chiplet_hop_cycles']} cyc")
    print()
    print(f"  CSV rows         : {total + dropped}")
    print(f"  Self-sends dropped: {dropped}")
    print(f"  Trace events     : {total}")
    print(f"  Total flits      : {total_flits}")
    print(f"  Intra-chiplet    : {intra}  ({100*intra/total:.1f}%)")
    print(f"  Inter-chiplet    : {total-intra}  ({100*(total-intra)/total:.1f}%)")
    print(f"  Cycle range      : {min(cycles)} – {max(cycles)}")
    print(f"  Flit size range  : {min(flits)} – {max(flits)}")
    print(f"  Avg flit/packet  : {total_flits/total:.2f}")
    print()
    print(f"  ✓  Trace  → {out_trace}")
    print(f"  ✓  Config → {out_config}")
    if out_anynet:
        print(f"  ✓  AnyNet → {out_anynet}")
    print("══════════════════════════════════════════════════")
    print()


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    args = parse_args()

    # ── Load inputs ────────────────────────────────────────────────────────
    if not os.path.exists(args.summary):
        sys.exit(f"ERROR: summary file not found: {args.summary}")
    if not os.path.exists(args.csv):
        sys.exit(f"ERROR: CSV file not found: {args.csv}")

    with open(args.summary) as f:
        summary = json.load(f)

    topo = summary["topology"]
    # Accept both key names the mapper might use
    topo.setdefault("phit_bytes",
                    summary.get("noc_params", {}).get("phit_bytes", 16))

    with open(args.csv, newline="") as f:
        rows = list(csv.DictReader(f))

    if not rows:
        sys.exit("ERROR: comm_latency.csv is empty.")

    # ── Derive parameters ──────────────────────────────────────────────────
    cores_per_chiplet = topo["cores_per_chiplet"]
    phit_bytes        = topo["phit_bytes"]
    num_chiplets      = topo["chiplet_count"]
    rings             = topo["rings"]
    intra_lat         = topo["intra_chiplet_hop_cycles"]
    inter_lat         = topo["inter_chiplet_hop_cycles"]
    total_nodes       = num_chiplets * cores_per_chiplet

    # ── Build trace ─────────────────────────────────────────────────────────
    events, dropped = build_trace(rows, cores_per_chiplet, phit_bytes)
    if not events:
        sys.exit("ERROR: No trace events after filtering self-sends.")

    last_cycle = max(e[0] for e in events)
    max_flits  = max(e[3] for e in events)

    # ── Output paths ────────────────────────────────────────────────────────
    os.makedirs(args.out_dir, exist_ok=True)

    out_trace  = os.path.join(args.out_dir, f"{args.name}_trace.txt")
    out_config = os.path.join(args.out_dir, f"{args.name}_config.txt")

    # anynet file: written to out_dir, but config references it via anynet_dir
    anynet_filename = f"hexamesh_r{rings}_m{int(math.isqrt(cores_per_chiplet))}_anynet"
    out_anynet  = None if args.no_anynet else \
                  os.path.join(args.out_dir, anynet_filename)

    # Paths as BookSim sees them (relative to its src/ working directory)
    trace_in_config  = f"{args.anynet_dir}/{args.name}_trace.txt"
    anynet_in_config = f"{args.anynet_dir}/{anynet_filename}"

    # ── Write outputs ────────────────────────────────────────────────────────
    write_trace(events, out_trace)

    write_config(
        path                 = out_config,
        topo                 = topo,
        trace_path_in_config = trace_in_config,
        anynet_path_in_config= anynet_in_config,
        max_flits            = max_flits,
        last_cycle           = last_cycle,
        total_nodes          = total_nodes,
    )

    if not args.no_anynet:
        adj, _ = build_adjacency(
            num_chiplets, cores_per_chiplet, rings, intra_lat, inter_lat
        )
        write_anynet(adj, total_nodes, out_anynet)

    # ── Print summary ────────────────────────────────────────────────────────
    print_summary(events, dropped, topo, out_trace, out_config, out_anynet)


if __name__ == "__main__":
    main()
