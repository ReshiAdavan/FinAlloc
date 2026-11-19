"""Write build provenance; preserve mtime if the configuration is unchanged."""
import argparse
import json
from pathlib import Path
import platform
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--output', required=True)
p.add_argument('--profile', required=True)
p.add_argument('--compiler', required=True)
p.add_argument('--flags', required=True)
a = p.parse_args()
def command(args):
    return subprocess.check_output(args, text=True).strip()
cpu = platform.processor() or platform.machine()
if platform.system() == 'Darwin':
    try:
        cpu = subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True, stderr=subprocess.DEVNULL).strip()
    except subprocess.CalledProcessError:
        cpu = platform.machine() + ' (CPU model unavailable)'
elif Path('/proc/cpuinfo').exists():
    for line in Path('/proc/cpuinfo').read_text().splitlines():
        if line.startswith('model name'):
            cpu = line.split(':', 1)[1].strip()
            break
values = {'PROFILE': a.profile, 'FLAGS': a.flags, 'COMPILER_COMMAND': a.compiler,
          'COMMIT': command(['git', 'rev-parse', 'HEAD']),
          'DIRTY': str(bool(command(['git', 'status', '--porcelain', '--untracked-files=normal']))).lower(),
          'CPU': cpu, 'OS': platform.platform()}
content = '#pragma once\n' + ''.join('#define FA_' + k + ' ' + json.dumps(v) + '\n' for k, v in values.items())
out = Path(a.output)
if not out.exists() or out.read_text() != content:
    out.write_text(content)
