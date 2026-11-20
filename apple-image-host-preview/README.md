# ARM64 macOS installed ImageIO host qualification

This is a separate host-only evidence checkpoint, not a macOS SDK release and
not iOS, Simulator, device, Android, HarmonyOS or full-engine validation.
No Simulator installation or boot, network inside the probe, user media,
outputs upload, new libraries or production/default changes.

The codec and header are byte-identical to published MadMedia
`a0a1523fcd7adcf7a63b292a063e8632ba36b8f8`, proven by pinned input digests.
A disposable producer builds them against the stock macOS SDK (Xcode 26.2,
17C52; SDK 26.2), installs a static archive and the one header, then is deleted
along with its build directory after package relocation. An independent
Objective-C++ command-line consumer links the relocated files. Both the
codec object and the final binary must be ARM64 with Mach-O macOS platform
metadata; the binary defines the real preview symbol and links Apple ImageIO
and CoreGraphics. The final binary runs explicitly as ARM64.

The host builds the original synthetic image fixture and two checks by reusing
the iOS sample's fixture/assertion source minus its UI and runtime scaffolding.
The two process modes verify real decode/downsize/encode/redecode, no enlargement,
broad pixel checks, a high-vs-low quality response, and argument/size/capacity
bounds with no output-buffer writes on failure. Metadata proof is narrowly that
the known input TIFF Artist marker is present on input and absent from output,
and that tested GPS/Exif/IPTC dictionaries are absent in the output. This does
not prove all possible metadata is absent or that ImageIO emits no new metadata.

Local Python tests are synthetic/mocked structure checks only. The native gate
is scoped to `checkpoint/madmedia-apple-image-host-preview`, with no automatic
crossover to the iOS qualification branch. Native evidence requires verifying
an exact successful future run at both host modes.
