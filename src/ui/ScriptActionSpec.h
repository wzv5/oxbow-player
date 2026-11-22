#pragma once

#include "pch.h"

// N173：script-action 协议的纯解析层，不碰控件与 mpv
namespace ScriptActionSpec {

// 动作的存储形态；id / owner 由宿主从协议参数填入
struct ScriptAction {
    QString id;
    QString owner;
    QString command;
    QString callback;
    QString icon;
    QString tooltip;
    bool checkable = false;
    bool checked = false;
    bool disabled = false;
    bool hidden = false;
};

// 解析形态：nullopt 即没给，engaged 即给了值（含空串与 false）
struct ScriptActionPatch {
    std::optional<QString> command;
    std::optional<QString> callback;
    std::optional<QString> icon;
    std::optional<QString> tooltip;
    std::optional<bool> checked;
    std::optional<bool> disabled;
    std::optional<bool> hidden;
};

enum class ParseError { None, BadJson };

struct ParseResult {
    ParseError error = ParseError::None;
    QString detail; // BadJson 时的成因串（解析器原文 / 根非对象）
    ScriptActionPatch patch;
};

// 解析动作属性对象；坏类型字段按未给处理，不废整条消息
ParseResult parse(const QByteArray &json);

// 局部更新合并：engaged 覆盖、nullopt 沿用；checked engaged 同时打开 checkable
ScriptAction apply(const ScriptAction &base, const ScriptActionPatch &patch);

// @id 绑定识别：命中返回 id，否则空串（不当绑定按普通命令串处理）
QString boundId(const QString &command);

// 绑定串的生成侧："@" + id，与 boundId 互为逆（N182）
QString commandText(const QString &id);

// id 字符集 [A-Za-z0-9_.-]{1,64}（与 N172 的 menu-id 同）
bool isValidId(const QString &id);

// 出站消息名：oxbow-script-action:<event>
QString eventName(const QString &event);

} // namespace ScriptActionSpec
