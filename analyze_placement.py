import csv
from collections import defaultdict

chiplet_tasks = defaultdict(list)
graph_tasks = defaultdict(list)

with open('output/E3S-Consumer/task_placement.csv') as f:
    reader = csv.DictReader(f)
    for row in reader:
        task_id = int(row['task_id'])
        task_name = row['name']
        chiplet = int(row['assigned_chiplet'])
        graph_id = int(task_name.split('_')[0][1:])
        
        chiplet_tasks[chiplet].append((task_id, task_name))
        graph_tasks[graph_id].append((task_id, task_name, chiplet))

print("=== Task Distribution by Chiplet ===")
for chiplet in sorted(chiplet_tasks.keys()):
    print(f"Chiplet {chiplet}: {len(chiplet_tasks[chiplet])} tasks")

print("\n=== Task Distribution by Graph ===")
for graph in sorted(graph_tasks.keys()):
    print(f"\nGraph {graph}:")
    chiplets_used = set(c for _, _, c in graph_tasks[graph])
    print(f"  Chiplets used: {sorted(chiplets_used)}")
    for task_id, name, chiplet in sorted(graph_tasks[graph]):
        print(f"  {name} → Chiplet {chiplet}")
