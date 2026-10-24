#pragma once

#include "pch.h"

// N134：单条自定义工具。name 是 '>' 分层的菜单路径——加载期只去首尾空白、保留手写
// 原样，经编辑对话框确定时按 normalizedPath 重组；note 仅编辑对话框可见，不进任何展示面
struct CustomTool {
    QString name;
    QString command;
    QString note;
};

// N134：自定义工具单表（oxbow.ini 的 [Tools]，形态与 BookmarkManager 同构）；
// load 一次，replaceTools 是唯一写入口。无信号、不入 ServiceRegistry。
// 名称路径的语义（切段 / 重组 / 成套校验）全部住这类，静态出口不碰 m_tools
class CustomToolManager : public QObject {
    Q_OBJECT
  public:
    explicit CustomToolManager(const QString &configFile, QObject *parent = nullptr);

    void load();
    // 落盘失败时内存表照常生效、不回滚，返回值交调用方处置
    bool replaceTools(QList<CustomTool> tools);

    const QList<CustomTool> &tools() const { return m_tools; }

    // 名称 → 菜单路径：按 '>' 切段、逐段 trim、丢空段；空表即无效名称。
    // 名称语义的唯一切分点——加载判空、确定校验、菜单渲染三处共用
    static QStringList pathSegments(const QString &name);

    // 段列表 → 路径串（" > " 连接）：落盘形态、菜单前缀键与冲突提示的唯一拼法
    static QString joinPath(const QStringList &segments);

    // 名称的落盘形态：pathSegments 切段后重组，即 '>' 前后统一单空格、段首尾多余
    // 空格消失；切段为空即空串
    static QString normalizedPath(const QString &name);

    // 一批工具（编辑对话框的工作副本）的校验结论
    enum class Problem {
        None,
        MissingName,    // 名称空（含规范化后为空——纯 '>' 与全空段），命令非空
        MissingCommand, // 命令空，名称非空
        PrefixConflict, // 一条路径是另一条的严格前缀，即"既是工具又是子菜单"
    };
    struct Sanitized {
        Problem problem = Problem::None;
        int row = -1;            // 违规行的原表序；None 时无意义
        QString path;            // PrefixConflict 的冲突前缀路径（已重组）
        QList<CustomTool> tools; // 通过时的保留行（已规范化）；违规时为空
    };
    // 确定语义的唯一收口：逐条规范化（名称重组、命令 trim、备注原样）→ 按表序校验
    // → 丢弃占位条目。名称与命令皆空的占位条目（等待输入）连备注一并丢弃、不判违规，
    // 故可以产出空表；恰一者为空即违规、tools 留空交调用方原地处置
    static Sanitized sanitize(const QList<CustomTool> &tools);

  private:
    bool save() const;

    QString m_configFile;
    QList<CustomTool> m_tools;
};
