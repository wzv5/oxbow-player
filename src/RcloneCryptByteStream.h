#pragma once

#include "ByteStream.h"
#include "pch.h"

#include "rclonecrypt/RcloneCrypt.h"

// N98 §5：解密流——ByteStream 装饰器。包装任意底层流（本地文件、将来的
// HttpByteStream），按**密文**偏移取数并解密成按**明文**偏移的只读流；来源无关，
// 是 WebDAV 上的 crypt 的留位落点（N98 背景）。对外能力：恒可寻位、总长恒为
// DecryptedSize（进度与时长因此按明文尺度）。
//
// 行为口径（N98 §5）：
//   - 缓存当前明文块（64 KiB），跨块读取逐块推进；块装载 = 底层 seek 到
//     32 + k·65552，读 ≤65552 字节（尾块短读合法），nonce = 文件 nonce + 块号；
//   - MAC 校验失败 ⇒ 读错误上抛；pass_bad_blocks 打开时该块交付全零、块号照常
//     推进、后续块继续解密（rclone 同口径）；
//   - seek 为明文绝对偏移，不做范围检查（越过总长恒成功返回目标偏移，N98 §5
//     定案）：只推进 pos 并作废块缓存，块装载时底层自行按偏移取数；
//   - cancel 置旗（原子），下一次块装载前检查即按错误收尾；同时转交底层流。
class RcloneCryptByteStream final : public ByteStream {
  public:
    // 起流工厂（open 期、消费者线程一次性调用）：读底层头 32 字节验 magic
    // （不足 32 字节 ⇒ TooShort；magic 不符 ⇒ BadMagic），按底层总长换算明文
    // 总长（TooShort / BadHeader 同 rclone）。失败即整条流失败，文案英文可读。
    // 底层位置由本类接管，调用方不必复位
    static std::expected<std::shared_ptr<RcloneCryptByteStream>, QString>
    open(const std::shared_ptr<const RcloneCrypt> &cipher,
         const std::shared_ptr<ByteStream> &underlying);

    ~RcloneCryptByteStream() override;

    // ByteStream 接口
    qint64 read(char *out, qint64 maxBytes) override;
    bool isSeekable() const override { return true; }
    qint64 seek(qint64 to) override;
    qint64 size() const override { return m_size; }
    void cancel() override;
    Stats stats() const override;

  private:
    RcloneCryptByteStream(std::shared_ptr<const RcloneCrypt> cipher,
                          std::shared_ptr<ByteStream> underlying, RcloneCrypt::Bytes fileNonce,
                          qint64 plainSize);

    // 装载块 k 到 m_block；失败返回错误码（BadHeader / BadBlock / 底层错误统一
    // kError 的映射在 read 侧）
    bool loadBlock(qint64 blockIndex);

    std::shared_ptr<const RcloneCrypt> m_cipher;
    std::shared_ptr<ByteStream> m_underlying;
    RcloneCrypt::Bytes m_fileNonce; // 文件头里的 24 字节 nonce
    qint64 m_size = 0;              // 明文总长
    qint64 m_pos = 0;               // 明文绝对偏移
    RcloneCrypt::Bytes m_block;     // 当前明文块缓存
    qint64 m_blockIndex = -1;
    std::atomic<bool> m_canceled{false};
    mutable Stats m_stats;
};
