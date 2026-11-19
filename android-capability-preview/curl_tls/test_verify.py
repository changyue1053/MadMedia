"""Synthetic parser, configure, ELF, and isolation checks; not target validation."""
from dataclasses import replace
import hashlib
import io
import lzma
from pathlib import Path
import subprocess
import tarfile
import tempfile

from verify import (CURL, Rejected, curl_config, stage_release, target_elf, trace)

def assert_rejected(action):
    try:
        action()
    except (Rejected,tarfile.TarError):
        return
    raise AssertionError('unsafe fixture accepted')

def packed(entries=(), version=b'#define LIBCURL_VERSION "8.23.0"\n'):
    content=io.BytesIO()
    with tarfile.open(fileobj=content,mode='w') as archive:
        for path,payload in ((CURL.marker,version),('COPYING',b'synthetic'),('CMakeLists.txt',b'synthetic'),*entries):
            if isinstance(path,str):
                member=tarfile.TarInfo(CURL.root+'/'+path)
                member.size=len(payload)
            else:
                member=path
            archive.addfile(member,io.BytesIO(payload))
    return lzma.compress(content.getvalue())

def verify_tar(data,workspace):
    synthetic=replace(CURL,size=len(data),digest=hashlib.sha256(data).hexdigest())
    return stage_release(io.BytesIO(data),synthetic,workspace)

def test_archive():
    valid=packed(entries=[('m4/lt~obsolete.m4',b'synthetic')])
    with tempfile.TemporaryDirectory() as temporary:
        root=Path(temporary)
        final=verify_tar(valid,root)
        assert (final/'COPYING').read_bytes()==b'synthetic'
        assert (final/'m4/lt~obsolete.m4').read_bytes()==b'synthetic'
        assert_rejected(lambda: stage_release(io.BytesIO(valid),CURL,root))
    fixtures=[valid+b'junk',packed(version=b'8.3.0'),packed(entries=[('../outside',b'x')]),
              packed(entries=[('/aliased',b'x')]),packed(entries=[('COPYING',b'duplicate')]),
              packed(entries=[('m4/other~name.m4',b'x')]),
              packed(entries=[('sub/m4/lt~obsolete.m4',b'x')]),
              packed(entries=[('parent',b'file'),('parent/child',b'nested')])]
    for type_ in (tarfile.SYMTYPE,tarfile.LNKTYPE,tarfile.FIFOTYPE):
        info=tarfile.TarInfo(CURL.root+'/special')
        info.type=type_
        info.linkname='COPYING'
        fixtures.append(packed(entries=[(info,b'')]))
    link=tarfile.TarInfo(CURL.root+'/m4/lt~obsolete.m4')
    link.type=tarfile.SYMTYPE
    link.linkname='COPYING'
    fixtures.append(packed(entries=[(link,b'')]))
    for payload in fixtures:
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            assert_rejected(lambda: verify_tar(payload,root))
            assert not list(root.iterdir())

def test_link():
    config='#define USE_MBEDTLS 1\n#define CURL_DISABLE_PROXY 1\n#define CURL_DISABLE_NETRC 1'
    for protocols in ('-- Protocols: http https','-- Protocols: http;https'):
        curl_config(config,protocols)
    assert_rejected(lambda: curl_config(config,'-- Protocols: http https ftp'))
    for flag in ('USE_OPENSSL','CURL_CA_NATIVE','CURL_CA_PATH','CURL_CA_BUNDLE','CURL_WITH_MULTI_SSL'):
        assert_rejected(lambda: curl_config(config+'\n#define '+flag+' 1','-- Protocols: http;https'))
    elf=(' Class: ELF64\n Type: DYN (PIE)\n Machine: AArch64\n'
         ' [Requesting program interpreter: /system/bin/linker64]\n'
         ' (NEEDED) Shared library: [libc.so]\n')
    syms='\n'.join('100 T '+k for k in ('curl_easy_setopt','curl_multi_init','curl_url','mbedtls_ssl_init'))
    target_elf(elf,syms)
    assert_rejected(lambda: target_elf(elf+' (NEEDED) Shared library: [libssl.so]',syms))
    assert_rejected(lambda: target_elf(elf+' (RPATH) injected',syms))
    assert_rejected(lambda: target_elf(elf,syms.replace(' T ',' U ')))

def test_isolation():
    with tempfile.TemporaryDirectory() as temp:
        try:
            iso=trace(['/usr/bin/readlink','/proc/self/ns/net'],Path(temp),'/usr/bin:/bin',True)
        except (subprocess.CalledProcessError,FileNotFoundError):
            print('NOT RUN: network namespace unavailable; real build must fail closed')
        else:
            assert iso.strip()!=str(Path('/proc/self/ns/net').readlink())
            print('PASS: build launcher enters a different network namespace')

if __name__=='__main__':
    test_archive()
    test_link()
    test_isolation()
    print('PASS: synthetic source receipt, trust configuration and ELF gates')
