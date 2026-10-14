#include "RcloneCryptByteStream.h"

std::expected<std::shared_ptr<RcloneCryptByteStream>, QString>
RcloneCryptByteStream::open(const std::shared_ptr<const RcloneCrypt> &cipher,
                            const std::shared_ptr<ByteStream> &underlying) {
    if (!cipher || !underlying)
        return std::unexpected(QStringLiteral("crypt stream: missing cipher or underlying stream"));

    // 头 32 字节：magic(8) ‖ nonce(24)。底层此刻必在起点（open 期一次），仍显式
    // seek(0) 一次，调用方无需关心底层来时位置
    if (underlying->seek(0) != 0)
        return std::unexpected(QStringLiteral("crypt stream: cannot rewind underlying stream"));
    // 具名变量 + 圆括号：vector<uint8_t> 的花括号初始化会命中
    // initializer_list 重载（单元素语义），vexing-parse 又不收裸 cast 表达式
    const std::size_t headerLen = std::size_t(RcloneCrypt::kHeaderSize);
    RcloneCrypt::Bytes header(headerLen);
    const qint64 got =
        underlying->read(reinterpret_cast<char *>(header.data()), qint64(header.size()));
    if (got < qint64(header.size()))
        return std::unexpected(
            QString::fromLatin1(RcloneCrypt::errorText(RcloneCrypt::Error::EncryptedFileTooShort)));
    if (std::memcmp(header.data(), RcloneCrypt::kMagic.data(), RcloneCrypt::kMagicLen) != 0)
        return std::unexpected(
            QString::fromLatin1(RcloneCrypt::errorText(RcloneCrypt::Error::EncryptedBadMagic)));

    // 明文总长按底层声明总长换算（N98 §5：对外总长一律是 DecryptedSize）
    const qint64 encTotal = underlying->size();
    const auto plain = RcloneCrypt::decryptedSize(encTotal);
    if (!plain)
        return std::unexpected(QString::fromLatin1(RcloneCrypt::errorText(plain.error())));

    RcloneCrypt::Bytes nonce(header.begin() + 8, header.begin() + 32);
    return std::shared_ptr<RcloneCryptByteStream>(
        new RcloneCryptByteStream(cipher, underlying, std::move(nonce), *plain));
}

RcloneCryptByteStream::RcloneCryptByteStream(std::shared_ptr<const RcloneCrypt> cipher,
                                             std::shared_ptr<ByteStream> underlying,
                                             RcloneCrypt::Bytes fileNonce, qint64 plainSize)
    : m_cipher(std::move(cipher)), m_underlying(std::move(underlying)),
      m_fileNonce(std::move(fileNonce)), m_size(plainSize) {
    m_stats.total = m_size;
}

RcloneCryptByteStream::~RcloneCryptByteStream() = default;

bool RcloneCryptByteStream::loadBlock(qint64 blockIndex) {
    // 密文块定界：块 k 的密文起于 32 + k·65552，长度 ≤65552（尾块短读合法）
    const qint64 underlyingOffset = RcloneCrypt::kHeaderSize + blockIndex * RcloneCrypt::kBlockSize;
    if (m_underlying->seek(underlyingOffset) != underlyingOffset)
        return false;
    const std::size_t want = std::size_t(RcloneCrypt::kBlockSize);
    RcloneCrypt::Bytes raw(want);
    const qint64 got =
        m_underlying->read(reinterpret_cast<char *>(raw.data()), RcloneCrypt::kBlockSize);
    if (got <= 0)
        return false;
    raw.resize(std::size_t(got));

    // 块 nonce = 文件 nonce + 块号（24 字节小端加法）
    RcloneCrypt::Bytes nonce = m_fileNonce;
    RcloneCrypt::nonceAdd(nonce.data(), std::uint64_t(blockIndex));

    auto plain = m_cipher->openBlock(raw.data(), raw.size(), nonce.data(), m_block);
    if (!plain) {
        // rclone 口径：pass_bad_blocks 打开时坏块交付全零、流不中断
        if (m_cipher->params().passBadBlocks &&
            plain.error() == RcloneCrypt::Error::EncryptedBadBlock) {
            m_block.assign(std::size_t(got) - std::size_t(RcloneCrypt::kBlockOverhead), 0);
        } else {
            return false;
        }
    }
    m_blockIndex = blockIndex;
    return true;
}

qint64 RcloneCryptByteStream::read(char *out, qint64 maxBytes) {
    qint64 got = 0;
    while (got < maxBytes) {
        if (m_pos >= m_size)
            break; // 明文尽头：按声明总长取完，kEof 是诚实终态
        if (m_canceled.load(std::memory_order_relaxed))
            return kError; // 撤销后的 read 按错误收尾，不算早断自愈
        const qint64 wantBlock = m_pos / RcloneCrypt::kBlockDataSize;
        if (wantBlock != m_blockIndex && !loadBlock(wantBlock))
            return kError;
        const qint64 inBlock = m_pos % RcloneCrypt::kBlockDataSize;
        const qint64 avail =
            qint64(m_block.size()) > inBlock ? qint64(m_block.size()) - inBlock : 0;
        if (avail <= 0)
            return kError; // 声明总长内取不到数据 ⇒ 头不实，按错误显影
        const qint64 take = qMin(avail, maxBytes - got);
        std::memcpy(out + got, m_block.data() + inBlock, std::size_t(take));
        got += take;
        m_pos += take;
    }
    m_stats.pos = m_pos;
    return got > 0 ? got : kEof;
}

qint64 RcloneCryptByteStream::seek(qint64 to) {
    // 不做范围检查（N98 §5 定案）：越界同样成功，随后的 read 在明文尽头落 kEof。
    // 只推进 pos 并作废块缓存——块装载时底层自行按密文偏移取数
    if (to == m_pos && m_blockIndex >= 0)
        return to; // 同位幂等：缓存仍有效
    m_pos = to;
    m_blockIndex = -1;
    m_block.clear();
    m_stats.pos = m_pos;
    return to;
}

void RcloneCryptByteStream::cancel() {
    m_canceled.store(true, std::memory_order_relaxed);
    m_underlying->cancel();
}

ByteStream::Stats RcloneCryptByteStream::stats() const { return m_stats; }
