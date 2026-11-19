"""Pinned official source receipt and private Android compile/link only gate."""
import argparse
import bz2
import hashlib
import io
import lzma
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
from dataclasses import dataclass

TOTAL_LIMIT = 128 * 1024**2
MEMBER_LIMIT = 16 * 1024**2
TOOL_OUTPUT = 1024**2
NDK_VERSION = '28.2.13676358'

@dataclass(frozen=True)
class Package:
    size: int
    digest: str
    root: str
    codec: str
    marker: str
    version_pattern: bytes
    notices: tuple[str,...]

CURL = Package(2990916,'8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5',
               'curl-8.23.0','xz','include/curl/curlver.h',
               rb'^#define[ \t]+LIBCURL_VERSION[ \t]+"8\.23\.0"[ \t]*$',('COPYING','CMakeLists.txt'))
MBED = Package(6552353,'392215e5e4846df04e2f4c4516079303bd73edde837d8f3e68fb4d4d7ddf0126',
               'mbedtls-4.1.2','bz2','include/mbedtls/build_info.h',
               rb'^#define[ \t]+MBEDTLS_VERSION_STRING[ \t]+"4\.1\.2"[ \t]*$',
               ('LICENSE','CMakeLists.txt','framework/LICENSE','tf-psa-crypto/LICENSE',
                'tf-psa-crypto/CMakeLists.txt'))

class Rejected(Exception):
    pass

def stage_release(handle, spec, workspace):
    original=handle.read(spec.size+1)
    if len(original)!=spec.size or hashlib.sha256(original).hexdigest()!=spec.digest:
        raise Rejected()
    # Bound decompressor output before tar metadata or member layout is parsed.
    if spec.codec=='xz':
        decoder=lzma.LZMADecompressor(format=lzma.FORMAT_XZ,memlimit=128*1024**2)
    elif spec.codec=='bz2':
        decoder=bz2.BZ2Decompressor()
    else:
        raise Rejected()
    raw=decoder.decompress(original,max_length=TOTAL_LIMIT+1)
    if len(raw)>TOTAL_LIMIT or not decoder.eof or decoder.unused_data:
        raise Rejected()
    names={}
    marker=None
    total=0
    with tarfile.open(fileobj=io.BytesIO(raw),mode='r:') as archive:
        for member in archive:
            if len(names)>=12000:
                raise Rejected()
            name=member.name
            path=PurePosixPath(name)
            if (path.is_absolute() or not path.parts or path.parts[0]!=spec.root or
                name!=path.as_posix() or name in names or
                any(not re.fullmatch(r'[A-Za-z0-9._+@-]{1,160}',part) or part in ('.','..')
                    for part in path.parts) or len(path.parts)>24 or member.issparse() or
                member.size<0 or member.size>MEMBER_LIMIT or
                member.mode & (stat.S_ISUID | stat.S_ISGID) or
                not (member.isfile() or member.isdir())):
                raise Rejected()
            if member.isdir() and member.size:
                raise Rejected()
            total+=member.size
            if total>TOTAL_LIMIT:
                raise Rejected()
            names[name]=member
            if name==spec.root+'/'+spec.marker and member.isfile():
                f=archive.extractfile(member)
                marker=f.read(128*1024+1) if f else b''
        if not marker or len(marker)>128*1024 or not re.search(spec.version_pattern,marker,re.M):
            raise Rejected()
        if any((spec.root+'/'+item not in names or not names[spec.root+'/'+item].isfile()) for item in spec.notices):
            raise Rejected()
        if any(str(p) in names and not names[str(p)].isdir()
               for name in names for p in PurePosixPath(name).parents):
            raise Rejected()
        # Manually write regular entries only. No ownership, modes, times, hooks,
        # symlinks or tar extraction filters inherited from the archive.
        for name,member in names.items():
            path=workspace/name
            if member.isdir():
                path.mkdir(parents=True,exist_ok=True)
                continue
            path.parent.mkdir(parents=True,exist_ok=True)
            f=archive.extractfile(member)
            if f is None:
                raise Rejected()
            with path.open('xb') as output:
                payload=f.read(member.size+1)
                if len(payload)!=member.size:
                    raise Rejected()
                output.write(payload)
    return workspace/spec.root

def trace(args,cwd,tool_path,capture=False):
    # Fresh user+network namespace for every build command, never a fallback
    # to the caller's network. Treat unsupported namespaces as hard failure.
    env={'PATH':tool_path,'HOME':str(cwd),'LC_ALL':'C','CMAKE_BUILD_PARALLEL_LEVEL':'2'}
    with tempfile.TemporaryFile() as log:
        subprocess.run(['/usr/bin/unshare','--user','--map-root-user','--net','--',
                        *[str(arg) for arg in args]],cwd=cwd,stdin=subprocess.DEVNULL,
                       stdout=log,stderr=subprocess.STDOUT,env=env,check=True,timeout=300)
        if capture:
            log.seek(0)
            data=log.read(TOOL_OUTPUT+1)
            if len(data)>TOOL_OUTPUT:
                raise Rejected()
            return data.decode('utf-8')
    return None

