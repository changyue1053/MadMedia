"""Synthetic package/map/ELF tests only; no target build or link is run."""
from contextlib import redirect_stdout
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from verify import (ARCHIVES, BASE, Reason, Rejected, Stage, SYMBOLS, diagnostic,
                    load, package, verify_elf, verify_map, verify_relocated)

def denied(action):
    try:
        action()
    except (Rejected,ValueError):
        return
    raise AssertionError('invalid synthetic input accepted')

def synthetic_layout(root):
    producer=root/'producers'
    curl=producer/'curl'
    sqlite=producer/'sqlite'
    for relative in ('curl/curl.h','mbedtls/ssl.h','psa/crypto.h','tf-psa-crypto/crypto.h'):
        dst=curl/'include'/relative
        dst.parent.mkdir(exist_ok=True,parents=True)
        dst.write_text('// synthetic, not a release header\n')
    (curl/'lib').mkdir()
    sqlite.mkdir()
    (sqlite/'sqlite3.h').write_text('// synthetic\n')
    for name in ARCHIVES:
        parent=sqlite if name==ARCHIVES[-1] else curl/'lib'
        (parent/name).write_bytes(b'!<arch>\nsynthetic nonlibrary fixture')
    return producer,curl,sqlite

def test_package():
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory)
        producer,curl,sqlite=synthetic_layout(root)
        staged=root/'original'
        manifest=package(curl,sqlite,staged,producer)
        relocated=root/'relocated dependency'
        staged.rename(relocated)
        verify_relocated(relocated,manifest,producer)
        extra=relocated/'lib/foreign.a'
        extra.write_bytes(b'!<arch>\nforeign')
        denied(lambda:verify_relocated(relocated,manifest,producer))
        extra.unlink()
        (relocated/'empty-foreign').mkdir()
        denied(lambda:verify_relocated(relocated,manifest,producer))
        (relocated/'empty-foreign').rmdir()
        (relocated/'lib'/ARCHIVES[0]).write_bytes(b'!<arch>\nmutated')
        denied(lambda:verify_relocated(relocated,manifest,producer))
        sqlite_header=sqlite/'sqlite3.h'
        sqlite_header.write_text(f'#include "{producer}/private.h"')
        denied(lambda:package(curl,sqlite,root/'absolute',producer))
        sqlite_header.unlink()
        sqlite_header.symlink_to(curl/'include/curl/curl.h')
        denied(lambda:package(curl,sqlite,root/'link',producer))
        sqlite_header.unlink()
        sqlite_header.write_text('// synthetic')
        (sqlite/ARCHIVES[-1]).write_bytes(b'!<thin>\nsynthetic thin archive')
        denied(lambda:package(curl,sqlite,root/'thin',producer))

def test_map():
    paths=[f'../relocated dependency/lib/{key}' for key in ARCHIVES]
    sdk=Path('/opt/testing-ndk')
    def row(path):
        return f'  1234 1234 20 4 {path}(sample.o):(.text)\n'
    text=''.join(row(path) for path in paths)
    text+=row(str(sdk/'toolchains/llvm/prebuilt/linux-x86_64/lib/clang/19/lib/linux/libclang_rt.builtins-aarch64-android.a'))
    verify_map(text,paths,sdk)
    for bad in (text.replace(row(paths[0]),''),text+row('/host/libcurl.a'),
                text+row('/host/libssl.a'),text+row('/host/libmystery.a'),
                text+row('/host/libunwind.a')):
        denied(lambda:verify_map(bad,paths,sdk))

def test_elf():
    elf=(' Class: ELF64\n Type: DYN (PIE)\n Machine: AArch64\n'
         ' [Requesting program interpreter: /system/bin/linker64]\n'
         ' (NEEDED) Shared library: [libc.so]\n')
    segments=(' LOAD 0x000000 0x000000 0x000000 0x1000 0x1000 R 0x4000\n'
              ' LOAD 0x000200 0x008200 0x008200 0x2000 0x2000 R E 0x8000\n')
    symbols='\n'.join('100 T '+name for name in SYMBOLS)
    verify_elf(elf+segments,symbols)
    for bad in (elf,elf+segments.replace('0x4000','0x1000'),
                elf+segments.replace('0x8000','0x9000'),
                elf+segments.replace('0x008200','0x008100'),
                elf+segments+' (NEEDED) Shared library: [libsqlite3.so]\n',
                elf+segments+' (RUNPATH) /unexpected\n'):
        denied(lambda:verify_elf(bad,symbols))
    denied(lambda:verify_elf(elf+segments,symbols.replace(' T ',' U ')))

