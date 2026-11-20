"""Native ARM64 macOS qualification of the unchanged iOS preview codec."""
import hashlib
import platform
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
INPUTS={
    'image.c':'483033bff86cd7fb45dda56f8ea91f2d656ef6fe97a0da2944050830d816d53f',
    'include/madmedia/ios_image_preview.h':'cedd77b095d5f2e2eedf646c94bf03f2bf8c0c522be701d3dbb2d47d1b0a717c',
}
ARCHIVE='libmadmedia_ios_image_preview.a'
SDK_VERSION=b'26.2'
TARGET='arm64-apple-macos15.0'

def run(*args,capture=False,timeout=120):
    result=subprocess.run(args,stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,timeout=timeout,check=True)
    return result.stdout if capture else None

def install(work,sdk,source_tree=ROOT/'ios-image-preview'):
    producer=work/'producer'; producer.mkdir()
    build=work/'build'; build.mkdir()
    prefix=work/'prefix'
    host=work/'consumer'
    for name,wanted in INPUTS.items():
        original=source_tree/name
        if original.is_symlink(): raise ValueError()
        data=original.read_bytes()
        if hashlib.sha256(data).hexdigest()!=wanted: raise ValueError()
        target=producer/name
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(data)
    obj=build/'image.o'; archive=build/ARCHIVE
    run('xcrun','--sdk','macosx','clang','-target',TARGET,'-isysroot',sdk,
        '-std=c11','-Wall','-Wextra','-Werror','-I',producer/'include',
        '-c',producer/'image.c','-o',obj)
    run('xcrun','libtool','-static','-o',archive,obj)
    check_macho(obj)
    if run('xcrun','lipo','-archs',archive,capture=True).strip()!=b'arm64':raise ValueError()
    (prefix/'lib').mkdir(parents=True)
    shutil.copyfile(archive,prefix/'lib'/ARCHIVE)
    shutil.copytree(producer/'include',prefix/'include')
    host.mkdir()
    shutil.move(prefix,host/'Installed')
    shutil.rmtree(producer);shutil.rmtree(build)
    if any(x.exists() for x in (producer,build,prefix)):raise ValueError()
    return host

def check_macho(path):
    if run('xcrun','lipo','-archs',path,capture=True).strip()!=b'arm64':raise ValueError()
    platform_lines=re.findall(rb'^\s*platform\s+(\S+)\s*$',
        run('xcrun','vtool','-show-build',path,capture=True),re.M)
    if platform_lines not in ([b'MACOS'],[b'1']):raise ValueError()

def host_fixture(app):
    # Reuse the iOS fixture and assertions without UIKit, app entrypoint or
    # Simulator guard. Transform only the explicit known platform scaffolding.
    marker='static NSData *fixture() {'
    runtime=('static BOOL intendedRuntime() {\n'
             '    NSOperatingSystemVersion version=NSProcessInfo.processInfo.operatingSystemVersion;\n'
             '    return version.majorVersion==26 && version.minorVersion==2;\n'
             '}\n\n')
    guard='    if (!intendedRuntime()) return NO;\n'
    if app.count(marker)!=1 or app.count(runtime)!=1 or app.count(guard)!=2:raise ValueError()
    shared=marker+app.split(marker,1)[1]
    shared=shared.replace(runtime,'',1).replace(guard,'')
    return ('#import <Foundation/Foundation.h>\n#import <ImageIO/ImageIO.h>\n'
            '#import <CoreGraphics/CoreGraphics.h>\n#include <TargetConditionals.h>\n'
            '#if !TARGET_OS_OSX || !defined(__arm64__)\n#error Requires native ARM64 macOS\n#endif\n'
            '#include <madmedia/ios_image_preview.h>\n#include <vector>\n#include <cstring>\n'
            +shared+'\nint main(int argc, const char **argv) {\n'
            '    @autoreleasepool {\n'
            '        if (argc != 2) return 2;\n'
            '        if (std::strcmp(argv[1],"roundtrip")==0) return madmedia_preview_roundtrip()?0:1;\n'
            '        if (std::strcmp(argv[1],"bounds")==0) return madmedia_preview_contract()?0:1;\n'
            '        return 2;\n    }\n}\n')

def main():
    global stage
    stage='host-preflight'
    if platform.system()!='Darwin' or platform.machine()!='arm64':raise ValueError()
    if run('xcodebuild','-version',capture=True,timeout=30).strip()!=b'Xcode 26.2\nBuild version 17C52':raise ValueError()
    if run('xcrun','--sdk','macosx','--show-sdk-version',capture=True,timeout=30).strip()!=SDK_VERSION:raise ValueError()
    sdk=run('xcrun','--sdk','macosx','--show-sdk-path',capture=True,timeout=30).decode().strip()
    with tempfile.TemporaryDirectory(prefix='madmedia-image-host-') as tmp:
        stage='installed-codec'
        host=install(Path(tmp),sdk)
        stage='independent-link'
        fixture=host/'probe.mm'
        fixture.write_text(host_fixture((ROOT/'ios-image-preview/App.mm').read_text()))
        installed=host/'Installed'; binary=host/'probe'
        run('xcrun','--sdk','macosx','clang++','-target',TARGET,'-isysroot',sdk,
            '-std=c++17','-fobjc-arc','-I',installed/'include',fixture,installed/'lib'/ARCHIVE,
            '-framework','Foundation','-framework','CoreFoundation','-framework','ImageIO',
            '-framework','CoreGraphics','-o',binary)
        stage='host-artifact'
        check_macho(binary)
        symbols=run('xcrun','nm','-g','-U',binary,capture=True)
        if not re.search(rb'\b_madmedia_ios_jpeg_resize_v1$',symbols,re.M):raise ValueError()
        deps=run('xcrun','otool','-L',binary,capture=True)
        if any(x not in deps for x in (b'/ImageIO.framework/',b'/CoreGraphics.framework/')) or re.search(rb'curl|sqlite|UIKit|madmedia_core',deps,re.I):raise ValueError()
        for stage in ('roundtrip','bounds'):
            run('/usr/bin/arch','-arm64',binary,stage,timeout=45)
            print('PASS: macOS host image '+stage,flush=True)

if __name__=='__main__':
    try:main()
    except Exception:
        print('FAIL: macOS host image '+stage,flush=True)
        raise SystemExit(1) from None
