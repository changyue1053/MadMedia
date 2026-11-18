// Candidate provider for HarmonyOS API 20+, libohcrypto; not qualified.
#include "entropy.h"
#include <CryptoArchitectureKit/crypto_common.h>
#include <CryptoArchitectureKit/crypto_rand.h>
#include <cstring>
namespace madmedia_platform {
bool entropy128(std::array<unsigned char,16>& out) noexcept {
 out.fill(0);OH_CryptoRand* ctx=nullptr;
 auto result=OH_CryptoRand_Create(&ctx);
 if(result!=CRYPTO_SUCCESS || !ctx){if(ctx)OH_CryptoRand_Destroy(ctx);return false;}
 Crypto_DataBlob blob{};
 result=OH_CryptoRand_GenerateRandom(ctx,16,&blob);
 const bool ok=result==CRYPTO_SUCCESS && blob.data && blob.len==16;
 if(ok)std::memcpy(out.data(),blob.data,16);
 if(blob.data)OH_Crypto_FreeDataBlob(&blob);
 OH_CryptoRand_Destroy(ctx);return ok;
}
}
