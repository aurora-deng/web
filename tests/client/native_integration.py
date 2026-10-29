"""Existing Windows executable -> loopback -> verified TLS -> isolated Linux.

Only the explicit test port 18444 is accepted. Does not touch formal accounts,
trust stores, firewall or the 18090/18443 service. Never prints credentials/nonces.
"""
from pathlib import Path
import argparse
import http.cookiejar
import json
import os
import queue
import subprocess
import threading
import time
import urllib.request
import urllib.error
import uuid
import zipfile
import io
import statistics
import socket
from measure_native import NativeMemory

root=Path(__file__).resolve().parents[2]
class Browser:
    def __init__(self,port):
        self.base=f'http://localhost:{port}';self.jar=http.cookiejar.CookieJar();self.http=urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar));self.csrf='';self.account='';self.local_csrf=''
    def call(self,method,path,data=None,raw=None,extra=None,expected=200):
        headers={'Origin':self.base,'X-CSRF-Token':self.csrf,'X-Local-CSRF':self.local_csrf,'X-Nebula-Account':self.account}
        headers.update(extra or {})
        if data is not None:raw=json.dumps(data,ensure_ascii=False).encode();headers['Content-Type']='application/json'
        request=urllib.request.Request(self.base+path,data=raw,method=method,headers=headers)
        try:response=self.http.open(request,timeout=45)
        except urllib.error.HTTPError as error:response=error
        body=response.read();value=json.loads(body) if body else {}
        if response.status!=expected:raise AssertionError((method,path,response.status,value))
        return value
    def local(self,method,path,**kw):return self.call(method,'/api/local/assistant'+path,**kw)
    def login_new(self,name):
        self.call('POST','/api/auth/register',{'username':name,'displayName':'本机隔离验证','password':'Native-test-only!'},expected=201)
        result=self.call('POST','/api/auth/login',{'username':name,'password':'Native-test-only!'})
        self.csrf=result['csrfToken'];self.account=str(result['user']['id']);return self.account

