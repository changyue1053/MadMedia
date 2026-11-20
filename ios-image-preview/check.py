"""ARM64 Simulator only. Fixed stages, no upload or raw diagnostics."""
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

from project import project

ROOT=Path(__file__).resolve().parents[1]
CASES={'PreviewTests/testInstalledImageRoundTrip','PreviewTests/testInstalledImageBoundsAndCapacity'}
stage='runner-architecture'
RUNTIME='com.apple.CoreSimulator.SimRuntime.iOS-26-2'
DEVICE='com.apple.CoreSimulator.SimDeviceType.iPhone-17'

XCODE_STAGES={'sim-build','sim-test'}
DIAGNOSTICS=(
    ('missing-input',r'(?:error|fatal error):[^\n]*(?:file not found|no such file or directory|build input files? cannot be found)'),
    ('platform',r'(?:error|fatal error):[^\n]*(?:unable to find a destination|sdk [^\n]+ cannot be located|requires an arm64 simulator)|building for [^\n]+ but (?:linking in|built for)'),
    ('signing',r'(?:error|fatal error):[^\n]*(?:provisioning profile|signing certificate|code signing|codesign)|command codesign failed'),
    ('link',r'undefined symbols? for architecture|duplicate symbol|ld: (?:framework|library) [^\n]+ not found|linker command failed|command ld failed'),
    ('compile',r'command (?:compilec|swiftcompile) failed|:\d+:\d+: (?:fatal )?error:'),
    ('project',r'(?:error|fatal error):[^\n]*(?:project is damaged|unable to read project|invalid project|pif transfer)'),
    ('test-runner',r'(?:error|fatal error):[^\n]*(?:failed to launch|test runner|failed to prepare device)'),
)

def xcode_text(log):
    # Scan bounded head/tail only. Never expose the selected text.
    chunk=128*1024
    size=log.seek(0,2);log.seek(0)
    head=log.read(chunk);log.seek(max(len(head),size-chunk))
    return (head+b'\n'+log.read(chunk)).decode(errors='replace')

def xcode_category(log):
    raw=xcode_text(log)
    if any(MISSING_LIBRARY_LINE.fullmatch(line) for line in raw.splitlines()):return 'link'
    text=raw.lower()
    for category,pattern in DIAGNOSTICS:
        if re.search(pattern,text):return category
    return 'unclassified'

LINK_FAILURES=(
    ('platform-mismatch',r'building for [^\r\n]+ but (?:linking in|built for)|ld: [^\r\n]*(?:wrong architecture|incompatible architecture|invalid platform)'),
    ('debug-dylib',r'ld: [^\r\n]*(?:debug\.dylib|debug dylib)'),
    ('bundle-loader',r'ld: [^\r\n]*(?:bundle loader|bundle_loader|\bbundle_loader\b)'),
    ('framework-missing',r'ld: (?:framework not found|framework [^\r\n]+ not found)'),
    ('library-missing',r'ld: (?:library not found|library [^\r\n]+ not found)'),
    ('input-missing',r'ld: [^\r\n]*(?:file not found|no such file|cannot open|could not open)'),
    ('output-path',r'ld: [^\r\n]*(?:could not write|cannot write|unable to write|failed to create output|cannot create output)'),
    ('archive-format',r'ld: [^\r\n]*(?:archive member|malformed archive|not a mach-o|invalid archive)'),
    ('invalid-argument',r'(?:ld|clang(?:\+\+)?): [^\r\n]*(?:unknown option|unknown argument|invalid argument|unsupported option)'),
    ('duplicate-symbol',r'duplicate symbols?'),
    ('undefined-symbol',r'undefined symbols?(?: for architecture [a-z0-9_]+)?:|ld: symbols? not found'),
)

def xcode_link_reason(log):
    text=xcode_text(log).lower()
    for reason, pattern in LINK_FAILURES:
        if re.search(pattern,text):return reason
    return 'unknown'

MISSING_LIBRARY_LINE=re.compile(
    r"\s*ld: library (?:not found for (?P<for_name>[^\r\n]{1,1024})|(?P<name>[^\r\n]{1,1024}) not found)\s*")
