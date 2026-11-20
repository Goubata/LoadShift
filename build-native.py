#!/usr/bin/env python3
"""Build LoadShift native parts (Zygisk module lib + on-device lab) with clang + the Android NDK sysroot.

Environment:
  ANDROID_NDK_HOME  NDK root (default: $ANDROID_HOME/ndk/<newest>)
  NDK_HOST_TAG      prebuilt host dir name (default: linux-x86_64)
  CLANG_RESOURCE    clang version dir inside the NDK (default: newest in lib/clang)
"""
import os, pathlib, subprocess, sys

root = pathlib.Path(__file__).resolve().parent

def ndk_root():
    if os.environ.get('ANDROID_NDK_HOME'):
        return pathlib.Path(os.environ['ANDROID_NDK_HOME'])
    home = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if not home:
        sys.exit('Set ANDROID_NDK_HOME or ANDROID_HOME')
    ndks = sorted((pathlib.Path(home) / 'ndk').iterdir())
    if not ndks:
        sys.exit('No NDK found under $ANDROID_HOME/ndk')
    return ndks[-1]

ndk = ndk_root() / 'toolchains/llvm/prebuilt' / os.environ.get('NDK_HOST_TAG', 'linux-x86_64')
res = os.environ.get('CLANG_RESOURCE') or sorted((ndk / 'lib/clang').iterdir())[-1].name
out = root / 'build/native'; out.mkdir(parents=True, exist_ok=True)
CC = ['clang++', '--target=aarch64-linux-android33', f'--sysroot={ndk}/sysroot', f'-resource-dir={ndk}/lib/clang/{res}',
      '-fuse-ld=lld', '-rtlib=compiler-rt', '--unwindlib=none', '-std=c++17', '-O2', '-fPIC', '-fno-exceptions',
      '-fno-rtti', '-nostdlib++', '-Wall', '-Wno-unused-function', '-Wl,-z,max-page-size=16384']

def run(c):
    print(' '.join(map(str, c[:3])), '...')
    subprocess.run(list(map(str, c)), check=True)

run(CC + ['-shared', '-fvisibility=hidden', '-Wl,--gc-sections', root / 'native/loadshift.cpp',
          '-llog', '-ldl', '-lEGL', '-lGLESv3', '-o', out / 'libloadshift.so'])
run(CC + ['-fPIE', '-pie', root / 'native/lab.cpp', '-ldl', '-lEGL', '-lGLESv3', '-lvulkan', '-o', out / 'lab'])
print('ok', out)
