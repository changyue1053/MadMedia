"""Portable-only tests. Fake compiler outputs never qualify the native gate."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import check

class Tests(unittest.TestCase):
    def test_fixture_reuse_and_platform_scope(self):
        source=(check.ROOT/'ios-image-preview/App.mm').read_text()
        host=check.host_fixture(source)
        self.assertNotIn('UIKit',host)
        self.assertNotIn('intendedRuntime',host)
        self.assertIn('!TARGET_OS_OSX || !defined(__arm64__)',host)
        for predicate in ('high.length<=low.length','resize(input,128,95)',
                          'Synthetic fixture only','kCGImagePropertyGPSDictionary',
                          'MADMEDIA_IMAGE_V1_CAPACITY','sentinel==0x55'):
            self.assertIn(predicate,host)
        with self.assertRaises(ValueError):check.host_fixture(source.replace('if (!intendedRuntime())','if (false)'))

    def test_roundtrip_diagnostic_codes_remain_bounded_and_fail_closed(self):
        source=(check.ROOT/'ios-image-preview/App.mm').read_text()
        host=check.host_fixture(source)
        for code in range(10,21):
            self.assertEqual(host.count(f'return hostFail({code});'),1)
        for code,expected in ((12,'fixture-metadata'),(17,'quality-response'),
                              (33,'codec-size-query-decode'),(44,'codec-output-encode')):
            self.assertEqual(check.ROUNDTRIP_CODES[code],expected)
        self.assertIn('return madmedia_preview_roundtrip()?0:hostDiagnostic;',host)
        self.assertNotIn('printf',host)
        with self.assertRaises(ValueError):check.host_fixture(source.replace('high.length<=low.length','NO'))

    def test_verified_package_moves_before_consumer(self):
        def fake(*args,**kw):
            if '-o' in args:Path(args[args.index('-o')+1]).write_bytes(b'not native')
            if 'lipo' in args:return b'arm64\n'
            if 'vtool' in args:return b'platform MACOS\n'
        with tempfile.TemporaryDirectory() as temp,patch.object(check,'run',side_effect=fake):
            root=Path(temp);consumer=check.install(root,'MOCKSDK')
            self.assertEqual(list(root.iterdir()),[consumer])
            files={x.relative_to(consumer).as_posix() for x in consumer.rglob('*') if x.is_file()}
            self.assertEqual(files,{'Installed/include/madmedia/ios_image_preview.h','Installed/lib/'+check.ARCHIVE})

    def test_pin_drift_fails_before_compile(self):
        with tempfile.TemporaryDirectory() as temp,patch.object(check,'run') as run:
            root=Path(temp);bad=root/'bad-source';bad.mkdir()
            (bad/'image.c').write_text('modified')
            with self.assertRaises(ValueError):check.install(root,'MOCKSDK',bad)
            run.assert_not_called()

    def test_wrong_arch_or_platform_rejected(self):
        for outputs in ([b'x86_64'],[b'arm64',b'platform IOSSIMULATOR'],[b'arm64',b'platform MACOS\nplatform IOSSIMULATOR']):
            with patch.object(check,'run',side_effect=outputs),self.assertRaises(ValueError):
                check.check_macho(Path('not-an-artifact'))

    def test_workflow_uses_exact_host_branch_only(self):
        wf=(check.ROOT/'.github/workflows/apple-image-host-preview.yml').read_text()
        self.assertIn('branches: [checkpoint/madmedia-apple-image-host-preview]',wf)
        self.assertIn('ref: ${{ github.sha }}',wf)
        for disallowed in ('simctl','upload-artifact','workflow_dispatch','sudo'):
            self.assertNotIn(disallowed,wf)

if __name__=='__main__':unittest.main()
