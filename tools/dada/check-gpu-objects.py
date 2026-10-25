#!/usr/bin/env python3
"""Compile isolated objects against an existing configured XNU build (Linux/WSL).

No sources or objects in the supplied XNU tree are changed. Its generated headers
and make rules are reused with make -n. This is not a kernel link or device test.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu', type=Path, required=True)
    ap.add_argument('--env', type=Path, required=True, help='existing xnu-env.sh')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--mesa', type=Path, help='Mesa source directory containing include/drm-uapi')
    ap.add_argument('--sdk', default='/opt/theos/sdks/iPhoneOS16.5.sdk')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    xnu, out = args.xnu.resolve(), args.out.resolve()
    if out == xnu or xnu in out.parents:
        ap.error('--out must be outside the XNU tree')
    out.mkdir(parents=True, exist_ok=True)
    env_result = subprocess.run(['bash', '-c', 'source "$1"; env -0',
                                 'gpu-check', str(args.env.resolve())],
                                check=True, stdout=subprocess.PIPE)
    env = dict(item.split('=', 1) for item in env_result.stdout.decode().split('\0') if '=' in item)
    env['XNU_TARGET'] = 'dada'
    obj = xnu / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837'
    common = [f'SRCROOT={xnu}', f'OBJROOT={xnu}/BUILD/obj', f'SYMROOT={xnu}/BUILD/sym',
              f'DSTROOT={xnu}/BUILD/dst', 'SDKROOT=iphoneos', 'ARCH_CONFIGS=ARM64',
              'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=DEVELOPMENT',
              'CC=xnu-clang', 'CXX=xnu-clang++', 'CURRENT_MACHINE_CONFIG=BCM2837',
              'CURRENT_MACHINE_CONFIG_LC=bcm2837', 'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64']
    results = []
    for group, original, sources in [
        ('bsd', 'redwood_adreno', ['dada_adreno', 'dada_gpu_fw', 'dada_gpu_va', 'redwood_adreno']),
        ('iokit', 'RedwoodGPU', ['RedwoodGPU'])
    ]:
        cwd = obj / group / 'DEVELOPMENT'
        suffix = '.cpo' if group == 'iokit' else '.o'
        dry = subprocess.run(['make', '-n', '-B', f'{original}{suffix}', *common], cwd=cwd,
                             env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        lines = [line for line in dry.stdout.splitlines()
                 if 'xnu-clang' in line and ' -c ' in line and f'/{original}.' in line]
        if not lines:
            raise RuntimeError(f'No {original} compiler command:\n{dry.stdout[-4000:]}')
        base = shlex.split(lines[-1])
        clean, i = [], 0
        while i < len(base):
            flag = base[i]
            if flag in ('-MF', '-MT', '-MQ', '-o'):
                i += 2
                continue
            if flag in ('-MD', '-MMD', '-MP') or flag.endswith(('.c', '.cpp')):
                i += 1
                continue
            clean.append(flag)
            i += 1
        for source in sources:
            ext = '.cpp' if group == 'iokit' else '.c'
            folder = 'iokit/Kernel' if group == 'iokit' else 'bsd/dev'
            path = repo / 'xnu/files' / folder / (source + ext)
            output = out / (source + '.o')
            # Leaf makefiles do not inherit the top-level board CPP flag.
            cmd = clean[:1] + [f'-I{repo}/xnu/files/bsd', '-DXNU_TARGET_DADA=1', '-DBCM2837'] + clean[1:]
            cmd += [str(path), '-o', str(output)]
            result = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            (out / (source + '.log')).write_text(result.stdout)
            (out / (source + '.command')).write_text(shlex.join(cmd) + '\n')
            print(f'{source}: {"PASS" if result.returncode == 0 else "FAIL"}', flush=True)
            if result.returncode:
                print(result.stdout[-8000:])
                raise SystemExit(result.returncode)
            results.append({'source': str(path.relative_to(repo)), 'compiled': True,
                            'object_bytes': output.stat().st_size})
    # The same unchanged IOKit ABI is compiled above. Check its backend references.
    defined = set()
    undefined = set()
    for name in ['dada_adreno', 'dada_gpu_fw', 'dada_gpu_va', 'redwood_adreno', 'RedwoodGPU']:
        data = subprocess.check_output(['llvm-nm', str(out / (name + '.o'))], text=True)
        for line in data.splitlines():
            fields = line.split()
            if len(fields) < 2:
                continue
            typ, symbol = fields[-2:]
            if symbol.startswith(('_dada_', '_redwood_gpu_', '_redwood_adreno_', '_RedwoodGPU')):
                (undefined if typ == 'U' else defined).add(symbol)
    missing = sorted(undefined - defined)
    if missing:
        raise RuntimeError(f'Missing GPU interface definitions: {missing}')
    # Compile the Darwin shim separately against the iOS SDK.
    shim = repo / 'tools/dada/mesa/drm-shim/drm_shim.c'
    cmd = ['xnu-clang', '-arch', 'arm64', '-isysroot', args.sdk, '-c', str(shim),
           '-I' + str(shim.parent), '-I' + str(args.mesa / 'include') if args.mesa else '-I.', '-o', str(out / 'drm_shim.o')]
    result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / 'drm_shim.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stdout)
    print('Darwin DRM shim and GPU ABI symbols: PASS')
    (out / 'result.json').write_text(json.dumps({'objects': results,
        'drm_shim_compiled': True, 'gpu_abi_resolved': True,
        'full_kernel_linked': False, 'device_tested': False}, indent=2) + '\n')


if __name__ == '__main__':
    main()
