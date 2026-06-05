import paramiko
import time
import sys

def run_cmd(client, cmd, timeout=300):
    print(f'\n>>> {cmd}')
    print('-' * 60)
    stdin, stdout, stderr = client.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode().strip()
    err = stderr.read().decode().strip()
    if out:
        for line in out.split('\n')[-30:]:
            print(line)
    if err:
        important = [l for l in err.split('\n') if 'error' in l.lower() or 'Error' in l or 'fatal' in l.lower()]
        if important:
            for l in important[-10:]:
                print(f'[ERR] {l}')
        elif len(err) < 500:
            print(f'[STDERR] {err[-500:]}')
    return out, err

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('connect.westd.seetacloud.com', port=47730, username='root', password='jaFrH7pgzWZ3', timeout=15)

ENV = 'source /root/miniconda3/bin/activate && export PATH=/usr/local/cuda-12.8/bin:$PATH && export CUDA_HOME=/usr/local/cuda-12.8 && export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH'

print("=" * 60)
print("STEP 1: Clean disk space")
print("=" * 60)
run_cmd(client, f'{ENV} && pip cache purge', timeout=60)
run_cmd(client, 'rm -rf /tmp/pip-unpack-* /tmp/extensions 2>/dev/null; conda clean --all -y 2>/dev/null; rm -rf /root/miniconda3/pkgs/*.tar.bz2 2>/dev/null', timeout=30)
run_cmd(client, 'rm -rf /root/autodl-tmp/trellis2_official_bak', timeout=10)
run_cmd(client, 'df -h /', timeout=5)

print("\n" + "=" * 60)
print("STEP 2: Install basic pip deps")
print("=" * 60)
run_cmd(client, f'{ENV} && pip install --no-cache-dir numpy scipy imageio imageio-ffmpeg tqdm easydict opencv-python-headless ninja trimesh kornia timm safetensors huggingface_hub plyfile zstandard lpips pandas tensorboard gradio utils3d', timeout=300)

print("\n" + "=" * 60)
print("STEP 3: Install CUDA extensions")
print("=" * 60)
run_cmd(client, f'{ENV} && nvcc --version | tail -1', timeout=10)

# CuMesh
print("\n--- CuMesh ---")
run_cmd(client, f'{ENV} && pip install --no-cache-dir git+https://github.com/JeffreyXiang/CuMesh.git --no-build-isolation', timeout=600)

# FlexGEMM
print("\n--- FlexGEMM ---")
run_cmd(client, f'{ENV} && pip install --no-cache-dir git+https://github.com/JeffreyXiang/FlexGEMM.git --no-build-isolation', timeout=600)

# o-voxel
print("\n--- o-voxel ---")
run_cmd(client, f'{ENV} && cd /root/autodl-tmp/trellis2/o-voxel && pip install --no-cache-dir -e . --no-build-isolation', timeout=300)

print("\n" + "=" * 60)
print("STEP 4: Install trellis2 package")
print("=" * 60)
run_cmd(client, f'{ENV} && cd /root/autodl-tmp/trellis2 && pip install --no-cache-dir -e .', timeout=120)

print("\n" + "=" * 60)
print("STEP 5: Set model path & verify")
print("=" * 60)
run_cmd(client, f'''{ENV} && python -c "
import torch
print(f'PyTorch: {{torch.__version__}}')
print(f'CUDA: {{torch.cuda.is_available()}}, Device: {{torch.cuda.get_device_name(0)}}')
import trellis2
print('trellis2 imported OK')
import o_voxel
print('o_voxel imported OK')
import cumesh
print('cumesh imported OK')
print('ALL CHECKS PASSED')
"''', timeout=60)

run_cmd(client, 'df -h /', timeout=5)
print("\n" + "=" * 60)
print("DONE")
print("=" * 60)

client.close()
