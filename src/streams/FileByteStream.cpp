#include "FileByteStream.h"

namespace {

// 64 位寻位/取位：Windows 走 _fseeki64 / _ftelli64，POSIX 走 fseeko / ftello。
// 仅这两个助手分平台（L3 传输允许平台分支，算法层不在此列）
int fileSeek(std::FILE *f, qint64 offset, int origin) {
#ifdef Q_OS_WIN
    return _fseeki64(f, offset, origin);
#else
    return fseeko(f, static_cast<off_t>(offset), origin);
#endif
}

qint64 fileTell(std::FILE *f) {
#ifdef Q_OS_WIN
    return _ftelli64(f);
#else
    return static_cast<qint64>(ftello(f));
#endif
}

} // namespace

std::expected<std::shared_ptr<FileByteStream>, QString>
FileByteStream::open(const QString &nativePath) {
    if (nativePath.isEmpty())
        return std::unexpected(QStringLiteral("empty file path"));

#ifdef Q_OS_WIN
    // Windows 路径以 UTF-16 打开（中文 / 空格 / 超路径原样可用）
    std::FILE *file = nullptr;
    _wfopen_s(&file, reinterpret_cast<const wchar_t *>(nativePath.utf16()), L"rb");
#else
    std::FILE *file = std::fopen(nativePath.toUtf8().constData(), "rb");
#endif
    if (!file)
        return std::unexpected(QStringLiteral("cannot open file \"%1\"").arg(nativePath));

    // 总长一次定型：seek 到尾取位，再回起点
    if (fileSeek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return std::unexpected(QStringLiteral("cannot seek file \"%1\"").arg(nativePath));
    }
    const qint64 total = fileTell(file);
    if (total < 0 || fileSeek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return std::unexpected(
            QStringLiteral("cannot determine size of file \"%1\"").arg(nativePath));
    }
    return std::shared_ptr<FileByteStream>(new FileByteStream(file, total));
}

FileByteStream::FileByteStream(std::FILE *file, qint64 total) : m_file(file), m_total(total) {
    m_stats.total = total;
}

FileByteStream::~FileByteStream() {
    if (m_file)
        std::fclose(m_file);
}

qint64 FileByteStream::read(char *out, qint64 maxBytes) {
    if (maxBytes <= 0)
        return kEof;
    const std::size_t got = std::fread(out, 1, std::size_t(maxBytes), m_file);
    if (got > 0) {
        m_pos += qint64(got);
        m_stats.pos = m_pos;
        return qint64(got);
    }
    // 零交付：文件尽头 ⇒ kEof（按声明总长取完的终态诚实口径；本地文件供数通道
    // 与内容终点天然一致）；其余按错误
    if (std::feof(m_file))
        return kEof;
    return kError;
}

qint64 FileByteStream::seek(qint64 to) {
    // 不做范围检查：越过末尾同样成功（N98 §5 定案），读取在尽头自然落 kEof。
    // fseek 同时清掉 feof 旗，seek 后读行为与位置一致
    if (fileSeek(m_file, to, SEEK_SET) != 0)
        return kSeekError;
    m_pos = to;
    m_stats.pos = m_pos;
    return to;
}

ByteStream::Stats FileByteStream::stats() const { return m_stats; }
