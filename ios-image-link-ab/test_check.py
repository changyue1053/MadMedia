"""Structural/synthetic tests only, no SDK/tool execution."""
import io
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import check

class Tests(unittest.TestCase):
    def test_variants_differ_only_in_link_representation(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            texts=[]
            for mode in ('pbx','explicit'):
                path=root/mode;path.mkdir()
                check.configure(path,mode=='explicit')
                project=path/'Preview.xcodeproj'
                scheme=ET.parse(project/'xcshareddata/xcschemes/Preview.xcscheme')
                self.assertFalse(scheme.findall('TestAction'))
                refs=scheme.findall('./BuildAction/BuildActionEntries/BuildActionEntry/BuildableReference')
                self.assertEqual([ref.get('BlueprintName') for ref in refs],['PreviewApp'])
                texts.append((project/'project.pbxproj').read_text())
            pbx,explicit=texts
            flags=r'"OTHER_LDFLAGS" = (\([^;]*\));'
            original=json.loads('['+re.search(flags,pbx)[1][1:-1]+']')
            alternate=json.loads('['+re.search(flags,explicit)[1][1:-1]+']')
            self.assertEqual(alternate,[original[0],'"$(PROJECT_DIR)/'+check.ARCHIVE+'"',*original[1:]])
            build_id=re.search(r'"files" = \("([A-F0-9]{24})"\); "buildActionMask" = "2147483647"; "runOnlyForDeploymentPostprocessing" = "0";',pbx)[1]
            # Reconstruct the exact alternate from the two isolated edits,
            # using the unique archive build phase (not app/test source phases).
            phase=r'("isa" = "PBXFrameworksBuildPhase"; "files" = )\("([A-F0-9]{24})"\)'
            match=re.search(phase,pbx)
            self.assertIsNotNone(match)
            self.assertNotEqual(build_id,match[2])
            expected=pbx[:match.start()]+match[1]+'()'+pbx[match.end():]
            expected=expected.replace(re.search(flags,pbx)[0],re.search(flags,explicit)[0],1)
            self.assertEqual(explicit,expected)

    def test_matrix(self):
        def result(m,p,e):return check.verdict(dict(manual=m,pbx=p,explicit=e))
        self.assertEqual(result('verified','library-missing-preview-archive','verified'),'confirmed-archive-resolution')
        self.assertEqual(result('verified','verified','verified'),'all-link-not-reproduced')
        self.assertEqual(result('verified','library-missing-other','verified'),'pbx-only-failure-not-attributed')
        self.assertEqual(result('verified','timeout','verified'),'inconclusive')
        self.assertEqual(result('launch','verified','verified'),'manual-not-proven')
        self.assertEqual(result('verified','verified','artifact-invalid'),'explicit-project-not-proven')
        self.assertEqual(check.verdict({}),'incomplete')

    def test_missing_library_is_fixed_and_exact(self):
        for raw,expected in ((b'ld: library not found for -lmadmedia_ios_image_preview','library-missing-preview-archive'),
                             (b'ld: library not found for -lmadmedia_ios_image_preview_private','library-missing-other'),
                             (b'ld: library not found for -lmadmedia_ios_image_preview\nld: library not found for -lprivate','library-missing-other')):
            def fail(*args,**kwargs):
                kwargs['stdout'].write(raw)
                raise subprocess.CalledProcessError(1,['private'])
            with patch.object(check.subprocess,'run',side_effect=fail):
                self.assertEqual(check.guarded(['not-executed'],Path('.'),1),expected)

    def test_artifact_proof(self):
        good=[b'arm64\n',b'    platform IOSSIMULATOR\n',b'0 T _madmedia_ios_jpeg_resize_v1\n',
              b'/System/Library/Frameworks/ImageIO.framework/ImageIO\n/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics\n']
        with patch.object(check.base,'run',side_effect=good):check.prove(Path('fake'))
        for i,value in enumerate((b'x86_64',b'platform MACOS',b'',b'')):
            bad=good.copy();bad[i]=value
            with patch.object(check.base,'run',side_effect=bad),self.assertRaises(ValueError):
                check.prove(Path('fake'))

if __name__=='__main__':unittest.main()
