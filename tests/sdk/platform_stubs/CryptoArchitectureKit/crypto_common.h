// Test-only stand-in, NOT a HarmonyOS SDK header or ABI validation.
#pragma once
#include <stddef.h>
#include <stdint.h>
using OH_Crypto_ErrCode=int;
constexpr OH_Crypto_ErrCode CRYPTO_SUCCESS=0;
struct Crypto_DataBlob{uint8_t* data;size_t len;};
void OH_Crypto_FreeDataBlob(Crypto_DataBlob*);
