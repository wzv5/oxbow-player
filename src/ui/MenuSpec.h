#pragma once

#include "pch.h"

#include "ActionManager.h"

// ——N119：菜单结构与顺序的唯一描述表——菜单栏与视频区右键菜单两个渲染面共用这一份。
// 表只管结构与顺序：连接住动作（A15 单源）或动态段的填充函数，启用态由动作自身或
// 填充函数在构建时判定。子菜单内容能静态表达的一律进表（`Kind::SubMenu` 递归渲染），
// 只有需要运行期回填的段才留给填充函数（N123）。段标题存源文案、渲染时经 tr() 取
// 译文——lupdate 只认 tr(...) 与 QT_TRANSLATE_NOOP(...) 两个抽取点，经变量转手的 tr
// 看不见裸字面量
namespace MenuSpec {

// 形态裁剪：命中当前窗口形态即在构建时跳过，不是置灰（共享动作做不到局部置灰）
enum class HideIn { None, Fullscreen, Mini, Both };

struct Item {
    enum class Kind {
        Action,              // ActionManager 主实例
        About,               // 主窗自建，不进登记表
        InstallDependencies, // N150：可选依赖安装器入口（主窗自建，同 About 判据）
        Separator,           // 分隔线
        Filters,             // 视频 / 音频两个滤镜子菜单
        Tracks,              // 四组轨道子菜单
        Chapter,             // 章节 + 书签整段
        Aspect,              // 画面比例子菜单（N123：内容仍由填充函数装配，勾选需按属性回填）
        Tools,               // N134：自定义工具段 + 编辑入口（buildToolSection 现填）
        SubMenu,             // N123：子菜单内容也由描述表定义（标题 + 子条目表）
    };
    Kind kind = Kind::Action;
    ActionManager::Id action = ActionManager::Id::Count; // 仅 Kind::Action 有意义
    HideIn hideIn = HideIn::None;
    const char *title = nullptr;    // 仅 Kind::SubMenu：QT_TRANSLATE_NOOP 源文案
    std::span<const Item> children; // 仅 Kind::SubMenu：子条目（空即普通条目）