def curl_config(config, summary):
    protocols=re.findall(r'^-- Protocols:[ \t]*([^\r\n]+)$',summary,re.M)
    if len(protocols)!=1 or set(re.split(r'[;\s]+',protocols[0].strip().lower()))!={'http','https'}:
        raise Rejected()
    defines=set(re.findall(r'^\s*#\s*define\s+([A-Za-z0-9_]+)',config,re.M))
    if 'USE_MBEDTLS' not in defines or not {'CURL_DISABLE_PROXY','CURL_DISABLE_NETRC'}<=defines:
        raise Rejected()
    forbidden={'USE_OPENSSL','USE_GNUTLS','USE_SCHANNEL','USE_WOLFSSL','USE_APPLE_SECTRUST',
               'USE_RUSTLS','CURL_CA_BUNDLE','CURL_CA_PATH','CURL_CA_NATIVE',
               'CURL_CA_FALLBACK','CURL_WITH_MULTI_SSL','CURL_CA_EMBED'}
    if defines & forbidden:
        raise Rejected()

def target_elf(text,symbols):
    if (not re.search(r'^\s*Class:\s*ELF64\s*$',text,re.M) or
        not re.search(r'^\s*Type:\s*DYN\b',text,re.M) or
        not re.search(r'^\s*Machine:\s*AArch64\s*$',text,re.M) or
        '[Requesting program interpreter: /system/bin/linker64]' not in text):
        raise Rejected()
    needed=set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',text))
    if 'libc.so' not in needed or not needed<={'libc.so','libm.so','libdl.so'} or re.search(r'\((RUNPATH|RPATH)\)',text):
        raise Rejected()
    for symbol in ('curl_easy_setopt','curl_multi_init','curl_url','mbedtls_ssl_init'):
        if not re.search(r'\b[Tt]\s+'+symbol+r'$',symbols,re.M):
            raise Rejected()

CURL_OFF=('CURL_USE_OPENSSL','CURL_USE_GNUTLS','CURL_USE_WOLFSSL','CURL_USE_RUSTLS',
          'CURL_USE_PKGCONFIG','BUILD_SHARED_LIBS','BUILD_CURL_EXE','BUILD_TESTING','BUILD_LIBCURL_DOCS',
          'CURL_ZLIB','CURL_BROTLI','CURL_ZSTD','USE_NGHTTP2','USE_NGTCP2','USE_QUICHE','USE_LIBIDN2',
          'CURL_USE_LIBPSL','CURL_USE_LIBSSH2','CURL_USE_LIBSSH','ENABLE_ARES','CURL_USE_GSASL',
          'CURL_USE_GSSAPI','CURL_CA_NATIVE','CURL_CA_FALLBACK')
PROTOCOL_DISABLE=('DICT','FILE','FTP','GOPHER','IMAP','IPFS','LDAP','LDAPS','MQTT','POP3',
                  'RTSP','SMTP','TELNET','TFTP','WEBSOCKETS','DOH','PROXY','NETRC','ALTSVC','COOKIES','HSTS')

