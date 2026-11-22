#pragma once

#include "pch.h"

class PlaylistProvider;
class ProviderSettingsPage;
class ServiceRegistry;

// 提供者登记表：可用类型的身份、参数页工厂与反序列化入口。
// 新增提供者＝自身实现 + ProviderCatalog::entries() 追加一行 + CMakeLists 两处源列表
namespace ProviderCatalog {

// 提供者类型在登记表内的身份与分派入口
struct Entry {
    // 静态存储期字面量（表为函数内 static）；与 QString 比较走 QLatin1String
    const char *typeKey;
    // 存函数而非裸常量，让翻译上下文留在提供者实现里
    QString (*displayName)();
    // seed 为同类型存量提供者（新建 / 换类型传 nullptr）；同类型由调用方保证
    ProviderSettingsPage *(*createPage)(const PlaylistProvider *seed,
                                        const std::shared_ptr<ServiceRegistry> &registry,
                                        QWidget *parent);
    // INI 反序列化分派目标。函数指针返回类型没有协变，各提供者的静态工厂必须
    // 直接返回 shared_ptr<PlaylistProvider>
    std::shared_ptr<PlaylistProvider> (*load)(QSettings &settings,
                                              const std::shared_ptr<ServiceRegistry> &registry);
};

// 登记表（函数内 static，规避跨编译单元静态初始化顺序）。
// 条目顺序＝新建列表对话框的单选顺序，不承载"默认提供者"语义；唯一的位置约束是
// 静态条目居首（自检守这一条）：对话框新建时默认选中首项，缺键 / 未知回落写死静态列表
const QList<Entry> &entries();

// 内建静态列表的创建出口：默认 tab 与复制为新表共用。静态身份是内建事实，
// 具体类型只在 ProviderCatalog.cpp 可见
std::shared_ptr<PlaylistProvider>
createStaticProvider(const std::shared_ptr<ServiceRegistry> &registry);

// INI 读回的类型分派：provider 键命中即走该条目的 load；键缺失 / 未知 /
// 工厂返空一律回落内建静态列表（写死，与条目顺序无关）。恒返回非空；
// transit 偏好归 PlaylistProvider 的持久化契约
std::shared_ptr<PlaylistProvider> loadProvider(QSettings &settings,
                                               const std::shared_ptr<ServiceRegistry> &registry);

} // namespace ProviderCatalog