LIBRARY_NAMES={
    'c++':'cxx','-lc++':'cxx','libc++.tbd':'cxx','libc++.dylib':'cxx',
    'objc':'objc','-lobjc':'objc','libobjc.tbd':'objc','libobjc.dylib':'objc',
    'libmadmedia_ios_image_preview.a':'preview-archive',
    'arclite':'arclite','-larclite':'arclite','libarclite.a':'arclite',
    'libarclite_iphonesimulator.a':'arclite','libarclite_iphoneos.a':'arclite',
}

def xcode_missing_library(log):
    labels=set()
    for line in xcode_text(log).splitlines():
        match=MISSING_LIBRARY_LINE.fullmatch(line)
        if match:
            name=(match['for_name'] or match['name']).strip()
            if len(name)>1 and name[0]==name[-1] and name[0] in ("'",'"'):
                name=name[1:-1]
            labels.add(LIBRARY_NAMES.get(name.rsplit('/',1)[-1],'other'))
    return next(iter(labels)) if len(labels)==1 else 'other'

def xcode_link_context(log):
    text=xcode_text(log)
    failure=text.rsplit('The following build commands failed:',1)
    targets=set()
    if len(failure)==2:
        for line in failure[1].splitlines():
            match=re.fullmatch(r"\s*Ld .{1,4096} \(in target '(PreviewApp|PreviewTests)' from project 'Preview'\)\s*",line)
            if match:targets.add(match[1])
    target={frozenset(('PreviewApp',)):'app',frozenset(('PreviewTests',)):'tests',
            frozenset(('PreviewApp','PreviewTests')):'both'}.get(frozenset(targets),'unknown')
    families=set()
    # Only process quoted symbol entries inside an actual undefined-symbol block.
    undefined=False
    for line in text.splitlines():
        if re.fullmatch(r'\s*(?:ld: )?Undefined symbols(?: for architecture [A-Za-z0-9_]+)?:\s*',line):
            undefined=True
            continue
        if not undefined:continue
        if line.lstrip().startswith(('ld:', 'clang:', 'clang++:', '**', 'The following')):
            undefined=False
            continue
        match=re.fullmatch(r'\s*"([^"\r\n]{1,512})", referenced from:\s*',line)
        if not match:continue
        symbol=match[1]
        if symbol in ('_madmedia_preview_roundtrip','_madmedia_preview_contract'):
            families.add('app-bridge')
        elif symbol=='_madmedia_ios_jpeg_resize_v1':
            families.add('installed-codec')
        elif symbol.startswith(('__Z','___cxa','std::','operator new','operator delete')):
            families.add('cxx-runtime')
        elif symbol.startswith(('_CF','_CG','_kCF','_kCG')):
            families.add('apple-image')
        elif symbol.startswith(('_XCT','_OBJC_CLASS_$_XCTest','__XCT')):
            families.add('xctest')
        else:families.add('other')
    family=next(iter(families)) if len(families)==1 else 'multiple' if families else 'unknown'
    return target,family

def run(*args,cwd=None,capture=False,timeout=120):
    if stage in XCODE_STAGES and args[0]=='xcodebuild':
        with tempfile.TemporaryFile() as log:
            try:
                subprocess.run(args,cwd=cwd,stdin=subprocess.DEVNULL,stdout=log,
                               stderr=subprocess.STDOUT,timeout=timeout,check=True)
            except subprocess.CalledProcessError:
                category=xcode_category(log)
                print('CATEGORY: '+stage+':'+category,flush=True)
                if category in ('link','platform','missing-input'):
                    target,family=xcode_link_context(log)
                    reason=xcode_link_reason(log)
                    if category=='link' or target!='unknown':
                        print('CATEGORY: '+stage+':link-target-'+target+':link-symbol-'+family,flush=True)
                        print('CATEGORY: '+stage+':link-reason-'+reason,flush=True)
                        if reason=='library-missing':
                            print('CATEGORY: '+stage+':link-library-'+xcode_missing_library(log),flush=True)
                raise
            except subprocess.TimeoutExpired:
                print('CATEGORY: '+stage+':timeout',flush=True)
                raise
            except OSError:
                print('CATEGORY: '+stage+':launch',flush=True)
                raise
        return None
    p=subprocess.run(args,cwd=cwd,stdin=subprocess.DEVNULL,
                     stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL,timeout=timeout,check=True)
    return p.stdout if capture else None


