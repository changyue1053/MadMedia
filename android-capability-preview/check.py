"""Cross-link isolated native capability fixtures with a preinstalled NDK only."""
import argparse
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

NDK_REVISION = '28.2.13676358'
OUTPUT_LIMIT = 262144
SOURCES = ('apple-capability-preview/lease.cpp',
           'apple-capability-preview/lease_test.cpp','core/platform/entropy.h',
           'ffi/platform/entropy_android.cpp','android-capability-preview/entropy_probe.cpp')

class Rejected(Exception):
    pass

def stage_sources(root, folder):
    names = {Path(item).name for item in SOURCES}
    if len(names) != len(SOURCES):
        raise Rejected()
    for item in SOURCES:
        shutil.copyfile(root/item,folder/Path(item).name)
    for name in names:
        for include in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"',(folder/name).read_text(),re.MULTILINE):
            if include not in names:
                raise Rejected()

def run(args, cwd):
    with tempfile.TemporaryFile() as output:
        result = subprocess.run([str(arg) for arg in args],cwd=cwd,stdin=subprocess.DEVNULL,
                                stdout=output,stderr=subprocess.STDOUT,check=False,timeout=90)
        output.seek(0)
        data = output.read(OUTPUT_LIMIT+1)
        if len(data) > OUTPUT_LIMIT:
            raise Rejected()
    return result.returncode,data.decode('utf8',errors='replace')

def api24_refused(status, diagnostics):
    if status <= 0:
        return False
    # Only a real getrandom API/symbol failure counts. Do not accept a compiler
    # launch error, arbitrary missing include, or unrelated source failure.
    text = diagnostics.lower()
    return any(re.search(pattern,text) for pattern in (
        r"(?:undeclared (?:identifier|function)|undefined symbol:)\s*[\"']?getrandom\b",
        r"getrandom[\"']? is (?:unavailable|only available)[^\n]*android\s+28",
        r"undefined reference to [`'\"]getrandom\b",
    ))

def android_elf(text):
    return (re.search(r'^\s*Class:\s*ELF64\s*$',text,re.MULTILINE) is not None and
            re.search(r'^\s*Machine:\s*AArch64\s*$',text,re.MULTILINE) is not None and
            re.search(r'^\s*Type:\s*DYN\b',text,re.MULTILINE) is not None and
            '[Requesting program interpreter: /system/bin/linker64]' in text)

def has_getrandom_undef(text):
    return re.search(r'^.*\bUND\s+getrandom(?:@+\S+)?(?:\s|$)',text,re.MULTILINE) is not None

def compiler_args(clang, api):
    if api not in (24,28):
        raise Rejected()
    return [clang,f'--target=aarch64-linux-android{api}','-std=c++17',
            '-Wall','-Wextra','-Werror','-Werror=unguarded-availability','-static-libstdc++',
            '-fPIE','-pie','-Wl,--no-undefined']

def positive(args, cwd):
    status,data = run(args,cwd)
    if status != 0:
        raise Rejected()
    return data

def check(ndk):
    if platform.system()!='Linux' or platform.machine()!='x86_64':
        raise Rejected()
    if re.findall(r'^\s*Pkg\.Revision\s*=\s*(\S+)\s*$',(ndk/'source.properties').read_text(),re.MULTILINE)!=[NDK_REVISION]:
        raise Rejected()
    tools = ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    clang, reader = tools/'clang++', tools/'llvm-readelf'
    if not clang.is_file() or not reader.is_file():
        raise Rejected()
    root = Path(__file__).resolve().parents[1]
    stage = 'stage'
    try:
        with tempfile.TemporaryDirectory(prefix='madmedia-ndk-') as temporary:
            cwd = Path(temporary)
            stage_sources(root,cwd)
            stage = 'lease-api24-link'
            positive(compiler_args(clang,24)+['lease_test.cpp','-o','lease24'],cwd)
            print('PASS: '+stage)
            stage = 'lease-api24-elf'
            if not android_elf(positive([reader,'-h','-lW','lease24'],cwd)):
                raise Rejected()
            stage = 'lease-api24-imports'
            if has_getrandom_undef(positive([reader,'--dyn-syms','-W','lease24'],cwd)):
                raise Rejected()
            print('PASS: API 24 independent advisory lease ELF only')
            stage = 'entropy-api28-link'
            positive(compiler_args(clang,28)+['-DEXPECTED_ANDROID_API=28','entropy_probe.cpp','entropy_android.cpp','-o','entropy28'],cwd)
            print('PASS: '+stage)
            stage = 'entropy-api28-elf'
            if not android_elf(positive([reader,'-h','-lW','entropy28'],cwd)):
                raise Rejected()
            stage = 'entropy-api28-imports'
            if not has_getrandom_undef(positive([reader,'--dyn-syms','-W','entropy28'],cwd)):
                raise Rejected()
            print('PASS: API 28 unchanged provider ELF imports bionic getrandom')
            stage = 'entropy-api24-refused'
            # Same probe and provider; only the selected target API differs.
            status,diagnostics = run(compiler_args(clang,24)+
                ['-DEXPECTED_ANDROID_API=24','entropy_probe.cpp','entropy_android.cpp','-o','entropy24'],cwd)
            if not api24_refused(status,diagnostics):
                raise Rejected()
            print('PASS: API 24 provider explicitly rejected; no fallback')
    except Exception:
        print('FAIL: '+stage)
        raise

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--ndk',required=True,type=Path)
    args=parser.parse_args()
    try:
        check(args.ndk.resolve())
    except Exception:
        print('FAIL: Android capability validation; no runtime result')
        return 1
    print('PASS: cross-link and API floor checks only; no device execution')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
