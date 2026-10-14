#pragma once

#include "pch.h"

// N99：上报开关的静态聚合表——"具体种类的唯一聚合点"（ProviderCatalog.cpp 同例）：
// ini 键、注册表种类键、ini 兜底值三件事一行收齐，连"这一行怎么读成当前值"也住这，
// 变更通路 / 启动期同步 / 启动判据 / 设置目录四处引用同一条事实，新增报告者开关 =
// 在本 .cpp 加一行（消费方零改动）。形状是装配期写定的静态数据
// （无锁、无运行期增删），与注册表的"实例账"（互斥锁 + 运行期跨线程增删）刻意分开
// ——同构的反面就是第二套语义，不并入注册表契约。依赖方向照 ProviderCatalog 的先例：
// 本表认识具体种类，消费方只认本头
struct ReporterSwitch {
    const char *iniKey;      // oxbow.ini 键（插件侧选项，非 mpv 选项）
    const char *reporterKey; // 注册表种类键（PlaybackReporterRegistry::Entry::key 同源）
    bool defaultValue;       // ini 兜底（键缺省即此值）
};

const std::vector<ReporterSwitch> &reporterSwitches(); // 全部行（装配期写定、首次构建时自检）
const ReporterSwitch &jellyfinSwitch();                // 单行具名出口（设置目录与读值侧同引）

// 现读全部账目行的当前值（键 = reporterKey，与驱动器启用表同键域）：configFile 为空
// 或 ini 不可读即整表落各行 defaultValue。"怎么把一行账目读成一个 bool"只此一份——
// 变更通路、启动期同步与启动判据三处都从这里取值，新增行不必再往主窗补读代码
// （否则读法仍是两处拷贝，而它漂移的表现依旧是"界面上有开关、驱动器收不到"）
QHash<QString, bool> readReporterSwitchValues(const QString &configFile);

// 启用聚合规则（N74 复验定案的形状）：values 内任一为真，或 registeredKeys 里存在
// values 之外的键（未登记的键按启用处理，与驱动器 keyEnabled 同口径）。registeredKeys
// 必须是**未经任何过滤的全部在册键**（调用方从注册表 snapshot() 提取）——启用表过滤
// 吃的正是这条规则，用它过滤出的子集反推会自指
bool anyEnabled(const QHash<QString, bool> &values, const std::vector<QString> &registeredKeys);