def verify(summary,tree):
    for k,v in dict(totalTestCount=2,passedTests=2,failedTests=0,skippedTests=0).items():
        if type(summary.get(k)) is not int or summary[k]!=v: raise ValueError()
    if summary.get('testStatus','Passed')!='Passed' or summary.get('expectedFailures',0)!=0: raise ValueError()
    seen=set()
    def walk(nodes):
        if not isinstance(nodes,list): raise ValueError()
        for n in nodes:
            if n.get('result','Passed')!='Passed': raise ValueError()
            if n.get('nodeType')=='Test Case':
                name=n['nodeIdentifier'].removesuffix('()')
                if name not in CASES or name in seen or n.get('result')!='Passed': raise ValueError()
                seen.add(name)
            walk(n.get('children',[]))
    walk(tree['testNodes'])
    if seen!=CASES: raise ValueError()

def install(root, work, sdk):
    source=work/'producer';source.mkdir()
    build=work/'build';build.mkdir()
    package=work/'prefix'
    header='include/madmedia/ios_image_preview.h'
    # Fixed source allowlist only. No root CMake or engine compile.
    for item in ('image.c',header):
        path=source/item;path.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(root/'ios-image-preview'/item,path)
    obj=build/'image.o';archive=build/'libmadmedia_ios_image_preview.a'
    run('xcrun','--sdk','iphonesimulator','clang','-target','arm64-apple-ios15.0-simulator',
        '-isysroot',sdk,'-std=c11','-Wall','-Wextra','-Werror','-I',source/'include','-c',source/'image.c','-o',obj)
    run('xcrun','libtool','-static','-o',archive,obj)
    if run('xcrun','lipo','-archs',archive,capture=True).strip()!=b'arm64': raise ValueError()
    if not re.search(rb'\bplatform\s+(?:IOSSIMULATOR|7)\b',run('xcrun','vtool','-show-build',obj,capture=True)):
        raise ValueError()
    (package/'lib').mkdir(parents=True)
    (package/header).parent.mkdir(parents=True)
    shutil.copyfile(archive,package/'lib'/archive.name)
    shutil.copyfile(source/header,package/header)
    consumer=work/'consumer';consumer.mkdir()
    shutil.move(package,consumer/'Installed')
    shutil.rmtree(source);shutil.rmtree(build)
    if source.exists() or build.exists() or package.exists(): raise ValueError()
    return consumer

class RuntimeRejected(ValueError):
    CODES={'timeout','query','json','absent'}

    def __init__(self, code):
        if code not in self.CODES: raise ValueError()
        self.code=code
        super().__init__(code)

def verify_runtime(document):
    if not isinstance(document,dict) or not isinstance(document.get('runtimes'),list):
        raise RuntimeRejected('json')
    if any(not isinstance(r,dict) or not isinstance(r.get('identifier'),str)
           for r in document['runtimes']):
        raise RuntimeRejected('json')
    matches=[r for r in document['runtimes'] if r['identifier']==RUNTIME]
    if len(matches)!=1 or matches[0].get('isAvailable') is not True:
        raise RuntimeRejected('absent')

def preflight_runtime():
    try:
        payload=run('xcrun','simctl','list','runtimes','available','-j',capture=True,timeout=120)
    except subprocess.TimeoutExpired:
        raise RuntimeRejected('timeout') from None
    except (subprocess.CalledProcessError,OSError):
        raise RuntimeRejected('query') from None
    try:
        document=json.loads(payload)
    except (ValueError,UnicodeError,RecursionError):
        raise RuntimeRejected('json') from None
    verify_runtime(document)


PARTIAL_LIMIT=256*1024
CASE_LABELS={
    'PreviewTests/testInstalledImageRoundTrip':'roundtrip',
    'PreviewTests/testInstalledImageBoundsAndCapacity':'bounds',
}
PARTIAL_OUTCOMES={'Passed':'passed','Failed':'failed','Skipped':'skipped'}

