"""Portable checker tests; no simulated result counts as a cross-link."""
from pathlib import Path
import subprocess
import sys
import tempfile

from check import android_elf, api24_refused, compiler_args, has_getrandom_undef, stage_sources

def main():
    # Representative lexical forms from Clang, linker and llvm-readelf.
    for message in ("error: use of undeclared identifier 'getrandom'",
                    "error: 'getrandom' is unavailable: introduced in Android 28",
                    "error: 'getrandom' is only available on Android 28 or newer",
                    "ld.lld: error: undefined symbol: getrandom"):
        assert api24_refused(1,message) and not api24_refused(0,message)
    for message in ("error: 'sys/random.h' file not found",
                    "error: 'getrandom' is only available on Android 29 or newer",
                    "error: unknown type name 'getrandom'",
                    "warning: getrandom", "undefined symbol: getrandom_seed"):
        assert not api24_refused(1,message)
    elf = '''  Class:  ELF64
  Type: DYN (Position-Independent Executable file)
  Machine: AArch64
  [Requesting program interpreter: /system/bin/linker64]'''
    assert android_elf(elf)
    for left,right in (('ELF64','ELF32'),('AArch64','X86-64'),('DYN','EXEC'),
                       ('/system/bin/linker64','/usr/bin/ld')):
        assert not android_elf(elf.replace(left,right))
    for version in ('','@LIBC_P','@@LIBC_P'):
        assert has_getrandom_undef('  11: 0000 0 FUNC GLOBAL DEFAULT UND getrandom'+version+' (2)')
    assert not has_getrandom_undef('  11: 0000 0 FUNC GLOBAL DEFAULT 12 getrandom')
    assert not has_getrandom_undef('  11: 0000 0 FUNC GLOBAL DEFAULT UND getrandom_seed')
    assert '--target=aarch64-linux-android24' in compiler_args('clang++',24)
    assert '--target=aarch64-linux-android28' in compiler_args('clang++',28)
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        root = Path(__file__).resolve().parents[1]
        stage_sources(root,folder)
        for origin in ('core/platform/entropy.h','ffi/platform/entropy_android.cpp'):
            source = root/origin
            assert (folder/source.name).read_bytes()==source.read_bytes()
        result = subprocess.run([sys.executable,str(Path(__file__).with_name('check.py')),
                                 '--ndk',str(folder/'absent')],capture_output=True,text=True,timeout=15)
        assert result.returncode != 0 and result.stdout=='FAIL: Android capability validation; no runtime result\n'
    print('PASS: Android fixture staging, refusal signatures, ELF/import and missing-NDK checks')

if __name__=='__main__':
    main()
