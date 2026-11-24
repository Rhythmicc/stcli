"""Read-only device queries in a typed Slurm allocation; no license/solve."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
target = sys.argv[1]
assert os.environ.get('SLURM_JOB_ID'), 'GPU queries require an allocation'
env = os.environ.copy()
runtimes = {
    'musa': '/usr/local/musa-5.2/lib:/usr/local/musa-5.1/lib:/usr/local/musa/lib',
    'rocm': '/opt/rocm/lib',
    'dtk': '/opt/dtk-26.04/lib:/opt/dtk-26.04/hip/lib:/opt/dtk/lib:/opt/dtk/hip/lib',
    'sycl': '/opt/intel/oneapi/compiler/latest/lib',
}
env['LD_LIBRARY_PATH'] = str(ROOT) + ':' + runtimes[target]
binary = ROOT / ('stcli-linux-x86_64-sycl' if target == 'sycl' else 'stcli-linux-x86_64')
backend = 'hip' if target in ('rocm', 'dtk') else target
query = subprocess.run([str(binary), '--backend', backend, '--json'], env=env,
                       capture_output=True, text=True, timeout=30)
record = dict(target=target, host=platform.node(), job=os.environ['SLURM_JOB_ID'],
              binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              exit_code=query.returncode, stdout=query.stdout, stderr=query.stderr)
if target == 'sycl':
    # Audit-only probe pinned to this exact binary, never shipped to customers.
    # Disassembly: getter at 0x39170, UUID offset 0x120, PrintGpuInfo at 0x21780.
    sdk = ROOT / 'SYCL-libpangulu.so'
    assert hashlib.sha256(sdk.read_bytes()).hexdigest() == '249aa1d390b2d5dfe0ba3e61cb486185da1ce1ebc10ae2f115bc20309ef9909c'
    child = subprocess.run([sys.executable, str(ROOT / 'validate_sycl_sdk.py')],
                           env=env, capture_output=True, text=True, timeout=30)
    record['sdk_probe'] = dict(exit_code=child.returncode, stdout=child.stdout, stderr=child.stderr)
(ROOT / ('hardware-' + target + '-' + os.environ['SLURM_JOB_ID'] + '.json')).write_text(json.dumps(record, indent=2))
print(json.dumps(record, indent=2))