def owned_result(work):
    result=work/'result.xcresult'
    return (not work.is_symlink() and work.is_dir() and work.resolve()==work and
            not result.is_symlink() and result.is_dir() and result.resolve()==result)

def partial_json(result,key):
    if key not in ('summary','tests'):raise ValueError()
    with tempfile.TemporaryFile() as log:
        subprocess.run(['xcrun','xcresulttool','get','test-results',key,'--path',result,'--compact'],
                       stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.DEVNULL,
                       timeout=30,check=True)
        log.seek(0)
        data=log.read(PARTIAL_LIMIT+1)
    if len(data)>PARTIAL_LIMIT:raise ValueError()
    doc=json.loads(data)
    if not isinstance(doc,dict):raise ValueError()
    return doc

def partial_outcomes(summary,tree):
    keys=('totalTestCount','passedTests','failedTests','skippedTests')
    if any(type(summary.get(k)) is not int or not 0<=summary[k]<=100 for k in keys):
        raise ValueError()
    found={}
    pending=[tree.get('testNodes')]
    while pending:
        nodes=pending.pop()
        if not isinstance(nodes,list):raise ValueError()
        for node in nodes:
            if not isinstance(node,dict):raise ValueError()
            if node.get('nodeType')=='Test Case':
                name=node.get('nodeIdentifier')
                outcome=node.get('result')
                if not isinstance(name,str) or not isinstance(outcome,str):raise ValueError()
                name=name.removesuffix('()')
                if name not in CASE_LABELS or name in found or outcome not in PARTIAL_OUTCOMES:raise ValueError()
                found[name]=PARTIAL_OUTCOMES[outcome]
            pending.append(node.get('children',[]))
    return [(label,found.get(name,'absent')) for name,label in CASE_LABELS.items()]

ROUNDTRIP_REASONS=frozenset((
    'runtime','fixture-encode','input-decode-or-shape','input-artist-bytes','input-artist-property-mismatch',
    'high-codec','low-codec','high-pixels','high-decode-or-shape',
    'low-pixels','low-decode-or-shape','quality-response','no-upscale-codec',
    'no-upscale-pixels','no-upscale-decode-or-shape','output-artist','output-artist-invalid','output-artist-bytes',
))
ROUNDTRIP_MARKER=re.compile(r'\[MADMEDIA-ROUNDTRIP:([a-z-]{1,40})\]')

def roundtrip_reason(tree):
    # xcresulttool tests emits a named Failure Message node below the Test Case.
    # Do not inspect other test cases, arbitrary fields or raw transcripts.
    stack=list(tree['testNodes'])
    cases=[]
    while stack:
        node=stack.pop()
        if node.get('nodeType')=='Test Case':
            if node['nodeIdentifier'].removesuffix('()')=='PreviewTests/testInstalledImageRoundTrip':
                cases.append(node)
            continue
        stack.extend(node.get('children',[]))
    if len(cases)!=1 or cases[0].get('result')!='Failed':return 'unavailable'
    messages=[n.get('name') for n in cases[0].get('children',[]) if n.get('nodeType')=='Failure Message']
    if len(messages)!=1 or not isinstance(messages[0],str):return 'unavailable'
    # XCTest can prepend its own short assertion label. Match a single exact
    # bracketed marker and emit nothing from the surrounding message.
    markers=ROUNDTRIP_MARKER.findall(messages[0])
    if messages[0].count('[MADMEDIA-ROUNDTRIP:')!=1 or len(markers)!=1 or markers[0] not in ROUNDTRIP_REASONS:
        return 'unavailable'
    return markers[0]

def report_partial(work):
    # Read only the job-created expected path, before deleting simulator/temp.
    result=work/'result.xcresult'
    if not result.exists() and not result.is_symlink():
        print('RESULT: sim-test-partial:absent',flush=True)
        return
    try:
        if not owned_result(work):
            print('RESULT: sim-test-partial:unsafe',flush=True)
            return
        summary=partial_json(result,'summary'); tree=partial_json(result,'tests')
        outcomes=partial_outcomes(summary,tree)
        reason=roundtrip_reason(tree) if ('roundtrip','failed') in outcomes else None
    except Exception:
        print('RESULT: sim-test-partial:unreadable',flush=True)
        return
    print('RESULT: sim-test-partial:available',flush=True)
    for case,outcome in outcomes:
        print('RESULT: sim-test-case:'+case+':'+outcome,flush=True)
    if reason is not None:print('RESULT: sim-test-roundtrip-reason:'+reason,flush=True)

