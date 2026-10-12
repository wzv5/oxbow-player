// AES 单块后端（Windows / CNG）。EME 需要正反两个方向的裸分组运算，而 libsodium
// 的公开 API 只有 AES-128-ECB，名字密钥恒为 32 字节（AES-256），故 AES 走 CNG。
// 仅 Windows 入源列表（构建条件唯一权威，源内不叠平台守卫）。
// 坑：windows.h 的 min / max 宏会吃掉 std::min 一类模板名（探针实测踩过），本 TU
// 在包含它之前先定义 NOMINMAX。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RcloneCryptAes.h"

#include <bcrypt.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

// 单块运算失败只可能是 init 契约被破坏（句柄无效 / 缓冲不齐），按不可达显影
void singleBlockFailed(const char *what) {
    std::fprintf(stderr, "oxbow: %s failed on a single AES block (init contract broken)\n", what);
    std::abort();
}

struct CngAesBlock final : AesBlock {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;

    ~CngAesBlock() override {
        if (key)
            BCryptDestroyKey(key);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
    }

    std::expected<void, std::string> init(const std::uint8_t *keyBytes,
                                          std::size_t keyLen) override {
        if (!aesKeyLenValid(keyLen))
            return std::unexpected("AES key length must be 16/24/32");
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0))
            return std::unexpected("BCryptOpenAlgorithmProvider failed");
        // ECB 模式挂在算法提供者上；不带 IV、不设 BCRYPT_BLOCK_PADDING——单块裸分组
        static const wchar_t kEcb[] = BCRYPT_CHAIN_MODE_ECB;
        if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(kEcb)), sizeof(kEcb),
                              0)) {
            BCryptCloseAlgorithmProvider(alg, 0);
            alg = nullptr;
            return std::unexpected("BCryptSetProperty(BCRYPT_CHAIN_MODE_ECB) failed");
        }
        // BCryptGenerateSymmetricKey 的入参是 PUCHAR（非 const），密钥本身只读
        if (BCryptGenerateSymmetricKey(
                alg, &key, nullptr, 0,
                reinterpret_cast<PUCHAR>(const_cast<std::uint8_t *>(keyBytes)),
                static_cast<ULONG>(keyLen), 0)) {
            BCryptCloseAlgorithmProvider(alg, 0);
            alg = nullptr;
            return std::unexpected("BCryptGenerateSymmetricKey failed");
        }
        return {};
    }

    void encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const override {
        ULONG done = 0;
        if (BCryptEncrypt(key, reinterpret_cast<PUCHAR>(const_cast<std::uint8_t *>(in)), 16,
                          nullptr, nullptr, 0, out, 16, &done, 0) ||
            done != 16)
            singleBlockFailed("BCryptEncrypt");
    }

    void decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const override {
        ULONG done = 0;
        if (BCryptDecrypt(key, reinterpret_cast<PUCHAR>(const_cast<std::uint8_t *>(in)), 16,
                          nullptr, nullptr, 0, out, 16, &done, 0) ||
            done != 16)
            singleBlockFailed("BCryptDecrypt");
    }
};

} // namespace

std::unique_ptr<AesBlock> createAesBlock() { return std::make_unique<CngAesBlock>(); }
