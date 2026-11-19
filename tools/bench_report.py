"""Run a bounded sweep or regenerate a Markdown comparison from raw JSON files."""
import argparse
import itertools
import json
from pathlib import Path
import statistics
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--binary', default='bin/release/allocBench')
p.add_argument('--output', type=Path, default=Path('benchmark-results'))
p.add_argument('--report-only', action='store_true')
p.add_argument('--allocators', default='empty,new,pool,lockfree,cached,arena,numa,pmr-pool,pmr-sync,pmr-arena')
p.add_argument('--threads', default='1,4')
p.add_argument('--sizes', default='32,256')
p.add_argument('--alignments', default='16')
p.add_argument('--live', default='1,64')
p.add_argument('--free', default='same')
p.add_argument('--iters', type=int, default=20000)
p.add_argument('--repetitions', type=int, default=5)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
if not a.report_only:
    dimensions = [a.allocators, a.threads, a.sizes, a.alignments, a.live, a.free]
    for allocator, threads, size, alignment, live, freeing in itertools.product(*(d.split(',') for d in dimensions)):
        config = dict(allocator=allocator, threads=threads, size=size, alignment=alignment, live=live, free=freeing)
        args = [a.binary] + [f'--{k}={v}' for k, v in config.items()]
        # Full overlap validation is separate from timed results.
        subprocess.run(args + ['--mode=verify', '--iters=512', '--warmup=128', '--repetitions=1'], check=True, stdout=subprocess.DEVNULL)
        for mode in ('throughput', 'latency'):
            raw = subprocess.check_output(args + [f'--mode={mode}', f'--iters={a.iters}', f'--repetitions={a.repetitions}'], text=True)
            name = '-'.join(config.values()) + '-' + mode + '.json'
            (a.output / name).write_text(raw)
rows = []
provenance = set()
for file in sorted(a.output.glob('*.json')):
    r = json.loads(file.read_text())
    if r['status'] == 'skipped':
        rows.append(f"| {r['allocator']} | skipped: {r['reason']} | | | | | | |")
        continue
    if r['profile'] != 'release' or r['instrumented'] or r['mode'] not in ('throughput', 'latency'):
        raise SystemExit(f'Refusing non-release, instrumented, or verification result: {file}')
    provenance.add((r['commit'], r['source_digest'], r['dirty'], r['compiler'], r['flags'], r['cpu'], r['os']))
    rates = [v['throughput_ops_s'] for v in r['runs']]
    tails = [v['allocate_ns']['p99'] for v in r['runs'] if v['allocate_ns']]
    tail = f'{statistics.median(tails):.1f}' if tails else '—'
    rows.append(f"| {r['allocator']} | {r['mode']}/{r['free']} | {r['threads']} | {r['size']}/{r['alignment']} | {r['live_per_thread']} | {statistics.mean(rates)/1e6:.3f} ± {statistics.pstdev(rates)/1e6:.3f} | {tail} | {r['lifetime']} |")
if len(provenance) > 1:
    raise SystemExit('Refusing to combine different commits, build settings, or hosts; use separate output directories.')
header = '''# Allocator comparison

Throughput is completed allocate/touch/check/retire cycles per second, including
live-set bookkeeping and any required epoch barriers. Latency mode adds sampled
per-operation clocks and is not directly comparable to throughput mode. Values
are mean ± population standard deviation across repetitions; p99 is the median
of per-run allocation p99s. Short smoke runs do not establish performance wins.

Arena/NUMA/PMR-arena rows use epoch-reset lifetimes; individual-free rows have
different reclamation costs. Empty rows expose harness/payload overhead; it is
not subtracted. No threshold is enforced on shared CI.

| Allocator | Mode/free | Threads | Bytes/alignment | Live/thread | Mcycles/s | Alloc p99 ns | Lifetime |
| --- | --- | --- | --- | --- | --- | --- | --- |
'''
text = header + '\n'.join(rows) + '\n\n## Provenance\n\n'
for commit, source_digest, dirty, compiler, flags, cpu, os_name in provenance:
    text += f'- Commit: `{commit}`; source digest: `{source_digest}`; dirty tree: `{dirty}`\n- Compiler: {compiler}\n- Flags: `{flags}`\n- CPU: {cpu}\n- OS: {os_name}\n'
text += '\nRaw JSON files beside this report include checksums, sample counts, allocation failures, setup/warmup times, and reserved-memory estimates.\n'
(a.output / 'report.md').write_text(text)
print(a.output / 'report.md')
