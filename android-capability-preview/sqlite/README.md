# Offline Android SQLite dependency probe

This isolated compile/link check needs a **pre-supplied exact** official
`2026/sqlite-amalgamation-3540100.zip` and a caller-trusted preinstalled official
NDK r28c (28.2.13676358), Linux x86-64. It never downloads, installs, signs,
uploads or runs Android code:

```sh
python3 android-capability-preview/sqlite/test_verify.py
python3 android-capability-preview/sqlite/verify.py --archive /trusted/archive.zip --ndk /trusted/ndk
```

The [publisher listing](https://sqlite.org/download.html?topic=1057) identifies
the ZIP digest; the [3.54.1 release](https://www.sqlite.org/releaselog/3_54_1.html)
identifies the sqlite3.c digest and source ID. The crate release's
[vendored header](https://raw.githubusercontent.com/rusqlite/rusqlite/v0.42.0/libsqlite3-sys/sqlite3/sqlite3.h)
matches that version/source ID; a Cargo.lock crate checksum is not a ZIP or
source member hash. SQLite deliverable code is [public domain](https://www.sqlite.org/copyright.html);
this does not apply MIT to SQLite, to MadMedia, or to independently licensed
wrapper/tooling.

The gate validates whole-archive and C-member SHA3-256, version/source ID macros
and bounded safe member layout. It copies two upstream members
from verified bytes into a disposable producer, builds SQLite with thread support
and loadable extensions omitted, then removes the producer. An independent C
consumer links the static archive with API 28/ARM64 and references WAL/checkpoint
and reopen calls. The ELF requires the Android interpreter, an explicit narrow
NDK dynamic closure and local SQLite symbols. This is not WAL runtime validation.
No host/system SQLite fallback, lock/entropy/curl changes, engine or ABI changes
or upstream/ELF artifacts are included.

## Separate target curl trust gap

Do not infer native libcurl trust from Android's framework Network Security
Config. Before any separate local TLS runtime check, pin exact libcurl and its
chosen TLS backend/provenance and verify the actual linked backend supports
[`CURLOPT_CAINFO_BLOB`](https://curl.se/libcurl/c/CURLOPT_CAINFO_BLOB.html).
Only in an isolated loopback fixture, with the correct separate result scope,
use a synthetic PEM CA scoped to that specific handle (copy the blob and check
the option return status). Require TLS
[peer verification](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYPEER.html) and
[hostname verification](https://curl.se/libcurl/c/CURLOPT_SSL_VERIFYHOST.html);
test good, wrong-CA and wrong-name cases, with no ambient trust fallback or
outside-network requests. If exact selected backend support or enforcement is
unproven, stop; do not change production trust or attach live credentials.
