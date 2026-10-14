#pragma once

#include "pch.h"

#include "mpvbridge/MpvConf.h"

class MpvController;
class QListWidget;
class QStackedWidget;
class QDialogButtonBox;
class QWidget;

// 目录项数据模型（R4 §4.1）：完整定义在 SettingsDialog.cpp 全局作用域——成员
// 函数签名需引用该类型，匿名命名空间类型无法与之同一实体。头文件仅以指针形态
// 引用：static 目录生命周期内指针稳定，可直接作为暂存 / 索引的 key
struct SettingItem;

// 主设置对话框（R4）：数据定义驱动。
// "分类 → 设置项"两层目录定义内嵌于 SettingsDialog.cpp（目录常量与选项
// provider 在匿名命名空间，数据模型类型在 cpp 全局作用域给出完整定义），
// 本类只保留构建 / 回填 / 应用三条通用路径，不含任何具体设置项的过程式代码；
// 新增设置项只改目录定义。
//
// 铁律（承 R3 §2.1，语义不变）：
//   - 回填：mpv 选项仅从 MpvController::propertyString/propertyDouble 取，不读
//           conf / INI；不可读取则落目录定义的 fallback（mpv 文档默认值）。
//   - 写盘：mpv 选项先经 MpvConf::open() 加载原 conf（保留注释/空行/section 头/
//           非法行），再 setGlobal 修改目标 key，最后 commit() 原子写回。
//   - 应用：valueChanged 仅更新内存暂存 m_pending（key=目录项指针，static 目录
//           生命周期内稳定），不下盘 / 不下 mpv；Apply/OK 才落盘+下发；Cancel 丢弃。
//   - 占位：mpvAssignments 为空的选项回填永不命中、应用整项跳过（不下发不写 conf）。
//   - 联动：dependsOn 主控 CheckBox 未选中 → 依赖项禁用、不回填、落 fallback，
//           自动不参与 dirty 比对与应用。
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit SettingsDialog(QWidget *parent, MpvController *controller,
                            const QString &pluginIniPath);

  signals:
    // N35：Apply/OK 应用成功后发出，keys 为本轮实际写入 oxbow.ini 的键集合
    // （与快照比对后确有变化者）。仅覆盖 ini 配置变化；mpv 配置变化已由
    // mpv 属性事件跟踪，不在此列。无 ini 变更则不发射
    void pluginIniKeysChanged(const QStringList &keys);

  private:
    // 通用 valueChanged 槽：item 为触发控件的目录项指针（connect 时以 lambda
    // 捕获传入，static 目录生命周期内稳定）；若该项是联动主控 CheckBox，
    // 则联动刷新其依赖项
    void onControlChanged(const SettingItem *item);

    // 构建：建主布局、左侧分类列表、右侧堆栈、底部按钮；遍历目录动态生成
    // 各分类页面与控件（含可选备注行），构建后统一做一次联动刷新
    void buildUi();

    // 完整回填（构造期与 N41 Apply 成功后共用）：无依赖项从后端读取；依赖项
    // 按主控状态分流——选中→启用并回填，未选中→禁用落 fallback 不读后端。
    // 整段用 m_blockApply 守护，避免回填触发 valueChanged 污染 m_pending
    void applyToControls();

    // 单项回填：从后端（INI / mpv）读取并写入控件与 m_pending，按 kind × 后端分派
    void fillItemFromBackend(const SettingItem *item);

    // 禁用落 fallback：不读后端，控件与 m_pending 直接落目录定义的兜底值
    void applyFallback(const SettingItem *item);

    // 联动刷新：masterId 主控状态变化 → 逐依赖项启用重回填 / 禁用落 fallback
    void refreshDependents(const QString &masterId);

    // Apply / OK 时统一落盘：遍历 m_pending，按目录定义的后端字段分发
    bool applyAll();

    // 从控件读取当前值（按控件类型分派：Combo 取 currentData、EditableCombo 取
    // currentText、SpinBox 取 value、CheckBox 取 isChecked）
    QVariant readControlValue(QWidget *control) const;

    MpvController *m_controller = nullptr;
    QString m_pluginIniPath;            // oxbow.ini 完整路径
    QString m_mpvConfPath;              // mpv.conf 路径（N92：expand-path 取目录再拼；
                                        // 空 = 配置目录不可用，仅运行期分支）
    std::optional<MpvConf> m_confCache; // 构造期 open 一次，写盘时直接 commit

    QListWidget *m_categoryList = nullptr;
    QStackedWidget *m_pageStack = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;

    // 目录项指针 → 控件；回填 / 联动刷新 / 应用时定位用
    QMap<const SettingItem *, QWidget *> m_controls;
    // 联动主控 id → CheckBox 控件
    QMap<QString, QWidget *> m_masters;

    // 统一 pending 模型（R4 §4.3.1）：key=目录项指针，value=当前控件值
    QMap<const SettingItem *, QVariant> m_pending;
    // 已应用快照：上次 Apply/OK 后的值（或初始回填值）；用于比对 "用户是否修改"
    QMap<const SettingItem *, QVariant> m_appliedSnapshot;
    bool m_blockApply = false;
    // Apply 按钮启用状态：任一控件值与 snapshot 不同 → dirty
    bool m_dirty = false;
    void updateApplyButtonState();
};
