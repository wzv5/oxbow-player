#include "pch.h"

#include "StaticProvider.h"

PlaylistProvider::Type StaticProvider::type() const { return Type::Static; }

QString StaticProvider::describe() const { return QObject::tr("Local static list"); }

void StaticProvider::setItems(QStringList &&items) { m_items = std::move(items); }

std::expected<QStringList, QString> StaticProvider::enumerate() const {
    return m_items; // 静态列表永成功
}

bool StaticProvider::supportsRemove() const { return true; }

bool StaticProvider::supportsSort() const { return true; }

void StaticProvider::saveSettings(QSettings &settings) const {
    // 静态列表不持久化排序键（顺序即添加顺序）
    settings.setValue(QStringLiteral("provider"), QStringLiteral("static"));
    settings.setValue(QStringLiteral("itemCount"), m_items.size());
    for (int j = 0; j < m_items.size(); ++j)
        settings.setValue(QString::number(j), m_items[j]);
}

std::unique_ptr<StaticProvider> StaticProvider::load(QSettings &settings) {
    auto provider = std::make_unique<StaticProvider>();
    // B6：按显式计数字段逐条读取。childKeys() 顺序未定义，键多时呈字典序
    // （"0","1","10","100","11"...），百项级列表会乱序呈现，不可用
    const int n = settings.value(QStringLiteral("itemCount"), 0).toInt();
    QStringList items;
    items.reserve(n);
    for (int j = 0; j < n; ++j) {
        const QString file = settings.value(QString::number(j)).toString();
        if (!file.isEmpty())
            items << file;
    }
    provider->setItems(std::move(items));
    return provider;
}
