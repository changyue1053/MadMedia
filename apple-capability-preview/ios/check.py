"""Standalone app-private iOS capability qualification. No engine or network."""
import json
import platform
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from project import project

CASES = {'MadMediaCapabilityTests/testAppPrivateAdvisoryLease',
         'MadMediaCapabilityTests/testActualAppleEntropy'}
STAGES = {'xcode', 'sdk-simulator', 'sdk-device', 'device-build', 'device-arch',
          'sim-create', 'sim-boot', 'sim-ready', 'sim-build', 'sim-arch',
          'sim-test', 'summary', 'test-tree', 'sim-delete'}

XCODE_STAGES = {'device-build', 'sim-build', 'sim-test'}
DIAGNOSTIC_LIMIT = 256 * 1024
# Order denotes cause specificity. Never return matched substrings or command text.
DIAGNOSTICS = {
    'missing-input': (r'(?:error|fatal error): [^\n]*(?:file not found|no such file or directory|build input files? cannot be found)',),
    'missing-module': (r'(?:error|fatal error): [^\n]*(?:no such module|could not build module|module [^\n]+ not found|missing required module)',),
    'platform': (r'(?:error|fatal error): [^\n]*(?:unable to find a destination|no destinations|sdk [^\n]+ cannot be located|deployment target|not available on ios)',
                 r'building for [^\n]+ but (?:linking in|built for)'),
    'signing': (r'(?:error|fatal error): [^\n]*(?:provisioning profiles?|signing certificate|code signing|requires a development team|codesign)',
                r'command codesign failed'),
    'link': (r'undefined symbols? for architecture|duplicate symbol|ld: (?:framework|library) [^\n]+ not found',
             r'linker command failed|command ld failed'),
    'compile': (r'command (?:compilec|swiftcompile) failed',
                r':\d+:\d+: (?:fatal )?error:'),
    'project': (r'(?:error|fatal error): [^\n]*(?:project is damaged|unable to read project|cannot open|invalid project|pif transfer)',
                r'failed to load project'),
    'test-runner': (r'(?:error|fatal error): [^\n]*(?:failed to launch|test runner|failed to prepare device)',),
}

def xcode_category(log):
    # Bound both byte reads and decoding regardless of Xcode output size.
    half = DIAGNOSTIC_LIMIT // 2
    end = log.seek(0,2)
    log.seek(0)
    head = log.read(half)
    log.seek(max(len(head),end-half))
    tail = log.read(half)
    message = (head+b'\n'+tail).decode('utf-8',errors='replace').lower()
    for label, patterns in DIAGNOSTICS.items():
        if any(re.search(pattern,message,re.MULTILINE) for pattern in patterns):
            return label
    # An ordinary transcript contains successful CompileC/Ld commands too.
    # Consult command verbs only inside Xcode's explicit failure summary.
    summary = message.rsplit('the following build commands failed:',1)
    if len(summary)==2:
        candidates = {'compile':r'^[ \t]*(?:compilec|swiftcompile)\s',
                      'link':r'^[ \t]*ld\s',
                      'signing':r'^[ \t]*codesign\s'}
        for label, pattern in candidates.items():
            if re.search(pattern,summary[1],re.MULTILINE):
                return label
    return 'unclassified'

def run(stage, args, cwd=None, capture=False, timeout=420):
    if stage not in STAGES:
        raise ValueError()
    print('START: '+stage, flush=True)
    with tempfile.TemporaryFile() as log:
        try:
            subprocess.run([str(arg) for arg in args], cwd=cwd, stdin=subprocess.DEVNULL,
                           stdout=log, stderr=(subprocess.STDOUT if stage in XCODE_STAGES else subprocess.DEVNULL),
                           check=True, timeout=timeout)
            if capture:
                log.seek(0)
                value = log.read(262145)
                if len(value) > 262144:
                    raise ValueError()
                result = value.decode('utf-8').strip()
            else:
                result = None
        except Exception as error:
            print('FAIL: '+stage, flush=True)
            if stage in XCODE_STAGES:
                if isinstance(error,subprocess.TimeoutExpired):
                    label = 'timeout'
                elif isinstance(error,OSError):
                    label = 'launch'
                else:
                    label = xcode_category(log)
                print('CATEGORY: '+stage+':'+label, flush=True)
            raise
    print('PASS: '+stage, flush=True)
    return result

