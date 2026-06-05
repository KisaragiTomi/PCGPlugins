import paramiko
import time

def run_cmd(client, cmd, timeout=600):
    print(f'\n>>> {cmd.split("&&")[-1].strip()[:100]}')
    print('-' * 60)
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode().strip()
    err = stderr.read().decode().strip()
    if out:
        for line in out.split('\n')[-20:]:
            print(line)
    if err:
        important = [l for l in err.split('\n') if 'error' in l.lower() or 'Error' in l or 'fatal' in l.lower() or 'success' in l.lower()]
        if important:
            for l in important[-10:]:
                print(f'[ERR] {l}')
        else:
            last_lines = err.split('\n')[-5:]
            for l in last_lines:
                print(f'[STDERR] {l}')
    return out, err

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('connect.westd.seetacloud.com', port=47730, username='root', password='jaFrH7pgzWZ3', timeout=15)

ENV = 'source /root/miniconda3/bin/activate && export PATH=/usr/local/cuda-12.8/bin:$PATH && export CUDA_HOME=/usr/local/cuda-12.8 && export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH && export TORCH_CUDA_ARCH_LIST="12.0" && export PYTHONPATH=/root/autodl-tmp/trellis2:$PYTHONPATH'

print("=" * 60)
print("FIX 1: o_voxel - check submodules and build")
print("=" * 60)

run_cmd(client, f'{ENV} && cd /root/autodl-tmp/trellis2/o-voxel && ls')
run_cmd(client, f'{ENV} && cat /root/autodl-tmp/trellis2/o-voxel/pyproject.toml 2>/dev/null | head -30 || cat /root/autodl-tmp/trellis2/o-voxel/setup.py 2>/dev/null | head -30')
run_cmd(client, f'{ENV} && cd /root/autodl-tmp/trellis2/o-voxel && git submodule status 2>/dev/null')

# Try without CuMesh dependency resolution
run_cmd(client, f'{ENV} && cd /root/autodl-tmp/trellis2/o-voxel && pip install --no-cache-dir --no-deps -e . --no-build-isolation 2>&1 | tail -20', timeout=600)

print("\n" + "=" * 60)
print("FIX 2: Verify all imports")
print("=" * 60)

run_cmd(client, f'''{ENV} && python -c "
import torch
print(f'PyTorch: {{torch.__version__}}')
print(f'CUDA: {{torch.cuda.is_available()}}, GPU: {{torch.cuda.get_device_name(0)}}')
print(f'SM: {{torch.cuda.get_device_capability(0)}}')

import trellis2
print('trellis2 OK')
try:
    import o_voxel
    print('o_voxel OK')
except Exception as e:
    print(f'o_voxel FAIL: {{e}}')
try:
    import cumesh
    print('cumesh OK')
except Exception as e:
    print(f'cumesh FAIL: {{e}}')
try:
    import flex_gemm
    print('flex_gemm OK')
except Exception as e:
    print(f'flex_gemm FAIL: {{e}}')
import nvdiffrast
print('nvdiffrast OK')
import xformers
print('xformers OK')
print('=== ALL DONE ===')
"''', timeout=60)

client.close()
