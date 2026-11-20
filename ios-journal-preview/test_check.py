"""Portable structural tests; not Apple runtime evidence."""
from pathlib import Path
import tempfile
import re
import sqlite3
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

    def test_three_exact_native_cases(self):
        counts=dict(totalTestCount=3,passedTests=3,failedTests=0,skippedTests=0)
        nodes=[dict(nodeType='Test Case',nodeIdentifier=n,result='Passed') for n in sorted(check.CASES)]
        check.verify(counts,{'testNodes':nodes})
        for invalid in (nodes[:2],nodes+nodes[:1],nodes+[dict(nodes[0],nodeIdentifier='Unexpected')]):
            with self.assertRaises(ValueError):check.verify(counts,{'testNodes':invalid})
        for result in ('Skipped','Failed'):
            with self.assertRaises(ValueError):
                check.verify(counts,{'testNodes':nodes[:2]+[dict(nodes[2],result=result)]})
        for key in ('totalTestCount','passedTests','failedTests','skippedTests'):
            with self.assertRaises(ValueError):check.verify(dict(counts,**{key:42}),{'testNodes':nodes})

    def test_exact_trigger_and_real_upsert_on_portable_sqlite(self):
        production=(check.ROOT/'core/journal/journal.cpp').read_text()
        ddl=re.search(r'kDDL = R"sql\((.*?)\)sql"',production,re.S)[1]
        upsert=re.search(r'kUpsert = R"sql\((.*?)\)sql"',production,re.S)[1]
        header=(check.ROOT/'ios-journal-preview/journal_rollback.h').read_text()
        trigger=re.search(r'R"sql\((.*?)\)sql"',header,re.S)[1]
        def bindings(state,done):
            row=[None]*23
            row[:4]=['rollback-fixture',0,state,0]
            row[16]=done
            return row
        with tempfile.TemporaryDirectory() as tmp:
            db=Path(tmp)/'isolated.sqlite'
            journal=sqlite3.connect(db,isolation_level=None)
            try:
                self.assertEqual(journal.execute('PRAGMA journal_mode=WAL').fetchone(),('wal',))
                journal.execute('PRAGMA synchronous=FULL')
                journal.executescript(ddl)
                journal.execute(upsert,bindings(0,11))
                journal.executescript(trigger)
                with self.assertRaises(sqlite3.IntegrityError):journal.execute(upsert,bindings(4,99))
                self.assertFalse(journal.in_transaction)
                self.assertEqual(journal.execute('SELECT state,bytes_done FROM transfer_task').fetchall(),[(0,11)])
                journal.execute(upsert,bindings(4,37))
            finally:journal.close()
            reopened=sqlite3.connect(db)
            try:self.assertEqual(reopened.execute('SELECT state,bytes_done FROM transfer_task').fetchall(),[(4,37)])
            finally:reopened.close()

if __name__=='__main__':unittest.main()
