#!/usr/bin/env python3
"""WSL/Linux command-line companion to the Windows WinForms application.

Python >= 3.8, standard library only. No shell interpolation or automatic installs.
"""
import argparse
import asyncio
import glob
import ipaddress
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path


def run(argv, timeout=20):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout, check=False)
    def decode(data):
        if b'\x00' in data:
            return data.decode('utf-16-le', errors='replace').strip()
        return data.decode('utf-8', errors='replace').strip()
    output, error = decode(result.stdout), decode(result.stderr)
    if result.returncode:
        raise RuntimeError('{}: {}'.format(argv[0], error or output or result.returncode))
    return output


def parse_os_release(text):
    values = {}
    for line in text.splitlines():
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            try:
                parts = shlex.split(value, comments=False)
                values[key] = parts[0] if len(parts) == 1 else value
            except ValueError:
                values[key] = value
    return values


def distro_info():
    for file in ('/etc/os-release', '/usr/lib/os-release'):
        try:
            return parse_os_release(Path(file).read_text())
        except OSError:
            pass
    return {'ID': 'unknown', 'PRETTY_NAME': platform.system()}


def install_hints(info):
    families = set((info.get('ID', '') + ' ' + info.get('ID_LIKE', '')).split())
    if 'ubuntu' in families:
        return ['sudo apt install usbutils pciutils iproute2',
                'USB/IP: apt-cache search usbip; apt-cache search linux-tools (包随发行版/内核而异)']
    if 'debian' in families:
        return ['sudo apt install usbip usbutils pciutils iproute2']
    if families & {'fedora', 'rhel', 'centos'}:
        return ['sudo dnf install usbip usbutils pciutils iproute (软件源可用性取决于版本)']
    if families & {'arch', 'manjaro'}:
        return ['sudo pacman -S usbip usbutils pciutils iproute2']
    if families & {'suse', 'opensuse', 'opensuse-tumbleweed', 'opensuse-leap'}:
        return ['zypper search usbip; sudo zypper install usbutils pciutils iproute2']
    if 'alpine' in families:
        return ['sudo apk add usbip-tools usbutils pciutils iproute2 (需启用对应软件源)']
    return ['请用本发行版包管理器查找 usbip、usbutils、pciutils、iproute2。']


def wsl_version(release=None):
    release = (release if release is not None else platform.release()).lower()
    if 'microsoft' not in release and 'wsl' not in release:
        return 0
    return 2 if 'wsl2' in release or 'microsoft-standard' in release else 1


def tool(name):
    location = shutil.which(name)
    if not location:
        raise RuntimeError('缺少工具 {}。请运行 doctor 查看依赖提示。'.format(name))
    return location


def windows_tool(name):
    if not wsl_version():
        raise RuntimeError('Windows 互操作命令只能在 WSL 中使用。')
    location = shutil.which(name)
    if not location and name == 'usbipd.exe':
        # PATH may omit Windows Program Files, but the standard mount may exist.
        candidate = Path('/mnt/c/Program Files/usbipd-win/usbipd.exe')
        if candidate.is_file():
            location = str(candidate)
    if not location:
        raise RuntimeError('找不到 {}；请检查 Windows 安装、WSL interop 和 PATH。'.format(name))
    return location


def port(value):
    try:
        number = int(value)
        if not 1 <= number <= 65535:
            raise ValueError()
        return number
    except ValueError:
        raise argparse.ArgumentTypeError('端口必须为 1–65535')


def ipv4(value):
    try:
        return str(ipaddress.IPv4Address(value))
    except ipaddress.AddressValueError:
        raise argparse.ArgumentTypeError('请输入 IPv4 地址')


def busid(value):
    if not re.fullmatch(r'\d+-\d+(?:\.\d+)*', value):
        raise argparse.ArgumentTypeError('BUSID 应类似 1-2 或 1-2.3')
    return value


def host_address():
    if wsl_version() != 2:
        raise RuntimeError('auto 目标需要 WSL 2；原生 Linux / WSL 1 请明确填写目标 IP。')
    mode = 'unknown'
    if shutil.which('wslinfo'):
        try:
            mode = run(['wslinfo', '--networking-mode']).strip().lower()
        except RuntimeError:
            pass
    if mode == 'mirrored':
        return '127.0.0.1'
    if mode == 'nat':
        routes = run([tool('ip'), '-4', 'route', 'show', 'default'])
        match = re.search(r'\bvia\s+(\d+\.\d+\.\d+\.\d+)', routes)
        if match:
            return ipv4(match.group(1))
    raise RuntimeError('网络模式 {} 无法可靠解析 Windows 地址，请显式填写 --target IP。'.format(mode))


