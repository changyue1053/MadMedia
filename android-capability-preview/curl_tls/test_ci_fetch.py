"""Synthetic receipts only; no calls to the release publishers."""
from dataclasses import replace
from email.message import Message
import hashlib
import urllib.request

from ci_fetch import CURL_URL, MBED_URL, Redirect, github_asset, receipt
from verify import CURL

ASSET=('https://release-assets.githubusercontent.com/github-production-release-asset/'
       '456/31cc0901-1144-2ab5-1134-1234567890ab?synthetic=not-a-credential')
DATA=b'synthetic, not publisher content'
SPEC=replace(CURL,size=len(DATA),digest=hashlib.sha256(DATA).hexdigest())

class Fixture:
    def __init__(self,url=CURL_URL,body=DATA,status=200,headers=()):
        self.url=url
        self.body=body
        self.status=status
        self.headers=Message()
        for name,value in headers:
            self.headers[name]=value
    def geturl(self):
        return self.url
    def read(self,size):
        return self.body[:size]

def rejected(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError('invalid receipt accepted')

def main():
    assert github_asset(ASSET)
    for invalid in (ASSET.replace('https:','http:'),ASSET.replace('.com/','.com.evil/'),
                    ASSET.replace('.com/','.com:443/'),ASSET.replace('assets.','assets@evil.'),
                    ASSET.replace('/456/','/not-id/'),ASSET.split('?')[0],ASSET+'#extra'):
        assert not github_asset(invalid)
    mbed=urllib.request.Request(MBED_URL)
    curl=urllib.request.Request(CURL_URL)
    handler=Redirect()
    assert handler.redirect_request(curl,None,302,'Found',Message(),ASSET) is None
    assert handler.redirect_request(mbed,None,301,'Moved',Message(),ASSET) is None
    assert handler.redirect_request(mbed,None,302,'Found',Message(),ASSET).full_url==ASSET
    assert handler.redirect_request(mbed,None,302,'Found',Message(),ASSET) is None
    assert receipt(Fixture(),CURL_URL,SPEC)==DATA
    assert receipt(Fixture(url=ASSET),MBED_URL,SPEC)==DATA
    for invalid in (Fixture(url=ASSET),Fixture(body=DATA+b'extra'),Fixture(body=DATA[::-1]),
                    Fixture(body=DATA[:-1]),Fixture(status=302),
                    Fixture(headers=[('Content-Encoding','gzip')]),
                    Fixture(headers=[('Content-Length','0')]),
                    Fixture(headers=[('Content-Length',str(len(DATA)))]*2)):
        rejected(lambda:receipt(invalid,CURL_URL,SPEC))
    print('PASS: fixed upstream receipt, redirect boundaries, and hash/size rejection')

if __name__=='__main__':
    main()