    // 当前窗口形态是否裁掉本条目
    constexpr bool hiddenIn(bool fullscreen, bool mini) const {
        if (hideIn == HideIn::Both)
            return fullscreen || mini;
        if (hideIn == HideIn::Fullscreen)
            return fullscreen;
        return hideIn == HideIn::Mini && mini;
    }
};

struct Group {
    const char *title;           // QT_TRANSLATE_NOOP 源文案
    std::span<const Item> items; // 长度由数组推导
};

// ---- 条目工厂：Kind 与裁剪由签名点名，表上只剩语义名 ----
// 指定初始化器：省略的字段取声明处默认值（`action` 即落 Id::Count 空位），新增可选字段零波及

constexpr Item action(ActionManager::Id id, HideIn hide = HideIn::None) {
    return {.kind = Item::Kind::Action, .action = id, .hideIn = hide};
}
constexpr Item about() { return {.kind = Item::Kind::About}; }
constexpr Item installDependencies() { return {.kind = Item::Kind::InstallDependencies}; }
constexpr Item separator(HideIn hide = HideIn::None) {
    return {.kind = Item::Kind::Separator, .hideIn = hide};
}
constexpr Item section(Item::Kind kind) { return {.kind = kind}; }
// 子菜单：标题源文案 + 子条目表。表须先于引用它的表定义（constexpr span 取数组地址）
constexpr Item subMenu(const char *title, std::span<const Item> children) {
    return {.kind = Item::Kind::SubMenu, .title = title, .children = children};
}

// ---- 描述表：一段一条、按序渲染 ----

inline constexpr Item kFileItems[] = {
    action(ActionManager::Id::Open),
    action(ActionManager::Id::OpenUrl), // N131
    separator(),
    action(ActionManager::Id::Settings),
    action(ActionManager::Id::Shortcuts),
    action(ActionManager::Id::MpvKeyBindings),
    separator(),
    action(ActionManager::Id::ScreenshotCopy),
    action(ActionManager::Id::ScreenshotSave),
    separator(),
    action(ActionManager::Id::Quit),
};

inline constexpr Item kPlayItems[] = {
    action(ActionManager::Id::PlayPause),
    action(ActionManager::Id::Stop),
    separator(),
    action(ActionManager::Id::PrevFile),
    action(ActionManager::Id::NextFile),
    separator(),
    action(ActionManager::Id::VolumeUp),
    action(ActionManager::Id::VolumeDown),
    action(ActionManager::Id::Mute),
    separator(),
    action(ActionManager::Id::SpeedUp),
    action(ActionManager::Id::SpeedDown),
    action(ActionManager::Id::SpeedReset),
    separator(),
    section(Item::Kind::Filters),
    separator(),
    section(Item::Kind::Tracks),
};

inline constexpr Item kChapterItems[] = {section(Item::Kind::Chapter)};

// N123：窗口尺寸子菜单内容——形态项与五档倍率都进表，形态裁剪由 hideIn 表达（小窗不
// 显示 Fullscreen、全屏只留 Fullscreen、分隔线只随倍率出现）
inline constexpr Item kWindowSizeItems[] = {
    action(ActionManager::Id::Fullscreen, HideIn::Mini),
    action(ActionManager::Id::MiniWindow, HideIn::Fullscreen),
    separator(HideIn::Fullscreen),
    action(ActionManager::Id::WindowSize0_25x, HideIn::Fullscreen),
    action(ActionManager::Id::WindowSize0_5x, HideIn::Fullscreen),
    action(ActionManager::Id::WindowSize1x, HideIn::Fullscreen),
    action(ActionManager::Id::WindowSize1_5x, HideIn::Fullscreen),
    action(ActionManager::Id::WindowSize2x, HideIn::Fullscreen),
};

// 覆盖态只留当前的退出口，其余形态项裁掉；段恒在，不出现空子菜单。窗口形态项收进
// 窗口尺寸子菜单（N122）
inline constexpr Item kViewItems[] = {
    action(ActionManager::Id::Playlist, HideIn::Mini),
    action(ActionManager::Id::Navigator, HideIn::Both),
    action(ActionManager::Id::SubtitleBrowser, HideIn::Both),
    action(ActionManager::Id::MenuBar, HideIn::Both),
    action(ActionManager::Id::ToggleStats),
    separator(),
    subMenu(QT_TRANSLATE_NOOP("MainWindow", "Window Size"), kWindowSizeItems),
    section(Item::Kind::Aspect),
};

// N150：安装器入口随 OXBOW_WITH_OPTIONAL_DEPENDENCY_INSTALLER 门禁（关掉时动作不存在）
#ifdef OXBOW_WITH_OPTIONAL_DEPENDENCY_INSTALLER
inline constexpr Item kHelpItems[] = {installDependencies(), about()};
#else
inline constexpr Item kHelpItems[] = {about()};
#endif

// N134：Tools 组
inline constexpr Item kToolsItems[] = {
    section(Item::Kind::Tools),
    action(ActionManager::Id::CustomizeTools),
};

inline constexpr Group kGroups[] = {
    {.title = QT_TRANSLATE_NOOP("MainWindow", "&File"), .items = kFileItems},
    {.title = QT_TRANSLATE_NOOP("MainWindow", "&Play"), .items = kPlayItems},
    {.title = QT_TRANSLATE_NOOP("MainWindow", "&Chapter"), .items = kChapterItems},
    {.title = QT_TRANSLATE_NOOP("MainWindow", "&View"), .items = kViewItems},
    {.title = QT_TRANSLATE_NOOP("MainWindow", "&Tools"), .items = kToolsItems},
    {.title = QT_TRANSLATE_NOOP("MainWindow", "&Help"), .items = kHelpItems},
};

} // namespace MenuSpec
