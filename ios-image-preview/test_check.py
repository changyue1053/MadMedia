"""Portable source, package layout and result state tests, not Apple execution."""
from pathlib import Path
import tempfile
import io
import json
import contextlib
import subprocess
import unittest
from unittest.mock import patch
import check

class Tests(unittest.TestCase):
    def test_exact_named_results(self):
        counts=dict(totalTestCount=2,passedTests=2,failedTests=0,skippedTests=0)
        cases=[dict(nodeType='Test Case',nodeIdentifier=n,result='Passed') for n in sorted(check.CASES)]
        check.verify(counts,{'testNodes':cases})
        for bad in ([],cases[:1],cases+[cases[0]],cases+[dict(cases[0],nodeIdentifier='unknown')]):
            with self.assertRaises(ValueError):check.verify(counts,{'testNodes':bad})
        for field in counts:
            with self.assertRaises(ValueError):check.verify(dict(counts,**{field:1}),{'testNodes':cases})

    def test_relocated_two_file_install_and_producer_removed(self):
        def fake(*cmd,**kwargs):
            if 'clang' in cmd or 'libtool' in cmd:
                Path(cmd[cmd.index('-o')+1]).write_bytes(b'MOCK ONLY')
            if 'lipo' in cmd:return b'arm64\n'
            if 'vtool' in cmd:return b'platform IOSSIMULATOR\n'
        with tempfile.TemporaryDirectory() as tmp,patch.object(check,'run',side_effect=fake) as spy:
            work=Path(tmp)
            app=check.install(check.ROOT,work,'MOCKSDK')
            self.assertEqual(list(work.iterdir()),[app])
            files={p.relative_to(app).as_posix() for p in app.rglob('*') if p.is_file()}
            self.assertEqual(files,{'Installed/include/madmedia/ios_image_preview.h','Installed/lib/libmadmedia_ios_image_preview.a'})
            self.assertEqual(spy.call_count,4)
            self.assertNotIn('cmake',str(spy.call_args_list))

    def test_consumer_uses_installed_archive_not_producer(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);check.project(root)
            pbx=(root/'Preview.xcodeproj/project.pbxproj').read_text()
            self.assertEqual(pbx.count('Installed/lib/libmadmedia_ios_image_preview.a'),1)
            self.assertIn('Installed/include',pbx)
            self.assertIn('"CoreFoundation"',pbx)
            for disallowed in ('image.c','producer','sqlite','curl','Security'):
                self.assertNotIn(disallowed,pbx)

    def test_exact_runtime_and_app_architecture_qualification(self):
        exact=dict(identifier=check.RUNTIME,isAvailable=True)
        check.verify_runtime({'runtimes':[exact]})
        for runtimes in ([],[exact,exact],[dict(exact,isAvailable=False)],
                         [dict(exact,identifier=check.RUNTIME+'-wrong')]):
            with self.assertRaises(ValueError):check.verify_runtime({'runtimes':runtimes})
        source=(check.ROOT/'ios-image-preview/App.mm').read_text()
        self.assertIn('#if !TARGET_OS_SIMULATOR || !defined(__arm64__)',source)
        self.assertEqual(source.count('if (!intendedRuntime()) return NO;'),2)
        self.assertIn('version.majorVersion==26 && version.minorVersion==2',source)
        runner=(check.ROOT/'ios-image-preview/check.py').read_text()
        self.assertNotIn("'spawn'",runner)
        self.assertIn("stage='sim-ready'",runner)
        self.assertIn("'simctl','boot',uuid,timeout=60",runner)
        self.assertIn("'simctl','bootstatus',uuid,'-b',timeout=240",runner)

    def test_runtime_query_categories_and_single_call(self):
        good=json.dumps({'runtimes':[dict(identifier=check.RUNTIME,isAvailable=True)]}).encode()
        with patch.object(check,'run',return_value=good) as query:
            check.preflight_runtime()
            query.assert_called_once_with('xcrun','simctl','list','runtimes','available','-j',
                                          capture=True,timeout=120)
        for reason,payload,error in (('timeout',None,subprocess.TimeoutExpired(['redacted'],120)),
                ('query',None,subprocess.CalledProcessError(1,['redacted'])),
                ('query',None,FileNotFoundError('redacted')),('json',b'not json',None),
                ('json',b'{"runtimes":[12]}',None),('absent',b'{"runtimes":[]}',None)):
            with patch.object(check,'run',return_value=payload,side_effect=error) as query:
                with self.assertRaises(check.RuntimeRejected) as caught: check.preflight_runtime()
                self.assertEqual(caught.exception.code,reason)
                self.assertEqual(query.call_count,1)

    def test_xcode_categories_do_not_echo_transcript(self):
        fixtures=(('error: /not-public: file not found','missing-input'),
                  ('building for ios simulator but linking in object','platform'),
                  ('ld: framework not-public not found','link'),
                  ('/not-public:12:3: error: error description','compile'),
                  ('routine CompileC and Ld lines /not-public','unclassified'))
        for line,expected in fixtures:
            self.assertEqual(check.xcode_category(io.BytesIO(line.encode())),expected)
        self.assertEqual(check.xcode_category(io.BytesIO(b'x'*400000+
                         b'\nundefined symbols for architecture arm64\n')),'link')
        def fail(*args,**kwargs):
            kwargs['stdout'].write(b'/not-public:12:3: error: unreleased description')
            raise subprocess.CalledProcessError(65,['not-public'])
        captured=io.StringIO()
        with patch.object(check,'stage','sim-build'),patch.object(check.subprocess,'run',side_effect=fail),contextlib.redirect_stdout(captured):
            with self.assertRaises(subprocess.CalledProcessError):
                check.run('xcodebuild','build-for-testing')
        self.assertEqual(captured.getvalue(),'CATEGORY: sim-build:compile\n')


    def test_branch_scope(self):
        workflow=(check.ROOT/'.github/workflows/ios-image-preview.yml').read_text()
        self.assertIn('branches: [checkpoint/madmedia-ios-image-preview]',workflow)
        self.assertIn('ref: ${{ github.sha }}',workflow)
        for operation in ('upload-artifact','workflow_dispatch','pull_request','sudo','curl'):
            self.assertNotIn(operation,workflow)

if __name__=='__main__':unittest.main()