async def relay(reader, writer):
    while True:
        data = await reader.read(65536)
        if not data:
            if writer.can_write_eof():
                writer.write_eof()
                await writer.drain()
            return
        writer.write(data)
        await writer.drain()


async def proxy_connection(reader, writer, target, target_port):
    remote_writer = None
    tasks = []
    try:
        remote_reader, remote_writer = await asyncio.wait_for(
            asyncio.open_connection(target, target_port), timeout=10)
        tasks = [asyncio.create_task(relay(reader, remote_writer)),
                 asyncio.create_task(relay(remote_reader, writer))]
        await asyncio.gather(*tasks)
    except (OSError, asyncio.TimeoutError) as exc:
        print('连接失败: {}'.format(exc), file=sys.stderr)
    finally:
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        for stream in (writer, remote_writer):
            if stream is not None:
                stream.close()
                try:
                    await stream.wait_closed()
                except OSError:
                    pass


async def forward(args):
    target = host_address() if args.target == 'auto' else ipv4(args.target)
    server = await asyncio.start_server(
        lambda r, w: proxy_connection(r, w, target, args.target_port),
        args.listen, args.port, family=2)
    print('{}:{} → {}:{} | Ctrl+C 停止；退出即清理监听'.format(
        args.listen, args.port, target, args.target_port), flush=True)
    async with server:
        await server.serve_forever()


def usb_command(args):
    if args.backend == 'windows':
        if wsl_version() != 2:
            raise RuntimeError('Windows USB → WSL 需要 WSL 2。')
        executable = windows_tool('usbipd.exe')
        if args.action == 'list':
            return run([executable, 'list'])
        if args.action not in ('bind', 'unbind', 'attach', 'detach'):
            raise RuntimeError('Windows 后端支持 list/bind/unbind/attach/detach。')
        if not args.busid:
            raise RuntimeError('此操作需要 --busid。')
        if args.action == 'attach':
            if '--wsl' not in run([executable, 'attach', '--help']):
                raise RuntimeError('请升级 Windows usbipd-win；当前版本不支持 attach --wsl。')
            return run([executable, 'attach', '--wsl', '--busid', args.busid])
        try:
            return run([executable, args.action, '--busid', args.busid])
        except RuntimeError as exc:
            if args.action in ('bind', 'unbind'):
                raise RuntimeError('{}\n请在 Windows 管理员界面执行共享/取消共享；WSL sudo 不会提升 Windows 权限。'.format(exc))
            raise
    executable = tool('usbip')
    if args.action == 'list':
        return run([executable, 'list', '-r', args.remote] if args.remote else [executable, 'list', '-l'])
    if args.action == 'ports':
        return run([executable, 'port'])
    if args.action not in ('bind', 'unbind', 'attach', 'detach'):
        raise RuntimeError('不支持此 USB 操作。')
    if not hasattr(os, 'geteuid') or os.geteuid() != 0:
        raise RuntimeError('Linux USB 修改操作需要 root；请用 sudo 重新运行此命令。')
    if args.action == 'detach':
        if args.usb_port is None:
            raise RuntimeError('需要 --usb-port（用 usb ports 查看导入端口号，不是 BUSID）。')
        return run([executable, 'detach', '--port', str(args.usb_port)])
    if not args.busid:
        raise RuntimeError('需要 --busid。')
    if args.action == 'attach':
        if not args.remote:
            raise RuntimeError('Linux USB/IP attach 需要 --remote 服务器 IPv4。')
        return run([executable, 'attach', '--remote', args.remote, '--busid', args.busid])
    return run([executable, args.action, '--busid', args.busid])


def doctor():
    info = distro_info()
    version = wsl_version()
    result = {'distro': info, 'kernel': platform.release(),
              'environment': 'WSL {}'.format(version) if version else 'Native Linux',
              'wsl_distro_name': os.environ.get('WSL_DISTRO_NAME'),
              'python': platform.python_version(),
              'tools': {n: shutil.which(n) for n in ('ip', 'usbip', 'usbipd', 'usbipd.exe',
                                                     'lsusb', 'lspci', 'nvidia-smi', 'wslinfo')},
              'usb_modules': {n: Path('/sys/module/' + n).exists()
                              for n in ('usbip_core', 'usbip_host', 'vhci_hcd')},
              'install_hints_only': install_hints(info)}
    print(json.dumps(result, ensure_ascii=False, indent=2))


