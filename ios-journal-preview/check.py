"""Separate test-only installed Journal and Apple entropy proof."""
import importlib.util
import json
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'ios-image-preview'))
spec=importlib.util.spec_from_file_location('image_runtime',ROOT/'ios-image-preview/check.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
sys.path.pop(0)
CASES={'PreviewTests/testInstalledJournalCommitCloseReopen','PreviewTests/testActualAppleEntropyProvider'}
FILES=('core/journal/journal.cpp','core/journal/journal.h','core/scheduler/types.h',
       'core/util/error.h','core/util/bitmap.h','core/util/log.h',
       'core/platform/entropy.h','ffi/platform/entropy_apple.cpp')
ARCHIVE='libmadmedia_journal_preview.a'
stage='architecture'

def install(work,sdk):
    source=work/'source';build=work/'object';prefix=work/'prefix'
    source.mkdir();build.mkdir()
    for name in FILES:
        dest=source/name;dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(ROOT/name,dest)
    # Existing Apple candidate includes entropy.h by short name.
    objects=[]
    for name in ('core/journal/journal.cpp','ffi/platform/entropy_apple.cpp'):
        obj=build/(Path(name).stem+'.o')
        base.run('xcrun','--sdk','iphonesimulator','clang++','-target','arm64-apple-ios15.0-simulator',
                 '-isysroot',sdk,'-std=c++17','-Wall','-Wextra','-Werror',
                 '-I',source,'-I',source/'core/platform','-c',source/name,'-o',obj)
        proof=base.run('xcrun','vtool','-show-build',obj,capture=True)
        if re.findall(rb'^\s*platform\s+(\S+)\s*$',proof,re.M) not in ([b'IOSSIMULATOR'],[b'7']):
            raise ValueError()
        objects.append(obj)
    archive=build/ARCHIVE
    base.run('xcrun','libtool','-static','-o',archive,*objects)
    if base.run('xcrun','lipo','-archs',archive,capture=True).strip()!=b'arm64':raise ValueError()
    (prefix/'lib').mkdir(parents=True)
    shutil.copyfile(archive,prefix/'lib'/ARCHIVE)
    for name in ('core/journal/journal.h','core/scheduler/types.h','core/util/error.h','core/util/bitmap.h'):
        target=prefix/'include'/name;target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(source/name,target)
    shutil.copyfile(source/'core/platform/entropy.h',prefix/'include/entropy.h')
    app=work/'consumer';app.mkdir()
    shutil.move(prefix,app/'Installed')
    shutil.rmtree(source);shutil.rmtree(build)
    if any(p.exists() for p in (source,build,prefix)):raise ValueError()
    return app

def project(app):
    base.project(app)
    path=app/'Preview.xcodeproj/project.pbxproj'
    text=path.read_text()
    original='libmadmedia_ios_image_preview.a'
    frameworks='"-framework","CoreFoundation","-framework","ImageIO","-framework","CoreGraphics"'
    if text.count(original)!=2 or text.count(frameworks)!=1:raise ValueError()
    text=text.replace(original,ARCHIVE).replace(frameworks,'"-lsqlite3","-framework","Security"')
    path.write_text(text)

def main():
    global stage
    if platform.system()!='Darwin' or platform.machine()!='arm64':raise ValueError()
    stage='toolchain'
    if base.run('xcodebuild','-version',capture=True,timeout=30).strip()!=b'Xcode 26.2\nBuild version 17C52':raise ValueError()
    if base.run('xcrun','--sdk','iphonesimulator','--show-sdk-version',capture=True,timeout=30).strip()!=b'26.2':raise ValueError()
    sdk=base.run('xcrun','--sdk','iphonesimulator','--show-sdk-path',capture=True,timeout=30).decode().strip()
    if not (Path(sdk)/'usr/include/sqlite3.h').is_file() or not (Path(sdk)/'usr/lib/libsqlite3.tbd').is_file():
        raise ValueError()
    base.preflight_runtime()
    with tempfile.TemporaryDirectory(prefix='madmedia-journal-') as tmp:
        work=Path(tmp).resolve()
        if not re.fullmatch(r'/[A-Za-z0-9_./-]+',str(work)):raise ValueError()
        stage='install';app=install(work,sdk)
        for name in ('App.mm','Tests.mm','journal_probe.h'):shutil.copyfile(ROOT/'ios-journal-preview'/name,app/name)
        project(app)
        stage='sim-create'
        uuid=base.run('xcrun','simctl','create','MadMediaJournalPreview',base.DEVICE,base.RUNTIME,
                      capture=True,timeout=60).decode().strip()
        if not re.fullmatch(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}',uuid):raise ValueError()
        try:
            stage='sim-boot';base.run('xcrun','simctl','boot',uuid,timeout=60)
            stage='sim-ready';base.run('xcrun','simctl','bootstatus',uuid,'-b',timeout=240)
            derived=work/'derived'
            common=['-project','Preview.xcodeproj','-scheme','Preview','-configuration','Debug','ARCHS=arm64',
                    'ONLY_ACTIVE_ARCH=YES','-parallel-testing-enabled','NO','-destination','platform=iOS Simulator,id='+uuid,
                    '-derivedDataPath',derived,'CODE_SIGNING_ALLOWED=YES','CODE_SIGN_IDENTITY=-','CODE_SIGNING_REQUIRED=NO']
            stage='sim-build';base.stage=stage
            base.run('xcodebuild','build-for-testing',*common,'-sdk','iphonesimulator',cwd=app,timeout=420)
            stage='artifact'
            binary=derived/'Build/Products/Debug-iphonesimulator/PreviewApp.app/PreviewApp'
            if base.run('xcrun','lipo','-archs',binary,capture=True).strip()!=b'arm64':raise ValueError()
            deps=base.run('xcrun','otool','-L',binary,capture=True)
            if b'/usr/lib/libsqlite3.' not in deps or b'/Security.framework/' not in deps or re.search(rb'curl|madmedia_core',deps,re.I):
                raise ValueError()
            symbols=base.run('xcrun','nm','-g','-U',binary,capture=True)
            if b'Journal4open' not in symbols or b'entropy128' not in symbols:raise ValueError()
            stage='sim-test';base.stage=stage
            result=work/'result.xcresult'
            base.run('xcodebuild','test-without-building',*common,'-resultBundlePath',result,cwd=app,timeout=420)
            stage='test-results'
            def results(key):
                return json.loads(base.run('xcrun','xcresulttool','get','test-results',key,'--path',result,
                                           '--compact',capture=True,timeout=45))
            base.CASES=CASES
            base.verify(results('summary'),results('tests'))
        finally:
            base.run('xcrun','simctl','delete',uuid,timeout=90)
    print('PASS: installed iOS ARM64 Journal and Apple entropy exact two tests')

if __name__=='__main__':
    try:main()
    except Exception:
        print('FAIL: journal-preview:'+stage,flush=True)
        raise SystemExit(1) from None
