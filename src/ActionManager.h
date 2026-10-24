#pragma once

#include "pch.h"

// A15：全部 UI 动作的出生与登记处——集中声明使键位清单一眼可见；
// 带键动作创建即双挂载主窗口（快捷键不随菜单栏隐藏失活）；零信号连接，
// 业务逻辑归主窗口。
//
// 容器侧禁止自行创建 QAction，一律引用此处的主实例。判据是"要不要给人改绑"，不是
// "有没有默认键"：默认无键而入口本身值得改绑的照常登记（设置 / 快捷键 / mpv 键位
// 编辑器）；既不配默认键、也不会有人想自定义绑定的才由容器自建（关于对话框）——
// 登记它只会在快捷键编辑器里多占一行可改绑的空键行。
//
// N86：运行期改键原语（defaultKey / key / applyKey）与键位归一读取单点
// （effectiveKeys）；改键的生效收口在 MainWindow。
//
// 动作身份的唯一事实源：头文件枚举声明 Id（声明顺序即顺序）+ 构造函数的一次
// declare 调用（稳定英文名、显示名、键位同点声明）——除此之外无第二张表。
class ActionManager : public QObject {
    Q_OBJECT
  public:
    enum class Id {
        Open,
        OpenUrl, // N131
        Settings,
        Shortcuts,      // 快捷键编辑器入口（默认无键）
        MpvKeyBindings, // N89：mpv 侧键位编辑器入口（默认无键，管 input.conf）
        CustomizeTools, // N134：自定义工具列表编辑入口
        ScreenshotCopy, // N78：复制截图到剪贴板（Ctrl+C）
        ScreenshotSave, // N78：截图另存为（Ctrl+E）
        Quit,
        PlayPause,
        Stop,
        PrevFile,
        NextFile,
        VolumeUp,
        VolumeDown,
        Mute, // N38：静音开关（勾选态由 mpv 推送驱动，只作显示）
        SeekForward,
        SeekBackward, // 隐绑定：无菜单实体，仅键盘触发（N6）
        SpeedUp,
        SpeedDown,
        SpeedReset,
        Playlist,
        Navigator,
        Fullscreen,
        MiniWindow, // N68：小窗模式（勾选态由 toggleMini 收口回写）
        // N123：五档窗口尺寸倍率（一次性命令、不可勾选、默认无键；注释给动作名列表一个
        // 能读的行名，稳定英文名带 x 后缀以免被读成"第 N 号窗口尺寸"）
        WindowSize0_25x,
        WindowSize0_5x,
        WindowSize1x,
        WindowSize1_5x,
        WindowSize2x,
        MenuBar,     // N119：菜单栏开关（勾选态即偏好；默认无键，键位留改绑余地）
        ToggleStats, // 视图菜单：状态信息叠加层（由 mpv 侧 stats 脚本渲染）
        // N123：画面比例七项（勾选态由菜单装配按 video-aspect-override 回填）
        AspectOriginal,
        Aspect4x3,
        Aspect16x9,
        Aspect16x10,
        Aspect1_85,
        Aspect2_35,
        AspectCustom,
        Escape, // 隐绑定：无菜单实体，仅键盘触发
        AddBookmark,
        BookmarkEditor,

        // 哨兵：枚举计数与 orderedIds 的遍历上界，不是动作；新动作插在本行之前
        Count
    };

    explicit ActionManager(QWidget *host);

    // 容器挂载 / 状态控制取用（禁用一次生效处处）
    QAction *action(Id id) const;
    // 隐绑定取用（Escape）；连接与守卫逻辑由主窗口完成
    QShortcut *shortcut(Id id) const;

    // ---- N86：运行期改键 ----
    // applyKey 单点生效：QAction 走 setShortcut，QShortcut 走 setKey。空键出生的
    // QAction 首次设非空键时补挂载主窗口（幂等），清空不卸载
    QKeySequence defaultKey(Id id) const;
    QKeySequence key(Id id) const;
    void applyKey(Id id, const QKeySequence &seq);

    // ---- N86：名称（声明期随 declare 落表）----
    // 稳定英文名：持久化键与日志识别用，绝不过 tr
    QString name(Id id) const;
    // N123：菜单文案原样（含助记符）；隐绑定无菜单实体，返回空。它是绕开注释的专用出口
    QString menuText(Id id) const;
    // N123：动作名列表的取名出口——注释非空即取注释，否则动作取菜单文案、隐绑定取显示名。
    // 菜单渲染不经过它（菜单显示的是 QAction 自身文本），注释也永不进菜单
    QString displayText(Id id) const;
    // 不可改绑的条目不进编辑器，ini 覆盖被防卫跳过（恒用声明默认）
    bool modifiable(Id id) const;
    // 英文名反向表：读 ini 时识别未知键
    QHash<QString, Id> nameId() const;
    // 声明序的全部 Id：对话框行序与收口遍历共用
    QList<Id> orderedIds() const;

    // 键位归一读取单点：读 ini [shortcuts] → 逐 Id 归一（无键落声明默认 / "--"
    // 显式空 / 解析值），返回可改绑条目的键位表。主窗收口与对话框快照共用，
    // 此处之外不得再有第二份归一遍历
    QHash<Id, QKeySequence> effectiveKeys(const QString &iniPath) const;

  private:
    // 存储值解读单点："--" 即显式空，其余按 PortableText 解析（解析不出得空序列
    // 并留痕）。私有以防外部自拼"判空 + 解读"再走一遍归一
    static QKeySequence storedToKey(const QString &stored);
    // 声明动作：稳定英文名持久化用；menuText 即菜单文案（含助记符）；comment 非空时
    // 成为动作名列表的显示名（N123），不进菜单
    void declareAction(Id id, const QString &idName, const QString &menuText,
                       const QKeySequence &key = QKeySequence(), bool checkable = false,
                       bool modifiable = true, const QString &comment = QString());
    // 声明隐绑定（QShortcut）：无菜单实体，显示名另行声明（可翻译英文源串）
    void declareShortcut(Id id, const QString &idName, const QString &displayText,
                         const QKeySequence &key, bool modifiable = true);
    // 稳定英文名查重（重复会令 nameId 反向表失配）
    bool nameTaken(const QString &idName) const;

    QWidget *m_host;
    // 一个动作的全部属性住同一处
    struct Entry {
        QAction *action = nullptr;     // 菜单/按钮形态（隐绑定为空）
        QShortcut *shortcut = nullptr; // 隐绑定形态（动作为空）
        QKeySequence defaultKey;       // 声明默认值
        QString name;                  // 稳定英文名（持久化键，绝不过 tr）
        QString menuText;              // 动作侧：菜单文案原样（含助记符）；隐绑定侧为空
        QString displayText;           // 隐绑定侧：声明的可翻译显示名；动作侧为空
        QString comment;               // N123：动作名列表的显示覆盖（非空即用），不进菜单、不落 ini
        bool modifiable = true;        // 不可改绑则不进编辑器，ini 覆盖被防卫跳过
    };
    QHash<Id, Entry> m_entries;
};