def main():
    p=argparse.ArgumentParser();p.add_argument('--certificate-sha256',required=True);p.add_argument('--port',type=int,default=18091);p.add_argument('--browser',action='store_true');p.add_argument('--ai-user-id',type=int);p.add_argument('--migration',action='store_true');p.add_argument('--actions',action='store_true');p.add_argument('--speech',action='store_true');p.add_argument('--measure',action='store_true');p.add_argument('--configuration',choices=('release','debug'),default='release');a=p.parse_args()
    output=root/'build-client'/'integration';output.mkdir(parents=True,exist_ok=True);stop=output/'stop';stop.unlink(missing_ok=True)
    # 仅兼容本测试早期创建的固定目录；不修改用户任意选择的既有目录。
    private=output/'private';private.mkdir(exist_ok=True)
    if not (private/'NEBULA_PRIVATE_ROOT').exists():
        assert {x.name for x in private.iterdir()}<= {'credentials','accounts'}
        (private/'NEBULA_PRIVATE_ROOT').write_bytes(b'nebula-private-root-v1\n')
    executable=root/'build-client'/a.configuration/'nebula_companion.exe'
    command=[str(executable),'--server','192.168.239.135','--tls-port','18444','--connect-host','127.0.0.1','--certificate-sha256',a.certificate_sha256,'--port',str(a.port),'--frontend',str(root/'frontend/phase11'),'--data-dir',str(output/'private'),'--python',os.sys.executable,'--parser',str(root/'scripts/client/documents/extract_document.py'),'--stop-file',str(stop)]
    process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',creationflags=subprocess.CREATE_NO_WINDOW)
    memory_sampler=NativeMemory(process.pid);lines=queue.Queue()
    def reader():
        for line in process.stdout:lines.put(line)
    threading.Thread(target=reader,daemon=True).start();report={'configuration':a.configuration,'checks':[],'certificateTrust':'process-local, confirmed fingerprint','target':'existing isolated TLS 18444'}
    try:
        deadline=time.monotonic()+25;url=''
        while time.monotonic()<deadline:
            try:line=lines.get(timeout=1)
            except queue.Empty:
                if process.poll() is not None:raise RuntimeError('Native client exited during startup')
                continue
            if line.startswith('http://localhost:'):url=line.strip();break
        if not url:raise RuntimeError('Native client did not expose a bootstrap URL')
        browser=Browser(a.port)
        browser.call('POST','/api/local/session/bootstrap',{'token':'wrong'},expected=403)
        browser.call('POST','/api/local/session/bootstrap',{'token':url.split('companion=')[1]})
        info=browser.call('GET','/api/local/session/info');browser.local_csrf=info['csrfToken']
        report['idleWorkingSetMiB']=round(memory_sampler.sample()/1024**2,3);assert report['idleWorkingSetMiB']<200,'Native idle working set exceeds 200 MiB target'
        if info.get('accountId'):
            restored=browser.call('GET','/api/me');browser.csrf=restored['csrfToken'];browser.call('POST','/api/auth/logout',{})
            report['checks'].append('DPAPI cookie restored and revalidated by Linux after client restart')
        browser.local('GET','/capabilities',expected=401)
        prefix='native_'+uuid.uuid4().hex[:10];account_a=browser.login_new(prefix+'_a')
        assert 'phase11_session' not in [cookie.name for cookie in browser.jar]
        assert browser.local('GET','/capabilities')['accountId']==account_a
        report['checks'].append('loopback bootstrap, real TLS register/login, server cookie kept out of browser')
        friend=browser.local('POST','/threads',data={'surface':'friend','title':'好友式私人历史'})
        sidebar=browser.local('POST','/threads',data={'surface':'sidebar','title':'侧栏私人历史'})
        assert [t['id'] for t in browser.local('GET','/threads?surface=friend')]==[friend['id']]
        assert [t['id'] for t in browser.local('GET','/threads?surface=sidebar')]==[sidebar['id']]
        browser.local('POST','/memories',data={'text':'未经确认','confirmed':False},expected=400)
        memory=browser.local('POST','/memories',data={'text':'我在学习中文网络架构','confirmed':True});assert browser.local('GET','/memories')[0]['id']==memory['id']
        doc=browser.local('POST','/documents',data={'title':'本机笔记','text':'苦情树社区使用异步业务执行器。星云助手帮助学习 C++。','authorized':False})
        assert browser.local('POST','/search',data={'query':'苦情树异步'})==[]
        browser.local('PATCH','/documents/'+doc['id'],data={'enabled':True,'authorized':True})
        assert browser.local('POST','/search',data={'query':'苦情树异步'})[0]['documentId']==doc['id']
        archive=io.BytesIO()
        with zipfile.ZipFile(archive,'w') as z:z.writestr('word/document.xml','<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body><w:p><w:r><w:t>本机导入的中文DOCX资料。</w:t></w:r></w:p></w:body></w:document>')
        imported=browser.local('POST','/documents/import',raw=archive.getvalue(),extra={'Content-Type':'application/octet-stream','X-Document-Extension':'.docx','X-Document-Authorized':'true'})
        assert browser.local('POST','/search',data={'query':'中文DOCX'})
        report['checks'].append('separate histories, shared confirmed memories, consent-gated CJK retrieval, real DOCX parser')
        if a.migration:
            document=browser.call('POST','/api/knowledge/documents',{'title':'旧私人知识验证','sourceName':'legacy.md','mediaType':'text/plain','content':'迁移专属 quarkMigrationOnly，仅从服务器复制到当前电脑。'*20},expected=201)
            resources=browser.local('GET','/migration/resources');assert any(str(item['id'])==str(document['id']) and item['kind']=='knowledge' for item in resources)
            copied=browser.local('POST','/migration/import',data={'kind':'knowledge','resourceId':str(document['id']),'after':'0','upper':'0'})
            assert copied['verified'] and not copied['serverCopyDeleted']
            assert any(str(item['id'])==str(document['id']) for item in browser.call('GET','/api/knowledge/documents'))
            assert browser.local('POST','/search',data={'query':'quarkMigrationOnly'})==[]
            browser.local('POST','/migration/purge',data={'receiptId':copied['receiptId'],'confirmed':False},expected=400)
            browser.local('POST','/migration/purge',data={'receiptId':copied['receiptId'],'confirmed':True})
            browser.local('POST','/migration/purge',data={'receiptId':copied['receiptId'],'confirmed':True})
            assert all(str(item['id'])!=str(document['id']) for item in browser.call('GET','/api/knowledge/documents'))
            report['checks'].append('legacy private knowledge copied, C++ OpenSSL/BCrypt digest agreement, local consent default off, explicit and duplicate purge')
        if a.actions:
            group=browser.call('POST','/api/conversations',{'kind':'group','title':'确认卡验证','memberIds':str(account_a)},expected=201)
            action=browser.local('POST','/tools/proposals',data={'tool':'send_message','arguments':{'conversationId':str(group['id']),'text':'只有确认后才发送这条消息'}})
            browser.local('POST','/tools/confirm',data={'id':action['id'],'digest':'wrong'},expected=400)
            browser.local('POST','/tools/confirm',data={'id':action['id'],'digest':action['digest'],'arguments':{'text':'tampered'}},expected=400)
            first=browser.local('POST','/tools/confirm',data={'id':action['id'],'digest':action['digest']})
            second=browser.local('POST','/tools/confirm',data={'id':action['id'],'digest':action['digest']});assert first['result']['id']==second['result']['id']
            profile=browser.local('POST','/tools/proposals',data={'tool':'update_profile','arguments':{'displayName':'确认卡名称','biography':'资料也需要具体载荷确认'}})
            browser.local('POST','/tools/confirm',data={'id':profile['id'],'digest':profile['digest']})
            browser.call('PATCH','/api/me',{'displayName':'稍后正常修改','biography':'later'})
            replay=browser.call('PATCH','/api/me',{'displayName':'确认卡名称','biography':'资料也需要具体载荷确认','operationId':'agent:'+profile['id']});assert replay['user']['displayName']=='稍后正常修改'
            report['checks'].append('actual Agent message/profile writes, payload substitution rejected, repeated confirmation dedup, server replay preserves later profile edit')
        if a.ai_user_id:
            report['privateInference']=[]
            for index in range(2):
                started=time.monotonic();request=uuid.uuid4().hex
                prompt='只用一个词回答：你好' if index==0 else '请详细介绍网络服务器的设计，持续写作，稍后我会主动停止。'
                generation=browser.local('POST','/generations',data={'threadId':friend['id'],'requestId':request,'text':prompt,'aiUserId':str(a.ai_user_id)})
                deadline=time.monotonic()+150;first=None
                while time.monotonic()<deadline:
                    result=browser.local('GET','/generations/'+generation['id'])
                    if result['body'] and first is None:
                        first=time.monotonic()-started
                        if index==1:browser.local('DELETE','/generations/'+generation['id'])
                    if result['state']!='streaming':break
                    time.sleep(.3)
                assert first is not None,(result['state'],result.get('error'))
                assert result['state']==('completed' if index==0 else 'cancelled'),result
                history=browser.local('GET','/threads/'+friend['id']+'/messages')
                assert history[-1]['state'] in ('completed','cancelled')
                report['privateInference'].append({'run':index+1,'firstResponseSeconds':round(first,3),'terminalState':result['state'],'citations':len(result['citations'])})
            report['checks'].append('real private ONNX generation, incremental response, cancellation, local terminal history')
        if a.speech:
            profile={'provider':'azure','voice':'zf_001','style':'calm','speed':.95,'pitch':-2,'region':'eastasia','freeTierConfirmed':True,'dataUseConsent':False}
            marker='test-only-not-a-real-key-'+uuid.uuid4().hex
            browser.local('PATCH','/speech/settings',data={**profile,'apiKey':marker})
            settings=browser.local('GET','/speech/settings');assert settings['azureKeyConfigured'] and marker not in json.dumps(settings)
            browser.local('PATCH','/speech/settings',data={**profile,'removeKey':True})
            assert not browser.local('GET','/speech/settings')['azureKeyConfigured']
            accepted=browser.local('POST','/speech/preview',data={},expected=202);deadline=time.monotonic()+115
            while time.monotonic()<deadline:
                preview=browser.local('GET','/speech/preview/'+accepted['voice'])
                if preview['state']!='pending':break
                time.sleep(.5)
            assert preview['state']=='ready' and preview['actualProvider']=='local' and preview['fallbackReason']=='missing-key',preview
            response=browser.http.open(urllib.request.Request(browser.base+'/api/local/assistant/speech/preview/'+accepted['voice']+'/audio',headers={'Origin':browser.base,'X-Local-CSRF':browser.local_csrf,'X-Nebula-Account':browser.account}),timeout=15)
            audio=response.read();assert audio[:4]==b'RIFF' and audio[8:12]==b'WAVE' and len(audio)>1000
            report['speech']={'fallback':preview['fallbackReason'],'waveBytes':len(audio),'cloudCalls':0,'realCloudVerified':False}
            report['checks'].append('personal DPAPI key redaction/removal, real missing-key fallback to Linux Kokoro, native audio retrieval')
        old_headers={'X-Nebula-Account':account_a};browser.login_new(prefix+'_b')
        browser.local('GET','/threads?surface=friend',extra=old_headers,expected=409)
        assert browser.local('GET','/capabilities')['accountId']==browser.account
        assert browser.local('GET','/memories')==[] and browser.local('GET','/documents')==[]
        assert browser.local('GET','/threads?surface=friend')==[]
        if a.speech:
            settings=browser.local('GET','/speech/settings');assert not settings['azureKeyConfigured'] and not settings['geminiKeyConfigured'] and settings['provider']=='local'
        report['checks'].append('account switch rejects stale actor and isolates documents, memories and threads')
        if a.browser:
            cookies=[{'name':cookie.name,'value':cookie.value,'url':browser.base,'httpOnly':True,'sameSite':'Strict'} for cookie in browser.jar]
            subprocess.run([r'D:\nodejs\node.exe',str(root/'tests/client/proxy_browser.cjs')],input=json.dumps({'base':browser.base,'peerId':account_a,'cookies':cookies}),text=True,encoding='utf-8',env=os.environ.copy(),cwd=root,check=True,timeout=180)
            report['checks'].append('real Edge: upstream WebSocket ACK, SSE, viewport layout, private sidebar')
        if a.measure:
            report['proxyMeasurements']=[]
            report['loopbackTcpMs']={}
            for address in ('127.0.0.1','::1'):
                start=time.perf_counter()
                with socket.create_connection((address,a.port),timeout=3):pass
                report['loopbackTcpMs'][address]=round((time.perf_counter()-start)*1000,3)
            for run in range(1,4):
                direct=subprocess.run([str(root/'build-client'/a.configuration/'companion_tls_probe.exe'),'192.168.239.135','18444',a.certificate_sha256,'127.0.0.1','--measure'],capture_output=True,text=True,check=True,timeout=120)
                values=[]
                for _ in range(30):
                    start=time.perf_counter();browser.call('GET','/health/live');values.append((time.perf_counter()-start)*1000)
                values.sort();baseline=json.loads(direct.stdout);report['proxyMeasurements'].append({'round':run,'directVerifiedTls':baseline,'localProxy':{'samples':30,'p50Ms':round(statistics.median(values),3),'p95Ms':round(values[28],3)},'p50AddedMs':round(statistics.median(values)-baseline['p50Ms'],3)})
        report['httpPool']=browser.call('GET','/api/local/session/info').get('httpPool')
        report['passed']=True;print('PASS native loopback/TLS/storage/import/account integration')
    finally:
        report['resources']=memory_sampler.stop()
        stop.touch()
        try:process.wait(timeout=45)
        except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10);report['gracefulShutdown']=False
        else:report['gracefulShutdown']=process.returncode==0
        (output/'validation.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        scenario=a.configuration+'-'+('-'.join(name for name,enabled in [('ai',a.ai_user_id),('migration',a.migration),('actions',a.actions),('speech',a.speech),('browser',a.browser),('measure',a.measure)] if enabled) or 'base')
        (output/('validation-'+scenario+'.json')).write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if not report.get('gracefulShutdown'):raise AssertionError('Companion did not shut down gracefully')
if __name__=='__main__':main()
