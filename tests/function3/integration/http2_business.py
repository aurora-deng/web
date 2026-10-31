"""Real TLS/ALPN h2 request, authenticated business dispatch, and a reset sibling stream.

HPACK uses only literal fields without indexing/Huffman; production still uses nghttp2.
"""
import json
import socket
import ssl

def integer(value,prefix):
    bound=(1<<prefix)-1
    if value<bound:return bytes([value])
    result=bytearray([bound]);value-=bound
    while value>=128:result.append((value&127)|128);value>>=7
    result.append(value);return bytes(result)

def text(value):
    data=value.encode('utf-8');return integer(len(data),7)+data

def frame(kind,flags,stream,payload=b''):
    return len(payload).to_bytes(3,'big')+bytes([kind,flags])+stream.to_bytes(4,'big')+payload

def check_http2(client):
    # Keep the business suite on the owned test listener. The former hard-coded
    # 18443 targeted the daily service and used a different certificate.
    port=18444
    server_name='192.168.239.135'
    context=ssl.create_default_context(cafile='/home/pikachu/phase11-data/function3/runtime/localhost.crt')
    context.set_alpn_protocols(['h2'])
    fields=[(':method','GET'),(':scheme','https'),(':path','/api/tree/posts?limit=1'),(':authority',f'{server_name}:{port}'),('cookie',client.cookie)]
    block=b''.join(b'\x00'+text(name)+text(value) for name,value in fields)
    with socket.create_connection((client.host,port),timeout=20) as plain,context.wrap_socket(plain,server_hostname=server_name) as channel:
        assert channel.selected_alpn_protocol()=='h2'
        # Streams 1 and 5 finish independently; stream 3 is cancelled in the same input batch.
        channel.sendall(b'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n'+frame(4,0,0)+frame(1,5,1,block)+frame(1,5,3,block)+frame(3,0,3,(8).to_bytes(4,'big'))+frame(1,5,5,block))
        def exact(count):
            result=bytearray()
            while len(result)<count:
                part=channel.recv(count-len(result));assert part,'h2 connection closed early';result.extend(part)
            return bytes(result)
        bodies={1:bytearray(),5:bytearray()};complete=set();headers=set()
        for _ in range(100):
            h=exact(9);size=int.from_bytes(h[:3],'big');assert size<=1024*1024
            kind,flags=h[3],h[4];stream=int.from_bytes(h[5:],'big')&0x7fffffff;payload=exact(size)
            if kind==4 and not flags&1:channel.sendall(frame(4,1,0))
            if kind==7:raise AssertionError('unexpected GOAWAY during h2 business request')
            if stream==3 and kind==0:raise AssertionError('reset stream received late DATA')
            if stream in bodies:
                if kind==1:headers.add(stream)
                if kind==0:
                    if flags&8:payload=payload[1:len(payload)-payload[0]]
                    bodies[stream].extend(payload)
                if kind in (0,1) and flags&1:complete.add(stream)
            if complete=={1,5}:break
        assert complete==headers=={1,5}
        for body in bodies.values():assert 'posts' in json.loads(body),'authenticated route did not return its JSON DTO'
    print('Verified certificate, ALPN=h2, two authenticated business streams and sibling RST_STREAM passed')
