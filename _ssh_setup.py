import paramiko

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('connect.westd.seetacloud.com', port=47730, username='root', password='jaFrH7pgzWZ3', timeout=15)

commands = [
    'cat /root/autodl-tmp/trellis2/requirements.txt',
    'cat /root/autodl-tmp/trellis2/autodl_setup.sh',
    'ls /root/autodl-tmp/trellis2/trellis2/',
    'find /usr/local -name "nvcc" 2>/dev/null',
    'find /usr -name "cuda" -type d -maxdepth 3 2>/dev/null',
    'ls /usr/local/cuda*/bin/nvcc 2>/dev/null || echo "no nvcc found in /usr/local/cuda"',
    'cat /root/autodl-tmp/trellis2/setup.sh | head -60',
]

for cmd in commands:
    stdin, stdout, stderr = client.exec_command(cmd, timeout=15)
    out = stdout.read().decode().strip()
    err = stderr.read().decode().strip()
    print(f'>>> {cmd}')
    if out:
        print(out)
    if err and 'Permission denied' not in err:
        print(f'[ERR] {err}')
    print()

client.close()
