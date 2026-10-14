#pragma once

#include "pch.h"

class PlaylistProvider;
class ProviderSettingsPage;
class QSettings;
class QWidget;
class ServiceRegistry;

// 提供者登记表条目（N44）：提供者对外可见的全部身份与唯一构造入口。
// 新增提供者＝新增自身实现文件 + providerCatalog() 追加一行（+ CMakeLists.txt 两处源列表）
struct ProviderEntry {
    // 类型键：提供者自身的身份常量（kXxxProviderKey），在提供者头文件单点定义，
    // 本表只做引用。取值必须是静态存储期的字面量（本表为函数内 static，
    // 生命周期覆盖全程序，无悬挂风险）；与 QString 比较时显式转换
    // （typeKey() == QLatin1String(entry.typeKey)），不依赖隐式 ASCII 转换
    const char *typeKey;
    // 显示名取值：提供者的静态函数（翻译上下文即提供者自身，对话框无需知道上下文）。
    // 不直接存裸常量的理由：常量只承载源文案与 lupdate 标记，运行期取值必须有上下文，
    // 存函数可让"上下文归提供者"落到实现里，否则对话框得另持一份上下文字符串
    QString (*displayName)();
    // 创建参数页：seed 为同类型存量提供者（新建 / 换类型传 nullptr）；
    // 实现方在 seed 非空时可直接 static_cast 到自身类型——同类型由调用方（对话框）保证
    ProviderSettingsPage *(*createPage)(const PlaylistProvider *seed,
                                        const std::shared_ptr<ServiceRegistry> &registry,
                                        QWidget *parent);
    // INI 反序列化（原 PlaylistProvider::load 的分派目标）。
    // 注意：函数指针的返回类型没有协变——各提供者的静态工厂必须直接返回
    // shared_ptr<PlaylistProvider>，否则 &XxxProvider::load 赋不进来
    std::shared_ptr<PlaylistProvider> (*load)(QSettings &settings,
                                              const std::shared_ptr<ServiceRegistry> &registry);
    // 无参出生：默认 tab 的提供者没有参数可给，只能凭空造一个。
    // 仅第 0 项（默认提供者）必须有值，其余条目置 nullptr——其余类型没有有意义的
    // "无参实例"（空根 DirProvider 是无效状态，不该被造出来）。
    // 做成表字段而非在登记表外硬编码 StaticProvider：让"默认提供者是谁"只有登记表
    // 这一个出处，重排表不会静默把默认 tab 换成别的类型
    std::shared_ptr<PlaylistProvider> (*createDefault)(
        const std::shared_ptr<ServiceRegistry> &registry);
};

// 登记表（函数内 static，规避跨编译单元静态初始化顺序问题）。
// 不变式：第 0 项即"默认提供者"——新建列表默认选中它，INI 键缺失 / 未知也回落它，
// 默认 tab 的提供者亦经它的 createDefault 出生。
// 条目顺序＝对话框单选顺序＝现状（静态 → 本地目录 → WebDAV）
const QList<ProviderEntry> &providerCatalog();

// 默认提供者实例（默认 tab 的出生点）：取登记表第 0 项，与"第 0 项即默认"同源。
// 视图层经此造默认提供者，不再认识任何具体提供者类型。
// 实现侧：断言第 0 项 createDefault 非空后直接调用（空即登记表漏配，属编程错误）
std::shared_ptr<PlaylistProvider>
createDefaultProvider(const std::shared_ptr<ServiceRegistry> &registry);
