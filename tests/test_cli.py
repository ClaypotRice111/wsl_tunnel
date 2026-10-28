import argparse
import asyncio
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import wsl_tunnel as app


class CompatibilityTests(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(app.wsl_version('5.15.167.4-microsoft-standard-WSL2'), 2)
        self.assertEqual(app.wsl_version('4.4.0-19041-Microsoft'), 1)
        self.assertEqual(app.wsl_version('6.8.0-generic'), 0)

    def test_distro_families(self):
        mint = app.parse_os_release('ID=linuxmint\nID_LIKE="ubuntu debian"\nPRETTY_NAME="Linux Mint"')
        self.assertEqual(mint['PRETTY_NAME'], 'Linux Mint')
        self.assertIn('apt', app.install_hints(mint)[0])
        for family, manager in [('fedora', 'dnf'), ('arch', 'pacman'), ('alpine', 'apk')]:
            self.assertIn(manager, app.install_hints({'ID': family})[0])
        # os-release is data, never evaluated as shell code.
        self.assertEqual(app.parse_os_release('ID="$(id)"')['ID'], '$(id)')

    def test_input_rejection(self):
        for value in ('0', '65536', '80;id'):
            with self.assertRaises(argparse.ArgumentTypeError):
                app.port(value)
        for value in ('::1', 'host', '127.0.0.1;id'):
            with self.assertRaises(argparse.ArgumentTypeError):
                app.ipv4(value)
        self.assertEqual(app.busid('1-2.3'), '1-2.3')
        with self.assertRaises(argparse.ArgumentTypeError):
            app.busid('1-2 --force')

    @patch.object(app, 'wsl_version', return_value=2)
    @patch.object(app.shutil, 'which', return_value='/usr/bin/wslinfo')
    def test_network_modes(self, *_):
        with patch.object(app, 'run', return_value='mirrored'):
            self.assertEqual(app.host_address(), '127.0.0.1')
        with patch.object(app, 'run', side_effect=['nat', 'default via 172.22.0.1 dev eth0']):
            self.assertEqual(app.host_address(), '172.22.0.1')
        with patch.object(app, 'run', return_value='virtioproxy'):
            with self.assertRaises(RuntimeError):
                app.host_address()

    @patch.object(app, 'wsl_version', return_value=2)
    @patch.object(app.shutil, 'which', return_value=None)
    def test_old_wsl_requires_explicit_ip(self, *_):
        with self.assertRaises(RuntimeError):
            app.host_address()

    @patch.object(app, 'wsl_version', return_value=2)
    @patch.object(app, 'windows_tool', return_value='usbipd.exe')
    @patch.object(app, 'run', side_effect=['--wsl', 'attached'])
    def test_windows_usb_backend(self, execute, *_):
        args = app.parser().parse_args(['usb', 'attach', '--busid', '1-2'])
        self.assertEqual(app.usb_command(args), 'attached')
        self.assertEqual(execute.call_args.args[0], ['usbipd.exe', 'attach', '--wsl', '--busid', '1-2'])


class RelayTests(unittest.IsolatedAsyncioTestCase):
    @unittest.skipIf(os.name == 'nt', 'Linux CLI TCP half-close test; run inside WSL/Linux')
    async def test_binary_transfer_half_close_and_listener_conflict(self):
        payload = bytes(range(256)) * 2048
        async def destination(reader, writer):
            # Response only after the request EOF: exercise TCP half-close.
            data = await reader.read()
            writer.write(data[::-1])
            await writer.drain()
            writer.close()
            await writer.wait_closed()
        upstream = await asyncio.start_server(destination, '127.0.0.1', 0)
        upstream_port = upstream.sockets[0].getsockname()[1]
        proxy = await asyncio.start_server(
            lambda r, w: app.proxy_connection(r, w, '127.0.0.1', upstream_port), '127.0.0.1', 0)
        proxy_port = proxy.sockets[0].getsockname()[1]
        try:
            reader, writer = await asyncio.open_connection('127.0.0.1', proxy_port)
            writer.write(payload)
            await writer.drain()
            writer.write_eof()
            result = await asyncio.wait_for(reader.read(), 10)
            self.assertEqual(result, payload[::-1])
            writer.close()
            await writer.wait_closed()
            with self.assertRaises(OSError):
                await asyncio.start_server(destination, '127.0.0.1', proxy_port)
        finally:
            proxy.close()
            upstream.close()
            await proxy.wait_closed()
            await upstream.wait_closed()


if __name__ == '__main__':
    unittest.main()
