"""Rebuild the current r16h loader; never access the phone or replace the IMG."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parent
BASE = 0xB9200000
UNITS = ['loader', 'dada_gpu_loader', 'dada_touch_uefi', 'dada_touch_config',
         'dada_touch_clock', 'dada_touch_dttrace', 'dada_touch_fwtrace',
         'dada_touch_gpio', 'dada_touch_gpio_uefi', 'dada_battery_uefi', 'dada_touch_tcm']


def rebase(raw):
    data = bytearray(raw)
    pe, = struct.unpack_from('<I', data, 60)
    assert data[pe:pe + 4] == b'PE\0\0'
    machine, sections = struct.unpack_from('<HH', data, pe + 4)
    optional_size, = struct.unpack_from('<H', data, pe + 20)
    opt = pe + 24
    assert machine == 0xaa64 and struct.unpack_from('<H', data, opt)[0] == 0x20b
    assert struct.unpack_from('<H', data, opt + 68)[0] == 10
    assert struct.unpack_from('<II', data, opt + 120) == (0, 0), 'Unexpected Windows imports'
    old, = struct.unpack_from('<Q', data, opt + 24)
    assert old in (0, BASE)
    if old == BASE:
        return bytes(data), 0
    segments = []
    for i in range(sections):
        s = opt + optional_size + i * 40
        va, size, offset = struct.unpack_from('<III', data, s + 12)
        segments.append((va, size, offset))
    def file_offset(rva):
        for va, size, offset in segments:
            if va <= rva < va + size:
                return offset + rva - va
        raise ValueError('RVA does not map to file: ' + hex(rva))
    relocation_rva, relocation_size = struct.unpack_from('<II', data, opt + 112 + 5 * 8)
    pos = file_offset(relocation_rva)
    end, count = pos + relocation_size, 0
    image_size, = struct.unpack_from('<I', data, opt + 56)
    while pos < end:
        page, size = struct.unpack_from('<II', data, pos)
        assert size >= 8 and pos + size <= end
        for cursor in range(pos + 8, pos + size, 2):
            entry, = struct.unpack_from('<H', data, cursor)
            kind, displacement = entry >> 12, entry & 0xfff
            if not kind:
                continue
            assert kind == 10, 'Only PE DIR64 relocation is supported'
            target = file_offset(page + displacement)
            value, = struct.unpack_from('<Q', data, target)
            assert value < image_size
            struct.pack_into('<Q', data, target, value + BASE)
            count += 1
        pos += size
    assert pos == end and count
    struct.pack_into('<Q', data, opt + 24, BASE)
    return bytes(data), count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, default=shutil.which('zig'))
    parser.add_argument('--output', type=Path, default=ROOT / 'out/loader')
    args = parser.parse_args()
    if args.zig is None:
        parser.error('Provide --zig with the path to Zig 0.14.1')
    zig = str(args.zig.resolve(strict=True))
    version = subprocess.check_output([zig, 'version'], text=True).strip()
    if version != '0.14.1':
        parser.error('This snapshot was built with Zig 0.14.1; found ' + version)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sources = ROOT / 'loader-r16h'
    flags = ['-target', 'aarch64-windows-gnu', '-Xclang', '-target-feature', '-Xclang', '+el3',
             '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-stack-arg-probe',
             '-fshort-wchar', '-mgeneral-regs-only', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter',
             '-DXNU_TARGET_DADA=1', '-I' + str(ROOT / 'include'), '-I' + str(sources)]
    commands, objects = [], []
    def run(command):
        commands.append(command)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with (output / 'build.log').open('ab') as log:
            log.write(('\n' + ' '.join(command) + '\n').encode())
            log.write(result.stdout)
        if result.returncode:
            raise RuntimeError(result.stdout.decode(errors='replace'))
    for name in UNITS:
        obj = output / (name + '.obj')
        run([zig, 'cc', *flags, '-c', str(sources / (name + '.c')), '-o', str(obj)])
        objects.append(str(obj))
    loader = output / 'loader-dada.efi'
    run([zig, 'build-exe', *objects, '-target', 'aarch64-uefi-msvc', '-O', 'ReleaseSmall',
         '-fentry=efi_main', '--subsystem', 'efi_application', '--image-base', hex(BASE),
         '-fno-stack-check', '-fno-stack-protector', '-femit-bin=' + str(loader)])
    data, count = rebase(loader.read_bytes())
    assert b'dada-touch-spi-v16-mynewt-r16h' in data
    loader.write_bytes(data)
    report = {'created_utc': datetime.now(timezone.utc).isoformat(), 'zig_version': version,
              'loader_identity': 'dada-touch-spi-v16-mynewt-r16h', 'image_base': hex(BASE),
              'relocations_applied': count, 'sha256': hashlib.sha256(data).hexdigest(),
              'source_snapshot': str(sources), 'commands': commands,
              'repacked_img': False, 'device_commands_sent': False}
    (output / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(loader)
    print(report['sha256'])


if __name__ == '__main__':
    main()
