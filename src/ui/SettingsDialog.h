#pragma once

#include "pch.h"

#include "mpvbridge/MpvConf.h"

class MpvController;

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
// 铁律（承 R3 §2.1）：
//   - 回填：mpv 选项仅从 MpvController 取，不读 conf / INI；不可读取则落目录定义的
//           fallback（兼 mpv 默认回退，值以当前 mpv 实测照抄为准）。
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
    // N35：Apply/OK 应用成功后发出，keys 为本轮实际写入或删除的 oxbow.ini 键（含因等于
    // 默认值而删除者）。mpv 配置变化由属性事件跟踪，不在此列；无 ini 变更则不发射
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
    // 持久值参照系：mpv 项取 mpv.conf 保存值（缺键 / conf 不可用落 m_fillValue）；ini 与
    // 禁用项取 m_fillValue。只经 rebuildSnapshot() 重建
    QMap<const SettingItem *, QVariant> m_appliedSnapshot;
    // 运行期基线：本次打开 / Apply 重读时的回填值，与快照、pending 三者不全等即未提交
    QMap<const SettingItem *, QVariant> m_fillValue;
    // 默认值缓存（两级来源 option-info → 目录 fallback，对话框生命周期内不变）
    QHash<const SettingItem *, QVariant> m_defaultCache;
    // 逐 key 规范形默认串（回落时取 fallback 命中选项的赋值）——写盘期删除判定的比较源
    QHash<const SettingItem *, QMap<QString, QString>> m_defaultAssignments;
    // 行内重置按钮（未提交的启用项才显示，干净行由同宽空槽占位）
    QMap<const SettingItem *, QToolButton *> m_resetButtons;
    bool m_blockApply = false;
    bool m_dirty = false;
    void refreshDerivedState();

    // —— 值层 ——
    // mpv.conf 保存值映射为控件同型值（2.2）：任一管辖 key 缺失（含复合项缺一）或 conf
    // 不可用 → nullopt（"无保存值"，快照落回填值）；映射不了（两态未命中 / SpinBox 不可
    // 解析）落目录 fallback
    std::optional<QVariant> savedControlValue(const SettingItem *item) const;
    // 目录 fallback 的同型控件值（Combo findData miss 落第 0 项 data；SpinBox 钳制；
    // FilterList 空串）——ini 默认值与 mpv 默认回退共用
    QVariant fallbackControlValue(const SettingItem *item) const;
    // 默认值（2.3 两级来源，恒可用）：mpv 优先 option-info 逐 key，任一失败整项回落
    // fallback；结果缓存 m_defaultCache，比较源缓存 m_defaultAssignments
    QVariant defaultControlValue(const SettingItem *item);
    // 按控件写值（重置动作 / 回填共用；Combo findData miss 落 0；FilterList 空串 →
    // setEntries({})）；调用方负责 m_blockApply 守护与 m_pending 更新
    void writeControlValue(const SettingItem *item, const QVariant &value);
    // 控件值 → 逐 key authored 赋值（Combo / 两态 CheckBox 反查 choice，占位返回空；
    // 其余按 mpvType 生成单 key 串）——下发与默认回退的比较源共用
    QMap<QString, QString> mpvAssignmentsForValue(const SettingItem *item,
                                                  const QVariant &value) const;
    // Apply 的逐 key typed set 下发；任一失败收集进 failedKeys 并返回 false
    // （失败不写 conf / 不删键，避免持久与运行期不一致）
    bool dispatchMpvAssignments(const SettingItem *item, const QMap<QString, QString> &assignments,
                                QStringList &failedKeys);
    // 运行期逐 key 回读（Apply 写盘源）：FilterList 走 filterEntriesChecked、
    // 其余走 readCanonicalMpvValue；任一 key 读取失败返回空 map（合法空值不受影响）
    QMap<QString, QString> runtimeMpvAssignments(const SettingItem *item) const;
    // 重建快照（2.1）：兜底一律取 fill 不得取 pending——联动重建可能发生在用户已编辑
    // 之后，取 pending 会吞掉未提交并污染 Discard 的回退目标
    void rebuildSnapshot();

    // —— 行内重置 ——
    // 统一值写入 helper：m_blockApply 守护下写控件并回读 pending，随后重算派生态。
    // 不记任何动作状态：效果由"三者不全等"的脏判据与 Apply 值路径涌现（2.6）
    void applyResetChoice(const SettingItem *item, const QVariant &target);
    void showResetMenu(const SettingItem *item, QToolButton *button);
    void retintResetIcons();

  protected:
    void changeEvent(QEvent *event) override;
};