def verify(summary, tests):
    for key, count in {'totalTestCount':2,'passedTests':2,'failedTests':0,'skippedTests':0}.items():
        if type(summary.get(key)) is not int or summary[key]!=count:
            raise ValueError()
    if summary.get('testStatus','Passed')!='Passed' or summary.get('expectedFailures',0)!=0:
        raise ValueError()
    found = set()
    def visit(nodes):
        if not isinstance(nodes,list):
            raise ValueError()
        for node in nodes:
            if node.get('result','Passed')!='Passed':
                raise ValueError()
            if node.get('nodeType')=='Test Case':
                name = node['nodeIdentifier'].removesuffix('()')
                if name not in CASES or name in found or node.get('result')!='Passed':
                    raise ValueError()
                found.add(name)
            visit(node.get('children',[]))
    visit(tests['testNodes'])
    if found!=CASES:
        raise ValueError()

def main():
    if platform.system()!='Darwin' or platform.machine()!='arm64':
        raise ValueError()
    if run('xcode',['xcodebuild','-version'],capture=True,timeout=30).splitlines()[0]!='Xcode 26.2':
        raise ValueError()
    for stage,sdk in [('sdk-simulator','iphonesimulator'),('sdk-device','iphoneos')]:
        if run(stage,['xcrun','--sdk',sdk,'--show-sdk-version'],capture=True,timeout=30)!='26.2':
            raise ValueError()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='madmedia-ios-') as work:
        temporary = Path(work)
        app = temporary/'independent preview'
        app.mkdir()
        for relative in ('apple-capability-preview/ios/App.mm','apple-capability-preview/ios/Tests.mm',
                         'apple-capability-preview/lease_fixture.h','apple-capability-preview/lease.cpp',
                         'ffi/platform/entropy.h','ffi/platform/entropy_apple.cpp'):
            shutil.copyfile(root/relative,app/Path(relative).name)
        project(app)
        base = ['-project','Preview.xcodeproj','-configuration','Debug','ARCHS=arm64',
                'ONLY_ACTIVE_ARCH=YES']
        device = temporary/'device'
        run('device-build',['xcodebuild','build',*base,'-target','PreviewApp','-sdk','iphoneos',
                            'CONFIGURATION_BUILD_DIR='+str(device),'CODE_SIGNING_ALLOWED=NO',
                            'CODE_SIGNING_REQUIRED=NO'],cwd=app)
        if run('device-arch',['xcrun','lipo','-archs',device/'PreviewApp.app/PreviewApp'],
               capture=True,timeout=30)!='arm64':
            raise ValueError()
        print('PASS: unsigned ARM64 iOS App build and link only; device not run',flush=True)
        udid = run('sim-create',['xcrun','simctl','create','MadMediaCapability',
                   'com.apple.CoreSimulator.SimDeviceType.iPhone-17',
                   'com.apple.CoreSimulator.SimRuntime.iOS-26-2'],capture=True,timeout=60)
        if not re.fullmatch(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}',udid):
            raise ValueError()
        try:
            run('sim-boot',['xcrun','simctl','boot',udid],timeout=60)
            run('sim-ready',['xcrun','simctl','bootstatus',udid,'-b'],timeout=240)
            derived = temporary/'derived'
            common = base+['-scheme','Preview','-destination','platform=iOS Simulator,id='+udid,
                           '-derivedDataPath',derived,'-parallel-testing-enabled','NO',
                           'CODE_SIGNING_ALLOWED=YES','CODE_SIGN_IDENTITY=-','CODE_SIGNING_REQUIRED=NO']
            run('sim-build',['xcodebuild','build-for-testing',*common],cwd=app)
            binary = derived/'Build/Products/Debug-iphonesimulator/PreviewApp.app/PreviewApp'
            if run('sim-arch',['xcrun','lipo','-archs',binary],capture=True,timeout=30)!='arm64':
                raise ValueError()
            result = temporary/'sim.xcresult'
            run('sim-test',['xcodebuild','test-without-building',*common,'-resultBundlePath',result],cwd=app)
            args = ['--path',result,'--compact']
            summary = json.loads(run('summary',['xcrun','xcresulttool','get','test-results','summary',*args],
                                     capture=True,timeout=45))
            tree = json.loads(run('test-tree',['xcrun','xcresulttool','get','test-results','tests',*args],
                                  capture=True,timeout=45))
            verify(summary,tree)
        finally:
            run('sim-delete',['xcrun','simctl','delete',udid],timeout=60)
        print('PASS: exactly 2 real ARM64 iOS Simulator App capability tests',flush=True)

if __name__=='__main__':
    try:
        main()
    except Exception:
        print('FAIL: iOS capability validation',flush=True)
        raise SystemExit(1) from None
