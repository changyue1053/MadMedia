"""Hosted receipt for two fixed releases; no archive parsing or execution."""
import argparse
import hashlib
from pathlib import Path
import re
import ssl
import urllib.parse
import urllib.request

from verify import CURL, MBED

CURL_URL='https://curl.se/download/curl-8.23.0.tar.xz'
MBED_URL='https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.2/mbedtls-4.1.2.tar.bz2'
SPECS=((CURL_URL,CURL,'curl.tar.xz'),(MBED_URL,MBED,'mbedtls.tar.bz2'))

def github_asset(url):
    if len(url)>8192:
        return False
    parts=urllib.parse.urlsplit(url)
    return (parts.scheme=='https' and parts.netloc=='release-assets.githubusercontent.com' and
            re.fullmatch(r'/github-production-release-asset/[0-9]+/[a-f0-9-]{36}',parts.path) is not None and
            bool(parts.query) and not parts.fragment)

class Redirect(urllib.request.HTTPRedirectHandler):
    def __init__(self):
        self.followed=False

    def redirect_request(self, request, fp, code, msg, headers, newurl):
        if (self.followed or request.full_url!=MBED_URL or code!=302 or
            not github_asset(newurl)):
            return None
        self.followed=True
        return super().redirect_request(request,fp,code,msg,headers,newurl)

def receipt(response, url, package):
    final=response.geturl()
    if response.status!=200 or not (final==url or (url==MBED_URL and github_asset(final))):
        raise ValueError()
    if response.headers.get_all('Content-Encoding') not in (None,['identity']):
        raise ValueError()
    lengths=response.headers.get_all('Content-Length')
    if lengths is not None and lengths!=[str(package.size)]:
        raise ValueError()
    data=response.read(package.size+1)
    if len(data)!=package.size or hashlib.sha256(data).hexdigest()!=package.digest:
        raise ValueError()
    return data

def fetch(folder):
    for url,spec,name in SPECS:
        opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),Redirect(),
                            urllib.request.HTTPSHandler(context=ssl.create_default_context()))
        request=urllib.request.Request(url,headers={'Accept-Encoding':'identity'})
        with opener.open(request,timeout=15) as response:
            data=receipt(response,url,spec)
        with (folder/name).open('xb') as output:
            output.write(data)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--dest',type=Path,required=True)
    args=parser.parse_args()
    try:
        fetch(args.dest)
    except Exception:
        raise SystemExit(1) from None
