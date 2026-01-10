// Simple MD5 implementation - compatible with OpenSSL MD5 interface
// Public domain implementation
#pragma once

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>

// Map CommonCrypto to OpenSSL-style interface
#define MD5_DIGEST_LENGTH CC_MD5_DIGEST_LENGTH
#define MD5(data, len, md) CC_MD5(data, len, md)

#else
// For non-Apple platforms, we'll need to include openssl
#include <openssl/md5.h>
#endif
