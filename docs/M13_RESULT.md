# M13: installed multi-app consumption gate

Added standalone C++ and pure-C app consumers, an install-relocation verifier,
and a fixed-OS cross-compiler CI matrix. The verifier deletes only its owned
producer copies/build products before consuming the moved installation, and
checks separate-app journal/ID isolation after reopening each data directory.
Optional MadObs coverage is explicitly a pinned observer-contract adapter test,
not qualification of the full MadObs runtime.

Local GCC 14.2 Obs OFF and ON runs passed. Cross GCC 13/Clang 18 claims depend on
actual four-cell CI for the published commit, recorded in the external phase
publication evidence. No native runtime change, mobile qualification, release
or license change is implied. See PACKAGE_MULTI_APP.md for the exact boundaries.