def build(args,workspace,curl,mbed):
    ndk=args.ndk.resolve()
    cmake=args.cmake.resolve()
    ninja=args.ninja.resolve()
    tools=ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
    chain=ndk/'build/cmake/android.toolchain.cmake'
    if platform.system()!='Linux' or platform.machine()!='x86_64':
        raise Rejected()
    if re.findall(r'^\s*Pkg\.Revision\s*=\s*(\S+)\s*$',(ndk/'source.properties').read_text(),re.M)!=[NDK_VERSION]:
        raise Rejected()
    if not all(x.is_file() for x in [cmake,ninja,chain,tools/'clang',tools/'llvm-readelf',tools/'llvm-nm']):
        raise Rejected()
    def run(argv,cwd=workspace,capture=False):
        return trace(argv,cwd,'/usr/bin:/bin',capture)
    # Check namespace support before invoking any upstream build metadata.
    run(['/usr/bin/true'])
    print('PASS: isolated command environment',flush=True)
    common=[f'-DCMAKE_TOOLCHAIN_FILE={chain}','-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-28',
            '-DCMAKE_POSITION_INDEPENDENT_CODE=ON','-DCMAKE_BUILD_TYPE=Release',
            '-DFETCHCONTENT_FULLY_DISCONNECTED=ON','-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY',
            '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY',f'-DCMAKE_MAKE_PROGRAM={ninja}']
    prefix=workspace/'target-deps'
    mbed_build=workspace/'mbed-build'
    run([cmake,'-S',mbed,'-B',mbed_build,'-G','Ninja',*common,
         f'-DCMAKE_INSTALL_PREFIX={prefix}','-DCMAKE_INSTALL_LIBDIR=lib',
         '-DUSE_STATIC_MBEDTLS_LIBRARY=ON','-DUSE_SHARED_MBEDTLS_LIBRARY=OFF',
         '-DUSE_STATIC_TF_PSA_CRYPTO_LIBRARY=ON','-DUSE_SHARED_TF_PSA_CRYPTO_LIBRARY=OFF',
         '-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF','-DGEN_FILES=OFF'])
    run([cmake,'--build',mbed_build,'--parallel','2'])
    run([cmake,'--install',mbed_build])
    print('PASS: static target mbedtls/x509/tfpsacrypto',flush=True)
    curl_build=workspace/'curl-build'
    lib=prefix/'lib'
    mbed_libs=[lib/('lib'+name+'.a') for name in ('mbedtls','mbedx509','tfpsacrypto')]
    if any(not x.is_file() for x in mbed_libs):
        raise Rejected()
    # Exact target archives and include path bypass pkg-config, host find
    # scripts and package registry. Curl's finder maps MBEDCRYPTO_LIBRARY to
    # the crypto archive even though v4 names it libtfpsacrypto.a.
    args=[f'-D{key}=OFF' for key in CURL_OFF]+[f'-DCURL_DISABLE_{key}=ON' for key in PROTOCOL_DISABLE]
    summary=run([cmake,'-S',curl,'-B',curl_build,'-G','Ninja',*common,*args,
         f'-DCMAKE_INSTALL_PREFIX={prefix}','-DCMAKE_INSTALL_LIBDIR=lib',
         '-DCURL_USE_CMAKECONFIG=OFF','-DCURL_ENABLE_SSL=ON','-DCURL_USE_MBEDTLS=ON','-DBUILD_STATIC_LIBS=ON',
         '-DCURL_CA_BUNDLE=none','-DCURL_CA_PATH=none','-DCURL_CA_EMBED=',
         f'-DMBEDTLS_INCLUDE_DIR={prefix}/include',f'-DMBEDTLS_LIBRARY={mbed_libs[0]}',
         f'-DMBEDX509_LIBRARY={mbed_libs[1]}',f'-DMBEDCRYPTO_LIBRARY={mbed_libs[2]}',
         '-DMBEDTLS_USE_STATIC_LIBS=ON'],capture=True)
    curl_config((curl_build/'lib/curl_config.h').read_text(),summary)
    run([cmake,'--build',curl_build,'--parallel','2'])
    run([cmake,'--install',curl_build])
    print('PASS: static target curl, HTTP(S) only and no default CA setup',flush=True)
    # Consumer sees only the staged target prefix, never producer inputs.
    for folder in (curl,mbed,curl_build,mbed_build):
        shutil.rmtree(folder)
    consumer=workspace/'consumer'
    consumer.mkdir()
    source=Path(__file__).with_name('consumer.c')
    (consumer/'consumer.c').write_bytes(source.read_bytes())
    run([tools/'clang','--target=aarch64-linux-android28','-std=c11','-Wall','-Wextra','-Werror',
         '-fPIE','-pie','-Wl,--no-undefined',f'-I{prefix}/include','consumer.c',
         lib/'libcurl.a',*mbed_libs,'-lm','-o','consumer'],consumer)
    target_elf(run([tools/'llvm-readelf','-h','-l','-d','-W','consumer'],consumer,True),
               run([tools/'llvm-nm','--defined-only','consumer'],consumer,True))

def main():
    parser=argparse.ArgumentParser()
    for key in ('curl','mbedtls','ndk','cmake','ninja'):
        parser.add_argument('--'+key,required=True,type=Path)
    args=parser.parse_args()
    stage='publisher-curl'
    try:
        with tempfile.TemporaryDirectory(prefix='madmedia-curl-tls-') as temp:
            path=Path(temp)
            with args.curl.open('rb') as source:
                curl=stage_release(source,CURL,path)
            stage='publisher-mbedtls'
            with args.mbedtls.open('rb') as source:
                mbed=stage_release(source,MBED,path)
            print('PASS: exact official curl and Mbed TLS sources with license notices',flush=True)
            stage='isolated-target-compile-link'
            build(args,path,curl,mbed)
    except Exception:
        print('FAIL: '+stage,flush=True)
        return 1
    print('PASS: isolated Android API 28 dependency link only; no TLS or Android runtime executed')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
