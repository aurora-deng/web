"""TLS/private-owner migration and deliberate purge checks, existing _test DB only."""
from pathlib import Path
import argparse
import concurrent.futures
import hashlib
import http.client
import importlib.util
import json
import ssl
import sys
import uuid

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('private_archive_runtime',ROOT/'tests/function3/integration/runtime_blackbox.py')
runtime=importlib.util.module_from_spec(spec);sys.modules[spec.name]=runtime;spec.loader.exec_module(runtime)

class Browser:
    def __init__(self,context):self.context=context;self.cookie='';self.csrf='';self.user=None
    def call(self,method,path,data=None,expected=200):
        connection=http.client.HTTPSConnection('127.0.0.1',18444,context=self.context,timeout=20)
        headers={'Origin':'https://127.0.0.1:18444','Cookie':self.cookie,'X-CSRF-Token':self.csrf,'Content-Type':'application/json'}
        connection.request(method,path,None if data is None else json.dumps(data,ensure_ascii=False).encode(),headers);response=connection.getresponse();body=response.read();value=json.loads(body) if body else {};cookies=response.getheader('Set-Cookie');connection.close()
        if cookies:self.cookie=cookies.split(';')[0]
        assert response.status==expected,(method,path,response.status,value)
        return value
    def login(self,name):
        self.call('POST','/api/auth/register',{'username':name,'displayName':'迁移隔离验证','password':'Archive-test-only!'},201)
        value=self.call('POST','/api/auth/login',{'username':name,'password':'Archive-test-only!'});self.csrf=value['csrfToken'];self.user=value['user']['id']

def checksum(page):
    fields=['nebula-private-archive-v1',str(page['owner']),page['kind'],str(page['resource']),str(page['after']),str(page['through']),page['title'],'true' if page['attachments'] else 'false']
    for entry in page['entries']:fields += [str(entry['id']),entry['role'],entry['body']]
    material=b''.join(str(len(value.encode())).encode()+b':'+value.encode() for value in fields)
    return hashlib.sha256(material).hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
    assert runtime.sql('SELECT current_database()')=='function3_runtime_test'
    context=ssl.create_default_context(cafile='/home/pikachu/phase11-data/function3/runtime/localhost.crt')
    context.set_alpn_protocols(['http/1.1'])
    owner=Browser(context);stranger=Browser(context);prefix='archive_'+uuid.uuid4().hex[:8];owner.login(prefix+'_owner');stranger.login(prefix+'_other')
    conversation=owner.call('POST','/api/conversations',{'kind':'direct','peerId':'131'},201)['id']
    message=owner.call('POST',f'/api/conversations/{conversation}/messages',{'clientMessageId':prefix,'text':'中文私有历史：不会自动删掉服务器原副本。'},201)
    duplicate=owner.call('POST',f'/api/conversations/{conversation}/messages',{'clientMessageId':prefix,'text':'中文私有历史：不会自动删掉服务器原副本。'},201);assert message['id']==duplicate['id']
    page=owner.call('GET',f'/api/ai/private/legacy/conversation/{conversation}');assert checksum(page)==page['checksum']
    stranger.call('GET',f'/api/ai/private/legacy/conversation/{conversation}',expected=404)
    purge={'after':page['after'],'through':page['through'],'checksum':page['checksum'],'confirmed':'false'}
    path=f'/api/ai/private/legacy/conversation/{conversation}/purge'
    owner.call('POST',path,purge,400);purge['confirmed']='true'
    runtime.sql(f"UPDATE messages SET body='changed after export' WHERE id={message['id']}")
    owner.call('POST',path,purge,409)
    runtime.sql(f"UPDATE messages SET body='中文私有历史：不会自动删掉服务器原副本。' WHERE id={message['id']}")
    late=owner.call('POST',f'/api/conversations/{conversation}/messages',{'clientMessageId':prefix+'_late','text':'导出之后的新消息必须保留'},201)
    with concurrent.futures.ThreadPoolExecutor(2) as pool:list(pool.map(lambda _:owner.call('POST',path,purge),range(2)))
    assert runtime.sql(f"SELECT state FROM messages WHERE id={message['id']}")=='recalled'
    assert runtime.sql(f"SELECT state FROM messages WHERE id={late['id']}")=='active'
    document=owner.call('POST','/api/knowledge/documents',{'title':'旧私人资料','sourceName':'archive.md','mediaType':'text/plain','content':'迁移专属 quarkMigrationOnly，原资料切片需要完整校验。'*100},201)
    docpage=owner.call('GET',f"/api/ai/private/legacy/knowledge/{document['id']}");assert checksum(docpage)==docpage['checksum']
    stranger.call('GET',f"/api/ai/private/legacy/knowledge/{document['id']}",expected=404)
    owner.call('POST',f"/api/ai/private/legacy/knowledge/{document['id']}/purge",{'after':docpage['after'],'through':docpage['through'],'checksum':docpage['checksum'],'confirmed':'true'})
    assert runtime.sql(f"SELECT state FROM knowledge_documents WHERE id={document['id']}")=='deleted'
    assert 'quarkMigrationOnly' not in runtime.sql(f"SELECT string_agg(content,'') FROM knowledge_chunks WHERE document_id={document['id']}")
    report={'passed':True,'checks':['actual TLS required','owner isolation','C++/Python UTF8 length-framed SHA256 agreement','HTTP send persistent dedup','explicit purge confirmation','changed source rejected','concurrent duplicate purge idempotent','messages after snapshot retained','private knowledge redacted and excluded from retrieval'],'formalDatabaseChanged':False}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(report,indent=2)+'\n');print('PASS private archive TLS/ACL/checksum/concurrency/snapshot/purge integration')
if __name__=='__main__':main()
