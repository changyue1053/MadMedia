# Isolated Apple capability preview

This optional test-only gate does not compile/link the MadMedia engine or
enable its C ABI. It uses no transferable media, networking, curl, SQLite,
third-party source, Java/JNI, artifacts or distribution. The workflow runs
synthetic operations on an ARM64 macOS GitHub runner with Xcode/macOS SDK 26.2.
No iOS package, simulator, device, SDK integration or background transfer is
claimed.

- The standalone lease keeps a nonblocking advisory flock on a fixed file in
  an owned mode-0700 directory. The fixture checks contention, reacquisition,
  and invalid inputs including final directory/lock symlinks, hard-linked
  lock files, FIFO lock files and non-private directories. The parent path
  must be trusted and stable; this is cooperative coordination, not an
  arbitrary path sandbox. Never replace/remove a lock file with a live lease.
- The original public Apple entropy candidate links the system Security
  framework, not a substitute. The fixture checks two successful 16-byte
  calls, with a simple catastrophic repeated/zero output check; that is not
  certification of randomness, collision resistance or error injection.

Official references: [Apple flock manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/flock.2.html)
and [SecRandomCopyBytes](https://developer.apple.com/documentation/security/secrandomcopybytes(_:_:_:)).
