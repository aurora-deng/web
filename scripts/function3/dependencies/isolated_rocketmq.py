"""Reuse the installed RocketMQ 5.1.4 in an isolated localhost-only test instance."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time
import re

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('action',choices=('start','stop','status'))
parser.add_argument('--program',type=Path,default=Path('/home/pikachu/rocketmq-all-5.1.4-bin-release'))
parser.add_argument('--namespace-child',action='store_true',help=argparse.SUPPRESS)
parser.add_argument('--namespace-tag',help=argparse.SUPPRESS)
args=parser.parse_args()
root=Path('/home/pikachu/phase11-data/function3/rocketmq')
marker=root/'FUNCTION3_PRIVATE_BROKER'
if root.exists() and not marker.exists():parser.error('unmarked directory; refusing to alter it')
root.mkdir(parents=True,exist_ok=True,mode=0o700)
marker.write_text('Function3 isolated RocketMQ; preserve existing broker and data\n')

def owned_pid(name):
    pidfile=root/(name+'.pid')
    if not pidfile.exists():return None
    pid=int(pidfile.read_text())
    try:
        process=Path('/proc')/str(pid)
        if (process/'stat').read_text().split()[2]=='Z':return None
        command=process/'cmdline';text=command.read_bytes().decode().replace('\0',' ')
    except FileNotFoundError:return None
    # 退出可以发生在读取 stat 与 cmdline 之间；空命令不再代表可控制的任务。
    if not text.strip():return None
    if str(root) not in text:raise RuntimeError('PID was reused; refusing to control it')
    return pid

if args.action=='stop':
    for name in ('broker','namesrv','namespace'):
        if pid:=owned_pid(name):os.kill(pid,signal.SIGTERM)
    deadline=time.monotonic()+45
    while time.monotonic()<deadline:
        if not any(owned_pid(name) for name in ('broker','namesrv','namespace')):break
        time.sleep(.1)
    else:raise RuntimeError('private Broker did not stop; inspect owned process logs')
elif args.action=='status':
    print({name:owned_pid(name) for name in ('namespace','namesrv','broker')})
else:
    if not args.namespace_child:
        if owned_pid('namespace'):parser.error('private namespace already running')
        # 5.1.4 Proxy 的 gRPC 监听只能指定端口，因此放进仅有 loopback 的用户网络
        # namespace。宿主网络完全没有这些监听口；无需 root、iptables 或全局改动。
        process=subprocess.Popen(['unshare','--user','--map-root-user','--net',sys.executable,
            str(Path(__file__).resolve()),'start','--program',str(args.program),
            '--namespace-child','--namespace-tag',str(root)],
            stdout=(root/'namespace.log').open('ab'),stderr=subprocess.STDOUT,start_new_session=True)
        (root/'namespace.pid').write_text(str(process.pid))
        # Popen 返回时 unshare 可能还没切换命名空间；不能马上对同一 user namespace 做 setns。
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            if process.poll() is not None:raise RuntimeError('private namespace startup failed; inspect namespace.log')
            try:
                if os.readlink(f'/proc/{process.pid}/ns/user')!=os.readlink('/proc/self/ns/user') and os.readlink(f'/proc/{process.pid}/ns/net')!=os.readlink('/proc/self/ns/net'):break
            except FileNotFoundError:pass
            time.sleep(.05)
        else:raise RuntimeError('private namespace did not become ready')
        print('Started loopback-only user network namespace',process.pid)
        raise SystemExit(0)
    subprocess.run(['ip','link','set','lo','up'],check=True)
    java=shutil.which('java')
    if not java or not (args.program/'lib').is_dir():parser.error('existing Java/RocketMQ installation required')
    # 复用原二进制，只复制本测试实例的日志配置；不修改已有安装目录。
    profile=root/'runtime';(profile/'conf').mkdir(parents=True,exist_ok=True)
    library=profile/'lib'
    if not library.exists():library.symlink_to((args.program/'lib').resolve(),target_is_directory=True)
    if library.resolve()!=(args.program/'lib').resolve():parser.error('private runtime library link mismatch')
    for original in (args.program/'conf').glob('*.xml'):
        text=original.read_text()
        text=re.sub(r'<maxFileSize>[^<]+</maxFileSize>','<maxFileSize>8MB</maxFileSize>',text)
        text=re.sub(r'<maxIndex>[^<]+</maxIndex>','<maxIndex>3</maxIndex>',text)
        for name in ('RocketmqPop','RocketmqWaterMark','RocketmqStats'):
            text=text.replace('name="'+name+'" additivity="false" level="INFO"','name="'+name+'" additivity="false" level="WARN"')
        (profile/'conf'/original.name).write_text(text)
    if shutil.disk_usage(root).free<2*1024**3:parser.error('less than 2 GiB free; broker not started')
    for port in (19876,20909,20911,20912,20880,20881):
        with socket.socket() as s:
            try:s.bind(('127.0.0.1',port))
            except OSError:parser.error(f'port {port} already used; refusing to replace another service')
    (root/'namesrv.conf').write_text('listenPort=19876\nbindAddress=127.0.0.1\n')
    config={
        'brokerClusterName':'Function3','brokerName':'function3-broker','brokerId':'0',
        'namesrvAddr':'127.0.0.1:19876','listenPort':'20911','haListenPort':'20912','bindAddress':'127.0.0.1',
        'brokerIP1':'127.0.0.1','brokerIP2':'127.0.0.1',
        'storePathRootDir':str(root/'store'),'storePathCommitLog':str(root/'store/commitlog'),
        'mappedFileSizeCommitLog':'67108864','mappedFileSizeConsumeQueue':'600000',
        'fileReservedTime':'2','deleteWhen':'04','autoCreateTopicEnable':'false',
        'autoCreateSubscriptionGroup':'false','brokerRole':'ASYNC_MASTER','flushDiskType':'SYNC_FLUSH',
        'maxMessageSize':'65536','diskMaxUsedSpaceRatio':'90',
    }
    (root/'broker.conf').write_text(''.join(k+'='+v+'\n' for k,v in config.items()))
    (root/'proxy.json').write_text(json.dumps({'rocketMQClusterName':'Function3',
        'namesrvAddr':'127.0.0.1:19876','grpcServerPort':20881,'remotingListenPort':20880,
        'localServeAddr':'127.0.0.1','remotingAccessAddr':'127.0.0.1'}))
    env=os.environ.copy();env['ROCKETMQ_HOME']=str(profile)
    common=[java,'-Xms64m','-Xss512k','-XX:MaxDirectMemorySize=256m',
        '--add-opens=java.base/java.nio=ALL-UNNAMED','--add-opens=java.base/sun.nio.ch=ALL-UNNAMED',
        '-Duser.home='+str(root),'-Drocketmq.home.dir='+str(profile),'-cp',str(args.program/'lib/*')]
    processes=[('namesrv',['-Xmx128m','org.apache.rocketmq.namesrv.NamesrvStartup','-c',str(root/'namesrv.conf')]),
        ('broker',['-Xmx512m','org.apache.rocketmq.proxy.ProxyStartup','-pm','local','-bc',str(root/'broker.conf'),'-pc',str(root/'proxy.json')])]
    for name,arguments in processes:
        log=(root/(name+'.log')).open('ab')
        process=subprocess.Popen(common[:1]+arguments[:1]+common[1:]+arguments[1:],cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        (root/(name+'.pid')).write_text(str(process.pid))
        print('Started isolated',name,process.pid)
    while True:signal.pause()
