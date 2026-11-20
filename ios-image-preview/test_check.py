"""Portable source, package layout and result state tests, not Apple execution."""
from pathlib import Path
import tempfile
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
            for disallowed in ('image.c','producer','sqlite','curl','Security'):
                self.assertNotIn(disallowed,pbx)

    def test_branch_scope(self):
        workflow=(check.ROOT/'.github/workflows/ios-image-preview.yml').read_text()
        self.assertIn('branches: [checkpoint/madmedia-ios-image-preview]',workflow)
        self.assertIn('ref: ${{ github.sha }}',workflow)
        for operation in ('upload-artifact','workflow_dispatch','pull_request','sudo','curl'):
            self.assertNotIn(operation,workflow)

if __name__=='__main__':unittest.main()
