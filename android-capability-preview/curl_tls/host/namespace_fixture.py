"""Runs only inside a distinct, unprivileged experiment user+network namespace."""
import ipaddress
import json
import os
from pathlib import Path
import queue
import re
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

PAYLOAD=b'madmedia synthetic loopback\n'
CASES=(('match','a',True),('wrong-ca','b',False),('wrong-name','a',False),('match','a',True))

class Rejected(Exception):
    pass

def scope_ok(out_user,out_net,inner_user,inner_net,uid_map,expected_uid,euid):
    return (euid==0 and expected_uid>0 and
            re.fullmatch(r'user:\[[0-9]+\]',out_user) is not None and
            re.fullmatch(r'net:\[[0-9]+\]',out_net) is not None and
            re.fullmatch(r'user:\[[0-9]+\]',inner_user) is not None and
            re.fullmatch(r'net:\[[0-9]+\]',inner_net) is not None and
            inner_user!=out_user and inner_net!=out_net and
            uid_map.split()==['0',str(expected_uid),'1'])

def links_ok(links,up):
    if len(links)!=1 or links[0].get('ifname')!='lo':
        return False
    flags=links[0].get('flags',[])
    return 'LOOPBACK' in flags and ('UP' in flags)==up

def routes_ok(routes):
    for route in routes:
        if route.get('dev')!='lo':
            return False
        try:
            if not ipaddress.ip_network(route.get('dst','default'),strict=False).is_loopback:
                return False
        except ValueError:
            return False
        if route.get('gateway') or route.get('via'):
            return False
    return True

def call(argv):
    with tempfile.TemporaryFile() as log:
        subprocess.run([str(x) for x in argv],stdin=subprocess.DEVNULL,stdout=log,
                       stderr=subprocess.DEVNULL,timeout=8,check=True)
        log.seek(0)
        data=log.read(32769)
        if len(data)>32768:
            raise Rejected()
        return data

def activate_lo(ip):
    if not links_ok(json.loads(call([ip,'-j','link','show'])),False):
        raise Rejected()
    for af in ('-4','-6'):
        if not routes_ok(json.loads(call([ip,'-j',af,'route','show','table','all']))):
            raise Rejected()
    call([ip,'link','set','dev','lo','up'])
    if not links_ok(json.loads(call([ip,'-j','link','show'])),True):
        raise Rejected()
    for af in ('-4','-6'):
        if not routes_ok(json.loads(call([ip,'-j',af,'route','show','table','all']))):
            raise Rejected()

def certs(openssl,workspace):
    os.umask(0o077)
    for ca in ('a','b'):
        call([openssl,'req','-new','-x509','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1',
              '-nodes','-days','1','-subj',f'/CN=MadMedia synthetic CA {ca}',
              '-addext','basicConstraints=critical,CA:TRUE','-addext','keyUsage=critical,keyCertSign',
              '-keyout',workspace/f'{ca}.key','-out',workspace/f'{ca}.pem'])
    call([openssl,'req','-new','-newkey','ec','-pkeyopt','ec_paramgen_curve:prime256v1','-nodes',
          '-subj','/CN=media.test','-keyout',workspace/'leaf.key','-out',workspace/'leaf.csr'])
    extension=workspace/'leaf.ext'
    extension.write_text('basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n'
                         'extendedKeyUsage=serverAuth\nsubjectAltName=DNS:media.test\n')
    call([openssl,'x509','-req','-in',workspace/'leaf.csr','-CA',workspace/'a.pem',
          '-CAkey',workspace/'a.key','-CAserial',workspace/'a.serial','-CAcreateserial',
          '-extfile',extension,'-days','1','-out',workspace/'leaf.pem'])

def request_ok(request,port):
    lines=request.split(b'\r\n')
    expected=f'host: media.test:{port}'.encode()
    return (request.endswith(b'\r\n\r\n') and lines[0]==b'GET /madmedia-proof HTTP/1.1' and
            sum(line.lower()==expected for line in lines[1:])==1)

def serve(listener,context,port,results):
    for _,_,http_expected in CASES:
        try:
            client,addr=listener.accept()
            with client:
                if addr[0]!='127.0.0.1':
                    raise Rejected()
                client.settimeout(5)
                try:
                    with context.wrap_socket(client,server_side=True) as peer:
                        request=b''
                        while not request.endswith(b'\r\n\r\n'):
                            data=peer.recv(1024)
                            if not data:
                                break
                            # A TLS shutdown after partial application data is
                            # not proof of certificate rejection.
                            if not http_expected:
                                raise Rejected()
                            request+=data
                            if len(request)>4096:
                                raise Rejected()
                        if request:
                            if not http_expected or not request_ok(request,port):
                                raise Rejected()
                            response=b'HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: '
                            peer.sendall(response+str(len(PAYLOAD)).encode()+b'\r\n\r\n'+PAYLOAD)
                            results.put(('http',True))
                        elif not http_expected:
                            results.put(('peer-rejected',True))
                        else:
                            raise Rejected()
                except ssl.SSLError:
                    if http_expected:
                        raise
                    results.put(('peer-rejected',True))
        except Exception:
            results.put(('error',False))
            return

def run(client,workspace):
    ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version=ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(workspace/'leaf.pem',workspace/'leaf.key')
    with socket.socket(socket.AF_INET,socket.SOCK_STREAM) as listener:
        listener.settimeout(7)
        listener.bind(('127.0.0.1',0))
        listener.listen(1)
        port=listener.getsockname()[1]
        results=queue.Queue(maxsize=4)
        worker=threading.Thread(target=serve,args=(listener,ctx,port,results),daemon=True)
        worker.start()
        for name,ca,good in CASES:
            call([client,port,workspace/f'{ca}.pem',name])
            if results.get(timeout=7)!=(('http' if good else 'peer-rejected'),True):
                raise Rejected()
        worker.join(timeout=2)
        if worker.is_alive() or not results.empty():
            raise Rejected()

def main():
    out_user,out_net,uid,ip,openssl,binary,cert_dir=sys.argv[1:]
    if not scope_ok(out_user,out_net,str(Path('/proc/self/ns/user').readlink()),
                    str(Path('/proc/self/ns/net').readlink()),Path('/proc/self/uid_map').read_text(),
                    int(uid),os.geteuid()):
        raise Rejected()
    activate_lo(Path(ip))
    stage=Path(cert_dir)
    certs(Path(openssl),stage)
    run(Path(binary),stage)
    print('PASS: Linux loopback TLS exact two HTTP, two certificate rejections',flush=True)

if __name__=='__main__':
    try:
        main()
    except Exception:
        print('FAIL: contained Linux loopback TLS acceptance',flush=True)
        raise SystemExit(1) from None
