#pragma once

#include "PlaylistProvider.h"

// 本地目录提供者（N9）：绑定一个目录，可选含子目录，按扩展名清单同步枚举。
// 枚举结果平铺为单一序列（子目录不分组），排序由视图层按每 tab 规则施加
class DirProvider : public PlaylistProvider {
  public:
    DirProvider();
    DirProvider(const QString &root, bool recursive, const QStringList &extensions);

    Type type() const override;
    QString describe() const override;
    QStringList enumerate() const override;
    bool supportsRemove() const override;
    bool supportsSort() const override;

    // 参数合法性：根目录非空且真实存在（列表设置应用前的校验依据）
    bool isValid() const;
    QString rootPath() const { return m_root; }
    bool recursive() const { return m_recursive; }
    QStringList extensions() const { return m_extensions; }

    void saveSettings(QSettings &settings) const override;

    static std::unique_ptr<DirProvider> load(QSettings &settings);

  private:
    // 递归收集辅助：dir 内命中过滤器的文件追加至 out
    void collect(const QDir &dir, const QStringList &filters, QStringList &out) const;

    QString m_root;           // 绑定目录（绝对路径）
    bool m_recursive = false; // 是否包含子目录（平铺）
    QStringList m_extensions; // 无点小写扩展名；空表表示不过滤
};
