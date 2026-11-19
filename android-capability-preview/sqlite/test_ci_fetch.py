"""Offline receipt tests; no simulated bytes equal publisher proof."""
from email.message import Message
import hashlib
import io

from ci_fetch import NoRedirect, SOURCE, receipt

class Fixture:
    def __init__(self, content, url=SOURCE, status=200, headers=()):
        self.data = io.BytesIO(content)
        self.status = status
        self.url = url
        self.headers = Message()
        for name,value in headers:
            self.headers[name] = value

    def read(self, length):
        return self.data.read(length)

    def geturl(self):
        return self.url

def main():
    data = b'explicitly synthetic archive bytes'
    expected = hashlib.sha3_256(data).hexdigest()
    assert receipt(Fixture(data),size=len(data),expected=expected)==data
    for invalid in (Fixture(data+b'overflow'),Fixture(data[:-1]),Fixture(data[::-1]),
                    Fixture(data,url='https://unrelated.invalid/archive.zip'),
                    Fixture(data,status=302),
                    Fixture(data,headers=[('Content-Encoding','gzip')]),
                    Fixture(data,headers=[('Content-Length','100')]),
                    Fixture(data,headers=[('Content-Length',str(len(data)))]*2)):
        try:
            receipt(invalid,size=len(data),expected=expected)
        except ValueError:
            continue
        raise AssertionError('bad receipt accepted')
    assert NoRedirect().redirect_request(None,None,302,None,None,SOURCE) is None
    print('PASS: synthetic fixed HTTPS receipt, hash/length/redirect rejection')

if __name__=='__main__':
    main()
