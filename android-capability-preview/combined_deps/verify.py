"""Nonshipping Android static dependency relocation/link check; never executes target."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

BASE=Path(__file__).resolve().parents[1]
ARCHIVES=('libcurl.a','libmbedtls.a','libmbedx509.a','libtfpsacrypto.a','libsqlite3-preview.a')
HEADER_DIRS=('curl','mbedtls','psa','tf-psa-crypto')
SYMBOLS=('curl_easy_init','curl_easy_setopt','curl_multi_init','curl_url','mbedtls_ssl_init',
         'sqlite3_open_v2','sqlite3_prepare_v2','sqlite3_wal_checkpoint_v2','sqlite3_sourceid')

class Rejected(Exception):
    pass

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    sys.modules[name]=module
    spec.loader.exec_module(module)
    return module

def artifact(path,source_root,archive):
    if path.is_symlink() or not path.is_file():
        raise Rejected()
    with path.open('rb') as file:
        data=file.read(64*1024**2+1)
    if not data or len(data)>64*1024**2 or os.fsencode(source_root) in data:
        raise Rejected()
    if archive and not data.startswith(b'!<arch>\n'):
        raise Rejected()
    return data

def package(curl_prefix,sqlite_prefix,destination,producer_root):
    if any(path.is_symlink() or not path.is_dir()
           for path in (curl_prefix,curl_prefix/'include',curl_prefix/'lib',sqlite_prefix)):
        raise Rejected()
    headers=destination/'include'
    libraries=destination/'lib'
    headers.mkdir(parents=True)
    libraries.mkdir()
    manifest={}
    def copy(path,target,archive=False):
        data=artifact(path,producer_root,archive)
        with target.open('xb') as file:
            file.write(data)
        manifest[str(target.relative_to(destination))]=hashlib.sha256(data).hexdigest()
    for dirname in HEADER_DIRS:
        origin=curl_prefix/'include'/dirname
        if origin.is_symlink():
            raise Rejected()
        if not origin.is_dir():
            if dirname=='tf-psa-crypto':
                continue
            raise Rejected()
        for src in sorted(origin.rglob('*')):
            if src.is_symlink():
                raise Rejected()
            if src.is_dir():
                continue
            relative=src.relative_to(origin)
            if len(manifest)>=5000 or not re.fullmatch(r'[A-Za-z0-9_./+-]+\.h',relative.as_posix()):
                raise Rejected()
            dst=headers/dirname/relative
            dst.parent.mkdir(parents=True,exist_ok=True)
            copy(src,dst)
    required=('curl/curl.h','mbedtls/ssl.h','psa/crypto.h')
    if any('include/'+key not in manifest for key in required):
        raise Rejected()
    copy(sqlite_prefix/'sqlite3.h',headers/'sqlite3.h')
    for name in ARCHIVES:
        source=sqlite_prefix if name==ARCHIVES[-1] else curl_prefix/'lib'
        copy(source/name,libraries/name,True)
    return manifest

def verify_relocated(sdk,manifest,producer_root):
    observed=set()
    expected_dirs={parent.as_posix() for artifact_name in manifest
                   for parent in Path(artifact_name).parents}
    if sdk.is_symlink() or not sdk.is_dir():
        raise Rejected()
    for path in sdk.rglob('*'):
        if path.is_symlink():
            raise Rejected()
        relative=path.relative_to(sdk).as_posix()
        if path.is_dir():
            if relative not in expected_dirs:
                raise Rejected()
            continue
        if relative not in manifest:
            raise Rejected()
        data=artifact(path,producer_root,relative.startswith('lib/'))
        if hashlib.sha256(data).hexdigest()!=manifest[relative]:
            raise Rejected()
        observed.add(relative)
    if observed!=set(manifest):
        raise Rejected()

def verify_map(report,relative_archives,ndk_root):
    # LLD map input rows have an archive(member):section operand. Require
    # each supplied static archive; the NDK may additionally pull compiler
    # runtime objects, but no alternate copy/name of either dependency family.
    known={Path(path).name:path for path in relative_archives}
    found=set()
    for line in report.splitlines():
        row=re.fullmatch(r'\s*[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+\d+\s+(.+?\.a)\([^)]+\):[^\r\n]+',line)
        if not row:
            continue
        path=row[1]
        name=Path(path).name
        if name in known:
            if path!=known[name]:
                raise Rejected()
            found.add(name)
        else:
            ndk_archive=Path(path)
            ndk_names={'libclang_rt.builtins-aarch64-android.a','libunwind.a','libatomic.a'}
            if (name not in ndk_names or not ndk_archive.is_absolute() or '..' in ndk_archive.parts or
                not ndk_archive.is_relative_to(ndk_root/'toolchains/llvm/prebuilt/linux-x86_64')):
                raise Rejected()
    if found!=set(known):
        raise Rejected()

def verify_elf(report,symbols):
    if (not re.search(r'^\s*Class:\s*ELF64\s*$',report,re.M) or
        not re.search(r'^\s*Type:\s*DYN\b',report,re.M) or
        not re.search(r'^\s*Machine:\s*AArch64\s*$',report,re.M) or
        '[Requesting program interpreter: /system/bin/linker64]' not in report):
        raise Rejected()
    needed=set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',report))
    if ('libc.so' not in needed or not needed<={'libc.so','libm.so','libdl.so'} or
        re.search(r'\((RPATH|RUNPATH)\)',report)):
        raise Rejected()
    for name in SYMBOLS:
        if not re.search(r'\b[Tt]\s+'+name+r'$',symbols,re.M):
            raise Rejected()
    segments=0
    for line in report.splitlines():
        fields=line.split()
        if not fields or fields[0]!='LOAD':
            continue
        if len(fields)<8:
            raise Rejected()
        offset,virtual,*_=map(lambda value:int(value,16),fields[1:6])
        align=int(fields[-1],16)
        if align<16384 or align & (align-1) or offset%align!=virtual%align:
            raise Rejected()
        segments+=1
    if not segments:
        raise Rejected()

def verify(args,workspace):
    curl=load('madmedia_curl_receipts',BASE/'curl_tls/verify.py')
    db=load('madmedia_sqlite_receipts',BASE/'sqlite/verify.py')
    clang,ar,readelf,nm=db.toolchain(args.ndk.resolve())
    producer=workspace/'producers'
    producer.mkdir()
    curl_area=producer/'curl'
    curl_area.mkdir()
    with args.curl.open('rb') as src:
        curl_source=curl.stage_release(src,curl.CURL,curl_area)
    with args.mbedtls.open('rb') as src:
        mbed_source=curl.stage_release(src,curl.MBED,curl_area)
    curl.build(args,curl_area,curl_source,mbed_source)
    with args.sqlite.open('rb') as src:
        sources=db.verified_sqlite(src,db.ZIP_HASH,db.C_HASH)
    sql_area=producer/'sqlite'
    sql_area.mkdir()
    for name,data in sources.items():
        (sql_area/name).write_bytes(data)
    def step(argv,cwd,capture=False):
        return curl.trace(argv,cwd,'/usr/bin:/bin',capture)
    base=[clang,'--target=aarch64-linux-android28','-std=gnu11']
    step(base+['-O2','-fPIC','-DSQLITE_THREADSAFE=1','-DSQLITE_OMIT_LOAD_EXTENSION=1',
               '-c','sqlite3.c','-o','sqlite3.o'],sql_area)
    step([ar,'rcs',ARCHIVES[-1],'sqlite3.o'],sql_area)
    initial=workspace/'staged'
    manifest=package(curl_area/'target-deps',sql_area,initial,producer)
    relocated=workspace/'relocated dependency'
    initial.rename(relocated)
    shutil.rmtree(producer)
    if initial.exists() or producer.exists():
        raise Rejected()
    verify_relocated(relocated,manifest,producer)
    print('PASS: independently verified dependency families staged and relocated',flush=True)
    consumer=workspace/'independent consumer'
    consumer.mkdir()
    shutil.copyfile(Path(__file__).with_name('consumer.c'),consumer/'consumer.c')
    archives=[f'../relocated dependency/lib/{name}' for name in ARCHIVES]
    step(base+['-Wall','-Wextra','-Werror','-fPIE','-pie','-Wl,--no-undefined',
               '-Wl,-z,max-page-size=16384','-Wl,-z,common-page-size=16384',
               '-Wl,-Map=consumer.map','-I../relocated dependency/include',
               'consumer.c',*archives,'-lm','-o','consumer'],consumer)
    with (consumer/'consumer.map').open('r') as file:
        report=file.read(16*1024**2+1)
    if len(report)>16*1024**2 or str(producer) in report:
        raise Rejected()
    verify_map(report,archives,args.ndk.resolve())
    verify_elf(step([readelf,'-h','-l','-d','-W','consumer'],consumer,True),
               step([nm,'--defined-only','consumer'],consumer,True))

def main():
    parser=argparse.ArgumentParser()
    for name in ('curl','mbedtls','sqlite','ndk','cmake','ninja'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='madmedia-combined-deps-') as work:
            verify(args,Path(work))
    except Exception:
        print('FAIL: isolated Android combined dependency closure',flush=True)
        return 1
    print('PASS: relocated single native dependency link and 16 KiB ELF alignment; target not run')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
