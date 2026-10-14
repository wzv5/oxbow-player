#pragma once

#include "ByteStream.h"
#include "pch.h"

// N98 §1：第二个传输（L3）——本地文件的 ByteStream 实现，N52 分层里"第二个
// 可偏移重启的传输"的第一次补齐。只读、可寻位、总长已知（open 期一次定型）。
//
// 实现口径：
//   - C stdio（FILE*）而非 QFile——ByteStream 契约要求实现类不含 QObject、
//     任意线程可析构，QFile 本体是 QObject，为守住字母面不走它；
//   - seek 不做范围检查：越过末尾同样成功返回目标偏移，随后的 read 在文件
//     尽头落 kEof（与 RcloneCryptByteStream 的定长流语义一致，N98 §5）；
//     系统调用失败（几乎不可达）才落 kSeekError；
//   - cancel 无事可做：本地盘读不产生需要解救的阻塞（契约上仍任意线程安全）；
//   - 位置守恒：read 交付与 seek 赋值推进同一个 m_pos（Stats::pos）。
class FileByteStream final : public ByteStream {
  public:
    // 起流工厂（open 期、消费者线程一次性调用）：打开文件并取定总长。
    // nativePath 为原生分隔符路径；失败带英文可读文案
    static std::expected<std::shared_ptr<FileByteStream>, QString> open(const QString &nativePath);

    ~FileByteStream() override;

    // ByteStream 接口
    qint64 read(char *out, qint64 maxBytes) override;
    bool isSeekable() const override { return true; }
    qint64 seek(qint64 to) override;
    qint64 size() const override { return m_total; }
    void cancel() override {}
    Stats stats() const override;

  private:
    FileByteStream(std::FILE *file, qint64 total);

    std::FILE *m_file = nullptr;
    qint64 m_total = -1;
    qint64 m_pos = 0;
    mutable Stats m_stats;
};
