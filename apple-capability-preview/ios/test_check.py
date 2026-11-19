"""Portable tests of exact-case proof and project isolation; not Apple evidence."""
import contextlib
import copy
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from unittest.mock import patch

from check import (CASES, DIAGNOSTIC_LIMIT, run, stage_sources, verify,
                   verify_local_includes, xcode_category)
from project import project

SUMMARY = dict(totalTestCount=2,passedTests=2,failedTests=0,skippedTests=0)
TREE = {'testNodes':[{'nodeType':'Test Case','result':'Passed','nodeIdentifier':case+'()'}
                    for case in sorted(CASES)]}

def rejected(summary,tree):
    try:
        verify(summary,tree)
    except (ValueError,KeyError):
        return
    raise AssertionError('unexpected acceptance')

def diagnostics():
    leak = '/private/input-must-never-appear'
    fixtures = {
        'missing-input': f'{leak}:1:5: fatal error: {leak} file not found',
        'missing-module': f'{leak}:4:2: error: no such module {leak}',
        'platform': f'ld: building for iOS Simulator, but linking in {leak}',
        'signing': f'error: No signing certificate {leak} found',
        'link': f'Undefined symbols for architecture arm64:\n{leak}',
        'compile': (f'Ld {leak} (in target X)\n'
                    f'{leak}:9:26: error: invalid argument type'),
        'project': f'error: project is damaged {leak}',
        'test-runner': f'error: failed to launch {leak}',
        'unclassified': f'Ld {leak} (in target X)\nCompileC {leak} (in target X)',
    }
    for label, message in fixtures.items():
        with tempfile.TemporaryFile() as log:
            log.write(b'x'*DIAGNOSTIC_LIMIT)
            log.write(("\n"+message).encode())
            assert xcode_category(log)==label
    with tempfile.TemporaryFile() as log:
        log.write((f'CompileC {leak} (in target X)\nThe following build commands failed:\n'
                   f'    Ld {leak} (in target X)\n(1 failure)').encode())
        assert xcode_category(log)=='link'
    def fail(args,**kwargs):
        assert kwargs['stderr']==subprocess.STDOUT
        kwargs['stdout'].write(fixtures['missing-input'].encode())
        raise subprocess.CalledProcessError(65,args)
    out = io.StringIO()
    with patch('check.subprocess.run',side_effect=fail), contextlib.redirect_stdout(out):
        try:
            run('device-build',['xcodebuild',leak])
        except subprocess.CalledProcessError:
            pass
        else:
            raise AssertionError('expected build failure')
    assert out.getvalue()==('START: device-build\nFAIL: device-build\n'
                            'CATEGORY: device-build:missing-input\n')

def main():
    diagnostics()
    verify(SUMMARY,TREE)
    for key in ('passedTests','skippedTests','totalTestCount','failedTests','expectedFailures'):
        rejected(dict(SUMMARY,**{key:1}),TREE)
    rejected(dict(SUMMARY,testStatus='Failed'),TREE)
    rejected(SUMMARY,{'testNodes':[TREE['testNodes'][0],TREE['testNodes'][0]]})
    tree = copy.deepcopy(TREE)
    tree['testNodes'][0]['nodeIdentifier']='unexpected'
    rejected(SUMMARY,tree)
    with tempfile.TemporaryDirectory() as path:
        root = Path(__file__).resolve().parents[2]
        app = Path(path)
        stage_sources(root,app)
        canonical = (root/'core/platform/entropy.h').read_bytes()
        assert (app/'entropy.h').read_bytes()==canonical
        assert (app/'entropy_apple.cpp').read_bytes()==(root/'ffi/platform/entropy_apple.cpp').read_bytes()
        # Regression: the forwarding header cannot resolve from a flat copy.
        staged = app/'entropy.h'
        staged.write_bytes((root/'ffi/platform/entropy.h').read_bytes())
        try:
            verify_local_includes(app)
        except ValueError:
            pass
        else:
            raise AssertionError('escaping source include accepted')
        staged.write_bytes(canonical)
        project(app)
        pbx = (Path(path)/'Preview.xcodeproj/project.pbxproj').read_text()
        scheme = ET.parse(Path(path)/'Preview.xcodeproj/xcshareddata/xcschemes/Preview.xcscheme')
        assert (('entropy_apple.cpp' in pbx) and ('"Security"' in pbx) and
                ('"BUNDLE_LOADER"' in pbx) and ('"DEAD_CODE_STRIPPING" = "NO"' in pbx) and
                (path not in pbx) and ('ffi/' not in pbx))
        assert len(scheme.findall('.//TestableReference'))==1
    if sys.platform!='darwin':
        result = subprocess.run([sys.executable,str(Path(__file__).with_name('check.py'))],
                                capture_output=True,text=True,check=False,timeout=10)
        assert result.returncode!=0 and result.stdout=='FAIL: iOS capability validation\n'
    print('PASS: isolated App configuration and strict XCTest proof gates')

if __name__=='__main__':
    main()
