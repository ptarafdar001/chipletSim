#!/usr/bin/env python3
"""
HexaMesh Results Visualization Suite
=====================================
Generates publication-quality figures for task mapping results.

Usage:
    python visualize.py --benchmark FFT-16
    python visualize.py --all
    python visualize.py --compare
"""

import argparse
import json
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import RegularPolygon, FancyBboxPatch, Rectangle
from matplotlib.collections import PatchCollection
import seaborn as sns
from pathlib import Path
import warnings
warnings.filterwarnings('ignore')

# ══════════════════════════════════════════════════════════════
# CONFIGURATION
# ══════════════════════════════════════════════════════════════

# Publication-quality settings
plt.rcParams['figure.dpi'] = 300
plt.rcParams['savefig.dpi'] = 300
plt.rcParams['font.family'] = 'serif'
plt.rcParams['font.serif'] = ['Times New Roman', 'DejaVu Serif']
plt.rcParams['font.size'] = 10
plt.rcParams['axes.labelsize'] = 11
plt.rcParams['axes.titlesize'] = 12
plt.rcParams['xtick.labelsize'] = 9
plt.rcParams['ytick.labelsize'] = 9
plt.rcParams['legend.fontsize'] = 9

# Color schemes
COLORS = {
    'HIGH': '#d62728',      # Red
    'MEDIUM': '#ff7f0e',    # Orange
    'LOW': '#2ca02c',       # Green
    'COMM_HEAVY': '#e74c3c',
    'COMM_LESS': '#3498db',
    'DEGRADED': '#e67e22',
    'ACTIVE': '#27ae60',
    'palette': sns.color_palette('Set2', 8)
}

BENCHMARKS = ['FFT-16', 'JPEG-Encoder', 'E3S-Auto', 
              'Fork-Join-8x4', 'Random-DAG-64', 'Random-DAG-128']

# ══════════════════════════════════════════════════════════════
# DATA LOADING
# ══════════════════════════════════════════════════════════════

def load_benchmark_data(base_path, benchmark):
    """Load all data files for a benchmark."""
    bench_dir = Path(base_path) / benchmark
    
    try:
        with open(bench_dir / 'summary.json', 'r') as f:
            summary = json.load(f)
        
        tasks = pd.read_csv(bench_dir / 'task_placement.csv')
        cores = pd.read_csv(bench_dir / 'core_utilization.csv')
        clusters = pd.read_csv(bench_dir / 'cluster_report.csv')
        events = pd.read_csv(bench_dir / 'event_log.csv')
        
        return {
            'summary': summary,
            'tasks': tasks,
            'cores': cores,
            'clusters': clusters,
            'events': events
        }
    except Exception as e:
        print(f"Error loading {benchmark}: {e}")
        return None

# ══════════════════════════════════════════════════════════════
# 1. GANTT CHART - Task Execution Timeline
# ══════════════════════════════════════════════════════════════