def gpu():
    version = wsl_version()
    print('环境：{}'.format('WSL {}'.format(version) if version else '原生 Linux'))
    if version == 1:
        raise RuntimeError('WSL 1 不支持 WSL NVIDIA GPU 加速。')
    if version == 2:
        print('使用 Windows NVIDIA 驱动；不要安装 Linux NVIDIA 显示驱动。')
        if not Path('/dev/dxg').exists():
            raise RuntimeError('缺少 /dev/dxg，请检查 WSL 更新与 Windows NVIDIA 驱动。')
    executable = shutil.which('nvidia-smi')
    if not executable and version == 2 and Path('/usr/lib/wsl/lib/nvidia-smi').is_file():
        executable = '/usr/lib/wsl/lib/nvidia-smi'
    if not executable:
        raise RuntimeError('未找到 nvidia-smi。原生 Linux 需要发行版支持的 NVIDIA 驱动。')
    print(run([executable]))
    print('仅验证 GPU 可见性；加载 Ollama 模型后用 ollama ps 查看 GPU 使用情况。')


def parser():
    p = argparse.ArgumentParser(description='WSL / Linux 命令行隧道管理；Windows 使用 WSLTunnel-Native.exe 图形界面')
    sub = p.add_subparsers(dest='command', required=True)
    sub.add_parser('doctor', help='检测发行版、WSL 版本、工具和模块；不安装依赖')
    f = sub.add_parser('forward', help='前台 TCP/IPv4 转发，Ctrl+C 停止，无 socat 依赖')
    f.add_argument('--listen', type=ipv4, default='127.0.0.1')
    f.add_argument('--port', type=port, required=True)
    f.add_argument('--target', required=True, help='目标 IPv4；auto 表示自动检测 Windows 主机')
    f.add_argument('--target-port', type=port, required=True)
    u = sub.add_parser('usb', help='Windows USB → WSL 或 Linux 原生 USB/IP')
    u.add_argument('action', choices=['list', 'bind', 'unbind', 'attach', 'detach', 'ports'])
    u.add_argument('--backend', choices=['windows', 'linux'],
                   default='windows' if wsl_version() else 'linux')
    u.add_argument('--busid', type=busid)
    u.add_argument('--remote', type=ipv4)
    u.add_argument('--usb-port', type=int)
    sub.add_parser('serial', help='列出 ESP32 等串口路径及当前用户组')
    sub.add_parser('gpu', help='检测 NVIDIA / WSL GPU 桥接')
    o = sub.add_parser('ollama', help='测试 Ollama /api/tags；不加载模型')
    o.add_argument('--host', type=ipv4, default='127.0.0.1')
    o.add_argument('--port', type=port, default=11434)
    return p


def main():
    args = parser().parse_args()
    try:
        if args.command == 'doctor':
            doctor()
        elif args.command == 'forward':
            asyncio.run(forward(args))
        elif args.command == 'usb':
            print(usb_command(args))
        elif args.command == 'serial':
            paths = sorted(set(glob.glob('/dev/ttyUSB*') + glob.glob('/dev/ttyACM*') +
                               glob.glob('/dev/serial/by-id/*')))
            print('\n'.join(paths) if paths else '未发现串口；请检查 USB attach 和 Linux 驱动。')
            print(run([tool('id')]))
        elif args.command == 'gpu':
            gpu()
        elif args.command == 'ollama':
            url = 'http://{}:{}/api/tags'.format(args.host, args.port)
            # Local connection should not be redirected through HTTP_PROXY.
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(url, timeout=5) as response:
                data = json.load(response)
            print(json.dumps(data, ensure_ascii=False, indent=2))
            print('API 可访问；此测试不证明模型已使用 GPU，也不确认服务属于哪个系统。')
        return 0
    except KeyboardInterrupt:
        print('\n已停止。')
        return 0
    except (OSError, RuntimeError, ValueError, asyncio.TimeoutError, subprocess.TimeoutExpired) as exc:
        print('错误: {}'.format(exc), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
