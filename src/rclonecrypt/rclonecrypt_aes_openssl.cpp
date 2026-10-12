// AES 单块后端（Linux / OpenSSL libcrypto EVP）。与 CNG 后端同契约：单块、ECB、
// 无填充、密钥 16 / 24 / 32。仅非 Windows 入源列表（构建条件唯一权威，源内不叠
// 平台守卫）；链接 OpenSSL::Crypto，由 CMake 自带 Find 模块给出（N98 §8.3）。

#include "RcloneCryptAes.h"

#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>

namespace {

// 与 CNG 后端同口径：单块运算失败只可能是 init 契约被破坏，按不可达显影
void singleBlockFailed(const char *what) {
    std::fprintf(stderr, "oxbow: %s failed on a single AES block (init contract broken)\n", what);
    std::abort();
}

struct OpensslAesBlock final : AesBlock {
    // 正反两个方向各一个上下文——EVP 的方向是 ctx 属性，不能同 ctx 混用
    EVP_CIPHER_CTX *enc = nullptr;
    EVP_CIPHER_CTX *dec = nullptr;

    ~OpensslAesBlock() override {
        if (enc)
            EVP_CIPHER_CTX_free(enc);
        if (dec)
            EVP_CIPHER_CTX_free(dec);
    }

    std::expected<void, std::string> init(const std::uint8_t *keyBytes,
                                          std::size_t keyLen) override {
        if (!aesKeyLenValid(keyLen))
            return std::unexpected("AES key length must be 16/24/32");
        const EVP_CIPHER *cipher = keyLen == 16   ? EVP_aes_128_ecb()
                                   : keyLen == 24 ? EVP_aes_192_ecb()
                                                  : EVP_aes_256_ecb();
        enc = EVP_CIPHER_CTX_new();
        dec = EVP_CIPHER_CTX_new();
        if (!enc || !dec)
            return std::unexpected("EVP_CIPHER_CTX_new failed");
        // ECB 无 IV；方向用 init 的 enc 标志表达
        if (EVP_CipherInit_ex(enc, cipher, nullptr, keyBytes, nullptr, 1) <= 0 ||
            EVP_CipherInit_ex(dec, cipher, nullptr, keyBytes, nullptr, 0) <= 0) {
            return std::unexpected("EVP_CipherInit_ex failed");
        }
        // 关掉填充：单块裸分组（EVP 对 ECB 默认开 PKCS 填充）
        if (EVP_CIPHER_CTX_set_padding(enc, 0) <= 0 || EVP_CIPHER_CTX_set_padding(dec, 0) <= 0)
            return std::unexpected("EVP_CIPHER_CTX_set_padding(0) failed");
        return {};
    }

    void encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const override {
        int outLen = 0;
        if (EVP_EncryptUpdate(enc, out, &outLen, in, 16) <= 0 || outLen != 16)
            singleBlockFailed("EVP_EncryptUpdate");
    }

    void decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const override {
        int outLen = 0;
        if (EVP_DecryptUpdate(dec, out, &outLen, in, 16) <= 0 || outLen != 16)
            singleBlockFailed("EVP_DecryptUpdate");
    }
};

} // namespace

std::unique_ptr<AesBlock> createAesBlock() { return std::make_unique<OpensslAesBlock>(); }
