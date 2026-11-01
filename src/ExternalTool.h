#pragma once

#include "pch.h"

// N142：外部工具的定义与路径发现。只放与具体工具无关的定义与逻辑——MediaInfo 是
// 常驻库、ffmpeg 是外部进程，生命周期不同，两者共用的只有"定义 + 发现 + 路径选择"，
// "加载"概念不进这一层。
// 外部工具：新增一种即加一个枚举值，并在 externalToolSpec 补它那一份定义
enum class ExternalTool { MediaInfo, Ffmpeg };

struct ExternalToolSpec {
    const char *iniKey = nullptr;      // oxbow.ini 的路径设置键名
    const char *displayName = nullptr; // 可翻译源串（QT_TRANSLATE_NOOP 口径，浏览过滤器用）

    // 要发现 / 要加载的目标文件名：定义处按平台条件编译写那一份值。候选发现、基名校验
    // 与浏览过滤器都从这一份取，这一层因此不认识任何具体工具
    QString targetFileName;
};

// 工具的定义：穷举 switch，漏了哪个值在编译期与断言上显形，调用点不必再判"认不认得出"
const ExternalToolSpec &externalToolSpec(ExternalTool tool);

// 路径设置键名（定义里那份窄串转成 QString）：启动侧的 ini 读写从这里取，
// 设置目录那侧按自己的声明写、由 validateCatalog 对着定义校验
QString externalToolIniKey(ExternalTool tool);

// 核心模块所在目录（即插件目录；动态部署下与加载壳同目录）由 App::paths().pluginDir()
// 单点持有（N147），本层不重复反查

// 候选路径：插件目录（App::paths().pluginDir()）→ 配置文件目录 → PATH 各目录 →
// （Linux）shim 同款标准库目录。只返回存在的文件，按规范化后的完整路径去重（Windows
// 下大小写不敏感），保持发现顺序；出表按平台原生分隔符呈现（这些串直接进下拉框与 ini）；
// configDir 由调用方传入（设置对话框取 ini 所在目录，启动侧取 App::paths().iniDir()）
QStringList externalToolCandidates(const ExternalToolSpec &spec, const QString &configDir);

// 文件对话框过滤器：显示名 + 目标文件名，只此一个（不提供 All files——浏览面
// 只能命中 shim 认的目标名）
QString externalToolBrowseFilter(const ExternalToolSpec &spec);

// 路径选择（N146）：ini 显式值 trim 后非空即原样优先（相对写法交调用方的运行机制
// 自行解析），否则取候选表首项，都没有返回空串（判不可用）。ffmpeg 进程与 MediaInfo
// 库的"ini 值优先、缺席自动发现"共用这一个选择点；加载目标的校验与绝对化是另一条
// 口径，住 MediaInfoHelper
QString externalToolResolvedPath(const ExternalToolSpec &spec, const QString &configured,
                                 const QString &configDir);
