import csv

intra = inter = 0
intra_bytes = inter_bytes = 0

with open('output/E3S-Consumer/comm_latency.csv') as f:
    reader = csv.DictReader(f)
    for row in reader:
        src_c = int(row['src_chiplet'])
        dst_c = int(row['dst_chiplet'])
        volume = int(row['volume_bytes'])
        
        if src_c == dst_c:
            intra += 1
            intra_bytes += volume
        else:
            inter += 1
            inter_bytes += volume
            print(f"INTER-CHIPLET: {row['src_name']} (C{src_c}) → "
                  f"{row['dst_name']} (C{dst_c})")

total = intra + inter
print(f"\n{'='*50}")
print(f"Intra-chiplet: {intra}/{total} ({100*intra/total:.1f}%)")
print(f"Inter-chiplet: {inter}/{total} ({100*inter/total:.1f}%)")
print(f"\n🎯 TARGET: Inter-chiplet should be 20-40%")
print(f"{'='*50}")
