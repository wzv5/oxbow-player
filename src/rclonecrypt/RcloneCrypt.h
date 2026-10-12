#pragma once

// N98 §1：rclone crypt 纯算法层——与 Qt / mpv 无关。密钥派生（scrypt，交
// libsodium）、名字加解密（EME + PKCS#7 + base32hex / base64 + obfuscate / off +
// 版本串）、尺寸换算、calculateUnderlying 与内容层原语（块解密、nonce 加法）。
// 平台差异只经 RcloneCryptAes.h 的 AesBlock 一个接口进入，本层不含任何平台分支；
// 错误面是自有的 Error 枚举（取值与 rclone crypt 的错误名一一对应，供单测向量
// 逐码断言），可读文案由提供者边界拼装。蓝本：探针 _crypt_probe 的
// Prims.h / Encoding.cpp / Eme.cpp / Cipher.cpp。

#include "RcloneCryptAes.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class RcloneCrypt {
  public:
    // ——名字加密三态与编码（与 rclone 的 filename_encryption / filename_encoding
    //   一一对应；base32768 非目标，Params 里没有它）——
    enum class NameMode { Standard, Obfuscate, Off };
    enum class NameEncoding { Base32, Base64 };

    // 错误码：与 rclone crypt 的错误名对应（探针 Err 枚举的收编；FileClosed /
    // Platform 属探针与流的内部事务，不进算法层）。base64 的解码失败上游是直接
    // 外抛 stdlib 的 CorruptInputError、没有具名错误，本层另立一码，否则 base64
    // 列表只能拿到 "bad base32 filename encoding" 这种指错编码的文案
    enum class Error {
        BadBase32Encoding,
        BadBase64Encoding,
        NotAMultipleOfBlocksize,
        TooShortAfterDecode,
        TooLongAfterDecode,
        PaddingNotFound,
        PaddingNotAMultiple,
        PaddingTooLong,
        PaddingTooShort,
        PaddingNotAllTheSame,
        NotAnEncryptedFile,
        EncryptedFileTooShort,
        EncryptedFileBadHeader,
        EncryptedBadMagic,
        EncryptedBadBlock,
    };
    static const char *errorText(Error);

    // 内容层布局：文件 = 头(32) ‖ 块[0..)，块 = MAC(16) ‖ 密文(≤65536)
    static constexpr std::int64_t kHeaderSize = 32;
    static constexpr std::int64_t kBlockDataSize = 64 * 1024;
    static constexpr std::int64_t kBlockOverhead = 16;
    static constexpr std::int64_t kBlockSize = kBlockOverhead + kBlockDataSize;
    // 文件头 magic："RCLONE\x00\x00"（8 字节，后两位 NUL——string_view 带显式长度，
    // 禁止用 sv 字面量：operator""sv 按 strlen 取长会停在首个 NUL）
    static constexpr std::size_t kMagicLen = 8;
    static constexpr std::string_view kMagic{"RCLONE\x00\x00", 8};

    struct Params {
        std::string password; // 空 ⇒ 三段密钥全零（rclone 的明示性质，不走 scrypt）
        std::string salt;     // 空 ⇒ 用 rclone 内置 salt
        NameMode mode = NameMode::Standard;
        NameEncoding encoding = NameEncoding::Base32;
        bool dirNameEncrypt = true;  // directory_name_encryption
        std::string suffix = ".bin"; // 仅 Off 模式生效；字面 "none" 大小写不敏感地归空
        bool passBadBlocks = false;  // 坏块交付全零而非报错（读侧策略由流执行）
    };

    using Bytes = std::vector<std::uint8_t>;

    // 构造唯一入口：派生三段密钥并初始化名字层 AES。base32768 在本层类型上不可
    // 表示（NameEncoding 只有两取值），参数页也没有该入口——手改 INI 塞进来的域外
    // 取值由提供者侧按"配置数据异常"显影，不下到本层；口令为空 ⇒ 全零密钥；否则
    // scrypt(password, salt?, N=16384, r=8, p=1, dkLen=80)。派生一次完成，
    // 结果不可变，供枚举线程与 demux 线程共享（N98 §3）
    static std::expected<std::shared_ptr<const RcloneCrypt>, std::string> create(const Params &p);

    // ——名字层：路径按 '/' 分段；文件名的末段版本串留明文，目录名整段加密——
    // 加密侧失败实际不可达：可解密名经 unpad 后 ≤2032 字节，重加密必落 2048 内；
    // 本仓只用自己解出的名字（或本地目录段 ≤255 字符）回加密
    std::string encryptFileName(std::string_view plain) const;
    std::string encryptDirName(std::string_view plain) const;
    std::expected<std::string, Error> decryptFileName(std::string_view in) const;
    std::expected<std::string, Error> decryptDirName(std::string_view in) const;

    // ——尺寸换算（列表尺寸与播放总长都靠它，不读内容）——
    static std::int64_t encryptedSize(std::int64_t plain);
    static std::expected<std::int64_t, Error> decryptedSize(std::int64_t enc);

    // 明文 (offset,limit) → 底层 (offset,limit) + 首块丢弃字节 + 块号；
    // limit < 0 表示读到文件尾
    static void calculateUnderlying(std::int64_t offset, std::int64_t limit,
                                    std::int64_t &underlyingOffset, std::int64_t &underlyingLimit,
                                    std::int64_t &discard, std::int64_t &blocks);

    // ——内容层原语（RcloneCryptByteStream 编排用；buffering 与 seek 策略在流侧）——
    // 24 字节 nonce 按小端整数加 x（块序号 → 块 nonce）
    static void nonceAdd(std::uint8_t nonce[24], std::uint64_t x);
    // 块解密：box = MAC(16) ‖ 密文，nonce = 文件 nonce + 块序号；MAC 不符 ⇒
    // Error::EncryptedBadBlock（是否降级全零由流按 params().passBadBlocks 决定）
    std::expected<void, Error> openBlock(const std::uint8_t *box, std::size_t boxLen,
                                         const std::uint8_t nonce[24], Bytes &plain) const;

    // ——名字编码与 PKCS7：公开为静态面（单测向量须逐条触达判定口径；实现自写，
    //   接受域与错误分支与 rclone 严格一致）——
    static std::string base32HexEncode(const Bytes &in);
    static std::expected<Bytes, Error> base32HexDecode(std::string_view s);
    static std::string base64UrlEncode(const Bytes &in);
    static std::expected<Bytes, Error> base64UrlDecode(std::string_view s);
    static Bytes pkcs7Pad(std::string_view in);
    static std::expected<Bytes, Error> pkcs7Unpad(const Bytes &in);

    const Params &params() const { return m_params; }
    // 密钥三段的只读出口（TestKey 向量断言用；dataKey 另供内容层原语内部使用）
    const Bytes &dataKey() const { return m_dataKey; }
    const Bytes &nameKey() const { return m_nameKey; }
    const Bytes &nameTweak() const { return m_nameTweak; }

  private:
    RcloneCrypt() = default;

    std::string encryptSegment(const std::string &plain) const;
    std::expected<std::string, Error> decryptSegment(const std::string &in) const;
    std::string encryptPath(const std::string &plain, bool stripVersion) const;
    std::expected<std::string, Error> decryptPath(const std::string &in, bool stripVersion) const;

    Params m_params;
    Bytes m_dataKey;   // 32：secretbox 用
    Bytes m_nameKey;   // 32：AES-256 名字分组密码
    Bytes m_nameTweak; // 16：EME 的 tweak
    std::unique_ptr<AesBlock> m_aes;
};
