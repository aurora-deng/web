"""Fail-closed checks for credentials and the isolated metrics access boundary."""
import os
import json
import math
from pathlib import Path
import stat
import time
import urllib.error
import urllib.parse
import urllib.request


def private_file(path):
    path=Path(path)
    info=path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_size>256*1024:
        raise ValueError('Credential input must be a small regular file')
    if os.name!='nt' and (info.st_uid!=os.geteuid() or info.st_mode & 0o077):
        raise ValueError('Credential input must belong to this user with mode 0600 or stricter')


def metrics_access(port,ops_token,member_token=None):
    if not 0<port<65536 or not ops_token:
        raise ValueError('Ops preflight requires a loopback target and a nonempty token')
    def status(token):
        request=urllib.request.Request(f'http://127.0.0.1:{port}/metrics',
            headers={'Authorization':'Bearer '+token} if token else {})
        try:
            with urllib.request.urlopen(request,timeout=3) as response:return response.status
        except urllib.error.HTTPError as error:return error.code
    result={'anonymous':status(None),'ops':status(ops_token)}
    if member_token:result['member']=status(member_token)
    if result['anonymous']!=401 or result['ops']!=200 or (member_token and result['member']!=403):
        raise ValueError('Metrics authentication check failed; anonymous/member/ops access must be 401/403/200')
    return {'verified':True,**result,'scope':'loopback-and-SSH; not multi-user RBAC'}


def prometheus_baseline(port,target,timeout=30):
    """Wait for the owned target's initial counter before measuring increments."""
    if type(port) is not int or type(target) is not int or not (0<port<65536 and 0<target<65536):
        raise ValueError('The metrics baseline requires two loopback ports')
    expression=('nebula_function_seconds_count{job="nebula",instance="127.0.0.1:'+str(target)+
                '",function="ChatService::sendMessage"}')
    url=f'http://127.0.0.1:{port}/api/v1/query?'+urllib.parse.urlencode({'query':expression})
    deadline=time.monotonic()+timeout
    while True:
        try:
            with urllib.request.urlopen(url,timeout=3) as response:rows=json.load(response).get('data',{}).get('result',[])
            if rows and math.isfinite(count:=float(rows[0]['value'][1])) and count>=0:return
        except (urllib.error.URLError,TimeoutError,ValueError,KeyError,IndexError):pass
        if time.monotonic()>=deadline:raise ValueError('Prometheus has no baseline for this owned target')
        time.sleep(.2)
