# Third-party notices and first-party status

First-party IMMedia source was supplied by repository owner for migration/publication. The source has no top-level LICENSE. This repository does not invent an open-source license grant for the owner's code; a reusable public licensing choice remains with the owner before general distribution promises.

- `third_party/stb/stb_image.h`, `stb_image_write.h`, `stb_image_resize2.h`: embedded dual MIT / public-domain licenses retained in full. Prefer MIT for redistribution; copyright and permission notices remain in each file. Source bytes pinned in provenance manifest.
- Blurhash portion of `core/pipeline/image/image_pipeline.cpp`: identifies Wolt blurhash C reference derivation. MIT license from https://github.com/woltapp/blurhash/blob/master/License.md (blob 981f9fc5c5c3625329731fb5b9018d74481a06de) included at `LICENSES/blurhash-MIT.txt`, Copyright (c) 2018 Wolt Enterprises. Original derivation revision not recorded by source; do not claim full upstream-revision traceability.
- libcurl: external link dependency, curl license; not copied into this repository.
- SQLite3: external link dependency, public-domain upstream; not copied into this repository.
- Standard C/C++ runtime and OS libraries: supplied by toolchain/platform; redistributors must inspect their exact packaged dependencies.
- MadObs adapter includes contract header supplied by the consumer, not copied or linked to its runtime. A consumer using that optional header must comply with the MadObs contract package's own license.

No third-party binaries are bundled. This inventory is not a substitute for release SBOM and platform redistribution review.

Local patch: `stb_image_write.h` JPEG bit-buffer variables/internal parameters use unsigned int and unsigned shifts to remove reproduced UBSan signed-left-shift undefined behavior. No upstream license text changed. Full image unit tests exercise this path with ASan/UBSan enabled.
