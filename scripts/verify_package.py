#!/usr/bin/env python3
"""Build a disposable SDK copy, relocate its install, consume from two apps.
Only this invocation's TemporaryDirectory is removed. Source checkout is read-only.
"""
import argparse, hashlib, json, os, pathlib, re, shutil, subprocess, tempfile, sys
p=argparse.ArgumentParser()
p.add_argument('--producer-c', required=True);p.add_argument('--producer-cxx', required=True)
p.add_argument('--consumer-c', required=True);p.add_argument('--consumer-cxx', required=True)
p.add_argument('--cmake', default='cmake');p.add_argument('--obs', choices=['ON','OFF'], default='OFF')
p.add_argument('--madobs-include');p.add_argument('--cmake-arg', action='append', default=[])
p.add_argument('--evidence', required=True)
a=p.parse_args();repo=pathlib.Path(__file__).resolve().parents[1]
if a.obs=='ON' and not a.madobs_include:p.error('--obs ON requires --madobs-include')
allowed={'SQLite3_INCLUDE_DIR','SQLite3_LIBRARY','CURL_INCLUDE_DIR','CURL_LIBRARY_RELEASE','CURL_LIBRARY_DEBUG'}
for value in a.cmake_arg:
 if not value.startswith('-D') or '=' not in value or value[2:].split('=',1)[0] not in allowed:
  p.error('--cmake-arg accepts only explicit SQLite3/CURL dependency locations')
def run(cmd, **kw):
 print('+', ' '.join(map(str,cmd)), flush=True)
 try:
  return subprocess.run(list(map(str,cmd)),check=True,text=True,**kw)
 except subprocess.CalledProcessError as error:
  if error.stdout: print(error.stdout, file=sys.stderr)
  if error.stderr: print(error.stderr, file=sys.stderr)
  raise
versions={name:run([getattr(a,name.replace('-','_')),'--version'],capture_output=True).stdout for name in ['producer-c','producer-cxx','consumer-c','consumer-cxx']}
print(json.dumps({'toolchains': versions}), flush=True)
with tempfile.TemporaryDirectory(prefix='madmedia-package-') as tmp:
 work=pathlib.Path(tmp);source=work/'producer-source';source.mkdir()
 # Copy tracked files only, including staged local additions. Never copy .git,
 # sibling checkouts or CI dependencies, and never modify the real checkout.
 tracked=run(['git','-C',repo,'ls-files','-z'],capture_output=True).stdout.split('\0')
 for name in filter(None,tracked):
  rel=pathlib.PurePosixPath(name)
  if rel.is_absolute() or '..' in rel.parts:raise RuntimeError('unsafe tracked path')
  target=source/name;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(repo/name,target,follow_symlinks=False)
 consumer=work/'consumer-source';shutil.copytree(source/'examples/multi_app',consumer)
 build=work/'producer-build';prefix=work/'original-prefix';moved=work/'relocated-prefix'
 common=list(a.cmake_arg)
 obs_args=['-DMADOBS_INCLUDE_DIR='+a.madobs_include] if a.obs=='ON' else []
 run([a.cmake,'-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DMADMEDIA_BUILD_TESTS=OFF','-DMADMEDIA_BUILD_C_ABI=ON','-DMADMEDIA_WITH_MADOBS='+a.obs,'-DCMAKE_C_COMPILER='+a.producer_c,'-DCMAKE_CXX_COMPILER='+a.producer_cxx,*common,*obs_args])
 run([a.cmake,'--build',build,'-j2']);run([a.cmake,'--install',build,'--prefix',prefix])
 prefix.rename(moved)
 # These are verified, disposable copies/build products owned by this invocation.
 shutil.rmtree(build);shutil.rmtree(source)
 cb=work/'consumer-build';consumer_obs=['-DMadMedia_MADOBS_INCLUDE_DIR='+a.madobs_include] if a.obs=='ON' else []
 run([a.cmake,'-S',consumer,'-B',cb,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_PREFIX_PATH='+str(moved),'-DCMAKE_C_COMPILER='+a.consumer_c,'-DCMAKE_CXX_COMPILER='+a.consumer_cxx,'-DWITH_OBS='+a.obs,*common,*consumer_obs])
 run([a.cmake,'--build',cb,'-j2'])
 da=work/'app-a-data';db=work/'app-b-data'
 ids=[]
 for app,directory in [('app_a',da),('app_b',db)]:
  value=run([cb/app,'create',directory],capture_output=True).stdout.strip()
  if not re.fullmatch(r'[A-Za-z0-9_-]{1,128}',value):raise RuntimeError('invalid output task ID')
  ids.append(value)
 if ids[0]==ids[1]:raise RuntimeError('cross-app task ID collision')
 run([cb/'app_a','foreign',da,ids[1]]);run([cb/'app_b','foreign',db,ids[0]])
 libraries={str(f.relative_to(moved)):hashlib.sha256(f.read_bytes()).hexdigest() for f in moved.rglob('*.a')}
 if not all(any(pathlib.Path(name).name==expected for name in libraries) for expected in ['libmadmedia_core.a','libmadmedia_ffi.a']):raise RuntimeError('missing installed archives')
 result={'scope':'Linux host install relocation and two independent app journals; not mobile qualification','obs':a.obs,'toolchains':versions,'libraries_sha256':libraries,'source_and_build_removed_before_consumer':True,'two_apps_own_rows_and_foreign_not_found':True}
 pathlib.Path(a.evidence).write_text(json.dumps(result,indent=2)+'\n')
 print('PACKAGE_MULTI_APP_PASS',flush=True)
