"""Offline publisher-byte and cross-link gate; never launch the target consumer."""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile

ZIP_HASH = '92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30'
C_HASH = 'e2b869d3003b33c942b7dd7063604fbc53553e1b66609570df39b9b7b558ee81'
SOURCE_ID = '2026-10-16 14:45:20 555e07ef33f69a0957d24d8e555b702464a131d6f1607f4c3c1e4551e8e6ca76'
PREFIX = 'sqlite-amalgamation-3540100'
ZIP_LIMIT = 4 * 1024 * 1024
MEMBER_LIMIT = 16 * 1024 * 1024
TOTAL_LIMIT = 32 * 1024 * 1024
NDK_REVISION = '28.2.13676358'

class Rejected(Exception):
    pass

def assert_header(header):
    for macro, value in (('SQLITE_VERSION','"3.54.1"'),
                         ('SQLITE_VERSION_NUMBER','3054001'),
                         ('SQLITE_SOURCE_ID','"'+SOURCE_ID+'"')):
        seen = re.findall(rb'^[ \t]*#[ \t]*define[ \t]+'+macro.encode()+
                          rb'[ \t]+([^\r\n]+)',header,re.MULTILINE)
        if len(seen)!=1 or seen[0].strip()!=value.encode():
            raise Rejected()

def verified_sqlite(handle, zip_hash, c_hash):
    # Bound and hash the same in-memory object that is parsed: no path reopen.
    import io
    data = handle.read(ZIP_LIMIT+1)
    if len(data)>ZIP_LIMIT or hashlib.sha3_256(data).hexdigest()!=zip_hash:
        raise Rejected()
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        members = archive.infolist()
        if not 2<=len(members)<=16:
            raise Rejected()
        names = set()
        total = 0
        for member in members:
            name = member.orig_filename
            part = PurePosixPath(name)
            mode = stat.S_IFMT(member.external_attr>>16)
            if (member.filename!=name or name in names or
                member.flag_bits&1 or member.compress_type not in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED)):
                raise Rejected()
            names.add(name)
            if name==PREFIX+'/':
                if not member.is_dir() or mode not in (0,stat.S_IFDIR) or member.file_size!=0:
                    raise Rejected()
                continue
            if (len(part.parts)!=2 or part.parts[0]!=PREFIX or
                name!=part.as_posix() or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}',part.name) or
                mode not in (0,stat.S_IFREG) or not 0<=member.file_size<=MEMBER_LIMIT):
                raise Rejected()
            total += member.file_size
            if total>TOTAL_LIMIT:
                raise Rejected()
        sources = {}
        for name in ('sqlite3.c','sqlite3.h'):
            key = PREFIX+'/'+name
            if key not in names:
                raise Rejected()
            with archive.open(key) as member:
                payload = member.read(MEMBER_LIMIT+1)
            if len(payload)!=archive.getinfo(key).file_size or len(payload)>MEMBER_LIMIT:
                raise Rejected()
            sources[name] = payload
    if hashlib.sha3_256(sources['sqlite3.c']).hexdigest()!=c_hash:
        raise Rejected()
    assert_header(sources['sqlite3.h'])
    return sources

def command(args, cwd, capture=False):
    with tempfile.TemporaryFile() as output:
        subprocess.run([str(x) for x in args],cwd=cwd,stdin=subprocess.DEVNULL,
                       stdout=output,stderr=subprocess.STDOUT,check=True,timeout=300)
        if capture:
            output.seek(0)
            data = output.read(262145)
            if len(data)>262144:
                raise Rejected()
            return data.decode('utf-8')
    return None

def assert_elf(text, symbols):
    if (not re.search(r'^\s*Class:\s*ELF64\s*$',text,re.M) or
        not re.search(r'^\s*Type:\s*DYN\b',text,re.M) or
        not re.search(r'^\s*Machine:\s*AArch64\s*$',text,re.M) or
        '[Requesting program interpreter: /system/bin/linker64]' not in text):
        raise Rejected()
    dependencies = set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',text))
    if ('libc.so' not in dependencies or not dependencies<={'libc.so','libm.so','libdl.so'} or
        '(RPATH)' in text or '(RUNPATH)' in text):
        raise Rejected()
    for symbol in ('sqlite3_open_v2','sqlite3_wal_checkpoint_v2','sqlite3_sourceid'):
        if not re.search(r'\b[Tt]\s+'+symbol+r'$',symbols,re.M):
            raise Rejected()

def toolchain(ndk):
    if platform.system()!='Linux' or platform.machine()!='x86_64':
        raise Rejected()
    receipt = (ndk/'source.properties').read_text()
    if re.findall(r'^\s*Pkg\.Revision\s*=\s*(\S+)\s*$',receipt,re.M)!=[NDK_REVISION]:
        raise Rejected()
    root = ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    tools = tuple(root/name for name in ('clang','llvm-ar','llvm-readelf','llvm-nm'))
    if not all(tool.is_file() for tool in tools):
        raise Rejected()
    return tools

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--archive',type=Path,required=True)
    parser.add_argument('--ndk',type=Path,required=True)
    args = parser.parse_args()
    stage = 'publisher-bytes'
    try:
        with args.archive.open('rb') as handle:
            sources = verified_sqlite(handle,ZIP_HASH,C_HASH)
        print('PASS: exact SQLite publisher archive, source and header identity',flush=True)
        stage = 'ndk-receipt'
        clang,ar,readelf,nm = toolchain(args.ndk.resolve())
        stage = 'target-static-archive'
        with tempfile.TemporaryDirectory(prefix='madmedia-sqlite-') as temporary:
            root = Path(temporary)
            producer = root/'producer'
            producer.mkdir()
            for name,data in sources.items():
                (producer/name).write_bytes(data)
            flags = [clang,'--target=aarch64-linux-android28','-std=gnu11']
            command(flags+['-O2','-fPIC','-DSQLITE_THREADSAFE=1','-DSQLITE_OMIT_LOAD_EXTENSION=1',
                           '-c','sqlite3.c','-o','sqlite3.o'],producer)
            command([ar,'rcs','libsqlite3-preview.a','sqlite3.o'],producer)
            consumer = root/'consumer'
            consumer.mkdir()
            for name in ('sqlite3.h','libsqlite3-preview.a'):
                shutil.move(producer/name,consumer/name)
            shutil.rmtree(producer)
            del sources
            stage = 'independent-wal-consumer-link'
            shutil.copyfile(Path(__file__).with_name('wal_consumer.c'),consumer/'wal_consumer.c')
            command(flags+['-Wall','-Wextra','-Werror','-fPIE','-pie','-Wl,--no-undefined','-I.',
                           'wal_consumer.c','./libsqlite3-preview.a','-lm','-o','wal_consumer'],consumer)
            stage = 'android-elf-and-local-sqlite'
            assert_elf(command([readelf,'-h','-l','-d','-W','wal_consumer'],consumer,capture=True),
                       command([nm,'--defined-only','wal_consumer'],consumer,capture=True))
    except Exception:
        print('FAIL: '+stage)
        return 1
    print('PASS: Android ARM64 API 28 SQLite WAL/reopen fixture cross-link only; target not executed')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
