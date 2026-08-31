#pragma once

#include "pch.h"

#include "PlaylistProvider.h"

// 列表设置对话框（N9）：新建列表与存量列表共用。
// 入口直接传入当前提供者指针（新建传 nullptr），内部按需下探子类回填参数区；
// 确定时负责构建新提供者并经 takeProvider 交付：
//   新建（入口 null）→ 必产出新提供者；
//   存量同类型且参数未变 → nullopt（调用方仅应用改名）；
//   存量类型或参数有变 → 产出新提供者。
// 校验在 accept 内完成：名称非空/非"默认"/重名；目录要求根目录存在。
class ListSettingsDialog : public QDialog {
    Q_OBJECT
  public:
    // current 为存量提供者（只读回显用）；occupiedNames 为全部已占用列表名
    // （改名场景由调用方剔除自身），供 accept 内做重名拦截——校验全程不关闭对话框
    ListSettingsDialog(QWidget *parent, const QString &currentName, const PlaylistProvider *current,
                       const QStringList &occupiedNames);

    QString listName() const;
    PlaylistProvider::Type providerType() const;
    QString dirRoot() const;
    bool dirRecursive() const;
    QStringList dirExtensions() const;

    // 确定后的提供者交付：nullopt＝未变更；否则为新构建实例
    std::optional<std::unique_ptr<PlaylistProvider>> takeProvider();

  protected:
    void accept() override;

  private:
    // 控件创建完成后的数据回填：按入口提供者类型（if 分支 + static_cast 下探）
    // 设置单选、目录参数与扩展名初值；新建（无提供者）走静态+默认清单
    void populateFromCurrent();

    QLineEdit *m_nameEdit = nullptr;
    QRadioButton *m_staticRadio = nullptr;
    QRadioButton *m_dirRadio = nullptr;
    QLineEdit *m_rootEdit = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    QLineEdit *m_extensionsEdit = nullptr;       // 空格分隔扩展名
    QStringList m_occupiedNames;                 // 已占用列表名（重名校验用）
    const PlaylistProvider *m_current = nullptr; // 入口提供者（null＝新建）
    std::optional<std::unique_ptr<PlaylistProvider>> m_result;

    void browseRoot();
};
