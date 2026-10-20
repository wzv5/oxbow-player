#pragma once

#include "pch.h"

#include "ActionManager.h"

// ——N119：菜单结构与顺序的唯一描述表——菜单栏与视频区右键菜单两个渲染面共用这一份。
// 表只管结构与顺序：连接住动作（A15 单源）或动态段的填充函数，启用态由动作自身或
// 填充函数在构建时判定。段标题存源文案、渲染时经 tr() 取译文——lupdate 只认 tr(...) 与
// QT_TRANSLATE_NOOP(...) 两个抽取点，经变量转手的 tr 看不见裸字面量
namespace MenuSpec {

// 形态裁剪：命中当前窗口形态即在构建时跳过，不是置灰（共享动作做不到局部置灰）
enum class HideIn { None, Fullscreen, Mini, Both };

struct Item {
    enum class Kind {
        Action,    // ActionManager 主实例
        About,     // 主窗自建，不进登记表
        Separator, // 分隔线
        Filters,   // 视频 / 音频两个滤镜子菜单
        Tracks,    // 四组轨道子菜单
        Chapter,   // 章节 + 书签整段
        Aspect,    // 画面比例子菜单
    };
    Kind kind = Kind::Action;
    ActionManager::Id action = ActionManager::Id::Count; // 仅 Kind::Action 有意义
    HideIn hideIn = HideIn::None;

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

// ---- 条目工厂：Kind 与裁剪的位序藏在签名里，表上只剩语义名 ----

constexpr Item action(ActionManager::Id id, HideIn hide = HideIn::None) {
    return {Item::Kind::Action, id, hide};
}
constexpr Item about() { return {Item::Kind::About}; }
constexpr Item separator() { return {Item::Kind::Separator}; }
constexpr Item section(Item::Kind kind) { return {kind}; }

// ---- 描述表：一段一条、按序渲染 ----

inline constexpr Item kFileItems[] = {
    action(ActionManager::Id::Open),
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

// 覆盖态只留当前的退出口，其余形态项裁掉；五段恒在，不出现空子菜单
inline constexpr Item kViewItems[] = {
    action(ActionManager::Id::Playlist, HideIn::Mini),
    action(ActionManager::Id::Navigator, HideIn::Both),
    action(ActionManager::Id::Fullscreen, HideIn::Mini),
    action(ActionManager::Id::MiniWindow, HideIn::Fullscreen),
    action(ActionManager::Id::MenuBar, HideIn::Both),
    action(ActionManager::Id::ToggleStats),
    separator(),
    section(Item::Kind::Aspect),
};

inline constexpr Item kHelpItems[] = {about()};

inline constexpr Group kGroups[] = {
    {QT_TRANSLATE_NOOP("MainWindow", "&File"), kFileItems},
    {QT_TRANSLATE_NOOP("MainWindow", "&Play"), kPlayItems},
    {QT_TRANSLATE_NOOP("MainWindow", "&Chapter"), kChapterItems},
    {QT_TRANSLATE_NOOP("MainWindow", "&View"), kViewItems},
    {QT_TRANSLATE_NOOP("MainWindow", "&Help"), kHelpItems},
};

} // namespace MenuSpec
