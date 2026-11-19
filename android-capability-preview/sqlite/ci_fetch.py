"""Hosted-only receipt for one pinned official SQLite HTTPS archive."""
import hashlib
from pathlib import Path
import ssl
import sys
import urllib.request

from verify import ZIP_HASH

SOURCE = 'https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip'
SIZE = 2990966  # SQLite publisher PRODUCT size receipt.

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None

def open_source():
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
        NoRedirect(),urllib.request.HTTPSHandler(context=ssl.create_default_context())).open(
            urllib.request.Request(SOURCE,headers={'Accept-Encoding':'identity'}),
            timeout=15)

def receipt(response, *, size, expected):
    if response.status!=200 or response.geturl()!=SOURCE:
        raise ValueError()
    if response.headers.get('Content-Encoding','identity').lower()!='identity':
        raise ValueError()
    lengths = response.headers.get_all('Content-Length') or []
    if lengths and lengths!=[str(size)]:
        raise ValueError()
    payload = response.read(size+1)
    if len(payload)!=size or hashlib.sha3_256(payload).hexdigest()!=expected:
        raise ValueError()
    return payload

def main():
    try:
        if len(sys.argv)!=2:
            return 1
        with open_source() as response:
            payload = receipt(response,size=SIZE,expected=ZIP_HASH)
        with Path(sys.argv[1]).open('xb') as out:
            out.write(payload)
    except Exception:
        return 1
    return 0

if __name__=='__main__':
    raise SystemExit(main())