def main():
    global stage
    if platform.system()!='Darwin' or platform.machine()!='arm64': raise ValueError()
    stage='xcode-preflight'
    if run('xcodebuild','-version',capture=True,timeout=30).strip()!=b'Xcode 26.2\nBuild version 17C52': raise ValueError()
    stage='sdk-preflight'
    if run('xcrun','--sdk','iphonesimulator','--show-sdk-version',capture=True,timeout=30).strip()!=b'26.2': raise ValueError()
    sdk=run('xcrun','--sdk','iphonesimulator','--show-sdk-path',capture=True,timeout=30).decode().strip()
    stage='runtime-preflight'
    try: preflight_runtime()
    except RuntimeRejected as error:
        print('CATEGORY: runtime-preflight:'+error.code,flush=True)
        raise
    with tempfile.TemporaryDirectory(prefix='madmedia-image-') as tmp:
        work=Path(tmp).resolve()
        stage='install-relocate';app=install(ROOT,work,sdk)
        for src in ('App.mm','Tests.mm','fixture_artist_check.h'): shutil.copyfile(ROOT/'ios-image-preview'/src,app/src)
        project(app)
        stage='sim-create'
        uuid=run('xcrun','simctl','create','MadMediaImagePreview',
            DEVICE,RUNTIME,capture=True,timeout=60).decode().strip()
        if not re.fullmatch(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}',uuid): raise ValueError()
        try:
            stage='sim-boot'
            run('xcrun','simctl','boot',uuid,timeout=60)
            stage='sim-ready'
            run('xcrun','simctl','bootstatus',uuid,'-b',timeout=240)
            derived=work/'derived'
            common=['-project','Preview.xcodeproj','-scheme','Preview','-configuration','Debug','ARCHS=arm64',
                    'ONLY_ACTIVE_ARCH=YES','-parallel-testing-enabled','NO','-destination','platform=iOS Simulator,id='+uuid,
                    '-derivedDataPath',derived,'CODE_SIGNING_ALLOWED=YES','CODE_SIGN_IDENTITY=-','CODE_SIGNING_REQUIRED=NO']
            stage='sim-build'
            run('xcodebuild','build-for-testing',*common,'-sdk','iphonesimulator',cwd=app,timeout=420)
            binary=derived/'Build/Products/Debug-iphonesimulator/PreviewApp.app/PreviewApp'
            stage='app-linkage'
            if run('xcrun','lipo','-archs',binary,capture=True).strip()!=b'arm64': raise ValueError()
            symbols=run('xcrun','nm','-g','-U',binary,capture=True)
            if not re.search(rb'\b_madmedia_ios_jpeg_resize_v1$',symbols,re.M): raise ValueError()
            deps=run('xcrun','otool','-L',binary,capture=True)
            if b'/ImageIO.framework/' not in deps or b'/CoreGraphics.framework/' not in deps or re.search(rb'curl|sqlite|madmedia_core',deps,re.I): raise ValueError()
            stage='sim-test'
            result=work/'result.xcresult'
            if result.exists() or result.is_symlink():raise ValueError()
            try:
                run('xcodebuild','test-without-building',*common,'-resultBundlePath',result,cwd=app,timeout=420)
            except (subprocess.CalledProcessError,subprocess.TimeoutExpired):
                report_partial(work)
                raise
            stage='test-results'
            def results(key):
                return json.loads(run('xcrun','xcresulttool','get','test-results',key,'--path',result,'--compact',capture=True))
            verify(results('summary'),results('tests'))
        finally:
            run('xcrun','simctl','delete',uuid,timeout=90)
    print('PASS: exactly two installed ImageIO preview ARM64 Simulator tests')

if __name__=='__main__':
    try: main()
    except Exception:
        print('FAIL: iOS image preview stage='+stage)
        raise SystemExit(1) from None
