#!/usr/bin/env python3
"""Build the real Apple libpthread kernel library in an isolated output folder."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


PIN = 'a6c70ca0e544347a0377babf748f831577ec24f9'  # libpthread-514.100.2


def build(repo, source, xnu, out, env):
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise RuntimeError('Expected libpthread-514.100.2 at ' + PIN)
    if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--', 'kern'], text=True).strip():
        raise RuntimeError('libpthread kernel sources have local changes; review before building')
    out.mkdir(parents=True, exist_ok=False)
    headers = xnu / 'BUILD/obj/EXPORT_HDRS'
    includes = [repo / 'xnu-deps/build/pthread/inc', source,
                *[headers / group for group in ('osfmk', 'bsd', 'libkern', 'pexpert', 'iokit', 'security')],
                xnu / 'EXTERNAL_HEADERS', source / 'include', source / 'private']
    objects, rows = [], []
    for name in ('kern_init', 'kern_support', 'kern_synch'):
        src, obj = source / 'kern' / (name + '.c'), out / ('pthread_' + name + '.o')
        cmd = ['xnu-clang', '-arch', 'arm64', '-c', '-Os', '-mkernel', '-nostdinc',
               '-fno-builtin', '-fno-common', '-Wno-int-conversion', '-DKERNEL=1',
               '-DKERNEL_PRIVATE=1', '-DPRIVATE=1', '-D__PTHREAD_EXPOSE_INTERNALS__=1',
               '-Dpthread_kern=pthread_kext_kern', '-Dcurrent_uthread=pthread_kext_current_uthread',
               '-D__DARWIN_64_BIT_INO_T=1', '-DARM64_BOARD_CONFIG_BCM2837']
        cmd += ['-I' + str(p) for p in includes] + ['-o', str(obj), str(src)]
        result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (out / (name + '.command')).write_text(shlex.join(cmd) + '\n')
        (out / (name + '.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(result.stdout[-8000:])
        objects.append(obj)
        rows.append({'source': str(src.relative_to(source)),
                     'sha256': hashlib.sha256(src.read_bytes()).hexdigest()})
        print('pthread ' + name + ': PASS', flush=True)
    # A successful no-op pthread_start is not a usable kernel extension.
    reloc = subprocess.check_output(['llvm-objdump', '-r', str(objects[0])], text=True)
    symbols = subprocess.check_output(['llvm-nm', str(objects[0])], text=True)
    if '_pthread_kext_register' not in reloc or '_pthread_internal_functions' not in symbols:
        raise RuntimeError('pthread start object does not register a real function table')
    library = out / 'libpthread_kext.a'
    subprocess.run(['llvm-ar', 'rcs', str(library), *map(str, objects)], check=True)
    report = {'source_revision': revision, 'source_files': rows,
              'function_table_and_registration_verified': True,
              'library_sha256': hashlib.sha256(library.read_bytes()).hexdigest()}
    (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    return library, report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--xnu', type=Path, required=True)
    ap.add_argument('--env', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    raw = subprocess.check_output(['bash', '-c', 'source "$1"; env -0', 'pthread-build', str(a.env)])
    env = dict(x.split('=', 1) for x in raw.decode().split('\0') if '=' in x)
    print(build(Path(__file__).resolve().parents[2], a.source, a.xnu, a.out, env)[0])


if __name__ == '__main__':
    main()
