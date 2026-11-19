"""Synthetic shape/rejection checks; no synthetic artifacts qualify as publisher inputs."""
import hashlib
import io
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import warnings
import zipfile

from verify import (PREFIX, SOURCE_ID, ZIP_LIMIT, Rejected, assert_elf,
                    verified_sqlite)

HEADER = (f'#define SQLITE_VERSION "3.54.1"\n#define SQLITE_VERSION_NUMBER 3054001\n'
          f'#define SQLITE_SOURCE_ID "{SOURCE_ID}"\n').encode()
SOURCE = b'/* SYNTHETIC parser fixture; not SQLite from its publisher. */\n'

def digest(value):
    return hashlib.sha3_256(value).hexdigest()

def packed(header=HEADER, extra=()):
    file = io.BytesIO()
    with zipfile.ZipFile(file,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(PREFIX+'/',b'')
        archive.writestr(PREFIX+'/sqlite3.c',SOURCE)
        archive.writestr(PREFIX+'/sqlite3.h',header)
        for name,value in extra:
            archive.writestr(name,value)
    return file.getvalue()

def reject(action):
    try:
        action()
    except (Rejected,zipfile.BadZipFile):
        return
    raise AssertionError('unexpected acceptance')

def unpack(payload, zip_hash=None, c_hash=None):
    return verified_sqlite(io.BytesIO(payload),zip_hash or digest(payload),c_hash or digest(SOURCE))

def main():
    good = packed(extra=[(PREFIX+'/shell.c',b'not extracted')])
    assert unpack(good)=={'sqlite3.c':SOURCE,'sqlite3.h':HEADER}
    reject(lambda: unpack(good,zip_hash='0'*64))
    reject(lambda: unpack(good,c_hash='0'*64))
    reject(lambda: unpack(b'x'*(ZIP_LIMIT+1)))
    reject(lambda: unpack(packed(HEADER.replace(b'3054001',b'3054000'))))
    reject(lambda: unpack(packed(HEADER.replace(SOURCE_ID.encode(),b'synthetic bad ID'))))
    reject(lambda: unpack(packed(HEADER+HEADER)))
    for name in ('../evil',PREFIX+'/../evil',PREFIX+'//evil','/'+PREFIX+'/foo',
                 PREFIX+'/nested/path',PREFIX+'/foo\\name',PREFIX+'/otherdir/'):
        reject(lambda: unpack(packed(extra=[(name,b'bad')])))
    link = zipfile.ZipInfo(PREFIX+'/link')
    link.create_system = 3
    link.external_attr = (stat.S_IFLNK|0o777)<<16
    reject(lambda: unpack(packed(extra=[(link,b'elsewhere')])))
    with warnings.catch_warnings():
        warnings.simplefilter('ignore',UserWarning)
        reject(lambda: unpack(packed(extra=[(PREFIX+'/sqlite3.c',b'duplicate')])))
    elf = (' Class: ELF64\n Type: DYN (PIE)\n Machine: AArch64\n'
           ' [Requesting program interpreter: /system/bin/linker64]\n'
           ' (NEEDED) Shared library: [libc.so]\n')
    symbols = '\n'.join('100 T '+name for name in
                       ('sqlite3_open_v2','sqlite3_wal_checkpoint_v2','sqlite3_sourceid'))
    assert_elf(elf,symbols)
    reject(lambda: assert_elf(elf+' (NEEDED) Shared library: [libsqlite3.so]\n',symbols))
    reject(lambda: assert_elf(elf+' (RUNPATH) /tmp\n',symbols))
    reject(lambda: assert_elf(elf,symbols.replace(' T ',' U ')))
    with tempfile.TemporaryDirectory() as path:
        archive = Path(path)/'synthetic.zip'
        archive.write_bytes(good)
        command = subprocess.run([sys.executable,str(Path(__file__).with_name('verify.py')),
                                 '--archive',str(archive),'--ndk',path],capture_output=True,text=True,timeout=15)
        assert command.returncode!=0 and command.stdout=='FAIL: publisher-bytes\n'
    print('PASS: synthetic archive, source identity, safety and ELF gate checks')

if __name__=='__main__':
    main()
