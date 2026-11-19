"""Portable tests of exact-case proof and project isolation; not Apple evidence."""
import copy
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

from check import CASES, verify
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

def main():
    verify(SUMMARY,TREE)
    for key in ('passedTests','skippedTests','totalTestCount','failedTests','expectedFailures'):
        rejected(dict(SUMMARY,**{key:1}),TREE)
    rejected(dict(SUMMARY,testStatus='Failed'),TREE)
    rejected(SUMMARY,{'testNodes':[TREE['testNodes'][0],TREE['testNodes'][0]]})
    tree = copy.deepcopy(TREE)
    tree['testNodes'][0]['nodeIdentifier']='unexpected'
    rejected(SUMMARY,tree)
    with tempfile.TemporaryDirectory() as path:
        project(Path(path))
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
