#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise real GDB packets against nonidentity guest virtual mappings."""
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time


class Remote:
    def __init__(self, connection):
        self.connection = connection

    def byte(self):
        result = self.connection.recv(1)
        if not result:
            raise RuntimeError('Debugger connection closed early')
        return result

    def send(self, command):
        data = command.encode('ascii')
        self.connection.sendall(b'$' + data + b'#' +
                                f'{sum(data) & 255:02x}'.encode('ascii'))
        assert self.byte() == b'+'

    def receive(self):
        assert self.byte() == b'$'
        data = bytearray()
        while (value := self.byte()) != b'#':
            data.extend(value)
        checksum = self.byte() + self.byte()
        assert int(checksum, 16) == sum(data) & 255
        self.connection.sendall(b'+')
        return data.decode('ascii')

    def command(self, command):
        self.send(command)
        return self.receive()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gem5', type=Path, default=Path('build/X86/gem5.opt'))
    parser.add_argument('--outdir', type=Path, default=Path('m5out-paging-gdb'))
    parser.add_argument('--cpu', choices=['atomic', 'timing', 'o3'], default='timing')
    args = parser.parse_args()
    suite = Path(__file__).resolve().parent
    symbols = subprocess.check_output(['nm', str(suite / 'gdb.elf')], text=True)
    ready = next(int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.split()[-1] == 'gdb_ready')
    args.outdir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gem5-gdb-') as temporary:
        path = str(Path(temporary) / 'socket')
        with (args.outdir / 'console.log').open('w') as log:
            process = subprocess.Popen([
                str(args.gem5), '--listener-mode=on', f'--outdir={args.outdir}',
                str(suite / 'config.py'), '--cpu', args.cpu, '--caches',
                '--guest', 'gdb', '--gdb-socket', path,
            ], stdout=log, stderr=subprocess.STDOUT)
            try:
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(30)
                    deadline = time.monotonic() + 30
                    while True:
                        try:
                            connection.connect(path)
                            break
                        except (FileNotFoundError, ConnectionRefusedError):
                            if process.poll() is not None or time.monotonic() > deadline:
                                raise RuntimeError('Debugger listener did not start; see console.log')
                            time.sleep(0.02)
                    remote = Remote(connection)
                    assert remote.command(f'Z0,{ready:x},1') == 'OK'
                    remote.send('c')
                    assert remote.receive().startswith('T05')
                    va = 1 << 39
                    assert remote.command(f'm{va:x},8') == 'efcdab8967452301'
                    assert remote.command(f'm{va + 4096:x},8') == '1032547698badcfe'
                    assert remote.command(f'm{va + 4094:x},4') == 'aabb1032'
                    # The first byte is mapped; the second page is absent.
                    assert remote.command(f'm{va + 8191:x},2') == 'E05'
                    # Both endpoints are mapped, with an inaccessible interior.
                    assert remote.command(f'm{va:x},3001') == 'E05'
                    assert remote.command('mfffffffffffffffe,4') == 'E05'
                    assert remote.command(f'z0,{ready:x},1') == 'OK'
                    remote.send('D')
                assert process.wait(timeout=30) == 0
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
    print(f'GDB paging {args.cpu}: PASS (nonidentity, split, holes, overflow, no A/D)')


if __name__ == '__main__':
    main()
