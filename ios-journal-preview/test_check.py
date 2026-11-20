"""Portable structural tests; not Apple runtime evidence."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import check

class Tests(unittest.TestCase):
    def test_install_relocates_exact_journal_and_entropy_only(self):
        def fake(*args,**kw):
            if '-o' in args:Path(args[args.index('-o')+1]).write_bytes(b'not native')
            if 'lipo' in args:return b'arm64\n'
            if 'vtool' in args:return b'platform IOSSIMULATOR\n'
        with tempfile.TemporaryDirectory() as tmp,patch.object(check.base,'run',side_effect=fake) as run:
            root=Path(tmp)
            consumer=check.install(root,'MOCKSDK')
            self.assertEqual(list(root.iterdir()),[consumer])
            compiled={Path(call.args[call.args.index('-c')+1]).name for call in run.call_args_list if '-c' in call.args}
            self.assertEqual(compiled,{'journal.cpp','entropy_apple.cpp'})
            files={f.relative_to(consumer).as_posix() for f in consumer.rglob('*') if f.is_file()}
            self.assertEqual(files,{
                'Installed/lib/'+check.ARCHIVE,'Installed/include/entropy.h',
                'Installed/include/core/journal/journal.h','Installed/include/core/scheduler/types.h',
                'Installed/include/core/util/error.h','Installed/include/core/util/bitmap.h',
            })

    def test_generated_app_links_only_relocated_archive_and_system_dependencies(self):
        with tempfile.TemporaryDirectory() as tmp:
            parent=Path(tmp);check.project(parent)
            pbx=(parent/'Preview.xcodeproj/project.pbxproj').read_text()
            self.assertEqual(pbx.count(check.ARCHIVE),2)
            self.assertIn('$(PROJECT_DIR)/Installed/lib/'+check.ARCHIVE,pbx)
            self.assertIn('"-lsqlite3","-framework","Security"',pbx)
            for forbidden in ('ImageIO','CoreGraphics','libcurl','madmedia_core','journal.cpp'):
                self.assertNotIn(forbidden,pbx)

    def test_two_exact_native_cases(self):
        counts=dict(totalTestCount=2,passedTests=2,failedTests=0,skippedTests=0)
        nodes=[dict(nodeType='Test Case',nodeIdentifier=n,result='Passed') for n in sorted(check.CASES)]
        with patch.object(check.base,'CASES',check.CASES):
            check.base.verify(counts,{'testNodes':nodes})
            with self.assertRaises(ValueError):check.base.verify(counts,{'testNodes':nodes[:1]})
            with self.assertRaises(ValueError):check.base.verify(counts,{'testNodes':nodes+nodes[:1]})

if __name__=='__main__':unittest.main()
