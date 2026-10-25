#!/usr/bin/env python3
"""Compile GPU objects and relink a configured dada kernel archive, in isolation.

The baseline archive contains all existing XNU objects, including the device's
CPU/GIC/touch adaptations. GPU objects and the real pthread library are built
in isolation; an optional GIC workaround replaces one extra object. No baseline
files are modified. Firmware stays in local output and is never added to Git.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import struct


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def compile_input_gpu_hook(repo, xnu, obj, env):
    info = json.loads((repo / 'tools/dada/patches/dada-input-baseline.json').read_text())
    source = xnu / info['source']
    if sha(source) != info['baseline_sha256']:
        raise RuntimeError('Input source differs from the diagnosed baseline; review before applying hook')
    copied = obj / 'input-source' / info['source']
    copied.parent.mkdir(parents=True)
    shutil.copyfile(source, copied)
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i',
                    str(repo / 'tools/dada/patches/dada-input-gpu-hook.patch')],
                   cwd=obj / 'input-source', check=True)
    # Use this exact BSD object's rule; keep its existing dada touch integration.
    cwd = xnu / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT'
    common = [f'SRCROOT={xnu}', f'OBJROOT={xnu}/BUILD/obj', f'SYMROOT={xnu}/BUILD/sym',
              f'DSTROOT={xnu}/BUILD/dst', 'SDKROOT=iphoneos', 'ARCH_CONFIGS=ARM64',
              'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=DEVELOPMENT', 'CC=xnu-clang',
              'CXX=xnu-clang++', 'CURRENT_MACHINE_CONFIG=BCM2837',
              'CURRENT_MACHINE_CONFIG_LC=bcm2837', 'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64']
    dry = subprocess.run(['make', '-n', '-B', 'redwood_input.o', *common], cwd=cwd,
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = [line for line in dry.stdout.splitlines()
             if 'xnu-clang' in line and ' -c ' in line and '/redwood_input.c' in line]
    if not lines:
        raise RuntimeError('No input compiler command: ' + dry.stdout[-4000:])
    base, clean, i = shlex.split(lines[-1]), [], 0
    while i < len(base):
        flag = base[i]
        if flag in ('-MF', '-MT', '-MQ', '-o'):
            i += 2
            continue
        if flag not in ('-MD', '-MMD', '-MP') and not flag.endswith('.c'):
            clean.append(flag)
        i += 1
    cmd = clean[:1] + ['-DXNU_TARGET_DADA=1', '-DBCM2837', '-DARM64_BOARD_CONFIG_BCM2837=1'] + clean[1:]
    cmd += [f'-I{xnu}/bsd', str(copied), '-o', str(obj / 'redwood_input.o')]
    (obj / 'redwood_input.command').write_text(shlex.join(cmd) + '\n')
    result = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (obj / 'redwood_input.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stdout[-8000:])
    return {'gpu_call_before_dada_return': True,
            'baseline_source_sha256': info['baseline_sha256'],
            'source_tree_unchanged': sha(source) == info['baseline_sha256']}


def compile_gic_without_snapshot(repo, xnu, obj, env):
    """Patch/compile one copied baseline source; never edit the existing tree."""
    info = json.loads((repo / 'tools/dada/patches/dada-gic-baseline.json').read_text())
    source = xnu / info['source']
    if sha(source) != info['baseline_sha256']:
        raise RuntimeError('GIC source differs from the diagnosed baseline; review before applying patch')
    copied = obj / 'gic-source' / info['source']
    copied.parent.mkdir(parents=True)
    shutil.copyfile(source, copied)
    patch = repo / 'tools/dada/patches/dada-gic-no-snapshot.patch'
    subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)],
                   cwd=obj / 'gic-source', check=True)
    cwd = xnu / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT'
    common = [f'SRCROOT={xnu}', f'OBJROOT={xnu}/BUILD/obj', f'SYMROOT={xnu}/BUILD/sym',
              f'DSTROOT={xnu}/BUILD/dst', 'SDKROOT=iphoneos', 'ARCH_CONFIGS=ARM64',
              'MACHINE_CONFIGS=BCM2837', 'KERNEL_CONFIGS=DEVELOPMENT', 'CC=xnu-clang',
              'CXX=xnu-clang++', 'CURRENT_MACHINE_CONFIG=BCM2837',
              'CURRENT_MACHINE_CONFIG_LC=bcm2837', 'ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64']
    dry = subprocess.run(['make', '-n', '-B', 'gic_v3_irq.o', *common], cwd=cwd,
                         env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = [line for line in dry.stdout.splitlines()
             if 'xnu-clang' in line and ' -c ' in line and '/gic_v3_irq.c' in line]
    if not lines:
        raise RuntimeError('No baseline GIC compile command: ' + dry.stdout[-4000:])
    base, clean, i = shlex.split(lines[-1]), [], 0
    while i < len(base):
        flag = base[i]
        if flag in ('-MF', '-MT', '-MQ', '-o'):
            i += 2
            continue
        if flag not in ('-MD', '-MMD', '-MP') and not flag.endswith('.c'):
            clean.append(flag)
        i += 1
    cmd = clean[:1] + ['-DXNU_TARGET_DADA=1', '-DBCM2837', '-DARM64_BOARD_CONFIG_BCM2837=1',
                      '-DDADA_GIC_NO_FIRMWARE_SNAPSHOT=1'] + clean[1:]
    cmd += [f'-I{xnu}/osfmk', '-idirafter', str(xnu / 'osfmk/libsa')]
    cmd += [str(copied), '-o', str(obj / 'gic_v3_irq.o')]
    (obj / 'gic_v3_irq.command').write_text(shlex.join(cmd) + '\n')
    result = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    (obj / 'gic_v3_irq.log').write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stdout[-8000:])
    # Verify the guarded save/restore code is absent from the compiler input.
    pre = [flag for flag in cmd if flag != '-c']
    at = pre.index('-o')
    pre[at:at+2] = ['-E']
    text = subprocess.check_output(pre, cwd=cwd, env=env, text=True)
    if 'gic_save_firmware_state' in text or 'gic_restore_firmware_state' in text:
        raise RuntimeError('Faulting firmware snapshot remains in GIC compiler input')
    return {'baseline_source_sha256': info['baseline_sha256'],
            'firmware_snapshot_disabled': True, 'source_tree_unchanged': sha(source) == info['baseline_sha256']}


def check_kernel_layout(kernel, baseline):
    def segments(path):
        data = path.read_bytes()
        magic, cpu, _, kind, count, cmdbytes, _, _ = struct.unpack_from('<8I', data)
        if magic != 0xfeedfacf or cpu != 0x100000c or kind != 2:
            raise RuntimeError('Expected ARM64 Mach-O kernel executable')
        at = 32
        result = {}
        entry = 0
        for _ in range(count):
            cmd, size = struct.unpack_from('<II', data, at)
            if size < 8 or at + size > 32 + cmdbytes:
                raise RuntimeError('Invalid load command')
            if cmd == 0x19:
                name = data[at+8:at+24].rstrip(b'\0').decode()
                va, vmbytes, off, filebytes = struct.unpack_from('<4Q', data, at+24)
                if off + filebytes > len(data) or filebytes > vmbytes:
                    raise RuntimeError('Invalid segment bounds')
                result[name] = vmbytes
            elif cmd == 5:
                entry = struct.unpack_from('<Q', data, at+16+32*8)[0]
            at += size
        if at != 32 + cmdbytes or not entry:
            raise RuntimeError('Kernel entry/load commands are missing')
        for marker in (b'XNULDRST', b'XNULIVE!', b'XNULIVE2'):
            if data.find(marker) < 0:
                raise RuntimeError(f'Missing loader ABI marker: {marker!r}')
        return result
    expected, actual = segments(baseline), segments(kernel)
    if set(expected) != set(actual):
        raise RuntimeError(f'Segment names differ from baseline: {set(expected) ^ set(actual)}')
    for name in ('__LASTDATA_CONST', '__LAST'):
        if bool(expected.get(name)) != bool(actual.get(name)):
            raise RuntimeError(f'{name} presence/emptiness differs from baseline')
    return {'baseline_segment_names_preserved': True,
            'last_data_const_separate': True, 'loader_abi_markers_present': True}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--xnu', type=Path, required=True)
    ap.add_argument('--env', type=Path, required=True)
    ap.add_argument('--firmware', type=Path, required=True)
    ap.add_argument('--mesa', type=Path, required=True)
    ap.add_argument('--pthread', type=Path, required=True, help='pinned Apple libpthread source; SDK stubs are not usable')
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--gic-no-firmware-snapshot', action='store_true',
                    help='apply the diagnosed r2 GIC workaround to an isolated baseline source copy')
    ap.add_argument('--dada-input-gpu-hook', action='store_true',
                    help='call the GPU diagnostic before the local baseline dada input early return')
    a = ap.parse_args()
    repo = Path(__file__).resolve().parents[2]
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    obj = out / 'objects'
    # Same compiler flags and SDK as the baseline, with isolated output.
    subprocess.run(['python3', str(repo / 'tools/dada/check-gpu-objects.py'),
        '--xnu', str(a.xnu), '--env', str(a.env), '--out', str(obj), '--mesa', str(a.mesa)], check=True)
    embedded = obj / 'sys/dada_gpu_embedded.h'
    embedded.parent.mkdir()
    rows = []
    text = ['/* Generated local firmware; excluded from source control. */']
    for i, name in enumerate(['gen80000_gmu.bin', 'gen80000_sqe.fw', 'gen80000_aqe.fw']):
        p = a.firmware / name
        data = p.read_bytes()
        text.append(f'static const uint8_t dada_fw_{i}[] __attribute__((aligned(64))) = {{')
        text.extend(','.join(f'0x{v:02x}' for v in data[n:n+32])+',' for n in range(0,len(data),32))
        text.append('};')
        rows.append({'name': name, 'bytes': len(data), 'sha256': sha(p)})
    text.append('static const uint8_t *dada_embedded_file(const char *dir, const char *name, uint64_t *size) {')
    text.append('if (strcmp(dir,"fw")) return NULL;')
    for i, row in enumerate(rows):
        text.append(f'if (!strcmp(name,"{row["name"]}")) {{ *size=sizeof(dada_fw_{i}); return dada_fw_{i}; }}')
    text.extend(['return NULL;', '}'])
    embedded.write_text('\n'.join(text)+'\n')
    env_bytes = subprocess.check_output(['bash','-c','source "$1"; env -0','gpu-build',str(a.env)])
    env = dict(item.split('=',1) for item in env_bytes.decode().split('\0') if '=' in item)
    env['XNU_TARGET'] = 'dada'
    spec = importlib.util.spec_from_file_location('pthread_build', repo / 'tools/dada/build-pthread-kext.py')
    pthread_build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pthread_build)
    pthread_library, pthread_report = pthread_build.build(repo, a.pthread, a.xnu, obj / 'pthread', env)
    cmd = shlex.split((obj / 'dada_adreno.command').read_text())
    cmd[1:1] = [f'-I{obj}', '-DDADA_GPU_EMBEDDED_FIRMWARE=1']
    cwd = a.xnu / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT'
    subprocess.run(cmd,cwd=cwd,env=env,check=True)
    gic = compile_gic_without_snapshot(repo, a.xnu, obj, env) if a.gic_no_firmware_snapshot else None
    input_hook = compile_input_gpu_hook(repo, a.xnu, obj, env) if a.dada_input_gpu_hook else None
    baseline = a.xnu / 'BUILD/obj/DEVELOPMENT_ARM64_BCM2837'
    original = baseline / 'mach.development.bcm2837.link'
    archive = out / 'kernel-objects.a'
    shutil.copyfile(original/'mach.development.bcm2837.a',archive)
    shutil.copyfile(obj/'RedwoodGPU.o',obj/'RedwoodGPU.cpo')
    replacements=[obj/name for name in ['redwood_adreno.o','RedwoodGPU.cpo','dada_adreno.o','dada_gpu_fw.o','dada_gpu_va.o']]
    if gic:
        replacements.append(obj / 'gic_v3_irq.o')
    if input_hook:
        replacements.append(obj / 'redwood_input.o')
    subprocess.run(['llvm-ar','r',str(archive),*map(str,replacements)],check=True)
    flags=shlex.split((original/'mach.development.bcm2837.linkarguments').read_text())
    # Exported linkarguments include a kernel-cache packaging transformation.
    # This loader consumes a raw kernel, matching the baseline .unstripped file.
    flags = [flag for flag in flags if flag != '-Wl,-rename_segment,__LASTDATA_CONST,__LAST']
    flags+=['-Wl,-alias_list,'+str(original/'mach.development.bcm2837.alias.exp'),
        '-Wl,-sectcreate,__LINKINFO,__symbolsets,'+str(baseline/'symbolsets.plist'),
        '-Wl,-segprot,__LINKINFO,r--,r--', '-Wl,-force_load,'+str(archive)]
    flags += [flag for flag in shlex.split((original/'mach.development.bcm2837.linkarchives').read_text())
              if flag != '-lpthread_kext']
    flags += ['-Wl,-force_load,' + str(pthread_library)]
    kernel=out/'mach.development.dada-gpu-diagnostic.unstripped'
    cmd=['xnu-clang',*flags,'-o',str(kernel)]
    (out/'link.command').write_text(shlex.join(cmd)+'\n')
    result=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (out/'link.log').write_text(result.stdout)
    if result.returncode: raise RuntimeError(result.stdout[-12000:])
    final=out/'mach.development.dada-gpu-diagnostic'
    subprocess.run(['strip','-S','-o',str(final),str(kernel)],env=env,check=True)
    layout = check_kernel_layout(final, baseline/'mach.development.bcm2837')
    symbols=subprocess.check_output(['llvm-nm',str(kernel)],text=True)
    if '_dada_adreno_init' not in symbols or '_dada_gmu_plan_firmware' not in symbols:
        raise RuntimeError('New backend is absent from linked kernel')
    disassembly = subprocess.check_output(['llvm-objdump', '-d', '--no-show-raw-insn',
                                           '--disassemble-symbols=_pthread_start', str(kernel)], text=True)
    (out / 'pthread-start.disassembly.txt').write_text(disassembly)
    if '<_pthread_kext_register>' not in disassembly:
        raise RuntimeError('Linked pthread_start does not call pthread_kext_register')
    if input_hook:
        disassembly = subprocess.check_output(['llvm-objdump', '-d', '--no-show-raw-insn',
                                               '--disassemble-symbols=_redwood_input_init', str(kernel)], text=True)
        (out / 'input-init.disassembly.txt').write_text(disassembly)
        if '<_redwood_adreno_init>' not in disassembly:
            raise RuntimeError('Linked dada input init does not call the GPU entry')
    if any(line.split()[-2:-1]==['U'] for line in symbols.splitlines()):
        raise RuntimeError('Linked kernel contains undefined symbols')
    (out/'build.json').write_text(json.dumps({'method':'recompile GPU objects and relink full baseline XNU archive',
        'baseline_archive_sha256':sha(original/'mach.development.bcm2837.a'),
        'kernel': {'path':final.name,'bytes':final.stat().st_size,'sha256':sha(final)},
        'embedded_firmware':rows,'zap_present':False,'gpu_backend_linked':True,
        'hardware_tested':False,'all_baseline_sources_recompiled':False,
        'layout_verification':layout, 'gic_workaround':gic, 'pthread':pthread_report,
        'dada_input_gpu_hook':input_hook},indent=2)+'\n')
    print(f'LINKED: {final} ({final.stat().st_size} bytes)',flush=True)


if __name__=='__main__': main()
