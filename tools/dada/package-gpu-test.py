#!/usr/bin/env python3
"""Package a local diagnostic build with the preserved dada release assets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True, help='local release folder containing package.json')
    ap.add_argument('--build', type=Path, required=True, help='isolated build-gpu-test.py output')
    ap.add_argument('--out', type=Path, required=True, help='new output directory')
    a = ap.parse_args()
    source, build, out = a.source.resolve(), a.build.resolve(), a.out.resolve()
    manifest = json.loads((source / 'package.json').read_text(encoding='utf-8-sig'))
    report = json.loads((build / 'build.json').read_text())
    if not report.get('dada_touch_bridge', {}).get('shared_abi_headers_match'):
        raise RuntimeError('This package requires the verified mode-3 touch bridge build')
    if not report.get('dada_input_gpu_hook', {}).get('gpu_call_before_dada_return'):
        raise RuntimeError('This package requires the dada GPU initialization hook')
    kernel = build / report['kernel']['path']
    with kernel.open('rb') as f:
        if hashlib.file_digest(f, 'sha256').hexdigest() != report['kernel']['sha256']:
            raise RuntimeError('Kernel differs from the build report')
    args = manifest['cmdline'].split()
    if 'bootcpu=1' not in args:
        raise RuntimeError('Touch bridge requires the dada kernel on a separate observer CPU')
    # Replace existing numeric options, so the loader and kernel see one value.
    replaced = {'dada_touch', 'dada_touch_mmio', 'dada_touch_power',
                'dada_touch_reset', 'dada_gpu', 'dada_gpu_pause'}
    args = [v for v in args if v.split('=', 1)[0] not in replaced]
    out.mkdir(parents=True, exist_ok=False)
    for folder in ('firmware', 'uefi', 'tools', 'tests', 'validation'):
        (out / folder).mkdir()
    for name in ('Start-iOS.ps1', 'Start-iOS.cmd', 'Get-Logs.cmd', 'Get-iOS-Logs.bat',
                 'Check-Device.cmd', 'Verify-Files.cmd'):
        shutil.copyfile(source / name, out / name)
    shutil.copytree(source / 'tools/platform-tools', out / 'tools/platform-tools')
    shutil.copyfile(source / 'tests/Test-Launcher.ps1', out / 'tests/Test-Launcher.ps1')
    launcher = out / 'Start-iOS.ps1'
    text = launcher.read_text(encoding='utf-8-sig')
    text = text.replace("Write-Host 'hackios-dada-dev1.0 | iOS 16.5 (20F66) | r16h-wallpaper-r1'",
                        "Write-Host 'dada r6 | GPU step 1 + UEFI touch bridge | touch pending device test'")
    launcher.write_text(text, encoding='utf-8-sig', newline='')
    for name in (manifest['system'], manifest['boot'], manifest['uefi']):
        shutil.copyfile(source / name, out / name)
    kernel_rel = 'firmware/mach.development.dada-gpu-diagnostic'
    shutil.copyfile(kernel, out / kernel_rel)
    shutil.copyfile(build / 'build.json', out / 'build.json')
    modes = [('package.json', 'firmware/ios-live-gpu-read.xnup', 1, 3, 'Start-Touch-Test.cmd'),
             ('package-gmu.json', 'firmware/ios-live-gmu.xnup', 13, 3, 'Start-GMU-Test.cmd'),
             ('package-software.json', 'firmware/ios-live-software.xnup', 0, 3, 'Start-Software-Control.cmd'),
             ('package-no-touch.json', 'firmware/ios-live-no-touch.xnup', 1, 0, 'Start-No-Touch-Control.cmd')]
    hashes = {}
    for config, payload, step, touch, entry in modes:
        m = dict(manifest)
        m.update(package='dada-gpu-diagnostic', revision='gpu-touch-test-r6', kernel=kernel_rel, payload=payload,
                 cmdline=' '.join(args + [f'dada_touch={touch}', 'dada_touch_mmio=0',
                                        'dada_touch_power=0', 'dada_touch_reset=0',
                                        'dada_gpu_pause=0', f'dada_gpu={step}']))
        body = bytearray()
        for name, data in [('kernel', kernel.read_bytes()), ('ramdisk', (out / m['boot']).read_bytes()),
                           ('cmdline', m['cmdline'].encode() + b'\0'), ('checkpoint', b'0\0')]:
            body += struct.pack('<16sQ', name.encode(), len(data)) + data + b'\0' * (-len(data) % 16)
        body += b'\0' * 24
        if len(body) + 8 >= 0x3fbf000:
            raise RuntimeError('XNUP payload exceeds the loader stage limit')
        (out / payload).write_bytes(b'XNUP' + struct.pack('<I', len(body)) + body)
        paths = [m['system'], m['boot'], m['kernel'], m['payload'], m['uefi']]
        paths += [r['path'] for r in manifest['files'] if r['path'].startswith('tools/')]
        m['files'] = []
        for rel in paths:
            p = out / rel
            if rel not in hashes:
                with p.open('rb') as f:
                    hashes[rel] = hashlib.file_digest(f, 'sha256').hexdigest()
            m['files'].append({'path': rel, 'bytes': p.stat().st_size, 'sha256': hashes[rel]})
        (out / config).write_text(json.dumps(m, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        (out / entry).write_text('@echo off\nchcp 65001 >nul\ncd /d "%~dp0"\n'
                                'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass '
                                '-File "%~dp0Start-iOS.ps1" -Manifest ' + config +
                                ' -ObserveSeconds 180 -ShowLog\npause\n', encoding='utf-8', newline='\r\n')
    shutil.copyfile(out / 'Start-Touch-Test.cmd', out / 'Start-GPU-Test.cmd')
    (out / 'README.md').write_text('''# Xiaomi 15 dada GPU / 触摸测试版 r6

手机手动进入原厂 FASTBOOT 后，双击 `Start-Touch-Test.cmd`。
工具临时启动原 r16h UEFI，上传原 System APFS 与新内核，只使用 RAM。
出现界面后，保持 USB 连接，连续点击、上下滑动约 10 秒。
采集约 180 秒，日志保存在 `logs/日期时间/`，主要文件为 `kernel-live.log`。
采集到期只结束电脑端记录，不会停止或重启手机。`Get-Logs.cmd` 可以再次采集。

r5 的触摸参数为 `dada_touch=4`，本机 XNU 源码仅支持模式 1～3，因此直接退出。
r6 改用原 r16h UEFI 与内核共同支持的 `dada_touch=3` 共享事件桥，记录初始化、
状态、错误、帧计数及前 32 个触摸事件。READY 的状态值为 2。
模式匹配、共享 ABI、编译与链接已经检查；触摸芯片初始化和界面响应仍需本次实机验证。
如果仍无触摸，请保留日志；初始化失败与收到事件但界面无响应可据此区分。

- `Start-Touch-Test.cmd` / `Start-GPU-Test.cmd`：触摸模式 3，GPU step 1 外围只读诊断。
- `Start-No-Touch-Control.cmd`：同一内核，触摸关闭、GPU step 1，用于比较启动问题。
- `Start-Software-Control.cmd`：触摸模式 3，GPU 关闭，用于比较 GPU 影响。
- `Start-GMU-Test.cmd`：触摸模式 3，GPU step 13，测试时钟、SMMU、GMU/HFI；尚未实机验证。

切换入口前需手动返回原厂 FASTBOOT；已有 iOS 运行时，脚本只采集日志，不切换参数。
测试后手动重启回 Android。r5 可开机的包保留，便于回退比较。

GPU step 1 已在 r5 实机完成，随后 BSD 初始化完成，SpringBoard 输出日志、心跳递增。
r6 保留 GPU 入口、GIC 与真实 pthread 修复；仅额外重编译触摸日志对象并调整启动参数。
没有重编译全部 XNU 源码，原 UEFI、System、Boot 资产不变。构建记录见 `build.json`。
没有 ZAP 固件，尚未提供 GPU 硬件加速或 Metal。
固件只嵌入本地测试内核，不上传到源码仓库。
''', encoding='utf-8')
    print(out)


if __name__ == '__main__':
    main()
