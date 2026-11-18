// Test-only stand-in; no target SDK declaration validation claimed.
#pragma once
#include "crypto_common.h"
struct OH_CryptoRand{};
OH_Crypto_ErrCode OH_CryptoRand_Create(OH_CryptoRand**);
OH_Crypto_ErrCode OH_CryptoRand_GenerateRandom(OH_CryptoRand*,int,Crypto_DataBlob*);
void OH_CryptoRand_Destroy(OH_CryptoRand*);