def plot_gantt_chart(data, benchmark, output_dir):
    """
    Gantt chart showing task execution timeline across chiplets/cores.
    """
    tasks = data['tasks']
    tasks = tasks[tasks['state'].isin(['COMPLETED', 'RUNNING'])].copy()
    
    if len(tasks) == 0:
        print(f"No completed tasks for {benchmark}")
        return
    
    # Sort by chiplet, then by start time
    tasks = tasks.sort_values(['assigned_chiplet', 'start_cycle'])
    
    fig, ax = plt.subplots(figsize=(14, 8))
    
    # Group by chiplet
    chiplets = tasks['assigned_chiplet'].unique()
    chiplets.sort()
    
    y_pos = {}
    current_y = 0
    
    for chiplet in chiplets:
        chiplet_tasks = tasks[tasks['assigned_chiplet'] == chiplet]
        y_pos[chiplet] = current_y
        
        for _, task in chiplet_tasks.iterrows():
            duration = task['finish_cycle'] - task['start_cycle']
            color = COLORS.get(task['criticality'], COLORS['palette'][0])
            
            rect = Rectangle(
                (task['start_cycle'], current_y - 0.4),
                duration,
                0.8,
                facecolor=color,
                edgecolor='black',
                linewidth=0.5,
                alpha=0.8
            )
            ax.add_patch(rect)
            
            # Add task label if space allows
            if duration > 3:
                ax.text(
                    task['start_cycle'] + duration/2,
                    current_y,
                    f"T{task['task_id']}",
                    ha='center',
                    va='center',
                    fontsize=7,
                    fontweight='bold',
                    color='white'
                )
        
        current_y += 1
    
    # Styling
    ax.set_ylim(-0.5, len(chiplets) + 0.5)
    ax.set_xlim(0, tasks['finish_cycle'].max() * 1.05)
    ax.set_yticks(range(len(chiplets)))
    ax.set_yticklabels([f'Chiplet {c}' for c in chiplets])
    ax.set_xlabel('Execution Time (cycles)', fontweight='bold')
    ax.set_ylabel('Chiplet ID', fontweight='bold')
    ax.set_title(f'Task Execution Timeline — {benchmark}', 
                 fontweight='bold', fontsize=14, pad=20)
    ax.grid(axis='x', alpha=0.3, linestyle='--')
    
    # Legend
    legend_elements = [
        mpatches.Patch(facecolor=COLORS['HIGH'], edgecolor='black', 
                      label='HIGH Criticality', alpha=0.8),
        mpatches.Patch(facecolor=COLORS['MEDIUM'], edgecolor='black', 
                      label='MEDIUM Criticality', alpha=0.8),
        mpatches.Patch(facecolor=COLORS['LOW'], edgecolor='black', 
                      label='LOW Criticality', alpha=0.8)
    ]
    ax.legend(handles=legend_elements, loc='upper right', framealpha=0.9)
    
    plt.tight_layout()
    plt.savefig(output_dir / f'{benchmark}_gantt.png', bbox_inches='tight')
    plt.savefig(output_dir / f'{benchmark}_gantt.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Gantt chart saved: {benchmark}_gantt.png")

# ══════════════════════════════════════════════════════════════
# 2. HEATMAP - Chiplet Utilization
# ══════════════════════════════════════════════════════════════

def plot_chiplet_heatmap(data, benchmark, output_dir):
    """
    Heatmap showing chiplet utilization and task distribution.
    """
    chiplets = data['summary']['chiplets']
    
    # Create utilization matrix
    chiplet_data = []
    for c in chiplets:
        chiplet_data.append({
            'Chiplet': c['id'],
            'Ring': c['ring'],
            'Tasks Hosted': c['tasks_hosted'],
            'Faulty Cores': c['faulty_cores'],
            'State': c['state']
        })
    
    df = pd.DataFrame(chiplet_data)
    
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    
    # 1. Tasks hosted heatmap
    max_tasks = df['Tasks Hosted'].max()
    pivot1 = df.pivot_table(values='Tasks Hosted', 
                            index='Ring', 
                            columns='Chiplet', 
                            fill_value=0)
    
    sns.heatmap(pivot1, annot=True, fmt='g', cmap='YlGnBu', 
                cbar_kws={'label': 'Tasks Hosted'},
                linewidths=0.5, linecolor='gray', ax=axes[0])
    axes[0].set_title('Task Distribution Across Chiplets', fontweight='bold')
    axes[0].set_xlabel('Chiplet ID', fontweight='bold')
    axes[0].set_ylabel('Ring', fontweight='bold')
    
    # 2. Faulty cores heatmap
    pivot2 = df.pivot_table(values='Faulty Cores', 
                            index='Ring', 
                            columns='Chiplet', 
                            fill_value=0)
    
    sns.heatmap(pivot2, annot=True, fmt='g', cmap='Reds', 
                cbar_kws={'label': 'Faulty Cores'},
                linewidths=0.5, linecolor='gray', ax=axes[1])
    axes[1].set_title('Fault Distribution', fontweight='bold')
    axes[1].set_xlabel('Chiplet ID', fontweight='bold')
    axes[1].set_ylabel('Ring', fontweight='bold')
    
    # 3. Utilization bar chart
    df_sorted = df.sort_values('Tasks Hosted', ascending=False)
    colors = [COLORS['DEGRADED'] if s == 'DEGRADED' else COLORS['ACTIVE'] 
              for s in df_sorted['State']]
    
    axes[2].barh(df_sorted['Chiplet'].astype(str), df_sorted['Tasks Hosted'], 
                 color=colors, edgecolor='black', linewidth=0.5)
    axes[2].set_xlabel('Tasks Hosted', fontweight='bold')
    axes[2].set_ylabel('Chiplet ID', fontweight='bold')
    axes[2].set_title('Chiplet Load Distribution', fontweight='bold')
    axes[2].grid(axis='x', alpha=0.3, linestyle='--')
    
    # Legend for bar chart
    legend_elements = [
        mpatches.Patch(facecolor=COLORS['ACTIVE'], edgecolor='black', 
                      label='Active', alpha=0.8),
        mpatches.Patch(facecolor=COLORS['DEGRADED'], edgecolor='black', 
                      label='Degraded', alpha=0.8)
    ]
    axes[2].legend(handles=legend_elements, loc='lower right')
    
    plt.suptitle(f'Chiplet Utilization Analysis — {benchmark}', 
                 fontweight='bold', fontsize=14, y=1.02)
    plt.tight_layout()
    plt.savefig(output_dir / f'{benchmark}_heatmap.png', bbox_inches='tight')
    plt.savefig(output_dir / f'{benchmark}_heatmap.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Heatmap saved: {benchmark}_heatmap.png")

# ══════════════════════════════════════════════════════════════
# 3. HEXAGONAL MESH TOPOLOGY with Task Overlay
# ══════════════════════════════════════════════════════════════

def plot_hex_topology(data, benchmark, output_dir):
    """
    Hexagonal mesh network topology with task placement overlay.
    """
    chiplets = data['summary']['chiplets']
    
    fig, ax = plt.subplots(figsize=(12, 10))
    
    # Hexagon positioning (approximate for rings=2)
    def get_hex_positions(n_chiplets):
        positions = {}
        radius = 1.5
        
        # Ring 0: center
        positions[0] = (0, 0)
        
        # Ring 1: 6 around center
        for i in range(1, min(7, n_chiplets)):
            angle = np.pi / 3 * (i - 1)
            positions[i] = (radius * 2 * np.cos(angle), 
                           radius * 2 * np.sin(angle))
        
        # Ring 2: 12 around ring 1
        for i in range(7, min(19, n_chiplets)):
            angle = np.pi / 6 * (i - 7)
            positions[i] = (radius * 4 * np.cos(angle), 
                           radius * 4 * np.sin(angle))
        
        return positions
    
    positions = get_hex_positions(len(chiplets))
    
    # Normalize task counts for color intensity
    max_tasks = max(c['tasks_hosted'] for c in chiplets) or 1
    
    # Draw hexagons
    for chiplet in chiplets:
        cid = chiplet['id']
        if cid not in positions:
            continue
        
        x, y = positions[cid]
        
        # Color by task load
        intensity = chiplet['tasks_hosted'] / max_tasks
        if chiplet['state'] == 'DEGRADED':
            color = (1.0, 0.4, 0.2, 0.6 + 0.4 * intensity)  # Red-orange
        else:
            color = (0.2, 0.6, 0.9, 0.3 + 0.7 * intensity)  # Blue
        
        hexagon = RegularPolygon(
            (x, y), 6, radius=1.2,
            facecolor=color,
            edgecolor='black' if chiplet['state'] == 'ACTIVE' else 'red',
            linewidth=2 if chiplet['state'] == 'DEGRADED' else 1
        )
        ax.add_patch(hexagon)
        
        # Chiplet ID label
        ax.text(x, y + 0.3, f'C{cid}', 
                ha='center', va='center', 
                fontweight='bold', fontsize=11,
                bbox=dict(boxstyle='round,pad=0.3', 
                         facecolor='white', alpha=0.8, edgecolor='none'))
        
        # Task count
        ax.text(x, y - 0.2, f'{chiplet["tasks_hosted"]}T', 
                ha='center', va='center', 
                fontsize=9, color='black')
        
        # Ring and degree info
        ax.text(x, y - 0.5, f'R{chiplet["ring"]}·D{chiplet["degree"]}', 
                ha='center', va='center', 
                fontsize=7, color='gray')
    
    # Draw edges (connections between neighbors - approximate)
    # For actual topology, you'd need neighbor info from the mesh structure
    
    ax.set_xlim(-7, 7)
    ax.set_ylim(-7, 7)
    ax.set_aspect('equal')
    ax.axis('off')
    ax.set_title(f'HexaMesh Topology & Task Placement — {benchmark}', 
                 fontweight='bold', fontsize=14, pad=20)
    
    # Legend
    legend_elements = [
        mpatches.Patch(facecolor=(0.2, 0.6, 0.9, 0.8), edgecolor='black', 
                      label='Active Chiplet', linewidth=1),
        mpatches.Patch(facecolor=(1.0, 0.4, 0.2, 0.8), edgecolor='red', 
                      label='Degraded Chiplet', linewidth=2),
        mpatches.Patch(facecolor='none', edgecolor='none', label=''),
        mpatches.Patch(facecolor='none', edgecolor='none', 
                      label='Color intensity = Task load')
    ]
    ax.legend(handles=legend_elements, loc='upper left', 
             framealpha=0.9, fontsize=10)
    
    plt.tight_layout()
    plt.savefig(output_dir / f'{benchmark}_topology.png', bbox_inches='tight')
    plt.savefig(output_dir / f'{benchmark}_topology.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Topology saved: {benchmark}_topology.png")

# ══════════════════════════════════════════════════════════════
# 4. CLUSTER VISUALIZATION
# ══════════════════════════════════════════════════════════════

def plot_cluster_analysis(data, benchmark, output_dir):
    """
    Visualization of task clustering: COMM_HEAVY vs COMM_LESS distribution.
    """
    clusters = data['clusters']
    
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # 1. Cluster type distribution
    type_counts = clusters['type'].value_counts()
    colors_type = [COLORS['COMM_HEAVY'] if t == 'COMM_HEAVY' else COLORS['COMM_LESS'] 
                   for t in type_counts.index]
    
    axes[0, 0].bar(type_counts.index, type_counts.values, 
                   color=colors_type, edgecolor='black', linewidth=1.5, alpha=0.8)
    axes[0, 0].set_ylabel('Number of Clusters', fontweight='bold')
    axes[0, 0].set_title('Cluster Type Distribution', fontweight='bold')
    axes[0, 0].grid(axis='y', alpha=0.3, linestyle='--')
    
    # Add value labels
    for i, (label, value) in enumerate(zip(type_counts.index, type_counts.values)):
        axes[0, 0].text(i, value + 0.5, str(value), 
                       ha='center', fontweight='bold', fontsize=11)
    
    # 2. Task count distribution per cluster
    axes[0, 1].hist([clusters[clusters['type'] == 'COMM_HEAVY']['task_count'],
                     clusters[clusters['type'] == 'COMM_LESS']['task_count']], 
                    bins=10, label=['COMM_HEAVY', 'COMM_LESS'],
                    color=[COLORS['COMM_HEAVY'], COLORS['COMM_LESS']], 
                    alpha=0.7, edgecolor='black', linewidth=1)
    axes[0, 1].set_xlabel('Tasks per Cluster', fontweight='bold')
    axes[0, 1].set_ylabel('Frequency', fontweight='bold')
    axes[0, 1].set_title('Cluster Size Distribution', fontweight='bold')
    axes[0, 1].legend()
    axes[0, 1].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 3. Communication volume analysis
    comm_data = clusters[clusters['total_comm_volume'] > 0]
    if len(comm_data) > 0:
        axes[1, 0].scatter(comm_data['task_count'], 
                          comm_data['total_comm_volume'],
                          c=[COLORS[t] for t in comm_data['type']],
                          s=100, alpha=0.6, edgecolors='black', linewidth=1)
        axes[1, 0].set_xlabel('Tasks in Cluster', fontweight='bold')
        axes[1, 0].set_ylabel('Total Communication Volume (bytes)', fontweight='bold')
        axes[1, 0].set_title('Communication Volume vs Cluster Size', fontweight='bold')
        axes[1, 0].grid(alpha=0.3, linestyle='--')
    else:
        axes[1, 0].text(0.5, 0.5, 'No communication data', 
                       ha='center', va='center', transform=axes[1, 0].transAxes)
    
    # 4. Execution cost distribution
    axes[1, 1].boxplot([clusters[clusters['type'] == 'COMM_HEAVY']['total_exec_cost'],
                        clusters[clusters['type'] == 'COMM_LESS']['total_exec_cost']], 
                       labels=['COMM_HEAVY', 'COMM_LESS'],
                       patch_artist=True,
                       boxprops=dict(facecolor=COLORS['palette'][0], alpha=0.6),
                       medianprops=dict(color='red', linewidth=2))
    axes[1, 1].set_ylabel('Total Execution Cost (cycles)', fontweight='bold')
    axes[1, 1].set_title('Execution Cost Distribution', fontweight='bold')
    axes[1, 1].grid(axis='y', alpha=0.3, linestyle='--')
    
    plt.suptitle(f'Task Clustering Analysis — {benchmark}', 
                 fontweight='bold', fontsize=14, y=0.995)
    plt.tight_layout()
    plt.savefig(output_dir / f'{benchmark}_clusters.png', bbox_inches='tight')
    plt.savefig(output_dir / f'{benchmark}_clusters.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Cluster analysis saved: {benchmark}_clusters.png")

# ══════════════════════════════════════════════════════════════
# 5. FAULT TOLERANCE - Replica Placement
# ══════════════════════════════════════════════════════════════

def plot_fault_tolerance(data, benchmark, output_dir):
    """
    Analyze fault tolerance: replica placement and recovery.
    """
    events = data['events']
    tasks = data['tasks']
    
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    
    # 1. Event type distribution
    event_counts = events['event_type'].value_counts()
    axes[0, 0].barh(event_counts.index, event_counts.values, 
                    color=COLORS['palette'][:len(event_counts)], 
                    edgecolor='black', linewidth=1, alpha=0.8)
    axes[0, 0].set_xlabel('Count', fontweight='bold')
    axes[0, 0].set_title('Event Type Distribution', fontweight='bold')
    axes[0, 0].grid(axis='x', alpha=0.3, linestyle='--')
    
    # 2. Replica placement timeline
    replicas = events[events['event_type'] == 'REPLICA_PLACED']
    if len(replicas) > 0:
        axes[0, 1].plot(replicas['cycle'], range(len(replicas)), 
                       'o-', color=COLORS['HIGH'], linewidth=2, markersize=6)
        axes[0, 1].set_xlabel('Cycle', fontweight='bold')
        axes[0, 1].set_ylabel('Cumulative Replicas', fontweight='bold')
        axes[0, 1].set_title('Replica Placement Over Time', fontweight='bold')
        axes[0, 1].grid(alpha=0.3, linestyle='--')
    else:
        axes[0, 1].text(0.5, 0.5, 'No replicas created', 
                       ha='center', va='center', transform=axes[0, 1].transAxes)
    
    # 3. Criticality vs Replica presence
    if 'replica_core' in tasks.columns:
        crit_replica = tasks.groupby('criticality')['replica_core'].apply(
            lambda x: (x >= 0).sum()
        )
        crit_total = tasks['criticality'].value_counts()
        
        x = np.arange(len(crit_total))
        width = 0.35
        
        axes[1, 0].bar(x - width/2, crit_total.values, width, 
                      label='Total Tasks', color=COLORS['palette'][0], 
                      edgecolor='black', linewidth=1, alpha=0.8)
        axes[1, 0].bar(x + width/2, 
                      [crit_replica.get(c, 0) for c in crit_total.index], 
                      width, label='With Replica', color=COLORS['HIGH'], 
                      edgecolor='black', linewidth=1, alpha=0.8)
        
        axes[1, 0].set_xticks(x)
        axes[1, 0].set_xticklabels(crit_total.index)
        axes[1, 0].set_ylabel('Task Count', fontweight='bold')
        axes[1, 0].set_title('Replica Coverage by Criticality', fontweight='bold')
        axes[1, 0].legend()
        axes[1, 0].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 4. Fault recovery events
    faults = events[events['event_type'] == 'FAULT_DETECTED']
    recovered = events[events['event_type'] == 'FAULT_RECOVERED']
    
    if len(faults) > 0 or len(recovered) > 0:
        labels = ['Faults\nDetected', 'Tasks\nRecovered']
        counts = [len(faults), len(recovered)]
        colors_fault = [COLORS['DEGRADED'], COLORS['ACTIVE']]
        
        axes[1, 1].bar(labels, counts, color=colors_fault, 
                      edgecolor='black', linewidth=1.5, alpha=0.8)
        axes[1, 1].set_ylabel('Count', fontweight='bold')
        axes[1, 1].set_title('Fault Detection & Recovery', fontweight='bold')
        axes[1, 1].grid(axis='y', alpha=0.3, linestyle='--')
        
        for i, count in enumerate(counts):
            axes[1, 1].text(i, count + 0.2, str(count), 
                           ha='center', fontweight='bold', fontsize=11)
    else:
        axes[1, 1].text(0.5, 0.5, 'No faults detected', 
                       ha='center', va='center', transform=axes[1, 1].transAxes)
    
    plt.suptitle(f'Fault Tolerance Analysis — {benchmark}', 
                 fontweight='bold', fontsize=14, y=0.995)
    plt.tight_layout()
    plt.savefig(output_dir / f'{benchmark}_fault_tolerance.png', bbox_inches='tight')
    plt.savefig(output_dir / f'{benchmark}_fault_tolerance.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Fault tolerance analysis saved: {benchmark}_fault_tolerance.png")

# ══════════════════════════════════════════════════════════════
# 6. COMPARATIVE METRICS - All Benchmarks
# ══════════════════════════════════════════════════════════════

def plot_comparative_metrics(base_path, output_dir):
    """
    Compare metrics across all benchmarks.
    """
    summaries = {}
    for bench in BENCHMARKS:
        data = load_benchmark_data(base_path, bench)
        if data:
            summaries[bench] = data['summary']
    
    if not summaries:
        print("No benchmark data found for comparison")
        return
    
    fig, axes = plt.subplots(2, 3, figsize=(18, 10))
    axes = axes.flatten()
    
    bench_names = list(summaries.keys())
    
    # 1. Makespan (Total Cycles)
    makespans = [summaries[b]['total_cycles'] for b in bench_names]
    axes[0].bar(range(len(bench_names)), makespans, 
                color=COLORS['palette'][0], edgecolor='black', linewidth=1, alpha=0.8)
    axes[0].set_xticks(range(len(bench_names)))
    axes[0].set_xticklabels(bench_names, rotation=45, ha='right')
    axes[0].set_ylabel('Cycles', fontweight='bold')
    axes[0].set_title('Makespan Comparison', fontweight='bold')
    axes[0].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 2. Average Chiplet Utilization
    utils = [summaries[b]['avg_chiplet_utilization'] * 100 for b in bench_names]
    axes[1].bar(range(len(bench_names)), utils, 
                color=COLORS['palette'][1], edgecolor='black', linewidth=1, alpha=0.8)
    axes[1].set_xticks(range(len(bench_names)))
    axes[1].set_xticklabels(bench_names, rotation=45, ha='right')
    axes[1].set_ylabel('Utilization (%)', fontweight='bold')
    axes[1].set_title('Average Chiplet Utilization', fontweight='bold')
    axes[1].grid(axis='y', alpha=0.3, linestyle='--')
    axes[1].set_ylim(0, 100)
    
    # 3. Load Imbalance Index
    imbalances = [summaries[b]['load_imbalance_index'] for b in bench_names]
    axes[2].bar(range(len(bench_names)), imbalances, 
                color=COLORS['palette'][2], edgecolor='black', linewidth=1, alpha=0.8)
    axes[2].set_xticks(range(len(bench_names)))
    axes[2].set_xticklabels(bench_names, rotation=45, ha='right')
    axes[2].set_ylabel('Std Dev (σ)', fontweight='bold')
    axes[2].set_title('Load Imbalance Index', fontweight='bold')
    axes[2].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 4. Tasks vs Clusters
    tasks = [summaries[b]['total_tasks'] for b in bench_names]
    clusters = [summaries[b]['clusters_formed'] for b in bench_names]
    
    x = np.arange(len(bench_names))
    width = 0.35
    
    axes[3].bar(x - width/2, tasks, width, label='Tasks', 
                color=COLORS['palette'][3], edgecolor='black', linewidth=1, alpha=0.8)
    axes[3].bar(x + width/2, clusters, width, label='Clusters', 
                color=COLORS['palette'][4], edgecolor='black', linewidth=1, alpha=0.8)
    axes[3].set_xticks(x)
    axes[3].set_xticklabels(bench_names, rotation=45, ha='right')
    axes[3].set_ylabel('Count', fontweight='bold')
    axes[3].set_title('Tasks vs Clusters', fontweight='bold')
    axes[3].legend()
    axes[3].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 5. Fault Statistics
    faults = [summaries[b]['faults_detected'] for b in bench_names]
    recovered = [summaries[b]['faults_recovered'] for b in bench_names]
    
    axes[4].bar(x - width/2, faults, width, label='Detected', 
                color=COLORS['DEGRADED'], edgecolor='black', linewidth=1, alpha=0.8)
    axes[4].bar(x + width/2, recovered, width, label='Recovered', 
                color=COLORS['ACTIVE'], edgecolor='black', linewidth=1, alpha=0.8)
    axes[4].set_xticks(x)
    axes[4].set_xticklabels(bench_names, rotation=45, ha='right')
    axes[4].set_ylabel('Count', fontweight='bold')
    axes[4].set_title('Fault Statistics', fontweight='bold')
    axes[4].legend()
    axes[4].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 6. Efficiency Metric (Tasks per Cycle)
    efficiency = [summaries[b]['total_tasks'] / max(summaries[b]['total_cycles'], 1) 
                  for b in bench_names]
    axes[5].bar(range(len(bench_names)), efficiency, 
                color=COLORS['palette'][5], edgecolor='black', linewidth=1, alpha=0.8)
    axes[5].set_xticks(range(len(bench_names)))
    axes[5].set_xticklabels(bench_names, rotation=45, ha='right')
    axes[5].set_ylabel('Tasks/Cycle', fontweight='bold')
    axes[5].set_title('Execution Efficiency', fontweight='bold')
    axes[5].grid(axis='y', alpha=0.3, linestyle='--')
    
    plt.suptitle('Comparative Performance Metrics', 
                 fontweight='bold', fontsize=16, y=0.998)
    plt.tight_layout()
    plt.savefig(output_dir / 'comparative_metrics.png', bbox_inches='tight')
    plt.savefig(output_dir / 'comparative_metrics.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Comparative metrics saved: comparative_metrics.png")

# ══════════════════════════════════════════════════════════════
# 7. HISTOGRAMS - Distribution Analysis
# ══════════════════════════════════════════════════════════════

def plot_histograms(data, benchmark, output_dir):
    """
    Histogram analysis of various distributions.
    """
    tasks = data['tasks']
    cores = data['cores']
    
    fig, axes = plt.subplots(2, 3, figsize=(18, 10))
    axes = axes.flatten()
    
    # 1. Task execution time distribution
    if 'exec_time' in tasks.columns:
        axes[0].hist(tasks['exec_time'], bins=20, 
                    color=COLORS['palette'][0], edgecolor='black', alpha=0.7)
        axes[0].set_xlabel('Execution Time (cycles)', fontweight='bold')
        axes[0].set_ylabel('Frequency', fontweight='bold')
        axes[0].set_title('Task Execution Time Distribution', fontweight='bold')
        axes[0].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 2. Task criticality distribution
    crit_counts = tasks['criticality'].value_counts()
    colors_crit = [COLORS[c] for c in crit_counts.index]
    axes[1].bar(crit_counts.index, crit_counts.values, 
                color=colors_crit, edgecolor='black', linewidth=1, alpha=0.8)
    axes[1].set_xlabel('Criticality', fontweight='bold')
    axes[1].set_ylabel('Count', fontweight='bold')
    axes[1].set_title('Task Criticality Distribution', fontweight='bold')
    axes[1].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 3. Core utilization distribution
    if 'utilization' in cores.columns:
        axes[2].hist(cores['utilization'] * 100, bins=20, 
                    color=COLORS['palette'][1], edgecolor='black', alpha=0.7)
        axes[2].set_xlabel('Utilization (%)', fontweight='bold')
        axes[2].set_ylabel('Frequency', fontweight='bold')
        axes[2].set_title('Core Utilization Distribution', fontweight='bold')
        axes[2].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 4. Tasks per chiplet distribution
    tasks_per_chiplet = tasks.groupby('assigned_chiplet').size()
    axes[3].hist(tasks_per_chiplet, bins=15, 
                color=COLORS['palette'][2], edgecolor='black', alpha=0.7)
    axes[3].set_xlabel('Tasks per Chiplet', fontweight='bold')
    axes[3].set_ylabel('Frequency', fontweight='bold')
    axes[3].set_title('Task Distribution Across Chiplets', fontweight='bold')
    axes[3].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 5. Cluster size distribution
    clusters = data['clusters']
    axes[4].hist(clusters['task_count'], bins=15, 
                color=COLORS['palette'][3], edgecolor='black', alpha=0.7)
    axes[4].set_xlabel('Cluster Size (tasks)', fontweight='bold')
    axes[4].set_ylabel('Frequency', fontweight='bold')
    axes[4].set_title('Cluster Size Distribution', fontweight='bold')
    axes[4].grid(axis='y', alpha=0.3, linestyle='--')
    
    # 6. Communication volume distribution (log scale)
    comm_data = clusters[clusters['total_comm_volume'] > 0]['total_comm_volume']
    if len(comm_data) > 0:
        axes[5].hist(np.log10(comm_data + 1), bins=20, 
                    color=COLORS['palette'][4], edgecolor='black', alpha=0.7)
        axes[5].set_xlabel('log₁₀(Communication Volume)', fontweight='bold')
        axes[5].set_ylabel('Frequency', fontweight='bold')
        axes[5].set_title('Communication Volume Distribution', fontweight='bold')
        axes[5].grid(axis='y', alpha=0.3, linestyle='--')
    
    plt.suptitle(f'Distribution Analysis — {benchmark}', 
                 fontweight='bold', fontsize=14, y=0.998)
    plt.tight_layout()
    plt.savefig(output_dir / f'{benchmark}_histograms.png', bbox_inches='tight')
    plt.savefig(output_dir / f'{benchmark}_histograms.pdf', bbox_inches='tight')
    plt.close()
    print(f"✓ Histograms saved: {benchmark}_histograms.png")

# ══════════════════════════════════════════════════════════════
# MAIN
# ══════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description='Visualize HexaMesh task mapping results',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python visualize.py --benchmark FFT-16
  python visualize.py --all
  python visualize.py --compare
  python visualize.py --benchmark FFT-16 --output ./figures
        """
    )
    parser.add_argument('--benchmark', type=str, 
                       help='Specific benchmark to visualize')
    parser.add_argument('--all', action='store_true',
                       help='Generate all visualizations for all benchmarks')
    parser.add_argument('--compare', action='store_true',
                       help='Generate comparative plots across benchmarks')
    parser.add_argument('--base-path', type=str, default='output',
                       help='Base path to output directory (default: output)')
    parser.add_argument('--output', type=str, default='figures',
                       help='Output directory for figures (default: figures)')
    
    args = parser.parse_args()
    
    base_path = Path(args.base_path)
    output_dir = Path(args.output)
    output_dir.mkdir(exist_ok=True)
    
    print("\n" + "="*60)
    print("  HexaMesh Results Visualization Suite")
    print("="*60 + "\n")
    
    if args.all:
        # Generate all visualizations for all benchmarks
        for benchmark in BENCHMARKS:
            print(f"\n📊 Processing {benchmark}...")
            data = load_benchmark_data(base_path, benchmark)
            if data:
                plot_gantt_chart(data, benchmark, output_dir)
                plot_chiplet_heatmap(data, benchmark, output_dir)
                plot_hex_topology(data, benchmark, output_dir)
                plot_cluster_analysis(data, benchmark, output_dir)
                plot_fault_tolerance(data, benchmark, output_dir)
                plot_histograms(data, benchmark, output_dir)
        
        # Comparative plots
        print(f"\n📊 Generating comparative metrics...")
        plot_comparative_metrics(base_path, output_dir)
        
    elif args.benchmark:
        # Single benchmark
        benchmark = args.benchmark
        print(f"\n📊 Processing {benchmark}...")
        data = load_benchmark_data(base_path, benchmark)
        if data:
            plot_gantt_chart(data, benchmark, output_dir)
            plot_chiplet_heatmap(data, benchmark, output_dir)
            plot_hex_topology(data, benchmark, output_dir)
            plot_cluster_analysis(data, benchmark, output_dir)
            plot_fault_tolerance(data, benchmark, output_dir)
            plot_histograms(data, benchmark, output_dir)
        else:
            print(f"❌ Failed to load data for {benchmark}")
    
    elif args.compare:
        # Comparative only
        print(f"\n📊 Generating comparative metrics...")
        plot_comparative_metrics(base_path, output_dir)
    
    else:
        parser.print_help()
        return
    
    print("\n" + "="*60)
    print(f"  ✓ All figures saved to: {output_dir.absolute()}")
    print("="*60 + "\n")

if __name__ == '__main__':
    main()
