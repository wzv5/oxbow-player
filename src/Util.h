#pragma once

#include <QString>
#include <QList>

class QWidget;

namespace Util {

// 文件大小自适应单位显示（B/KB/MB/GB，按量级就近选择）
QString formatFileSize(qint64 bytes);

// 数字化文件名的分段序列（N4）：文本段或数值段。
// 拆分依据仅为 ASCII 0-9；数值段按十进制解析（前导零不敏感，01 与 1 相等），
// 解析失败（超出 qlonglong 的超长数字串）时该段降级为普通文本段，
// 此时允许相邻出现多个文本段
struct NameSegment
{
    bool isNumber;
    QString text;      // isNumber == false 时有效
    qlonglong number;  // isNumber == true 时有效
};

// 预分段（装饰-排序：排序前对每项调用一次，比较仅在段序列间进行）
QList<NameSegment> segmentName(const QString &name);

// 段序列比较：逐段对比，同类型按类型内规则（数值比大小 / 文本大小写不敏感字典序），
// 跨类型时数值段恒小于文本段；前缀较短者小；全等返回 0（等键，交由稳定排序保序）
int naturalCompare(const QList<NameSegment> &a, const QList<NameSegment> &b);

// 打开文件所在目录并选中该文件。Windows 经 Shell API（不派生 explorer 进程）；
// 其他平台或失败时降级为打开所在目录。返回是否成功定位选中
bool openFileLocation(const QString &path, QWidget *parent);

} // namespace Util
