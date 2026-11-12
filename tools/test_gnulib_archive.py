#!/usr/bin/env python3
"""Check the real archive recipe removes deselected fallback members."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class GnulibArchiveTests(unittest.TestCase):
    def test_removed_provider_does_not_survive_incremental_archive(self):
        with tempfile.TemporaryDirectory(prefix='gnulib-archive-') as temp:
            work = Path(temp)
            objects = []
            for name in ('active', 'obsolete_fallback'):
                source = work / (name + '.c')
                source.write_text('int ' + name + '(void) { return 0; }\n')
                obj = work / (name + '.o')
                subprocess.run(['cc','-c',str(source),'-o',str(obj)],check=True)
                objects.append(obj)
            archive = work / 'libgnulib.a'
            subprocess.run(['ar','rcs',str(archive),*[str(p) for p in objects]],check=True)
            os.utime(archive,(1,1))
            makefile = os.environ.get('ARCHIVE_BASELINE_MAKEFILE', str(ROOT / 'Makefile'))
            result = subprocess.run(['make','-f',makefile,'ARCH=amd64','AR=ar',
                'BUILD_DIR='+str(work), 'GNULIB_LIB='+str(archive),
                'GNULIB_OBJS='+str(objects[0]),str(archive)],cwd=ROOT,
                capture_output=True,text=True,check=False)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            members=subprocess.check_output(['ar','t',str(archive)],text=True).splitlines()
            self.assertEqual(members,['active.o'], 'ar rcs on the old archive retains a removed provider')
            self.assertFalse(archive.with_suffix('.a.tmp').exists())

if __name__ == '__main__':
    unittest.main()
