#pragma once

#include "pch.h"

// mpv.conf 手写解析器（R3）：
// - 四种行：Empty / Comment / Section / KeyValue
// - 注释/空行/section 头/非法行按行原样保留 raw；KeyValue 行 value 改动时用 key=value 重组
// - 顶层裸 key（无 section 头包裹）独立 namespace；section 内 key 独立 namespace
// - commit() 用 QSaveFile 原子写回
//
// 不继承 QSettings（QSettings 不保留注释 + group 状态机污染 + 顶层裸 key 概念丢失）。
struct MpvConfLine {
    enum class Kind { Empty, Comment, Section, KeyValue };
    Kind kind = Kind::Empty;
    QString raw; // 原行（剥除行尾 \n 与首部 BOM；保留缩进/中间空白/CR 等）
    // KeyValue 专用：
    QString section; // 所属 section 名；空串 = 顶层裸 key
    QString key;
    QString value; // 不含首尾空白；不含引号
    // Section 专用：
    QString name; // 不含 '[' ']'
};

class MpvConf {
  public:
    MpvConf() = default; // std::optional<MpvConf> 需要默认构造；空 path + 空 lines
    // 加载现有文件；不存在则返回带 m_path + 空 m_lines 的实例（commit 时创建）。
    // 仅当文件存在但 IO 失败时返回 nullopt。
    static std::optional<MpvConf> open(const QString &path);

    // 顶层裸 key（"global" 命名空间）
    QString getGlobal(const QString &key, const QString &defaultValue = {}) const;
    QStringList globalKeys() const;
    void setGlobal(const QString &key, const QString &value);
    void removeGlobal(const QString &key);

    // section 内 key
    QString get(const QString &section, const QString &key, const QString &defaultValue = {}) const;
    QStringList keys(const QString &section) const;
    void set(const QString &section, const QString &key, const QString &value);
    void remove(const QString &section, const QString &key);

    // 写盘（QSaveFile 原子写）
    bool commit();

    // 单元测试友好 getter
    const QList<MpvConfLine> &lines() const { return m_lines; }

  private:
    QString m_path;
    QList<MpvConfLine> m_lines;

    void parse();
    QString serialize() const;
    void applyChange(const QString &section, const QString &key, const QString &value,
                     bool isRemove);
};