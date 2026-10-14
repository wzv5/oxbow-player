#pragma once

#include "pch.h"

class PlaylistProvider;
class QCheckBox;

// 提供者参数页（N44）：每个提供者一页，页内自持控件、输入校验与实例构造。
// 对话框只按本接口驱动，不认识任何具体提供者——新增提供者的改动面因此收敛为
// "自身实现文件 + providerCatalog() 一行 + CMakeLists.txt 两处源列表"。
// 页面构造入参：该提供者类型的存量实例（回显来源，可空）+ 注册表 + 父窗口；
// 页面自持回显指针，hasChanges 与 build 共用同一份"取规范化参数值"的私有辅助。
// 子类一律定义在各自提供者的 .cpp（页类不进头文件、不新增文件对），
// 只经提供者头文件声明的工厂自由函数产出，见 ProviderCatalog。
class ProviderSettingsPage : public QWidget {
    Q_OBJECT
  public:
    explicit ProviderSettingsPage(QWidget *parent = nullptr);

    // 输入校验：nullopt＝通过；否则为可直接展示的错误文案。
    // 页面不自行弹窗——弹窗与"留在对话框"由对话框统一负责
    virtual std::optional<QString> validate() const = 0;

    // 参数相对回显来源是否已有改动。无回显来源（新建 / 换类型）恒 true；
    // 对话框据此判定"仅改名"（false ⇒ 不产出新提供者，行为对齐 N9 现状）
    virtual bool hasChanges() const = 0;

    // 按当前输入构建提供者（仅在 validate 通过后调用）
    virtual std::shared_ptr<PlaylistProvider> build() const = 0;

    // ——N52 §2.8：转接行一次实现，UI 与持久化按能力渲染，不给非法组合留入口——
    // initial 有值 = Optional：加可勾选行并预填（新建列表 false 起）；
    // initial 无值 = Required：只加页面底部一行说明（英文 tr，无勾选框）；
    // Unsupported：不调本函数（页面无转接行）。
    // 子类在自身布局装配序列的"底部前"调用（stretch 之前），行序即页面视觉底行
    void enableTransitOption(std::optional<bool> initial);
    // 转接行是否存在（Unsupported 恒 false——对话框据此判定"仅 Optional 写偏好"）
    bool hasTransitOption() const { return m_transitCheck != nullptr; }
    // Optional 的勾选态；无行恒 false
    bool transitEnabled() const { return m_transitCheck && m_transitCheck->isChecked(); }

  private:
    QCheckBox *m_transitCheck = nullptr;
};
