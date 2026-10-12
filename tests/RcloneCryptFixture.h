#pragma once

// 测试侧的 rclone crypt 内容层**加密**装配（N98 §9）。本仓实现的是只读后端，算法层只有
// 解密原语（openBlock），跨块推进、边界 seek 与坏块语义因此无法用自产数据验证——
// 这里按 §5 的布局正向铺一层 libsodium secretbox，与 RcloneCrypt::openBlock /
// decryptedSize 互为逆运算，使单测能合成真密文而不依赖外部数据目录。
// 布局：文件 = "RCLONE\0\0"(8) ‖ nonce(24) ‖ 块[0..)，块 k = MAC(16) ‖ 密文(≤64 KiB)，
// 第 k 块的 nonce = 文件 nonce 按 24 字节小端整数 + k。

#include "rclonecrypt/RcloneCrypt.h"

#include <sodium.h>

#include <cstdint>
#include <string>
#include <vector>

namespace RcloneCryptFixture {

// 逐块 secretbox_easy 封装：产出恰为 N98 §5 的块序列（MAC 前置）
inline RcloneCrypt::Bytes sealFile(const RcloneCrypt::Bytes &dataKey,
                                   const RcloneCrypt::Bytes &fileNonce,
                                   const std::vector<RcloneCrypt::Bytes> &plainBlocks) {
    RcloneCrypt::Bytes out;
    const std::string magic("RCLONE\x00\x00", 8);
    out.insert(out.end(), magic.begin(), magic.end());
    out.insert(out.end(), fileNonce.begin(), fileNonce.end());
    for (std::size_t k = 0; k < plainBlocks.size(); ++k) {
        RcloneCrypt::Bytes nonce = fileNonce;
        RcloneCrypt::nonceAdd(nonce.data(), std::uint64_t(k));
        const RcloneCrypt::Bytes &plain = plainBlocks[k];
        RcloneCrypt::Bytes box(plain.size() + crypto_secretbox_MACBYTES);
        crypto_secretbox_easy(box.data(), plain.data(),
                              static_cast<unsigned long long>(plain.size()), nonce.data(),
                              dataKey.data());
        out.insert(out.end(), box.begin(), box.end());
    }
    return out;
}

// 确定性明文块：块 b 内第 i 字节 = (b*7 + i) & 0xFF，供逐字节比对
inline RcloneCrypt::Bytes pattern(std::size_t len, std::size_t blockIndex) {
    RcloneCrypt::Bytes out(len);
    for (std::size_t i = 0; i < len; ++i)
        out[i] = std::uint8_t((blockIndex * 7 + i) & 0xFF);
    return out;
}

// 拼接整段明文（与 sealFile 的块序一致）
inline RcloneCrypt::Bytes concatBlocks(const std::vector<RcloneCrypt::Bytes> &blocks) {
    RcloneCrypt::Bytes out;
    for (const RcloneCrypt::Bytes &b : blocks)
        out.insert(out.end(), b.begin(), b.end());
    return out;
}

} // namespace RcloneCryptFixture
