#pragma once

#include "pch.h"

#include "MpvConf.h"

class MpvController;
class QListWidget;
class QStackedWidget;
class QDialogButtonBox;
class QComboBox;
class QSpinBox;
class QWidget;

// 主设置对话框（R3）：从 MainWindow 独立出来，左侧分类列表 + 右侧分页选项。
// 分类与归属：
//   基本   → 插件本体选项（写 mpv-qt.ini）
//   播放   → mpv 选项（写 mpv.conf + 下发 MpvController）
//   视频   → mpv 选项（写 mpv.conf + 下发 MpvController）
//
// 铁律（R3 §3.3 / §3.4）：
//   - 回填：mpv 选项仅从 MpvController::propertyString/propertyDouble 取，不读 conf / INI；
//           不可读则取 mpv 文档默认值（hwdec="auto" / volume-max=130 / loopMode="none"）。
//   - 写盘：mpv 选项先经 MpvConf::open() 加载原 conf（保留注释/空行/section 头/非法行），
//           再 setGlobal 修改目标 key，最后 commit() 原子写回。
//
// 循环模式特例：mpv 端为双属性（loop-file /
// loop-playlist），插件侧以三态单值（none/file/playlist）抽象； 回填时检测到不在预设 3
// 种组合内（用户手改 conf 产生）则选“自定义（不干涉）”，Apply/OK 不下发不写 conf。
//
// 应用语义：valueChanged 仅更新内存暂存 m_pending，不下盘 / 不下 mpv；Apply/OK 才落盘+下发。
class SettingsDialog : public QDialog {
    Q_OBJECT
  public:
    explicit SettingsDialog(QWidget *parent, MpvController *controller,
                            const QString &pluginIniPath);

  private:
    // 通用 valueChanged 槽（任何标了 iniKey/mpvKey/loopModeOnly 的控件都连此槽）
    void onControlChanged();

    // 构建：建主布局、左侧分类列表、右侧堆栈、底部按钮、分类页面与控件
    void buildUi();
    QWidget *buildBasicPage();    // 插件本体：语言 / 滚轮步进 / 方向键步进
    QWidget *buildPlaybackPage(); // mpv：音量上限 / 循环模式
    QWidget *buildVideoPage();    // mpv：硬件解码

    // 入参回填（构造期一次性）：mpv 选项仅从 controller 取；插件选项仅从 INI 取；
    // 整段用 m_blockApply 守护，避免构造期 valueChanged 污染 m_pending
    void applyToControls();

    // Apply / OK 时统一落盘：遍历 m_pending，按 dynamic property 判定路径
    bool applyAll();

    // loopMode 特殊路径：仅下发 mpv（双属性），不写 mpv.conf（避免双 key 歧义）
    void applyLoopModeOption(const QString &mode);

    // 语言枚举：扫 :/i18n/mpvqt_*.qm + "en"，按字母序排；"Auto" 项 data 为空串
    void populateLanguageCombo(QComboBox *combo, const QString &currentLocale);

    // 从控件读取当前值（统一接口：QComboBox 取 currentData；QSpinBox 取 value）
    QVariant readControlValue(QWidget *control) const;

    MpvController *m_controller = nullptr;
    QString m_pluginIniPath;            // mpv-qt.ini 完整路径
    QString m_mpvConfPath;              // mpv.conf 路径（resolveMpvConfPath 解析后）
    std::optional<MpvConf> m_confCache; // 构造期 open 一次，写盘时直接 commit

    QListWidget *m_categoryList = nullptr;
    QStackedWidget *m_pageStack = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;

    // 控件指针缓存（applyAll / applyToControls / readControlValue 用）
    QComboBox *m_langCombo = nullptr;
    QComboBox *m_wheelStepCombo = nullptr;
    QComboBox *m_keyStepCombo = nullptr;
    QSpinBox *m_volumeMaxSpin = nullptr;
    QComboBox *m_loopModeCombo = nullptr;
    QComboBox *m_hwdecCombo = nullptr;

    // 统一 pending 模型（方案 §3.5）：key=控件指针（map 去重），value=当前控件值
    QMap<QWidget *, QVariant> m_pending;
    // 已应用快照：上次 Apply/OK 后的值（或初始回填值）；用于比对 "用户是否修改"
    QMap<QWidget *, QVariant> m_appliedSnapshot;
    bool m_blockApply = false;
    // Apply 按钮启用状态：任一控件值与 snapshot 不同 → dirty
    bool m_dirty = false;
    void updateApplyButtonState();
};