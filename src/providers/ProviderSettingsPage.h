#pragma once

#include "pch.h"

class PlaylistProvider;

// 提供者参数页：每个提供者一页，页内自持控件、输入校验与实例构造；对话框只按
// 本接口驱动，不认识具体提供者。子类定义在各自提供者的 .cpp，只经工厂函数产出。
class ProviderSettingsPage : public QWidget {
    Q_OBJECT
  public:
    explicit ProviderSettingsPage(QWidget *parent = nullptr);

    // 输入校验：nullopt＝通过；否则为可直接展示的错误文案。页面不弹窗。
    virtual std::optional<QString> validate() const = 0;

    // 无回显来源（新建 / 换类型）恒 true；false 时对话框只改名、不产出新提供者
    virtual bool hasChanges() const = 0;

    // 仅在 validate 通过后调用
    virtual std::shared_ptr<PlaylistProvider> build() const = 0;

    // Optional 加勾选行并预填，Required 只在页底加说明；Unsupported 不调用。
    // 须在 stretch 之前调用，行序即页面视觉底行
    void enableTransitOption(std::optional<bool> initial);
    bool hasTransitOption() const { return m_transitCheck != nullptr; }
    bool transitEnabled() const { return m_transitCheck && m_transitCheck->isChecked(); }

  private:
    QCheckBox *m_transitCheck = nullptr;
};
