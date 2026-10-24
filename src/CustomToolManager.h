#pragma once

#include "pch.h"

// N134：单条自定义工具。name 按原始串保存（只去首尾空白），`>` 分段语义由
// pathSegments 单点规范化；note 仅编辑对话框可见，不进任何展示面
struct CustomTool {
    QString name;
    QString command;
    QString note;
};

// N134：自定义工具单表（oxbow.ini 的 [Tools]，形态与 BookmarkManager 同构）；
// load 一次，replaceTools 是唯一写入口。无信号、不入 ServiceRegistry
class CustomToolManager : public QObject {
    Q_OBJECT
  public:
    explicit CustomToolManager(const QString &configFile, QObject *parent = nullptr);

    void load();
    // 落盘失败时内存表照常生效、不回滚，返回值交调用方处置
    bool replaceTools(QList<CustomTool> tools);

    const QList<CustomTool> &tools() const { return m_tools; }

    // 按 '>' 切段、逐段 trim、丢空段；空表即无效名称。名称语义的唯一规范化点
    static QStringList pathSegments(const QString &name);

  private:
    bool save() const;

    QString m_configFile;
    QList<CustomTool> m_tools;
};