def test_prefix_remap(curl):
    # Capture real common configure arguments from both calls without build.
    class StopBeforeConfigure(Exception):
        pass
    with tempfile.TemporaryDirectory() as path:
        root=Path(path).resolve()
        producer=root/'producers'
        work=producer/'curl'
        work.mkdir(parents=True)
        ndk=root/'ndk'
        tools=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
        chain=ndk/'build/cmake/android.toolchain.cmake'
        tools.mkdir(parents=True)
        chain.parent.mkdir(parents=True)
        (ndk/'source.properties').write_text('Pkg.Revision = 28.2.13676358\n')
        cmake,ninja=root/'cmake',root/'ninja'
        for path in (cmake,ninja,chain,tools/'clang',tools/'llvm-readelf',tools/'llvm-nm'):
            path.touch()
        cli=SimpleNamespace(ndk=ndk,cmake=cmake,ninja=ninja)
        def captured(combined=None):
            calls=[]
            def trace(args,*unused):
                if '-S' in args:
                    calls.append(args)
                    if len(calls)==2:
                        raise StopBeforeConfigure()
                if '--install' in args:
                    output=work/'target-deps/lib'
                    output.mkdir(parents=True,exist_ok=True)
                    for name in ('mbedtls','mbedx509','tfpsacrypto'):
                        (output/f'lib{name}.a').touch()
            with patch.object(curl,'trace',side_effect=trace), redirect_stdout(io.StringIO()):
                try:
                    curl.build(cli,work,work/'curl-source',work/'mbed-source',
                               combined_producer_root=combined)
                except StopBeforeConfigure:
                    pass
                else:
                    raise AssertionError('configure fixture did not stop')
            assert len(calls)==2
            return calls
        standalone=captured()
        hook=work/'combined-producer-prefix-map.cmake'
        assert not hook.exists()
        combined=captured(producer)
        argument=f'-DCMAKE_PROJECT_INCLUDE:FILEPATH={hook}'
        flag=f'-ffile-prefix-map={producer}/=./'
        assert curl.producer_prefix_flag(producer,work)==flag
        for old,new in zip(standalone,combined):
            assert argument not in old and new.count(argument)==1
            assert [x for x in new if x!=argument]==old
        assert hook.read_text()==('if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)\n'
                                 f'  add_compile_options("$<$<COMPILE_LANGUAGE:C>:{flag}>")\n'
                                 'endif()\n')
        for rejected in (str(producer),Path('producers'),root,work):
            try:
                curl.producer_prefix_flag(rejected,work)
            except curl.Rejected:
                pass
            else:
                raise AssertionError('invalid remap accepted')
        other=root/'bad;flag'/'producers'
        nested=other/'build'
        nested.mkdir(parents=True)
        try:
            curl.producer_prefix_flag(other,nested)
        except curl.Rejected:
            pass
        else:
            raise AssertionError('CMake option injection accepted')

def test_diagnostics():
    def outside_failure():
        # A path-looking value in an exception must never reach stdout.
        raise OSError('/internal/synthetic/private-path')
    out=io.StringIO()
    with redirect_stdout(out):
        try:
            with diagnostic(Stage.CURL_LINK):
                outside_failure()
        except OSError:
            pass
        try:
            with diagnostic(Stage.PACKAGE):
                raise Rejected(Reason.PRODUCER_PATH)
        except Rejected:
            pass
        with diagnostic(Stage.RELOCATE):
            pass
        denied(lambda:diagnostic('/internal/synthetic').__enter__())
    assert out.getvalue().splitlines()==[
        'START: curl-standalone-build-link','FAIL: curl-standalone-build-link',
        'START: artifact-package','FAIL: artifact-package',
        'CATEGORY: artifact-package:artifact-producer-path',
        'START: artifact-relocate','PASS: artifact-relocate']

if __name__=='__main__':
    curl=load('synthetic_curl_guard_import',BASE/'curl_tls/verify.py')
    db=load('synthetic_sqlite_guard_import',BASE/'sqlite/verify.py')
    assert curl.NDK_VERSION==db.NDK_REVISION=='28.2.13676358'
    assert callable(curl.stage_release) and callable(curl.build) and callable(db.verified_sqlite)
    test_package()
    test_map()
    test_elf()
    test_diagnostics()
    test_prefix_remap(curl)
    print('PASS: synthetic artifact relocation, linker map closure and 16 KiB ELF rejection cases')
