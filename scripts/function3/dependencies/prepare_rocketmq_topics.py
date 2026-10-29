"""Configure only topics/groups in the marked, private Function3 Broker. No downloads."""
from pathlib import Path
import shutil
import subprocess

root=Path('/home/pikachu/phase11-data/function3/rocketmq')
assert (root/'FUNCTION3_PRIVATE_BROKER').exists()
pid=(root/'namespace.pid').read_text().strip()
assert str(root) in (Path('/proc')/pid/'cmdline').read_bytes().decode()
program=Path('/home/pikachu/rocketmq-all-5.1.4-bin-release')
java=shutil.which('java');assert java,'reuse an existing Java installation'
command=['nsenter','--preserve-credentials','-t',pid,'-U','-n',java,'-Xms32m','-Xmx128m',
         '--add-opens=java.base/sun.nio.ch=ALL-UNNAMED','--add-opens=java.base/java.nio=ALL-UNNAMED',
         '-Duser.home='+str(root),'-cp',str(program/'lib/*'),'org.apache.rocketmq.tools.command.MQAdminStartup']
for topic,kind in [('Function3Tree','FIFO'),('Function3Chat','FIFO'),('Function3Speech','NORMAL')]:
    subprocess.run(command+['updateTopic','-n','127.0.0.1:19876','-b','127.0.0.1:20911','-t',topic,'-a','+message.type='+kind],check=True)
for group in ('Function3Delivery','Function3Roundtrip'):
    subprocess.run(command+['updateSubGroup','-n','127.0.0.1:19876','-b','127.0.0.1:20911','-g',group,'-o','true'],check=True)
print('Private topics and separate production-test/roundtrip consumer groups prepared.')
