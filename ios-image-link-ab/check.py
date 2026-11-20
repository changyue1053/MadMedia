"""Compile/link only: exact installed archive, manual versus two App-only projects."""
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'ios-image-preview'
sys.path.insert(0,str(BASE))
spec=importlib.util.spec_from_file_location('image_base',BASE/'check.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
sys.path.pop(0)
ARCHIVE='Installed/lib/libmadmedia_ios_image_preview.a'
TARGET='arm64-apple-ios15.0-simulator'
MODES=('manual','pbx','explicit')
stage='preflight'

def configure(path, explicit):
    base.project(path)
    pbx=path/'Preview.xcodeproj/project.pbxproj'
    text=pbx.read_text()
    if explicit:
        ref=re.findall(r'"([A-F0-9]{24})" = \{ "isa" = "PBXFileReference"; "path" = "'+re.escape(ARCHIVE)+r'"; [^}]+\}',text)
        if len(ref)!=1:raise ValueError()
        build=re.findall(r'"([A-F0-9]{24})" = \{ "isa" = "PBXBuildFile"; "fileRef" = "'+ref[0]+r'"; \}',text)
        if len(build)!=1 or text.count('("'+build[0]+'")')!=1:raise ValueError()
        # The only variant change is removing the archive from this link phase
        # and passing the same installed file directly. The project reference
        # remains listed but is not a build input in the explicit phase.
        text=text.replace('("'+build[0]+'")','()',1)
        flag='"OTHER_LDFLAGS" = ("-lc++",'
        if text.count(flag)!=1:raise ValueError()
        text=text.replace(flag,flag+json.dumps(chr(34)+'$(PROJECT_DIR)/'+ARCHIVE+chr(34))+',',1)
        pbx.write_text(text)
    scheme=path/'Preview.xcodeproj/xcshareddata/xcschemes/Preview.xcscheme'
    tree=ET.parse(scheme);root=tree.getroot()
    actions=root.findall('TestAction')
    buildables=root.findall('./BuildAction/BuildActionEntries/BuildActionEntry/BuildableReference')
    if len(actions)!=1 or len(buildables)!=1 or buildables[0].get('BlueprintName')!='PreviewApp':
        raise ValueError()
    root.remove(actions[0])
    # No test action or test target invocation; generated App remains unchanged.
    tree.write(scheme,encoding='unicode')

def guarded(args,cwd,timeout):
    with tempfile.TemporaryFile() as log:
        try:
            subprocess.run(args,cwd=cwd,stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,
                           timeout=timeout,check=True)
            return 'linked'
        except subprocess.CalledProcessError:
            reason=base.xcode_link_reason(log)
            if reason=='library-missing':
                text=base.xcode_text(log)
                logical=re.compile(r"\s*ld: library (?:not found for (?:-l)?madmedia_ios_image_preview|['\"]?(?:-l)?madmedia_ios_image_preview['\"]? not found)\s*")
                lib=base.xcode_missing_library(log)
                misses=[line for line in text.splitlines() if base.MISSING_LIBRARY_LINE.fullmatch(line)]
                if lib=='other' and misses and all(logical.fullmatch(line) for line in misses):
                    lib='preview-archive'
                return 'library-missing-'+lib
            category=base.xcode_category(log)
            return reason if reason!='unknown' else category
        except subprocess.TimeoutExpired:
            return 'timeout'
        except OSError:
            return 'launch'

def prove(binary):
    if base.run('xcrun','lipo','-archs',binary,capture=True).strip()!=b'arm64':raise ValueError()
    platform_lines=re.findall(rb'^\s*platform\s+(\S+)\s*$',
                              base.run('xcrun','vtool','-show-build',binary,capture=True),re.M)
    if platform_lines not in ([b'IOSSIMULATOR'],[b'7']):raise ValueError()
    symbols=base.run('xcrun','nm','-g','-U',binary,capture=True)
    if not re.search(rb'\b_madmedia_ios_jpeg_resize_v1$',symbols,re.M):raise ValueError()
    deps=base.run('xcrun','otool','-L',binary,capture=True)
    if any(x not in deps for x in (b'/ImageIO.framework/',b'/CoreGraphics.framework/')) or re.search(rb'curl|sqlite|madmedia_core',deps,re.I):
        raise ValueError()

def verdict(results):
    if set(results)!=set(MODES):return 'incomplete'
    if all(results[k]=='verified' for k in MODES):return 'all-link-not-reproduced'
    if (results['manual'],results['explicit'])==('verified','verified'):
        if results['pbx']=='library-missing-preview-archive':return 'confirmed-archive-resolution'
        if results['pbx'] not in ('timeout','launch','artifact-invalid'):
            return 'pbx-only-failure-not-attributed'
    if results['manual']!='verified':return 'manual-not-proven'
    if results['explicit']!='verified':return 'explicit-project-not-proven'
    return 'inconclusive'

def main():
    global stage
    if platform.system()!='Darwin' or platform.machine()!='arm64':raise ValueError()
    if base.run('xcodebuild','-version',capture=True,timeout=30).strip()!=b'Xcode 26.2\nBuild version 17C52':raise ValueError()
    if base.run('xcrun','--sdk','iphonesimulator','--show-sdk-version',capture=True,timeout=30).strip()!=b'26.2':raise ValueError()
    sdk=base.run('xcrun','--sdk','iphonesimulator','--show-sdk-path',capture=True,timeout=30).decode().strip()
    with tempfile.TemporaryDirectory(prefix='madmedia-link-ab-') as tmp:
        work=Path(tmp).resolve()
        if not re.fullmatch(r'/[A-Za-z0-9_./-]+',str(work)):raise ValueError()
        stage='install'
        original=base.install(ROOT,work,sdk)
        archive_hash=hashlib.sha256((original/ARCHIVE).read_bytes()).digest()
        results={}
        for mode in MODES:
            stage=mode
            consumer=work/mode;consumer.mkdir()
            shutil.copytree(original/'Installed',consumer/'Installed')
            if hashlib.sha256((consumer/ARCHIVE).read_bytes()).digest()!=archive_hash:raise ValueError()
            shutil.copyfile(BASE/'App.mm',consumer/'App.mm')
            if mode=='manual':
                binary=consumer/'manual-probe'
                args=['xcrun','--sdk','iphonesimulator','clang++','-target',TARGET,'-isysroot',sdk,
                      '-std=c++17','-fobjc-arc','-I',consumer/'Installed/include',consumer/'App.mm',
                      consumer/ARCHIVE,'-lc++','-framework','Foundation','-framework','UIKit',
                      '-framework','CoreFoundation','-framework','ImageIO','-framework','CoreGraphics','-o',binary]
            else:
                # Keep every project reference resolvable, but the App-only
                # scheme has no test action or dependency on the test target.
                shutil.copyfile(BASE/'Tests.mm',consumer/'Tests.mm')
                configure(consumer,mode=='explicit')
                derived=work/(mode+'-derived')
                args=['xcodebuild','build','-project','Preview.xcodeproj','-scheme','Preview',
                      '-configuration','Debug','-destination','generic/platform=iOS Simulator',
                      '-sdk','iphonesimulator','-derivedDataPath',derived,'ARCHS=arm64',
                      'ONLY_ACTIVE_ARCH=YES','CODE_SIGNING_ALLOWED=NO','CODE_SIGNING_REQUIRED=NO']
                binary=derived/'Build/Products/Debug-iphonesimulator/PreviewApp.app/PreviewApp'
            status=guarded(args,consumer,360)
            if status=='linked':
                try:prove(binary);status='verified'
                except Exception:status='artifact-invalid'
            results[mode]=status
            print('RESULT: image-link-'+mode+':'+status,flush=True)
        outcome=verdict(results)
        print('MATRIX: image-link-'+outcome,flush=True)
        # Native runtime isn't qualified. Success only means a concrete
        # hypothesis confirmed or all link representations verified.
        return 0 if outcome in ('confirmed-archive-resolution','all-link-not-reproduced') else 1

if __name__=='__main__':
    try:raise SystemExit(main())
    except Exception:
        print('FAIL: image-link-'+stage,flush=True)
        raise SystemExit(1) from None
