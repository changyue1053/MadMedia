// Test-only stand-in, NOT an Apple SDK header or platform ABI validation.
#pragma once
#include <stddef.h>
using SecRandomRef=const void*;
extern const SecRandomRef kSecRandomDefault;
int SecRandomCopyBytes(SecRandomRef,size_t,void*);
