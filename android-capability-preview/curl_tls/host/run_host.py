"""Linux HOST build and colocated loopback acceptance, not Android proof."""
import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile

# Load the existing pinned extraction/build guards from this fixture's parent,
# not a similarly named package from the caller's environment.
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from verify import (CURL, MBED, CURL_OFF, PROTOCOL_DISABLE, Rejected, curl_config,
                    stage_release, trace)

def host_elf(text,symbols):
    if (not re.search(r'^\s*Class:\s*ELF64\s*$',text,re.M) or
        not re.search(r'^\s*Type:\s*DYN\b',text,re.M) or
        not re.search(r'^\s*Machine:\s*Advanced Micro Devices X86-64\s*$',text,re.M) or
        '[Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]' not in text):
        raise Rejected()
    needed=set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]',text))
    if ('libc.so.6' not in needed or not needed<={'libc.so.6','libm.so.6','libpthread.so.0','libdl.so.2'} or
        re.search(r'\((RUNPATH|RPATH)\)',text)):
        raise Rejected()
    for k in ('curl_easy_perform','curl_version_info','mbedtls_ssl_init'):
        if not re.search(r'\b[Tt]\s+'+k+r'$',symbols,re.M):
            raise Rejected()

def tools_ready(args):
    if platform.system()!='Linux' or platform.machine()!='x86_64' or os.geteuid()==0:
        raise Rejected()
    ip=next((p for p in map(Path,('/usr/sbin/ip','/usr/bin/ip')) if p.is_file()),None)
    tools=[Path('/usr/bin/cc'),Path('/usr/bin/readelf'),Path('/usr/bin/nm'),Path('/usr/bin/openssl'),
           args.cmake.resolve(),args.ninja.resolve(),ip]
    if any(not p or not p.is_file() or not os.access(p,os.X_OK) for p in tools):
        raise Rejected()
    return tools

def build(root,curl,mbed,tools):
    cc,readelf,nm,_,cmake,ninja,_=tools
    def step(argv,cwd=root,capture=False):
        return trace(argv,cwd,'/usr/bin:/bin',capture)
    common=[f'-DCMAKE_C_COMPILER={cc}','-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
            '-DCMAKE_BUILD_TYPE=Release','-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
            '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',
            f'-DCMAKE_MAKE_PROGRAM={ninja}']
    prefix=root/'host-deps'
    mbed_build=root/'mbed-build'
    step([cmake,'-S',mbed,'-B',mbed_build,'-G','Ninja',*common,
         f'-DCMAKE_INSTALL_PREFIX={prefix}','-DCMAKE_INSTALL_LIBDIR=lib',
         '-DUSE_STATIC_MBEDTLS_LIBRARY=ON','-DUSE_SHARED_MBEDTLS_LIBRARY=OFF',
         '-DUSE_STATIC_TF_PSA_CRYPTO_LIBRARY=ON','-DUSE_SHARED_TF_PSA_CRYPTO_LIBRARY=OFF',
         '-DENABLE_PROGRAMS=OFF','-DENABLE_TESTING=OFF','-DGEN_FILES=OFF'])
    step([cmake,'--build',mbed_build,'--parallel','2'])
    step([cmake,'--install',mbed_build])
    lib=prefix/'lib'
    mbed_libs=[lib/('lib'+name+'.a') for name in ('mbedtls','mbedx509','tfpsacrypto')]
    if not all(path.is_file() for path in mbed_libs):
        raise Rejected()
    curl_build=root/'curl-build'
    options=[f'-D{key}=OFF' for key in CURL_OFF]+[f'-DCURL_DISABLE_{key}=ON' for key in PROTOCOL_DISABLE]
    summary=step([cmake,'-S',curl,'-B',curl_build,'-G','Ninja',*common,*options,
         f'-DCMAKE_INSTALL_PREFIX={prefix}','-DCMAKE_INSTALL_LIBDIR=lib',
         '-DCURL_USE_CMAKECONFIG=OFF','-DCURL_ENABLE_SSL=ON','-DCURL_USE_MBEDTLS=ON','-DBUILD_STATIC_LIBS=ON',
         '-DCURL_CA_BUNDLE=none','-DCURL_CA_PATH=none','-DCURL_CA_EMBED=',
         f'-DMBEDTLS_INCLUDE_DIR={prefix}/include',f'-DMBEDTLS_LIBRARY={mbed_libs[0]}',
         f'-DMBEDX509_LIBRARY={mbed_libs[1]}',f'-DMBEDCRYPTO_LIBRARY={mbed_libs[2]}',
         '-DMBEDTLS_USE_STATIC_LIBS=ON'],capture=True)
    curl_config((curl_build/'lib/curl_config.h').read_text(),summary)
    step([cmake,'--build',curl_build,'--parallel','2'])
    step([cmake,'--install',curl_build])
    for folder in (curl,mbed,curl_build,mbed_build):
        shutil.rmtree(folder)
    consumer=root/'consumer'
    consumer.mkdir()
    source=consumer/'client.c'
    shutil.copyfile(Path(__file__).with_name('client.c'),source)
    binary=consumer/'host-client'
    step([cc,'-std=c11','-Wall','-Wextra','-Werror','-fPIE','-pie','-Wl,--no-undefined','-pthread',
          f'-I{prefix}/include',source,lib/'libcurl.a',*mbed_libs,'-lm','-o',binary],consumer)
    host_elf(step([readelf,'-h','-l','-d','-W',binary],consumer,True),
             step([nm,'--defined-only',binary],consumer,True))
    return binary

def main():
    parser=argparse.ArgumentParser()
    for key in ('curl','mbedtls','cmake','ninja'):
        parser.add_argument('--'+key,required=True,type=Path)
    args=parser.parse_args()
    stage='host-toolchain-and-isolation'
    try:
        tools=tools_ready(args)
        outer_user=str(Path('/proc/self/ns/user').readlink())
        outer_net=str(Path('/proc/self/ns/net').readlink())
        with tempfile.TemporaryDirectory(prefix='madmedia-host-proof-') as workspace:
            root=Path(workspace)
            for path,outside in (('/proc/self/ns/user',outer_user),('/proc/self/ns/net',outer_net)):
                if trace(['/usr/bin/readlink',path],root,'/usr/bin:/bin',True).strip()==outside:
                    raise Rejected()
            print('PASS: separate host user and network namespaces',flush=True)
            stage='host-publisher-archive-receipts'
            with args.curl.open('rb') as source:
                curl=stage_release(source,CURL,root)
            with args.mbedtls.open('rb') as source:
                mbed=stage_release(source,MBED,root)
            stage='host-static-link-and-no-ambient-ca'
            binary=build(root,curl,mbed,tools)
            print('PASS: pinned Linux static client, no ambient CA or alternate TLS',flush=True)
            stage='host-localhost-peer-and-name-verification'
            cert_dir=root/'synthetic-identity'
            cert_dir.mkdir(mode=0o700)
            result=trace([sys.executable,'-I',Path(__file__).with_name('namespace_fixture.py'),
                          outer_user,outer_net,str(os.geteuid()),tools[-1],tools[3],binary,cert_dir],
                         root,'/usr/bin:/bin',True)
            if result!='PASS: Linux loopback TLS exact two HTTP, two certificate rejections\n':
                raise Rejected()
    except Exception:
        print('FAIL: '+stage,flush=True)
        return 1
    print('PASS: Linux HOST loopback TLS only; no Android or engine proof')
    return 0

if __name__=='__main__':
    raise SystemExit(main())
