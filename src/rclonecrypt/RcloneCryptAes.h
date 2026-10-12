#pragma once

// N98 §8：AES 单块抽象——EME（名字层）需要的全部密码学面：单块、ECB、无填充、
// 密钥 16 / 24 / 32。全仓只有 AES 需要平台后端（libsodium 的公开 AES 只有 128 位
// ECB，而名字密钥恒 32 字节），故抽象收成这个最小面，算法层（RcloneCrypt）只经
// 此接口取用 AES，自身不含任何平台分支。
//
// 两个后端各一个 .cpp，按平台入源列表（都在本目录内，整目录随 OXBOW_WITH_RCLONE_CRYPT
// 一起收放）：
//   src/rclonecrypt/rclonecrypt_aes_cng.cpp      Windows / BCrypt（WIN32）
//   src/rclonecrypt/rclonecrypt_aes_openssl.cpp  Linux / OpenSSL EVP（NOT WIN32）
// 以构建条件为唯一权威（X11Util 纪律）：错平台把它编进去会在缺 CNG / OpenSSL 处
// 直接编译失败；源内二次守卫反而把编译错降级成链接期未定义符号。工厂
// createAesBlock 落哪个后端由编译期决定，不做运行期探测。
//
// 本层与算法层同口径：不依赖 Qt / mpv，错误面用 std::string，提供者边界再转
// QString。encrypt / decrypt 无错误分支——单块裸分组失败只可能是 init 契约被破坏，
// 实现按不可达显影（终止），不给 EME 热路径添错误检查。

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

class AesBlock {
  public:
    virtual ~AesBlock() = default;

    // 初始化密钥：keyLen 只接受 16 / 24 / 32（共享校验见 aesKeyLenValid）。
    // 平台资源打开失败经错误串显影
    virtual std::expected<void, std::string> init(const std::uint8_t *key, std::size_t keyLen) = 0;

    // 单块正 / 反分组运算（16 字节进出，调用方保证缓冲）
    virtual void encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const = 0;
    virtual void decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const = 0;
};

// 具体后端由编译期源列表决定（Windows=CNG、Linux=OpenSSL）
std::unique_ptr<AesBlock> createAesBlock();

// 两后端共享的密钥长度校验（RcloneCryptAes.cpp 单点定义）：合法取 16 / 24 / 32
bool aesKeyLenValid(std::size_t keyLen);
