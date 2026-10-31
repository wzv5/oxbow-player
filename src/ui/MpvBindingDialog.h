#pragma once

#include "pch.h"

#include "ActionManager.h"
#include "CustomToolManager.h"
#include "mpvbridge/InputConf.h"
#include "mpvbridge/MpvController.h"

// N89：mpv 键位编辑器——可视化维护 input.conf。四列表格（来源 / 命令 / 键 / 注释），
// 内置档与脚本档来自 input-bindings（打开时读一次、只读呈现），自定义档来自
// InputConf 的解析结果、是唯一可编辑对象，写回文件即最终结果。
// 状态走行级视觉（灰 = 禁用、删除线 = 被顶掉、加粗 = 脏），无状态列与操作列，
// 操作走行级右键菜单（Duplicate / Disable / Enable / Delete，多选批量）。
// 生效口径 = 重启：只写文件、Apply 成功后弹一次重启提示，Apply 全程不下发任何
// mpv 命令。界面建制照 N86 的 ShortcutsDialog（非模态、按需创建、快照式脏跟踪、
// Apply 由脏态驱动）；唯一分叉是落盘失败支——保留脏态并锁死 Apply 与 Ok（§4.1），
// 不回滚快照：本对话框显示的是文件内容与重启后的生效态，写盘失败时文件未动、
// 界面上的编辑只是尚未落地的意图（§H）。
class MpvBindingDialog : public QDialog {
    Q_OBJECT
  public:
    explicit MpvBindingDialog(MpvController *mpv, ActionManager *actions,
                              const CustomToolManager *toolMgr, QWidget *parent = nullptr);

  protected:
    // 每次打开重读 input.conf 与 input-bindings（路径与两档数据各调一次），
    // 快照与表格随之重建——重开看到的就是文件现状，未落盘的改动随之丢弃
    void showEvent(QShowEvent *event) override;
    // 记录器 Del / 退格 / Esc 清空该行而非记作键（同 N86）
    bool eventFilter(QObject *watched, QEvent *event) override;
    // 调色板 / 样式变更重跑行视觉（解除编辑框 Text 色的构造期 pin）
    void changeEvent(QEvent *event) override;

  private slots:
    void showContextMenu(const QPoint &viewportPos);
    bool apply();

  private:
    struct Row {
        enum class Tier { Builtin, Script, Custom };
        Tier tier = Tier::Custom;
        int line = -1; // Custom：InputConf 行下标；其余 -1
        // Builtin / Script 静态内容（input-bindings 的一次读取）：
        QString key; // 原文键名（显示与 tooltip 用它；跨源比对一律现算归一形）
        QString command;
        QString comment;
        // Custom 快照（脏比对基准；删除引起的行号漂移不影响逐行快照）
        QString origKey;
        QString origCommand;
        QString origComment;
        bool origDisabled = false;
        bool appended = false; // 本轮新增行（恒脏）
        // Builtin 打开时的生效态快照
        int origState = StateNormal;
    };
    enum { StateNormal = 0, StateDisabled = 1, StateCovered = 2 };
    enum MenuOp { OpDuplicate, OpDisable, OpEnable, OpDelete };

    // 模型在场且路径可写才算可编辑：拿不到路径（--no-config）与"文件存在但读不到"走
    // 同一口径——报错、锁死编辑与 Apply。后者绝不能当空文件用：那样一次 Apply 会用
    // 新内容盖掉一份没能读进内存的原文件（§7）
    bool editable() const { return !m_noPath && m_conf.has_value(); }

    int blankRow() const { return m_rows.size(); } // 空白录入行恒为末行、不在 m_rows
    void rebuild();
    void rebuildTable();
    void populateRow(int row);
    void populateBlankRow();
    QKeySequenceEdit *makeRecorder(int row, const QKeySequence &seq);
    int appendCustomRow(const QString &key, const QString &command, const QString &comment);
    void removeCustomRow(int row);
    void rollSnapshotForward();
    void afterEdit(); // 一次编辑落地后的统一收口：行级视觉 + Apply 态

    QHash<QString, int> effectiveCustomRows() const; // 归一键名 → 生效自定义行号
    int builtinState(const Row &row, const QHash<QString, int> &eff) const;
    int rowOfEditor(const QKeySequenceEdit *edit) const; // 记录器信号时刻现查行号
    bool isDirty() const;
    // eff（生效行映射）由调用方算一次传入：菜单逐行逐操作判据，内部再算就是 O(n²)
    bool supports(int row, MenuOp op, const QHash<QString, int> &eff) const;
    // 三列取值：自定义档现取模型，内置 / 脚本档取打开时的那一次读取
    QString rowKeyText(int row) const;
    QString rowCommand(int row) const;
    QString rowComment(int row) const;
    bool customRowDirty(const Row &row, const InputConfLine &l) const; // 逐行脏判据
    void restoreCustomRow(int row); // 丢弃该行本轮改动，还原到打开时的状态

    void onRecorderChanged(int row, const QKeySequence &seq);
    void onItemChanged(QTableWidgetItem *item);
    void checkBlankPromotion();
    void refreshRowVisuals();
    void updateApplyButton();

    MpvController *m_mpv = nullptr;
    ActionManager *m_actions = nullptr;
    const CustomToolManager *m_toolMgr = nullptr; // N138：冲突提示并入工具已装键；可空

    QString m_confPath; // 控制器构造期一次求值的 input.conf 路径（N147）；空 = 无可写目录
    bool m_noPath = false;
    std::optional<InputConf> m_conf;
    QList<Row> m_rows;
    QSet<QString> m_builtinKeys; // 归一化内置键集（§3.5 归并判据第 3 条用）

    QTableWidget *m_table = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_okButton = nullptr;
    QLineEdit *m_filter = nullptr;
    QTimer *m_filterTimer = nullptr;

    // 空白录入行的两个待填格（键在 m_blankEditor 里，转正判据实时查）
    QKeySequenceEdit *m_blankEditor = nullptr;
    QString m_blankCommand;
    QString m_blankComment;

    bool m_batchUpdating = false; // 重建 / 批量操作期间守卫
    bool m_failedLock = false;    // 落盘失败锁定位：本次生命周期内不解除（§4.1）
    // 自定义行删除计数：逐行快照看不见删除（行与快照同归于一尽），须单独记。
    // 本轮新增行的删除不计——它们不在打开时的快照里，删掉即无净变化
    int m_deletedLines = 0;
};
