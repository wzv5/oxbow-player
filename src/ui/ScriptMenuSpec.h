#pragma once

#include "pch.h"

// N172：context-menu 协议的纯解析层，不碰控件与 mpv
namespace ScriptMenuSpec {

// 节点种类：标题恰为 "-" 的条目归入 Separator（逐字比较，不 trim）
enum class NodeKind { Submenu, Item, Separator };

struct Node {
    NodeKind kind = NodeKind::Item;
    QString title;
    QString command;
    QString callback;
    // 显式非空 id 原样保留；缺省为剪枝后的序号路径（分割线不占号，子菜单容器占号）
    QString id;
    // checked 字段存在即条目可勾选；缺席即不可勾选
    bool checkable = false;
    bool checked = false;
    bool disabled = false;
    QList<Node> children; // 仅 Submenu 持有；剪枝后恒非空
};

struct Menu {
    QList<Node> items; // 剪枝后的顶层节点表
};

enum class ParseError { None, BadJson, Empty };

struct ParseResult {
    ParseError error = ParseError::None;
    QString detail; // 只给日志，不进协议
    Menu menu;
};

// 解析、剪枝、赋条目身份；BadJson / Empty 时 menu 无效
ParseResult parse(const QByteArray &json);

// menu-id 字符集 [A-Za-z0-9_.-]{1,64}
bool isValidMenuId(const QString &menuId);

} // namespace ScriptMenuSpec
