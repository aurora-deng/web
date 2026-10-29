"""Apply the documented FIFO completion policy to the approved private SDK and rebuild it.

No downloads, new libraries, global configuration or other SDK installations.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root=Path('/home/pikachu/phase11-deps/rocketmq-cpp/5.1.1')
source=root/'rocketmq-clients/cpp'
assert (source/'include/rocketmq/FifoProducer.h').is_file()
patch=Path(__file__).parent/'patches/FifoProducerPartition/FifoProducerPartition.cpp'
destination=source/'source/rocketmq/FifoProducerPartition.cpp'
assert destination.resolve().is_relative_to(root.resolve())
shutil.copy2(patch,destination)
for command in (['cmake','--build',str(root/'build'),'-j','2'],['cmake','--install',str(root/'build')]):
    subprocess.run(command,check=True)
policy=root/'install/share/function3-sdk-policy.json';policy.parent.mkdir(parents=True,exist_ok=True)
policy.write_text(json.dumps({'sdkCommit':'da8271b572b41b627bcbbdfb5b3310c0c5e4736b',
    'policy':'bounded-fifo-callback-v1','sha256':hashlib.sha256(patch.read_bytes()).hexdigest()},indent=2)+'\n')
print('Private SDK FIFO failure callback policy installed; Outbox owns retries.')
