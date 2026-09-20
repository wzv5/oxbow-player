#pragma once

#include "pch.h"

#include "PlaylistProvider.h"

class ProviderSettingsPage;

// 列表设置对话框（N9 / N44）：新建列表与存量列表共用。
// N44 起退化为"名称校验 + 类型单选外壳 + 结果转发"——参数控件、回填、校验与
// 提供者构造全部下沉到各提供者自己的设置页（ProviderSettingsPage，经
// ProviderCatalog 取得），本对话框不再认识任何具体提供者。
// 布局为四行：名称 / 列表类型单选组 / 表单区（QStackedWidget，只显示选中类型的
// 表单）/ 按钮行；行 1 与行 2 同属一个 QFormLayout，标签列自动对齐。
// 确定时：名称三条校验 → 选中页 validate（错误文案由页面给出，弹窗并留在对话框）
// → 选中页 hasChanges（false 即仅改名，不产出新提供者）→ 选中页 build
class ListSettingsDialog : public QDialog {
    Q_OBJECT
  public:
    // current 为存量提供者（只读回显用；其参数由对应页面构造期自行回填，新建传
    // nullptr）；occupiedNames 为全部已占用列表名（改名场景由调用方剔除自身），
    // 供 accept 内做重名拦截——校验全程不关闭对话框。
    // registry 转交各设置页，由页面转交新构建的提供者，必填无缺省
    ListSettingsDialog(QWidget *parent, const QString &currentName, const PlaylistProvider *current,
                       const QStringList &occupiedNames,
                       const std::shared_ptr<ServiceRegistry> &registry);

    QString listName() const;

    // 确定后的提供者交付：nullopt＝未变更；否则为新构建实例
    // （N28：shared_ptr 交付，与视图容器的所有权模型一致）
    std::optional<std::shared_ptr<PlaylistProvider>> takeProvider();

  protected:
    void accept() override;

  private:
    QLineEdit *m_nameEdit = nullptr;
    QList<QRadioButton *> m_radios;        // 与 providerCatalog() 同序
    QList<ProviderSettingsPage *> m_pages; // 与 providerCatalog() 同序
    QStackedWidget *m_pageStack = nullptr; // 行 3：只显示选中提供者的表单
    QStringList m_occupiedNames;           // 重名校验用
    // N52：seed 存为成员——accept 期集中传接 listUuid（模态期间提供者恒存活，
    // 调用方在 accept 之后才替换）
    const PlaylistProvider *m_seed = nullptr;
    std::optional<std::shared_ptr<PlaylistProvider>> m_result;
};
