"""Portable policy/parser tests only. No namespace, OpenSSL, or sockets run."""
from queue import Queue
import ssl
from unittest.mock import patch

from namespace_fixture import CASES, links_ok, request_ok, routes_ok, scope_ok, serve
from run_host import Rejected, host_elf

def test_namespace():
    ok=['user:[1]','net:[2]','user:[3]','net:[4]','0 1000 1',1000,0]
    assert scope_ok(*ok)
    for index,value in ((0,'bad'),(1,'net:[4]'),(2,'user:[1]'),(3,'net:[2]'),
                        (4,'0 0 4294967295'),(4,'0 1000 1\n1 1001 1'),(5,0),(6,1000)):
        change=ok.copy()
        change[index]=value
        assert not scope_ok(*change)
    lo={'ifname':'lo','flags':['LOOPBACK','UP']}
    assert links_ok([lo],True)
    assert not links_ok([lo],False)
    assert not links_ok([lo,{'ifname':'eth0','flags':['UP']}],True)
    assert not links_ok([],True)
    assert routes_ok([])
    assert routes_ok([{'dev':'lo','dst':'127.0.0.0/8'},{'dev':'lo','dst':'::1'}])
    assert not routes_ok([{'dev':'eth0','dst':'127.0.0.1/32'}])
    assert not routes_ok([{'dev':'lo','dst':'default'}])
    assert not routes_ok([{'dev':'lo','dst':'127.0.0.0/8','gateway':'127.0.0.1'}])
    assert not routes_ok([{'dev':'lo','dst':'192.0.2.1'}])

def test_outcomes():
    request=b'GET /madmedia-proof HTTP/1.1\r\nHost: media.test:41000\r\n\r\n'
    assert request_ok(request,41000)
    for altered in (request[:-1],request.replace(b'GET',b'POST'),
                    request.replace(b'media.test',b'wrong.test'),request+b'x',
                    request.replace(b'\r\n\r\n',b'\r\nHost: media.test:41000\r\n\r\n')):
        assert not request_ok(altered,41000)
    assert CASES==(('match','a',True),('wrong-ca','b',False),('wrong-name','a',False),('match','a',True))

def test_negative_never_accepts_partial_http():
    class FakePeer:
        def __init__(self, partial):
            self.partial=partial
            self.recvs=0
        def __enter__(self):
            return self
        def __exit__(self, *_error):
            return False
        def settimeout(self, _timeout):
            pass
        def recv(self, _length):
            self.recvs+=1
            if self.partial and self.recvs==1:
                return b'G'
            raise ssl.SSLError('synthetic TLS shutdown')

    class FakeListener:
        def __init__(self, peer):
            self.peer=peer
        def accept(self):
            return self.peer,('127.0.0.1',41000)

    class FakeContext:
        def wrap_socket(self, client, *, server_side):
            assert server_side
            return client

    with patch('namespace_fixture.CASES',(('wrong-ca','b',False),)):
        for partial in (False,True):
            peer=FakePeer(partial)
            results=Queue()
            serve(FakeListener(peer),FakeContext(),41000,results)
            expected=('error',False) if partial else ('peer-rejected',True)
            assert results.get_nowait()==expected and results.empty()
            # The planned follow-up SSL error is never consumed after one byte.
            assert peer.recvs==1

def test_closure():
    elf=(' Class: ELF64\n Type: DYN (PIE)\n Machine: Advanced Micro Devices X86-64\n'
         ' [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]\n'
         ' (NEEDED) Shared library: [libc.so.6]\n')
    syms='\n'.join('100 T '+k for k in ('curl_easy_perform','curl_version_info','mbedtls_ssl_init'))
    host_elf(elf,syms)
    for bad,error in ((elf+' (NEEDED) Shared library: [libssl.so.3]',syms),
                      (elf+' (NEEDED) Shared library: [libcurl.so.4]',syms),
                      (elf+' (RUNPATH) /fake',syms),(elf.replace('X86-64','ARM'),syms),
                      (elf,syms.replace(' T ',' U '))):
        try:
            host_elf(bad,error)
        except Rejected:
            continue
        raise AssertionError('invalid closure accepted')

if __name__=='__main__':
    test_namespace()
    test_outcomes()
    test_closure()
    test_negative_never_accepts_partial_http()
    print('PASS: synthetic host isolation policy, response cases and ELF closure')
