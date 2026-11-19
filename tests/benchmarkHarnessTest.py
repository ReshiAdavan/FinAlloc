"""Contract checks for benchmark output and workload correctness (no speed thresholds)."""
import csv
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
binary = sys.argv[1]
def run(*args):
    return subprocess.check_output([binary, '--iters=97', '--warmup=23', '--repetitions=2', '--threads=2', '--live=7', *args], text=True)
checksums = set()
for allocator in ('empty', 'new', 'pool', 'lockfree', 'cached', 'arena', 'numa', 'pmr-pool', 'pmr-sync', 'pmr-arena'):
    for freeing in ('same', 'cross'):
        r = json.loads(run(f'--allocator={allocator}', f'--free={freeing}', '--mode=verify'))
        if r['status'] == 'skipped':
            assert freeing == 'cross' and allocator in ('pool', 'arena', 'pmr-pool', 'pmr-arena')
            continue
        for v in r['runs']:
            assert v['operations'] == 194 and v['allocation_failures'] == 0
            assert v['allocate_ns'] is None
            checksums.add(v['checksum'])
assert len(checksums) == 1
r = json.loads(run('--allocator=new', '--mode=latency', '--live=1', '--sample-every=3'))
assert r['runs'][0]['allocate_ns']['samples'] == 66
assert r['runs'][0]['pair_including_payload_ns']['samples'] == 66
assert r['runs'][0]['reserved_payload_or_upstream_peak_bytes'] is None
r = json.loads(run('--allocator=arena', '--mode=latency', '--sample-every=1'))
assert r['runs'][0]['reset_ns'] and r['runs'][0]['deallocate_ns'] is None
assert json.loads(run('--allocator=pool', '--alignment=256'))['status'] == 'skipped'
assert json.loads(run('--allocator=mimalloc'))['status'] == 'skipped'
r = list(csv.DictReader(io.StringIO(run('--allocator=new', '--format=csv'))))
assert len(r) == 2 and int(r[0]['operations']) == 194
for arg in ('--threads=0', '--threads=-1', '--size=bad', '--alignment=3', '--unknown=2', '--repetitions=0', '--allocator=missing'):
    result = subprocess.run([binary, arg], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode != 0, arg
# The report refuses instrumented input rather than silently comparing it.
with tempfile.TemporaryDirectory() as tmp:
    record = json.loads(run('--allocator=new'))
    record['instrumented'] = True
    Path(tmp, 'input.json').write_text(json.dumps(record))
    p = subprocess.run([sys.executable, 'tools/bench_report.py', '--output', tmp, '--report-only'], capture_output=True)
    assert p.returncode != 0 and b'Refusing' in p.stderr
print('benchmark harness contracts passed')
