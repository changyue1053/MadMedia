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

ROUNDTRIP_CODES={
    10:'fixture-encode',11:'fixture-decode',12:'fixture-metadata',13:'high-codec',
    14:'low-codec',15:'high-redecode',16:'low-redecode',17:'quality-response',
    18:'no-upscale-codec',19:'no-upscale-redecode',20:'output-tiff-artist',
    **{phase*10+code:label+'-'+kind
       for phase,label in ((3,'codec-size-query'),(4,'codec-output'))
       for code,kind in enumerate(('unexpected-ok','argument','limit','decode','encode','capacity','size-contract'))},
}

def diagnose_fixture(source):
    # Split exact existing test logic for diagnostics. On this host-only
    # path, test the seeded Artist field rather than unseeded dictionary presence.
    replacements=(
      ('    if (madmedia_ios_jpeg_resize_v1(bytes,input.length,edge,quality,NULL,0,&n)!=MADMEDIA_IMAGE_V1_CAPACITY ||\n'
       '        n==0 || n>8*1024*1024) return nil;',
       '    int32_t query=madmedia_ios_jpeg_resize_v1(bytes,input.length,edge,quality,NULL,0,&n);\n'
       '    if (query!=MADMEDIA_IMAGE_V1_CAPACITY) { hostCodecFail(30,query); return nil; }\n'
       '    if (n==0 || n>8*1024*1024) { hostCodecFail(30,6); return nil; }'),
      ('    if (madmedia_ios_jpeg_resize_v1(bytes,input.length,edge,quality,static_cast<uint8_t *>(out.mutableBytes),n,&written)!=MADMEDIA_IMAGE_V1_OK ||\n'
       '        written!=n) return nil;',
       '    int32_t output=madmedia_ios_jpeg_resize_v1(bytes,input.length,edge,quality,static_cast<uint8_t *>(out.mutableBytes),n,&written);\n'
       '    if (output!=MADMEDIA_IMAGE_V1_OK) { hostCodecFail(40,output); return nil; }\n'
       '    if (written!=n) { hostCodecFail(40,6); return nil; }'),
      ('    NSData *input=fixture();\n    NSDictionary *before=',
       '    NSData *input=fixture();\n    if (!input) return hostFail(10);\n    NSDictionary *before='),
      ('    if (![before[tiff][artist] isEqual:@"Synthetic fixture only"]) return NO;',
       '    if (!before) return hostFail(11);\n'
       '    if (![before[tiff][artist] isEqual:@"Synthetic fixture only"]) return hostFail(12);'),
      ('    NSData *high=resize(input,32,95), *low=resize(input,32,20);\n'
       '    NSDictionary *after=inspectJpeg(high,32,16,YES);\n'
       '    if (!after || !inspectJpeg(low,32,16,YES) || high.length<=low.length ||\n'
       '        !inspectJpeg(resize(input,128,95),96,48,YES)) return NO;',
       '    NSData *high=resize(input,32,95);\n'
       '    if (!high) return hostFail(13);\n'
       '    NSData *low=resize(input,32,20);\n'
       '    if (!low) return hostFail(14);\n'
       '    NSDictionary *after=inspectJpeg(high,32,16,YES);\n'
       '    if (!after) return hostFail(15);\n'
       '    NSDictionary *afterLow=inspectJpeg(low,32,16,YES);\n'
       '    if (!afterLow) return hostFail(16);\n'
       '    if (high.length<=low.length) return hostFail(17);\n'
       '    NSData *same=resize(input,128,95);\n'
       '    if (!same) return hostFail(18);\n'
       '    NSDictionary *afterSame=inspectJpeg(same,96,48,YES);\n'
       '    if (!afterSame) return hostFail(19);'),
      ('    return !after[tiff][artist] && !after[(__bridge NSString *)kCGImagePropertyGPSDictionary] &&\n'
       '        !after[(__bridge NSString *)kCGImagePropertyExifDictionary] &&\n'
       '        !after[(__bridge NSString *)kCGImagePropertyIPTCDictionary];',
       '    if (after[tiff][artist] || afterLow[tiff][artist] || afterSame[tiff][artist]) return hostFail(20);\n'
       '    return YES;'),
    )
    for old,new in replacements:
        if source.count(old)!=1:raise ValueError()
        source=source.replace(old,new,1)
    return ('static int hostDiagnostic=1;\n'
            'static void hostCodecFail(int base, int32_t code) { if (code>=0 && code<=6) hostDiagnostic=base+code; }\n'
            'static BOOL hostFail(int code) { if (hostDiagnostic==1) hostDiagnostic=code; return NO; }\n'+source)

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
    shared=diagnose_fixture(shared.replace(runtime,'',1).replace(guard,''))
    return ('#import <Foundation/Foundation.h>\n#import <ImageIO/ImageIO.h>\n'
            '#import <CoreGraphics/CoreGraphics.h>\n#include <TargetConditionals.h>\n'
            '#if !TARGET_OS_OSX || !defined(__arm64__)\n#error Requires native ARM64 macOS\n#endif\n'
            '#include <madmedia/ios_image_preview.h>\n#include <vector>\n#include <cstring>\n'
            +shared+'\nint main(int argc, const char **argv) {\n'
            '    @autoreleasepool {\n'
            '        if (argc != 2) return 2;\n'
            '        if (std::strcmp(argv[1],"roundtrip")==0) return madmedia_preview_roundtrip()?0:hostDiagnostic;\n'
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
            try:
                run('/usr/bin/arch','-arm64',binary,stage,timeout=45)
            except subprocess.CalledProcessError as error:
                if stage=='roundtrip':
                    print('CATEGORY: host-roundtrip:'+ROUNDTRIP_CODES.get(error.returncode,'unclassified'),flush=True)
                raise
            except subprocess.TimeoutExpired:
                if stage=='roundtrip':print('CATEGORY: host-roundtrip:timeout',flush=True)
                raise
            print('PASS: macOS host image '+stage,flush=True)

if __name__=='__main__':
    try:main()
    except Exception:
        print('FAIL: macOS host image '+stage,flush=True)
        raise SystemExit(1) from None
