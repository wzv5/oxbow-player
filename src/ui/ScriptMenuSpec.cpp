#include "pch.h"

#include "ScriptMenuSpec.h"

namespace ScriptMenuSpec {

namespace {

// 普通条目的动作字段直接落进 node：字段缺席即不动，类型不符即返回 false（整条跳过）；
// 空串等同未给（node 对应字段本就是空串）
bool fillItemFields(const QJsonObject &object, Node &node) {
    const auto stringValue = [&object](QLatin1String key, QString &out) -> bool {
        const auto it = object.find(key);
        if (it == object.end())
            return true;
        if (!it->isString())
            return false;
        out = it->toString();
        return true;
    };
    if (!stringValue(QLatin1String("command"), node.command) ||
        !stringValue(QLatin1String("callback"), node.callback) ||
        !stringValue(QLatin1String("id"), node.id))
        return false;
    // checked 字段存在即条目可勾选，缺席即不可勾选
    const auto checkedIt = object.find(QLatin1String("checked"));
    if (checkedIt != object.end()) {
        if (!checkedIt->isBool())
            return false;
        node.checkable = true;
        node.checked = checkedIt->toBool();
    }
    const auto disabledIt = object.find(QLatin1String("disabled"));
    if (disabledIt != object.end()) {
        if (!disabledIt->isBool())
            return false;
        node.disabled = disabledIt->toBool();
    }
    return true;
}

// 建树（未剪枝）：title 缺失 / 非字符串 / 空串跳过；"-" 归分割线（其余字段忽略）；
// 带 items 是子菜单（items 非数组整棵子树跳过）；坏动作字段跳过该条不废整单
QList<Node> buildNodes(const QJsonArray &array) {
    QList<Node> nodes;
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue; // 无标题对象，无从判形
        const QJsonObject object = value.toObject();
        const auto titleIt = object.find(QLatin1String("title"));
        if (titleIt == object.end() || !titleIt->isString())
            continue;
        const QString title = titleIt->toString();
        if (title.isEmpty())
            continue;
        if (title == QLatin1String("-")) {
            Node node;
            node.kind = NodeKind::Separator;
            node.title = title;
            nodes.append(std::move(node));
            continue;
        }
        const auto itemsIt = object.find(QLatin1String("items"));
        if (itemsIt != object.end()) {
            if (!itemsIt->isArray())
                continue;
            Node node;
            node.kind = NodeKind::Submenu;
            node.title = title;
            node.children = buildNodes(itemsIt->toArray());
            nodes.append(std::move(node));
            continue;
        }
        Node node;
        node.kind = NodeKind::Item;
        node.title = title;
        if (!fillItemFields(object, node))
            continue;
        nodes.append(std::move(node));
    }
    return nodes;
}

// 自底向上单趟：先递归剪子菜单（空则整树移除，不改动分割线折叠状态），同一趟再去当前层
// 前导 / 连续分割线，末尾的在返回前去掉
QList<Node> prune(QList<Node> nodes) {
    QList<Node> out;
    out.reserve(nodes.size());
    bool lastWasSeparator = true; // 层首分割线视为前导
    for (Node &node : nodes) {
        if (node.kind == NodeKind::Submenu) {
            node.children = prune(std::move(node.children));
            if (node.children.isEmpty())
                continue; // 空子菜单递归移除
        }
        if (node.kind == NodeKind::Separator) {
            if (lastWasSeparator)
                continue;
            lastWasSeparator = true;
        } else {
            lastWasSeparator = false;
        }
        out.append(std::move(node));
    }
    while (!out.isEmpty() && out.last().kind == NodeKind::Separator)
        out.removeLast();
    return out;
}

// 剪枝后赋身份：每层只对非分割线条目递增，子菜单容器占号；
// 显式非空 id 原样保留，其余用根到条目的序号路径
void assignIds(QList<Node> &nodes, const QString &prefix) {
    int index = 0;
    for (Node &node : nodes) {
        if (node.kind == NodeKind::Separator)
            continue;
        const QString path =
            prefix.isEmpty() ? QString::number(index) : prefix + u'/' + QString::number(index);
        if (node.kind == NodeKind::Item && node.id.isEmpty())
            node.id = path;
        ++index;
        if (node.kind == NodeKind::Submenu)
            assignIds(node.children, path);
    }
}

} // namespace

ParseResult parse(const QByteArray &json) {
    ParseResult result;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        result.error = ParseError::BadJson;
        result.detail = parseError.error == QJsonParseError::NoError
                            ? QStringLiteral("root is not an object")
                            : parseError.errorString();
        return result;
    }
    const QJsonObject root = doc.object(); // 迭代器须挂在活过整条解析路径的对象上
    const auto itemsIt = root.find(QLatin1String("items"));
    if (itemsIt == root.end() || !itemsIt->isArray()) {
        result.error = ParseError::BadJson;
        result.detail = QStringLiteral("items is missing or not an array");
        return result;
    }
    QList<Node> nodes = prune(buildNodes(itemsIt->toArray()));
    if (nodes.isEmpty()) {
        result.error = ParseError::Empty;
        result.detail = QStringLiteral("no items after pruning");
        return result;
    }
    assignIds(nodes, QString());
    result.menu.items = std::move(nodes);
    return result;
}

bool isValidMenuId(const QString &menuId) {
    if (menuId.isEmpty() || menuId.size() > 64)
        return false;
    for (const QChar ch : menuId) {
        const char16_t c = ch.unicode();
        const bool ok = (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'z') ||
                        (c >= u'A' && c <= u'Z') || c == u'_' || c == u'.' || c == u'-';
        if (!ok)
            return false;
    }
    return true;
}

} // namespace ScriptMenuSpec
