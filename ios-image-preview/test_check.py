"""Portable source, package layout and result state tests, not Apple execution."""
from pathlib import Path
import tempfile
import io
import json
import re
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
            archive='Installed/lib/libmadmedia_ios_image_preview.a'
            ref=re.search(r'"([A-F0-9]{24})" = \{ "isa" = "PBXFileReference"; "path" = "'+re.escape(archive)+r'";',pbx)
            self.assertIsNotNone(ref)
            self.assertNotIn('"fileRef" = "'+ref[1]+'";',pbx)
            flags=[json.loads('['+x+']') for x in re.findall(r'"OTHER_LDFLAGS" = \(([^;]*)\);',pbx)]
            self.assertEqual(len(flags),2)
            self.assertEqual(flags[0][1],'"$(PROJECT_DIR)/'+archive+'"')
            self.assertNotIn(archive,str(flags[1]))
            self.assertNotIn('LIBRARY_SEARCH_PATHS',pbx)
            self.assertIn('Installed/include',pbx)
            self.assertIn('"CoreFoundation"',pbx)
            self.assertEqual(pbx.count('"-lc++"'),1)
            self.assertIn('"CLANG_CXX_LIBRARY" = "libc++";',pbx)
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


    def test_link_context_reports_only_fixed_target_and_symbol_family(self):
        def log(symbol, target):
            return (f'Undefined symbols for architecture arm64:\n  "{symbol}", referenced from:\n'
                    '    /private/symbol-reference\nld: symbol(s) not found for architecture arm64\n'
                    'The following build commands failed:\n'
                    f"  Ld /private/Derived ARM64 (in target '{target}' from project 'Preview')\n").encode()
        for symbol,target,want in (
            ('_madmedia_preview_roundtrip','PreviewTests',('tests','app-bridge')),
            ('_madmedia_ios_jpeg_resize_v1','PreviewApp',('app','installed-codec')),
            ('std::__1::bad_alloc','PreviewApp',('app','cxx-runtime')),
            ('_CGImageSourceCreateWithData','PreviewApp',('app','apple-image')),
            ('_OBJC_CLASS_$_XCTestCase','PreviewTests',('tests','xctest')),
            ('_private_symbol','Foreign',('unknown','other'))):
            self.assertEqual(check.xcode_link_context(io.BytesIO(log(symbol,target))),want)
        self.assertEqual(check.xcode_link_context(io.BytesIO(
            b"Ld earlier (in target 'PreviewApp' from project 'Preview')\n")),
            ('unknown','unknown'))
        captured=io.StringIO()
        def fail(*args,**kwargs):
            kwargs['stdout'].write(log('_madmedia_preview_contract','PreviewTests'))
            raise subprocess.CalledProcessError(65,['not-public'])
        with patch.object(check,'stage','sim-build'),patch.object(check.subprocess,'run',side_effect=fail),contextlib.redirect_stdout(captured):
            with self.assertRaises(subprocess.CalledProcessError):
                check.run('xcodebuild','build-for-testing')
        self.assertEqual(captured.getvalue(),
            'CATEGORY: sim-build:link\nCATEGORY: sim-build:link-target-tests:link-symbol-app-bridge\n'
            'CATEGORY: sim-build:link-reason-undefined-symbol\n')

    def test_fixed_link_reasons(self):
        for line,expected in (
            ('ld: framework not found /private/name', 'framework-missing'),
            ('ld: library not found for /private/name', 'library-missing'),
            ('ld: file not found: /private/archive', 'input-missing'),
            ('ld: could not write output /private/path', 'output-path'),
            ('ld: unknown option /private/flag', 'invalid-argument'),
            ('ld: invalid archive /private/archive', 'archive-format'),
            ('ld: /private/test.debug.dylib file not found', 'debug-dylib'),
            ('ld: cannot open bundle loader /private/app', 'bundle-loader'),
            ('ld: building for iOS Simulator, but linking in /private/archive', 'platform-mismatch'),
            ('Undefined symbols:', 'undefined-symbol'),
            ('clang: error: linker command failed with exit code 1', 'unknown'),
        ):
            self.assertEqual(check.xcode_link_reason(io.BytesIO(line.encode())),expected)
        log=b'Undefined symbols:\n  "std::__1::bad_alloc", referenced from:\n'
        self.assertEqual(check.xcode_link_context(io.BytesIO(log)),('unknown','cxx-runtime'))

    def test_missing_library_allowlist_never_emits_names(self):
        for line,expected in (
            ("ld: library 'c++' not found",'cxx'),
            ('ld: library not found for -lc++','cxx'),
            ('ld: library "/private dir/libmadmedia_ios_image_preview.a" not found','preview-archive'),
            ('ld: library not found for /private/libobjc.tbd','objc'),
            ('ld: library not found for /private/libarclite_iphonesimulator.a','arclite'),
            ('ld: library not found for /private/libc++_arbitrary.tbd','other'),
            ('xcode printed ld: library not found for -lc++','other'),
            ('ld: library not found for -lc++\nld: library not found for -lobjc','other')):
            self.assertEqual(check.xcode_missing_library(io.BytesIO(line.encode())),expected)
        def fail(*args,**kwargs):
            kwargs['stdout'].write(b'ld: library not found for /private/unreleased-name\n')
            raise subprocess.CalledProcessError(65,['private'])
        output=io.StringIO()
        with patch.object(check,'stage','sim-build'),patch.object(check.subprocess,'run',side_effect=fail),contextlib.redirect_stdout(output):
            with self.assertRaises(subprocess.CalledProcessError):
                check.run('xcodebuild','build-for-testing')
        self.assertIn('CATEGORY: sim-build:link-library-other\n',output.getvalue())
        self.assertNotIn('unreleased-name',output.getvalue())

    def test_seeded_input_artist_absent_in_all_outputs(self):
        fixture=(check.ROOT/'ios-image-preview/App.mm').read_text()
        self.assertIn('if (![before[tiff][artist] isEqual:@"Synthetic fixture only"]) return NO;',fixture)
        self.assertIn('return !after[tiff][artist] && !afterLow[tiff][artist] && !afterSame[tiff][artist];',fixture)
        for unseeded in ('GPS','Exif','IPTC'):
            self.assertNotIn(f'kCGImageProperty{unseeded}Dictionary',fixture)

    def test_partial_owned_result_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            owner=Path(tmp).resolve()
            result=owner/'result.xcresult'
            other=owner/'unrelated.xcresult';other.mkdir()
            self.assertFalse(check.owned_result(owner))
            result.symlink_to(other,target_is_directory=True)
            self.assertFalse(check.owned_result(owner))
            result.unlink();result.touch()
            self.assertFalse(check.owned_result(owner))
            result.unlink();result.mkdir()
            self.assertTrue(check.owned_result(owner))
            alias=owner/'alias';alias.symlink_to(owner,target_is_directory=True)
            self.assertFalse(check.owned_result(alias))
            with patch.object(check,'partial_json') as reader,contextlib.redirect_stdout(io.StringIO()) as out:
                check.report_partial(alias)
            reader.assert_not_called()
            self.assertEqual(out.getvalue(),'RESULT: sim-test-partial:unsafe\n')

    def test_partial_reads_and_labels(self):
        summary=dict(totalTestCount=2,passedTests=1,failedTests=1,skippedTests=0)
        first,second=check.CASE_LABELS
        def node(name,outcome):return dict(nodeType='Test Case',nodeIdentifier=name,result=outcome)
        cases=[node(first+'()','Passed'),node(second,'Failed')]
        self.assertEqual(check.partial_outcomes(summary,{'testNodes':cases}),[('roundtrip','passed'),('bounds','failed')])
        self.assertEqual(check.partial_outcomes(summary,{'testNodes':[node(second,'Skipped')]}),[('roundtrip','absent'),('bounds','skipped')])
        for malformed in ([node(first,'private')],[node('unexpected/path','Passed')],
                          [node(first,'Passed'),node(first+'()','Passed')],['broken']):
            with self.assertRaises(ValueError):check.partial_outcomes(summary,{'testNodes':malformed})
        with self.assertRaises(ValueError):check.partial_outcomes({}, {'testNodes':cases})
        with tempfile.TemporaryDirectory() as tmp:
            owner=Path(tmp).resolve();(owner/'result.xcresult').mkdir()
            with patch.object(check,'partial_json',side_effect=[summary,{'testNodes':cases}]),contextlib.redirect_stdout(io.StringIO()) as out:
                check.report_partial(owner)
            self.assertEqual(out.getvalue(),'RESULT: sim-test-partial:available\nRESULT: sim-test-case:roundtrip:passed\nRESULT: sim-test-case:bounds:failed\n')

    def test_partial_json_is_bounded_and_object_only(self):
        for payload,good in ((b'{"testNodes":[]}',True),(b'[' ,False),(b'[]',False),
                             (b' '*check.PARTIAL_LIMIT+b'{}',False)):
            def command(*args,**kw):kw['stdout'].write(payload)
            with patch.object(check.subprocess,'run',side_effect=command):
                if good:self.assertEqual(check.partial_json(Path('fixture'),'tests'),{'testNodes':[]})
                else:
                    with self.assertRaises(ValueError):check.partial_json(Path('fixture'),'tests')

    def test_branch_scope(self):
        workflow=(check.ROOT/'.github/workflows/ios-image-preview.yml').read_text()
        self.assertIn('branches: [checkpoint/madmedia-ios-image-preview]',workflow)
        self.assertIn('ref: ${{ github.sha }}',workflow)
        for operation in ('upload-artifact','workflow_dispatch','pull_request','sudo','curl'):
            self.assertNotIn(operation,workflow)

if __name__=='__main__':unittest.main()
