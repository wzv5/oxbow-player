#pragma once

#include "pch.h"

#include "mpvbridge/MpvApiPort.h"
#include "ui/FilterListWidget.h" // FilterEntry 完整定义：空列表返回值要实例化 QList

// MpvApiPort 的测试替身：全部方法交默认值（失败 / 空），用例只覆写自己关心的那几个。
// 接口每加一个可跨线程方法，这里同步补一条默认实现——签名由 override 守住。
class ScriptPortStub : public MpvApiPort {
  public:
    int command(const QString &) override { return -1; }
    int command(const QStringList &) override { return -1; }
    void commandAsync(const QStringList &) override {}
    QString commandResultString(const QStringList &) const override { return {}; }
    QImage screenshotRaw() override { return {}; }
    int loadFile(const QString &, const QString &, const QVariantMap &) override { return -1; }
    bool pushBarrier(quint64) override { return false; }

    bool setProperty(const QString &, const QString &) override { return false; }
    bool setProperty(const QString &, int) override { return false; }
    bool setProperty(const QString &, double) override { return false; }
    bool setProperty(const QString &, bool) override { return false; }

    QString propertyString(const QString &) const override { return {}; }
    std::optional<QString> propertyStringOpt(const QString &) const override {
        return std::nullopt;
    }
    std::optional<QString> optionDefaultValue(const QString &) const override {
        return std::nullopt;
    }
    double propertyDouble(const QString &, double fallback) const override { return fallback; }
    bool propertyFlag(const QString &, bool fallback) const override { return fallback; }
    qint64 propertyInt64(const QString &, qint64 fallback) const override { return fallback; }
    QString currentPlayingPath() const override { return {}; }

    QList<FilterEntry> filterEntries(const QString &) override { return {}; }
    std::optional<QList<FilterEntry>> filterEntriesChecked(const QString &) override {
        return std::nullopt;
    }
    InputBindingTiers inputBindings() const override { return {}; }
    QList<MenuDataItem> menuData() const override { return {}; }
    QList<TrackInfo> readTracks() const override { return {}; }

    QString mpvConfPath() const override { return {}; }
    QString inputConfPath() const override { return {}; }

    QSize videoSize() const override { return {}; }
    QSize sourceVideoSize() const override { return {}; }
};
