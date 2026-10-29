"""Reproduce Debug or ASan/LSan/UBSan checks using installed dependencies only.

Stop the private Broker before this disk/CPU-intensive check. It never controls
formal services, downloads a dependency, or changes global configuration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('flavour',choices=('debug','asan'))
p.add_argument('--clean-generated-after',action='store_true')
p.add_argument('--evidence-root',type=Path,default=Path('/home/pikachu/phase11-data/function3/validation/checks'))
a=p.parse_args();source=Path('/home/pikachu/function3-src')
assert Path(__file__).resolve().is_relative_to(source)
broker=Path('/home/pikachu/phase11-data/function3/rocketmq/broker.pid')
if broker.exists():
    process=Path('/proc')/broker.read_text().strip()
    assert not process.exists() or (process/'stat').read_text().split()[2]=='Z','stop the private Broker before this check'
assert shutil.disk_usage(source).free>2*1024**3,'less than 2 GiB free; no build started'
build=source/('build-'+a.flavour)
assert a.evidence_root.resolve().is_relative_to(Path('/home/pikachu/phase11-data/function3/validation'))
evidence=a.evidence_root/a.flavour
evidence.mkdir(parents=True,exist_ok=True)
command=['cmake','-S',str(source),'-B',str(build),'-DCMAKE_BUILD_TYPE=Debug',
         '-DCMAKE_CXX_FLAGS_DEBUG=-Og -g1','-DCMAKE_C_FLAGS_DEBUG=-Og -g1','-DBUILD_TESTING=ON']
for line in (source/'build-release/CMakeCache.txt').read_text().splitlines():
    if not line.startswith(('WEBSERVER_','CMAKE_PREFIX_PATH:','GTest_DIR:')) or '=' not in line:continue
    key,value=line.split('=',1)
    if ':INTERNAL' in key or key.split(':')[0] in ('WEBSERVER_ENABLE_ASAN','WEBSERVER_ENABLE_UBSAN','WEBSERVER_ENABLE_TSAN'):continue
    command.append('-D'+key+'='+value)
command+=['-DWEBSERVER_ENABLE_ASAN='+('ON' if a.flavour=='asan' else 'OFF'),
          '-DWEBSERVER_ENABLE_UBSAN='+('ON' if a.flavour=='asan' else 'OFF'),'-DWEBSERVER_ENABLE_TSAN=OFF']
env=os.environ.copy()
env['PHASE11_POSTGRES_CONNECTION']='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test'
env['FUNCTION3_REDIS_TEST_PORT']='6379'
if a.flavour=='asan':
    env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1'
    env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
report={'flavour':a.flavour,'passed':False,'toolchain':subprocess.check_output(['c++','--version'],text=True).splitlines()[0],
        'testDatabase':'function3_phase11_test','flags':command,'steps':[]}
try:
    for name,args in [('configure',command),('build',['cmake','--build',str(build),'-j','2']),
                      ('ctest',['ctest','--test-dir',str(build),'--output-on-failure','-j','1'])]:
        start=time.monotonic();log=evidence/(name+'.log')
        with log.open('w') as output:subprocess.run(args,cwd=source,env=env,stdout=output,stderr=subprocess.STDOUT,check=True)
        report['steps'].append({'name':name,'seconds':round(time.monotonic()-start,3),'logSha256':hashlib.sha256(log.read_bytes()).hexdigest()})
        print(a.flavour,name,'passed',flush=True)
    report['passed']=True
finally:
    (evidence/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
    if a.clean_generated_after and report['passed']:
        # CMake 自己清理本轮构建输出，保留源码、安装依赖、缓存配置与测试证据。
        assert build.resolve().parent==source.resolve()
        subprocess.run(['cmake','--build',str(build),'--target','clean'],check=True,stdout=subprocess.DEVNULL)
print(json.dumps(report,indent=2))
