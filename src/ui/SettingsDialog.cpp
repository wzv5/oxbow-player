#include "pch.h"

#include "SettingsDialog.h"

#include "AppearanceManager.h" // N115：Style 候选唯一出处
#include "Constants.h"
#include "ExternalTool.h"
#include "ExternalToolPathCombo.h"
#include "FilterListWidget.h"
#include "Logging.h"
#include "ThemedIconEngine.h"     // N128：重置按钮染色
#include "ai/AiSubtitleCatalog.h" // N146：AI 设置项的键名与缺省单点
#include "mpvbridge/MpvConf.h"
#include "mpvbridge/MpvController.h"
#include "playbackreport/ReporterSwitchCatalog.h" // N99：上报开关目录项同引账目行（键名与缺省单点）

// ===================== 设置目录数据模型（R4 §4.1） =====================
// 数据模型类型定义在全局作用域而非匿名命名空间：SettingsDialog.h 以全局前置
// 声明引用本类型（成员函数签名需要同一实体），匿名命名空间类型无法与之统一；
// 仅本翻译单元定义与使用，无 ODR 顾虑。

enum class WidgetKind {
    Combo,
    EditableCombo,
    SpinBox,
    CheckBox,
    LineEdit,
    MultiLine, // N146：多行文本（QPlainTextEdit；仅 INI 后端，换行交给 QSettings 转义）
    FilterList,
    ToolPath // N142：外部工具路径选择（容器控件，非裸 QComboBox）
}; // CheckBox 预留

// mpv 属性类型提示（仅 mpv 后端有意义，INI 后端忽略）。决定读回路径取原生值的
// 方式与写路径的 typed set；数值属性永不信任 mpv 的字符串化——读原生类型后用
// 本项目统一的规范函数格式化，匹配两侧（读回 / 目录 authored）过同一规范化。
// String 为默认：choice 型属性（如 loop-file 取 no/inf）读回字符串即规范形式。
enum class MpvType { String, Flag, Int, Double };

// 一个可选值。mpv 枚举项的选项必须填全 mpvAssignments；
// assignments 为空的选项是"占位项"（回填永不命中、应用整项跳过）。
struct SettingChoice {
    QString label;                         // 未翻译原文（英文），构建控件时经 tr() 翻译
    QVariant value;                        // 逻辑值：INI 写入值 / 枚举匹配标识
    QMap<QString, QString> mpvAssignments; // 应用时的 key→value 全集；空 = 占位
};
using ChoiceProvider = std::function<QList<SettingChoice>()>;

// SpinBox 专用：取值范围（min/max 合并表达，指定初始化器一处写全）
struct IntRange {
    int min = 0;
    int max = 0;
};

struct SettingItem {
    const char *label = nullptr;       // 未翻译原文（英文），构建控件时经 tr() 翻译
    WidgetKind widget;                 // 控件类型（必填：无默认值，漏写编译报错）
    QString iniKey;                    // 后端互斥（恰好一个非空）：非空 ⇒ 插件选项（oxbow.ini）
    QStringList mpvKeys;               // 非空 ⇒ mpv 选项（本项管辖的全部 key，含复合项）
    ChoiceProvider choices;            // Combo 必填；EditableCombo = 快选预设；mpv CheckBox
                                       // = 两态映射（N33 §2.1）；SpinBox/LineEdit 忽略
    QVariant fallback;                 // 后端不可读 / 未命中 / 依赖禁用时的兜底值
    IntRange range;                    // SpinBox 专用
    const char *note = nullptr;        // 可选备注（空 = 不显示），灰色小字显示在控件下方
    const char *placeholder = nullptr; // N146：LineEdit 占位串（空 = 无）；显示面文案
    bool secret = false;               // N146：LineEdit 密码回显（仅 LineEdit 有意义）
    QString id;                        // 可选：联动主控项标识（仅 CheckBox 主控需要）
    QString dependsOn;                 // 可选：指向某 CheckBox 项 id（单层依赖，禁用不回填）
    MpvType mpvType = MpvType::String; // mpv 属性类型提示（默认 String；INI 后端忽略；
                                       // 带 choices 的 CheckBox 固定 String）
    // FilterList 专用（N42）：预设菜单数据源；其余控件忽略
    std::function<QList<FilterPreset>()> filterPresets;
    // N142：外部工具（仅 ToolPath 使用；定义与 ini 键名以它为准，串形态不再存在）
    std::optional<ExternalTool> tool;
};

struct SettingCategory {
    const char *title; // 未翻译原文
    QList<SettingItem> items;
};

// ===================== 设置目录定义（R4 §4.1 / §4.2） =====================
// "分类 → 设置项"两层有序目录：定义顺序即 UI 顺序。本命名空间是全对话框唯一的
// 设置项数据出处；SettingsDialog 类本体只按定义工作，不含具体项的过程式代码。
// 新增设置项 / 分类只改本处定义，机器零改动。
namespace {

// —— mpv 值域规范化（实施期修订：mpvType 类型提示方案）——
//
// 读回：按类型提示取原生值后统一规范化为字符串。Flag/Int/Double 先经字符串读
// 探测可读性（三者的合法值经 mpv 字符串化永不为空），不可读返回 false——调用方
// 据此让枚举匹配落占位项（与"和任何 authored 赋值都不相等"同义）。
// String 走 propertyStringOpt：读取失败与合法空串必须可分——回读写盘与缺键判定都靠它
bool readCanonicalMpvValue(MpvController *c, const QString &key, MpvType type, QString &out) {
    switch (type) {
    case MpvType::String:
        if (auto s = c->propertyStringOpt(key)) {
            out = *s;
            return true;
        }
        return false;
    case MpvType::Flag:
        if (c->propertyString(key).isEmpty())
            return false;
        out = c->propertyFlag(key, false) ? QStringLiteral("yes") : QStringLiteral("no");
        return true;
    case MpvType::Int:
        if (c->propertyString(key).isEmpty())
            return false;
        out = QString::number(c->propertyInt64(key, 0));
        return true;
    case MpvType::Double:
        if (c->propertyString(key).isEmpty())
            return false;
        out = QString::number(c->propertyDouble(key, 0.0), 'g', 15);
        return true;
    }
    return false;
}

// authored 侧：目录赋值字符串过同一规范形式（Int 十进制 / Double 'g',15），
// 目录手滑写成 130.0 也能与读回的 130 相互匹配。
QString canonicalAuthoredMpvValue(MpvType type, const QString &raw) {
    bool ok = false;
    switch (type) {
    case MpvType::String:
    case MpvType::Flag: // authored 必须已写 yes/no（目录自检校验）
        return raw;
    case MpvType::Int: {
        const qint64 v = raw.toLongLong(&ok);
        return ok ? QString::number(v) : raw;
    }
    case MpvType::Double: {
        const double v = raw.toDouble(&ok);
        return ok ? QString::number(v, 'g', 15) : raw;
    }
    }
    return raw;
}

// 写路径：按类型提示走 typed set（消灭"数值当字符串下发再由 mpv 解析"环节）。
// 返回值判失败：任一 key 下发失败即整项不写 conf，避免持久配置被半截污染
bool setTypedMpvProperty(MpvController *c, const QString &key, MpvType type, const QString &raw) {
    switch (type) {
    case MpvType::String:
        return c->setProperty(key, raw);
    case MpvType::Flag:
        return c->setProperty(key, raw == QLatin1String("yes"));
    case MpvType::Int:
        return c->setProperty(key, int(raw.toLongLong()));
    case MpvType::Double:
        return c->setProperty(key, raw.toDouble());
    }
    return false;
}

// 把"逐 key authored 串"（mpv.conf 保存值 / option-info 默认值两个来源共用）映射为
// 控件同型值，分派结构与 fillItemFromBackend 同构。映射不了（两态未命中 / SpinBox 不可
// 解析）返回 nullopt，由调用方落目录 fallback；Combo 占位恒命中（目录自检保证恰一个占位）
std::optional<QVariant> controlValueFromAuthoredKeys(const SettingItem *item,
                                                     const QMap<QString, QString> &authoredByKey) {
    switch (item->widget) {
    case WidgetKind::FilterList: {
        // 空串是合法保存值（`vf=` 空行进这里，缺行根本不进来）→ 空列表
        const QString raw = authoredByKey.value(item->mpvKeys[0]);
        if (raw.isEmpty())
            return QStringLiteral("");
        return FilterListWidget::compileEntries(FilterListWidget::parseString(raw));
    }
    case WidgetKind::Combo: {
        // 两侧同过 canonicalAuthoredMpvValue 再比（数值属性永不信任字符串化）；
        // 组合不命中任何非占位 choice → 占位项值
        QMap<QString, QString> current;
        for (auto it = authoredByKey.cbegin(); it != authoredByKey.cend(); ++it)
            current.insert(it.key(), canonicalAuthoredMpvValue(item->mpvType, it.value()));
        const auto choices = item->choices ? item->choices() : QList<SettingChoice>{};
        const SettingChoice *placeholder = nullptr;
        for (const auto &c : choices) {
            if (c.mpvAssignments.isEmpty()) {
                placeholder = &c;
                continue;
            }
            QMap<QString, QString> canon;
            for (auto a = c.mpvAssignments.cbegin(); a != c.mpvAssignments.cend(); ++a)
                canon.insert(a.key(), canonicalAuthoredMpvValue(item->mpvType, a.value()));
            if (canon == current)
                return c.value;
        }
        if (placeholder)
            return placeholder->value;
        return std::nullopt;
    }
    case WidgetKind::EditableCombo:
    case WidgetKind::LineEdit:
        return canonicalAuthoredMpvValue(item->mpvType, authoredByKey.value(item->mpvKeys[0]));
    case WidgetKind::SpinBox: {
        const QString raw =
            canonicalAuthoredMpvValue(item->mpvType, authoredByKey.value(item->mpvKeys[0]));
        bool ok = false;
        const double d = raw.toDouble(&ok);
        if (!ok)
            return std::nullopt;
        return qBound(item->range.min, qRound(d), item->range.max); // 同 fill：钳制后取整
    }
    case WidgetKind::CheckBox: {
        const QString raw = authoredByKey.value(item->mpvKeys[0]);
        if (item->choices) {
            // 两态映射：保存串与两项赋值精确比对，未命中交调用方落 fallback
            const auto choices = item->choices();
            for (const auto &c : choices) {
                if (c.mpvAssignments.value(item->mpvKeys[0]) == raw)
                    return c.value.toBool();
            }
            return std::nullopt;
        }
        // Flag 直通两态
        return canonicalAuthoredMpvValue(item->mpvType, raw) == QLatin1String("yes");
    }
    case WidgetKind::ToolPath:
        break; // 仅 INI 后端（validateCatalog 保证），mpv 读回不触达
    case WidgetKind::MultiLine:
        break; // 仅 INI 后端（N146），mpv 读回不触达
    }
    return std::nullopt;
}

// 音量步进四档（滚轮 / 方向键共用；INI 后端，写 int）
QList<SettingChoice> stepChoices() {
    QList<SettingChoice> l;
    for (int s : {1, 2, 5, 10})
        l.append({QString::number(s), s, {}});
    return l;
}

// N113：OSD 形态四档（插件 INI 后端，写串）——顺序即 UI 顺序，值与 OsdController
// 的 modeFromIniValue 对表
QList<SettingChoice> osdModeChoices() {
    return {
        {QT_TRANSLATE_NOOP("SettingsDialog", "Off"), QStringLiteral("off"), {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "mpv overlay"), QStringLiteral("overlay"), {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "mpv show-text"), QStringLiteral("show-text"), {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Standalone"), QStringLiteral("standalone"), {}},
    };
}

// N146：AI 两档模式（INI 后端，写串）——两个模式下拉同款取值；label 经 trS 翻译，
// 值是稳定数据不随翻译变
QList<SettingChoice> aiModeChoices() {
    return {
        {QT_TRANSLATE_NOOP("SettingsDialog", "Built-in"),
         QString::fromLatin1(AiSubtitleCatalog::kModeBuiltin),
         {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Custom"),
         QString::fromLatin1(AiSubtitleCatalog::kModeCustom),
         {}},
    };
}

// N146：目标语言候选（EditableCombo，可手输）——清单是数据不是界面文案，label 即
// 取值本身，不做显示名映射、不进翻译
QList<SettingChoice> translationTargetLangChoices() {
    QList<SettingChoice> choices;
    choices.reserve(AiSubtitleCatalog::targetLanguages().size());
    for (const QString &language : AiSubtitleCatalog::targetLanguages())
        choices.append({language, language, {}});
    return choices;
}

// 语言 native 显示名（迁自原 populateLanguageCombo 的显示逻辑）
QString nativeLanguageDisplayName(const QString &loc) {
    if (loc == QLatin1String("en"))
        return QStringLiteral("English");
    QLocale qloc(loc);
    QString display = qloc.nativeLanguageName();
    if (display.isEmpty())
        return loc;
    display[0] = display[0].toUpper();
    const QString terr = qloc.nativeTerritoryName();
    if (!terr.isEmpty() && !display.contains(terr))
        display = QStringLiteral("%1 (%2)").arg(display, terr);
    return display;
}

// 语言列表：动态枚举，每次构建控件时调用（不缓存，Auto 项 data 为空串）
QList<SettingChoice> languageChoices() {
    QList<SettingChoice> l;
    l.append({QT_TRANSLATE_NOOP("SettingsDialog", "Auto"), QString(), {}}); // 空 data = Auto
    QStringList locales{QStringLiteral("en")};
    QDir dir(QStringLiteral(":/i18n"));
    const QStringList qmFiles =
        dir.entryList(QStringList() << QStringLiteral("oxbow_*.qm"), QDir::Files);
    for (const QString &f : qmFiles) {
        if (f.startsWith(QStringLiteral("oxbow_")) && f.endsWith(QStringLiteral(".qm"))) {
            QString loc = f.chopped(3).mid(6);
            if (!loc.isEmpty() && !locales.contains(loc))
                locales << loc;
        }
    }
    std::sort(locales.begin(), locales.end(),
              [](const QString &a, const QString &b) { return a < b; });
    for (const QString &loc : locales)
        l.append({nativeLanguageDisplayName(loc), loc, {}});
    return l;
}

// N115：界面样式清单——候选经 AppearanceManager 取 keys() 原样顺序；Auto 项
// data 为空串（与语言项同形）
QList<SettingChoice> styleChoices() {
    QList<SettingChoice> l;
    l.append({QT_TRANSLATE_NOOP("SettingsDialog", "Auto"), QString(), {}}); // 空 data = Auto
    for (const QString &k : AppearanceManager::availableStyles())
        l.append({k, k, {}});
    return l;
}

// N115：配色方案三态
QList<SettingChoice> colorSchemeChoices() {
    return {
        {QT_TRANSLATE_NOOP("SettingsDialog", "System"), QStringLiteral("system"), {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Light"), QStringLiteral("light"), {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Dark"), QStringLiteral("dark"), {}},
    };
}

// 循环模式：复合项示例——一个 UI 项管辖两个 mpv key（R3 §3.7 语义的数据化）。
// 第 0 项为占位（assignments 为空）：用户手改 conf 产生的非预设组合回填到该项，
// 应用时不下发 mpv、不写 conf（不干涉用户意图）。
QList<SettingChoice> loopChoices() {
    return {
        {QT_TRANSLATE_NOOP("SettingsDialog", "Custom (do not override)"), QString(), {}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "No Loop"),
         QStringLiteral("none"),
         {{QStringLiteral("loop-file"), QStringLiteral("no")},
          {QStringLiteral("loop-playlist"), QStringLiteral("no")}}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Loop One"),
         QStringLiteral("file"),
         {{QStringLiteral("loop-file"), QStringLiteral("inf")},
          {QStringLiteral("loop-playlist"), QStringLiteral("no")}}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Loop Playlist"),
         QStringLiteral("playlist"),
         {{QStringLiteral("loop-file"), QStringLiteral("no")},
          {QStringLiteral("loop-playlist"), QStringLiteral("inf")}}},
    };
}

// 硬件解码：标量项（可编辑下拉），choices 仅作快选预设，逻辑值即文本本身
QList<SettingChoice> hwdecPresets() {
#ifdef Q_OS_WIN
    return {{QStringLiteral("no")},           {QStringLiteral("auto")},
            {QStringLiteral("auto-copy")},    {QStringLiteral("d3d11va")},
            {QStringLiteral("d3d11va-copy")}, {QStringLiteral("nvdec")},
            {QStringLiteral("nvdec-copy")},   {QStringLiteral("vulkan")},
            {QStringLiteral("vulkan-copy")},  {QStringLiteral("dxva2")},
            {QStringLiteral("dxva2-copy")},   {QStringLiteral("cuda")},
            {QStringLiteral("cuda-copy")}};
#else
    return {{QStringLiteral("no")},          {QStringLiteral("auto")},
            {QStringLiteral("auto-copy")},   {QStringLiteral("vaapi")},
            {QStringLiteral("vaapi-copy")},  {QStringLiteral("nvdec")},
            {QStringLiteral("nvdec-copy")},  {QStringLiteral("drm")},
            {QStringLiteral("drm-copy")},    {QStringLiteral("vulkan")},
            {QStringLiteral("vulkan-copy")}, {QStringLiteral("vdpau")},
            {QStringLiteral("vdpau-copy")},  {QStringLiteral("cuda")},
            {QStringLiteral("cuda-copy")}};
#endif
}

// N42：滤镜预置（插入模板，一组条目整体追加为勾选态新条目；name 为可翻译的
// @name: 标签，插入时挂到首条，Custom 不挂）。数据与设置目录同处一文件，翻译
// 上下文归属 SettingsDialog；清单为冻结规格的初始值，具体条目与参数属实施期
// 细节，改预设只改此处。
// label 与 name 一律套 QT_TRANSLATE_NOOP：两者的翻译点都在 FilterListWidget 侧
// （经 QCoreApplication::translate 按变量翻译），lupdate 无法从变量提取原文，
// 缺此标记则 .ts 不收录、译文永不生效
QList<FilterPreset> audioFilterPresets() {
    return {{QT_TRANSLATE_NOOP("SettingsDialog", "Loudness Normalization (Streaming, -14 LUFS)"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Loudness"),
             {QStringLiteral("loudnorm=I=-14:TP=-1:LRA=11")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Loudness Normalization (EBU R128, -23 LUFS)"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Loudness"),
             {QStringLiteral("loudnorm=I=-23:TP=-1:LRA=11")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Resample 48k"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Resample"),
             {QStringLiteral("aresample=48000")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Dynamic Range Compression"),
             QT_TRANSLATE_NOOP("SettingsDialog", "DRC"),
             {QStringLiteral("dynaudnorm=f=250:g=15")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Volume Boost (+6 dB)"),
             QT_TRANSLATE_NOOP("SettingsDialog", "VolumeBoost"),
             {QStringLiteral("volume=6dB")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Custom"), nullptr, {QString()}}};
}

QList<FilterPreset> videoFilterPresets() {
    // 90° 旋转用 transpose=clock 而非 rotate=PI/2：libavfilter 的 rotate 保持帧
    // 尺寸原地旋转（内容被裁、video-out-params 宽高不更新，窗口自适应失效），
    // transpose 才交换宽高
    return {{QT_TRANSLATE_NOOP("SettingsDialog", "Deinterlace"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Deinterlace"),
             {QStringLiteral("bwdif")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Denoise"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Denoise"),
             {QStringLiteral("hqdn3d")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Sharpen"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Sharpen"),
             {QStringLiteral("unsharp=5:5:1.0")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Deband"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Deband"),
             {QStringLiteral("deband")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Rotate 90°"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Rotate90"),
             {QStringLiteral("transpose=clock")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Flip Horizontally"),
             QT_TRANSLATE_NOOP("SettingsDialog", "FlipH"),
             {QStringLiteral("hflip")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Flip Vertically"),
             QT_TRANSLATE_NOOP("SettingsDialog", "FlipV"),
             {QStringLiteral("vflip")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Scale"),
             QT_TRANSLATE_NOOP("SettingsDialog", "Scale"),
             {QStringLiteral("scale=1920:1080")}},
            {QT_TRANSLATE_NOOP("SettingsDialog", "Custom"), nullptr, {QString()}}};
}

// N37：播放侧证书开关（mpv tls-verify）的两态映射（N33 §2.1）。勾选"忽略"对应
// tls-verify=no（与 Flag 默认语义相反，故不引入取反字段、按两态映射表达），
// 未勾选 → yes 转严格校验。逻辑值仅 CheckBox 内部使用，标签不显示
QList<SettingChoice> tlsVerifyChoices() {
    return {
        {QT_TRANSLATE_NOOP("SettingsDialog", "Ignore"),
         true,
         {{QStringLiteral("tls-verify"), QStringLiteral("no")}}},
        {QT_TRANSLATE_NOOP("SettingsDialog", "Strict"),
         false,
         {{QStringLiteral("tls-verify"), QStringLiteral("yes")}}},
    };
}

// 全局目录（函数内 static 单例，内容 const；规避跨编译单元静态初始化顺序问题）。
// 指定初始化器：省略的字段取声明处默认值，新增可选字段零波及。
const QList<SettingCategory> &settingCatalog() {
    static const QList<SettingCategory>
        catalog =
            {
                // ① 基本 —— 插件本体选项（写 oxbow.ini）
                {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Basic"),
                 .items =
                     {
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Language"),
                          .widget = WidgetKind::Combo,
                          .iniKey = "language",
                          .choices = languageChoices,
                          .fallback = App::languageDefault(),
                          .note =
                              QT_TRANSLATE_NOOP("SettingsDialog", "Takes effect after restart")},
                         // N115：Style 兜底空串 = 第 0 项 Auto
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Interface Style"),
                          .widget = WidgetKind::Combo,
                          .iniKey = "style",
                          .choices = styleChoices,
                          .fallback = App::interfaceStyleDefault() /* 空 = Auto（第 0 项） */},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Color Scheme"),
                          .widget = WidgetKind::Combo,
                          .iniKey = "colorScheme",
                          .choices = colorSchemeChoices,
                          .fallback = App::colorSchemeDefault(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Only some styles (such as Fusion) follow this setting; "
                              "others ignore it silently")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Wheel Step"),
                          .widget = WidgetKind::Combo,
                          .iniKey = "volumeWheelStep",
                          .choices = stepChoices,
                          .fallback = App::volumeWheelStepDefault(),
                          .note = QT_TRANSLATE_NOOP("SettingsDialog", "For adjusting volume")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Arrow Key Step"),
                          .widget = WidgetKind::Combo,
                          .iniKey = "volumeKeyStep",
                          .choices = stepChoices,
                          .fallback =
                              App::volumeKeyStepDefault(), // 对齐现版：miss 兜底 5 而非第 0 项
                          .note = QT_TRANSLATE_NOOP("SettingsDialog", "For adjusting volume")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Recursive Depth Limit"),
                          .widget = WidgetKind::SpinBox,
                          .iniKey = "recursiveDepthLimit",
                          .fallback = App::recursiveDepthLimitDefault(),
                          .range = {0, App::recursiveDepthLimitMax()},
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "For directory enumeration including subdirectories")},
                         // N113：OSD 形态四档（插件 INI 后端；只管插件自己的消息，mpv 内置
                         // OSD 不受影响）。位置取 Basic 末位，不打散音量步进与递归深度的
                         // 既有相邻关系
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "OSD Mode"),
                          .widget = WidgetKind::Combo,
                          .iniKey = "osdMode",
                          .choices = osdModeChoices,
                          .fallback = App::osdModeDefault(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Controls the plugin's own OSD only; mpv's built-in OSD is "
                              "unaffected")},
                     }},
                // ② 播放 —— mpv 选项（写 mpv.conf + 下发）
                {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Playback"),
                 .items =
                     {
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Loop Playback"),
                          .widget = WidgetKind::Combo,
                          .mpvKeys = {"loop-file", "loop-playlist"},
                          .choices = loopChoices,
                          // fallback 兼 mpv 默认回退（no/no → No Loop）；占位由"组合不命中任何
                          // choice"的回填承担，两者职责不同
                          .fallback = QStringLiteral("none")},
                         // N107：悬停缩略图开关（插件 INI 项与 mpv 项同分类混列，后端按项互斥，
                         // ignoreHttpsErrors 与 Audio 分类混列同先例）
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Show thumbnail on hover"),
                          .widget = WidgetKind::CheckBox,
                          .iniKey = "hoverThumbnail",
                          .fallback = App::hoverThumbnailDefault(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog", "Shows a preview frame while hovering the progress "
                                                "bar; not available for remote sources")},
                     }},
                // ③ 视频 —— mpv 选项
                {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Video"),
                 .items =
                     {
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Hardware Decoding"),
                          .widget = WidgetKind::EditableCombo,
                          .mpvKeys = {"hwdec"},
                          .choices = hwdecPresets,
                          .fallback = "no"},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Video Filters"),
                          .widget = WidgetKind::FilterList,
                          .mpvKeys = {"vf"},
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "To use video filters, set Hardware Decoding to a copy mode "
                              "(auto-copy) or no; changing it during playback may require stop "
                              "and play to take effect"),
                          .filterPresets = videoFilterPresets},
                     }},
                // ④ 音频 —— mpv 选项
                {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Audio"),
                 .items =
                     {
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Max Volume"),
                          .widget = WidgetKind::SpinBox,
                          .mpvKeys = {"volume-max"},
                          .fallback = 130,
                          .range = {100, 1000},
                          .mpvType = MpvType::Double},
                         // N108：MPC-HC 兼容音量曲线（插件 INI 选项）——勾选后滑条显示百分比
                         // 与量程按二次方听感标尺换算，换算与量程重算都在 MainWindow 侧
                         // （Util::Volume + updateVolumeSliderRange）；本项只管键的读写
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Louder Volume Curve"),
                          .widget = WidgetKind::CheckBox,
                          .iniKey = "quadraticVolumeCurve",
                          .fallback = App::quadraticVolumeCurveDefault(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Uses a quadratic volume curve instead of mpv's cubic one to match "
                              "MPC-HC's volume scale; the slider's range is rescaled so its top "
                              "still matches Max Volume")},
                         // N42：原单行文本框升级为滤镜列表编辑器（空值语义不变：无滤镜）
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Audio Filters"),
                          .widget = WidgetKind::FilterList,
                          .mpvKeys = {"af"},
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "loudnorm works internally at 192 kHz in dynamic mode and raises the "
                              "output sample rate; add aresample=48000 when a fixed rate is "
                              "needed"),
                          .filterPresets = audioFilterPresets},
                     }},
                // ⑤ 网络 —— N35：远端浏览链路（WebDAV 枚举）所用 client 的缺省参数（写
                // oxbow.ini）；N37 起并列 mpv 侧播放证书开关 （写 mpv.conf + 下发
                // tls-verify）——两条链路的证书校验各自独立，
                // 只关一侧就会出现"能列出、播不了"（或反之），两开关互不联动；
                // 并列 mpv 侧 ytdl hook 开关（写 mpv.conf + 下发 ytdl）
                {
                    .title = QT_TRANSLATE_NOOP("SettingsDialog", "Network"),
                    .items =
                        {
                            {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Remote Request Timeout"),
                             .widget = WidgetKind::SpinBox,
                             .iniKey = "remoteTimeoutSec",
                             .fallback = App::remoteTimeoutSecDefault(),
                             .range = {1, 120}},
                            {.label = QT_TRANSLATE_NOOP("SettingsDialog",
                                                        "Ignore HTTPS Certificate Errors"),
                             .widget = WidgetKind::CheckBox,
                             .iniKey = "ignoreHttpsErrors",
                             .fallback = App::ignoreHttpsErrorsDefault(),
                             // N37：澄清只管浏览侧（列表与元数据请求），不影响播放
                             .note = QT_TRANSLATE_NOOP(
                                 "SettingsDialog",
                                 "Applies to playlist and metadata requests only; "
                                 "playback is governed by the mpv-side option below")},
                            // N37：播放侧（mpv / ffmpeg 发请求）证书开关。fallback 兼 mpv 默认回退
                            // （实测默认 no → Ignore 勾选），只在属性不可读或取值未命中两项赋值时
                            // 兜底；可实时读写，正常运行落不到这里
                            {.label = QT_TRANSLATE_NOOP(
                                 "SettingsDialog",
                                 "Ignore HTTPS Certificate Errors (mpv side)"),
                             .widget = WidgetKind::CheckBox,
                             .mpvKeys = {"tls-verify"},
                             .choices = tlsVerifyChoices,
                             .fallback = true,
                             .mpvType = MpvType::String},
                            // mpv 侧 ytdl hook 开关（ytdl 选项，bool）：勾选 = ytdl=yes（mpv
                            // 内置默认即启用，可实时读写——ytdl hook 脚本逐文件加载时现读该
                            // 选项，改动对后续播放立即生效）；无 choices 走 Flag 直通两态
                            {.label = QT_TRANSLATE_NOOP("SettingsDialog",
                                                        "Enable youtube-dl hook script"),
                             .widget = WidgetKind::CheckBox,
                             .mpvKeys = {"ytdl"},
                             .fallback = true,
                             .mpvType = MpvType::Flag},
                        }},
                // ⑥ 播放上报 —— N74：报告者开关组（粒度 = 报告者种类，ini 键与注册表的键
                // 一一对应；分类不设总开关）。全部写 oxbow.ini，应用成功经既有
                // pluginIniKeysChanged 通知主窗改驱动器启用表并切闸门，实时生效、无需重启
                {
                    .title = QT_TRANSLATE_NOOP("SettingsDialog", "Playback Reporting"),
                    .items =
                        {
                            {.label =
                                 QT_TRANSLATE_NOOP("SettingsDialog", "Report playback to Jellyfin"),
                             .widget = WidgetKind::CheckBox,
                             .iniKey =
                                 jellyfinSwitch().iniKey, // 键名与缺省同引上报账目行（N99 单点）
                             .fallback = jellyfinSwitch().defaultValue,
                             .note = QT_TRANSLATE_NOOP(
                                 "SettingsDialog",
                                 "Sends watch position to the signed-in Jellyfin server so resume "
                                 "points and watched marks stay up to date")},
                        }},
                // ⑦ 外部工具 —— N142：工具的路径设置。空串 = 自动态（写盘期按"等于默认即删
                // 键"口径从 ini 移除）
                {
                    .title = QT_TRANSLATE_NOOP("SettingsDialog", "External Tools"),
                    .items =
                        {
                            {.label = QT_TRANSLATE_NOOP("SettingsDialog", "MediaInfo Library"),
                             .widget = WidgetKind::ToolPath,
                             .iniKey = "mediaInfoPath",
                             .fallback = QString(),
                             .note = QT_TRANSLATE_NOOP("SettingsDialog",
                                                       "Path changes take effect after restart"),
                             .tool = ExternalTool::MediaInfo},
                            // N143：ffmpeg 是外部进程，路径改动无需重启，下一次取数即按新路径
                            {.label = QT_TRANSLATE_NOOP("SettingsDialog", "FFmpeg Executable"),
                             .widget = WidgetKind::ToolPath,
                             .iniKey = "ffmpegPath",
                             .fallback = QString(),
                             .tool = ExternalTool::Ffmpeg},
                        }},
                // ⑧ AI —— N146：语音识别字幕与可选翻译（全部写 oxbow.ini；键名与缺省引
                // AiSubtitleCatalog 单点）。模式下拉是唯一主控，但不做 dependsOn 禁用：
                // 第 2、4、5、6 项始终可见可编辑、取值原样往返，切换模式不丢已填内容，
                // 运行期只看当前模式那一档（命令行／端点／key／模型归用户，内置档忽略）
                {.title = QT_TRANSLATE_NOOP("SettingsDialog", "AI"),
                 .items =
                     {
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "ASR Mode"),
                          .widget = WidgetKind::Combo,
                          .iniKey = AiSubtitleCatalog::kAsrModeKey,
                          .choices = aiModeChoices,
                          .fallback = QString::fromLatin1(AiSubtitleCatalog::kModeBuiltin),
                          .note = QT_TRANSLATE_NOOP("SettingsDialog",
                                                    "Built-in mode uses whisper")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Custom ASR Command"),
                          .widget = WidgetKind::LineEdit,
                          .iniKey = AiSubtitleCatalog::kAsrCommandKey,
                          .fallback = AiSubtitleCatalog::builtinAsrCommand(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Only used in Custom mode. Placeholder: ${wav_file}. The command "
                              "must "
                              "write subtitles to stdout in UTF-8 as SRT blocks or whisper "
                              "timestamp "
                              "lines (incrementally or all at once). The working directory is "
                              "always "
                              "the plugin directory, and no shell is involved.")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Translation Mode"),
                          .widget = WidgetKind::Combo,
                          .iniKey = AiSubtitleCatalog::kTranslationModeKey,
                          .choices = aiModeChoices,
                          .fallback = QString::fromLatin1(AiSubtitleCatalog::kModeBuiltin),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Built-in mode uses llama.cpp with the Hy-MT2 model")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Translation API Endpoint"),
                          .widget = WidgetKind::LineEdit,
                          .iniKey = AiSubtitleCatalog::kTranslationApiEndpointKey,
                          .fallback = QString(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Only used in Custom mode. OpenAI-compatible endpoint: base URL or "
                              "full path.")},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Translation API Key"),
                          .widget = WidgetKind::LineEdit,
                          .iniKey = AiSubtitleCatalog::kTranslationApiKeyKey,
                          .fallback = QString(),
                          .note = QT_TRANSLATE_NOOP("SettingsDialog", "Only used in Custom mode."),
                          .secret = true},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Translation Model"),
                          .widget = WidgetKind::LineEdit,
                          .iniKey = AiSubtitleCatalog::kTranslationModelKey,
                          .fallback = QString(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Only used in Custom mode. Empty means the request omits the model "
                              "field."),
                          .placeholder = QT_TRANSLATE_NOOP("SettingsDialog", "Automatic")},
                         {.label =
                              QT_TRANSLATE_NOOP("SettingsDialog", "Translation Target Language"),
                          .widget = WidgetKind::EditableCombo,
                          .iniKey = AiSubtitleCatalog::kTranslationTargetLangKey,
                          .choices = translationTargetLangChoices,
                          .fallback = AiSubtitleCatalog::defaultTargetLanguage()},
                         {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Translation Prompt"),
                          .widget = WidgetKind::MultiLine,
                          .iniKey = AiSubtitleCatalog::kTranslationPromptKey,
                          .fallback = AiSubtitleCatalog::defaultTranslationPrompt(),
                          .note = QT_TRANSLATE_NOOP(
                              "SettingsDialog",
                              "Placeholders: {target_lang} and {source_text}. Adjust it freely to "
                              "suit your model.")},
                     }},
    };
    return catalog;
}

// —— 预留能力语法示意（当前目录不含，实施期以"假想项演练"验证）——
//
// 备注（note 非空即显示灰色小字）：
//   {.label = QT_TRANSLATE_NOOP("SettingsDialog", "..."), .note =
//   QT_TRANSLATE_NOOP("SettingsDialog", "Changes require restart"), ...}
//
// CheckBox 主控 + 联动依赖（id / dependsOn；禁用不回填、应用跳过）：
//   {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Use Custom Steps"), .widget =
//   WidgetKind::CheckBox,
//    .iniKey = "useCustomSteps", .fallback = false,
//    .note = QT_TRANSLATE_NOOP("SettingsDialog", "Enable per-device step tuning"), .id =
//    "customSteps"},
//   {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Custom Step"), .widget = WidgetKind::SpinBox,
//    .mpvKeys = {"volume-step"}, .fallback = 5,
//    .range = {1, 10}, .dependsOn = "customSteps"},

// 两态映射自检（N33 §2.1）：带 choices 的 CheckBox 表达单个 mpv key 的 bool → 文本
// 映射。赋值文本强制非空，与"读回为空即未命中"的回填闭环配套——空串不可能是任何
// 一项的合法赋值，故两种情形无需区分。
void validateTwoStateCheckBox(const SettingItem &item, int ci, int ii) {
    const auto choices = item.choices ? item.choices() : QList<SettingChoice>{};
    if (item.mpvType != MpvType::String)
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] must declare "
                              "MpvType::String")
                   .arg(ci)
                   .arg(ii);
    if (item.mpvKeys.size() != 1)
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] must govern "
                              "exactly one key")
                   .arg(ci)
                   .arg(ii);
    if (item.fallback.typeId() != QMetaType::Bool)
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] fallback must be "
                              "bool")
                   .arg(ci)
                   .arg(ii);
    if (choices.size() != 2) {
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] needs exactly two "
                              "choices, got %3")
                   .arg(ci)
                   .arg(ii)
                   .arg(choices.size());
        return; // 后续检查依赖"恰两项"
    }
    const bool v0 = choices[0].value.toBool();
    const bool v1 = choices[1].value.toBool();
    if (choices[0].value.typeId() != QMetaType::Bool ||
        choices[1].value.typeId() != QMetaType::Bool || v0 == v1)
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] choices must carry "
                              "one true and one false bool value")
                   .arg(ci)
                   .arg(ii);
    QString texts[2];
    for (int k = 0; k < 2; ++k) {
        const QMap<QString, QString> &a = choices[k].mpvAssignments;
        const bool shapeOk =
            a.size() == 1 && item.mpvKeys.size() == 1 && a.firstKey() == item.mpvKeys[0];
        if (!shapeOk) {
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] choice %3 must "
                                  "assign the single governed key")
                       .arg(ci)
                       .arg(ii)
                       .arg(k);
            continue;
        }
        texts[k] = a.first();
        if (texts[k].isEmpty())
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] choice %3 "
                                  "assigns empty text")
                       .arg(ci)
                       .arg(ii)
                       .arg(k);
    }
    if (!texts[0].isEmpty() && texts[0] == texts[1])
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("SettingsDialog: two-state CheckBox item [%1/%2] choices must assign "
                              "distinct texts")
                   .arg(ci)
                   .arg(ii);
}

// 目录自检（构造期一次，只记警告、不中断；R4 §4.3.6）
void validateCatalog() {
    const auto &catalog = settingCatalog();
    QSet<QString> ids;
    int ci = 0;
    for (const auto &cat : catalog) {
        int ii = 0;
        for (const auto &item : cat.items) {
            const bool hasIni = !item.iniKey.isEmpty();
            const bool hasMpv = !item.mpvKeys.isEmpty();
            if (hasIni == hasMpv)
                OXBOW_LOG_WARNING().noquote()
                    << QStringLiteral("SettingsDialog: catalog item [%1/%2] must have exactly one "
                                      "backend (iniKey xor mpvKeys)")
                           .arg(ci)
                           .arg(ii);
            if (item.widget == WidgetKind::Combo && !item.choices)
                OXBOW_LOG_WARNING().noquote()
                    << QStringLiteral("SettingsDialog: Combo item [%1/%2] missing choices "
                                      "provider")
                           .arg(ci)
                           .arg(ii);
            if (hasMpv) {
                // mpvType 与控件语义配套检查
                if (item.widget == WidgetKind::CheckBox) {
                    // 有 choices 走两态映射（N33 §2.1）；无 choices 仍要求 Flag
                    if (item.choices)
                        validateTwoStateCheckBox(item, ci, ii);
                    else if (item.mpvType != MpvType::Flag)
                        OXBOW_LOG_WARNING().noquote()
                            << QStringLiteral("SettingsDialog: mpv CheckBox item [%1/%2] must "
                                              "declare MpvType::Flag when it carries no choices")
                                   .arg(ci)
                                   .arg(ii);
                }
                if (item.widget == WidgetKind::SpinBox && item.mpvType != MpvType::Int &&
                    item.mpvType != MpvType::Double)
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: mpv SpinBox item [%1/%2] must declare "
                                          "MpvType::Int or Double")
                               .arg(ci)
                               .arg(ii);
                if (item.widget == WidgetKind::FilterList) {
                    // N42：滤镜列表恰管辖一个 key、String 型下发；INI 后端无意义
                    if (item.mpvKeys.size() != 1)
                        OXBOW_LOG_WARNING().noquote()
                            << QStringLiteral("SettingsDialog: FilterList item [%1/%2] must "
                                              "govern exactly one key")
                                   .arg(ci)
                                   .arg(ii);
                    if (item.mpvType != MpvType::String)
                        OXBOW_LOG_WARNING().noquote()
                            << QStringLiteral("SettingsDialog: FilterList item [%1/%2] must "
                                              "declare MpvType::String")
                                   .arg(ci)
                                   .arg(ii);
                    if (!item.iniKey.isEmpty())
                        OXBOW_LOG_WARNING().noquote()
                            << QStringLiteral("SettingsDialog: FilterList item [%1/%2] must be an "
                                              "mpv item, not INI")
                                   .arg(ci)
                                   .arg(ii);
                }
                if (item.widget == WidgetKind::Combo) {
                    // mpv 枚举项必须有且仅有一个占位项（R3 §3.7 语义依赖）
                    int placeholders = 0;
                    const auto choices = item.choices ? item.choices() : QList<SettingChoice>{};
                    for (const auto &c : choices)
                        if (c.mpvAssignments.isEmpty())
                            ++placeholders;
                    if (placeholders != 1)
                        OXBOW_LOG_WARNING().noquote()
                            << QStringLiteral("SettingsDialog: mpv enum item [%1/%2] needs exactly "
                                              "one placeholder choice, got %3")
                                   .arg(ci)
                                   .arg(ii)
                                   .arg(placeholders);
                    // 赋值按声明类型可解析（读回匹配依赖两侧同一规范形式）
                    for (const auto &c : choices)
                        for (auto it = c.mpvAssignments.cbegin(); it != c.mpvAssignments.cend();
                             ++it) {
                            bool ok = true;
                            switch (item.mpvType) {
                            case MpvType::Flag:
                                ok = it.value() == QLatin1String("yes") ||
                                     it.value() == QLatin1String("no");
                                break;
                            case MpvType::Int:
                                it.value().toLongLong(&ok);
                                break;
                            case MpvType::Double:
                                it.value().toDouble(&ok);
                                break;
                            case MpvType::String:
                                break;
                            }
                            if (!ok)
                                OXBOW_LOG_WARNING().noquote()
                                    << QStringLiteral(
                                           "SettingsDialog: mpv enum item [%1/%2] assignment "
                                           "'%3' not parseable as declared MpvType")
                                           .arg(ci)
                                           .arg(ii)
                                           .arg(it.value());
                        }
                }
            }
            // 两态映射仅属 mpv 后端：项级校验必须在 hasMpv 分支之外，
            // 否则 INI 项触达不到
            if (item.widget == WidgetKind::CheckBox && item.choices && !hasMpv)
                OXBOW_LOG_WARNING().noquote()
                    << QStringLiteral("SettingsDialog: INI CheckBox item [%1/%2] must not carry "
                                      "choices")
                           .arg(ci)
                           .arg(ii);
            // N142：外部工具路径项——必须走 INI 后端、指定工具，且 iniKey 与该工具的定义
            // 一致；工具只许出现在该控件上
            if (item.widget == WidgetKind::ToolPath) {
                if (!hasIni || hasMpv)
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: ToolPath item [%1/%2] must be an INI "
                                          "item")
                               .arg(ci)
                               .arg(ii);
                if (!item.tool)
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: ToolPath item [%1/%2] names no tool")
                               .arg(ci)
                               .arg(ii);
                else if (const ExternalToolSpec &spec = externalToolSpec(*item.tool);
                         item.iniKey != QLatin1String(spec.iniKey))
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: ToolPath item [%1/%2] iniKey '%3' "
                                          "does not match tool definition '%4'")
                               .arg(ci)
                               .arg(ii)
                               .arg(item.iniKey, QString::fromLatin1(spec.iniKey));
            } else if (item.tool) {
                OXBOW_LOG_WARNING().noquote()
                    << QStringLiteral("SettingsDialog: item [%1/%2] names a tool but is not a "
                                      "ToolPath item")
                           .arg(ci)
                           .arg(ii);
            }
            if (item.widget == WidgetKind::SpinBox) {
                const int f = item.fallback.toInt();
                if (f < item.range.min || f > item.range.max)
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: SpinBox fallback out of range [%1/%2]")
                               .arg(ci)
                               .arg(ii);
            }
            // Combo 的 fallback 必须命中 choices 的 data：ini 项命中任意选项即可，mpv 项还须
            // 命中非占位选项——fallback 兼默认回退与写盘期删除判定的比较源，落占位则默认串
            // 为空 map，等默认删键永不触发
            if (item.widget == WidgetKind::Combo && item.choices) {
                bool hitAny = false;
                bool hitAssigned = false;
                for (const auto &c : item.choices()) {
                    if (c.value != item.fallback)
                        continue;
                    hitAny = true;
                    if (!c.mpvAssignments.isEmpty())
                        hitAssigned = true;
                }
                if (!hitAny || (hasMpv && !hitAssigned))
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: Combo fallback [%1/%2] does not match "
                                          "%3 data")
                               .arg(ci)
                               .arg(ii)
                               .arg(hasMpv ? QStringLiteral("an assigned choice")
                                           : QStringLiteral("any choice"));
            }
            if (!item.id.isEmpty()) {
                if (item.widget != WidgetKind::CheckBox)
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: id '%1' on item [%2/%3] must be a "
                                          "CheckBox master")
                               .arg(item.id)
                               .arg(ci)
                               .arg(ii);
                if (ids.contains(item.id))
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: duplicate master id '%1' on item "
                                          "[%2/%3]")
                               .arg(item.id)
                               .arg(ci)
                               .arg(ii);
                ids.insert(item.id);
            }
            ++ii;
        }
        ++ci;
    }
    // dependsOn 解析检查放第二遍（允许引用目录中后定义的主控项）
    int cj = 0;
    for (const auto &cat : catalog) {
        int jj = 0;
        for (const auto &item : cat.items) {
            if (!item.dependsOn.isEmpty()) {
                if (item.widget == WidgetKind::CheckBox)
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: CheckBox item [%1/%2] must not depend "
                                          "on others (single-level dependency)")
                               .arg(cj)
                               .arg(jj);
                if (!ids.contains(item.dependsOn))
                    OXBOW_LOG_WARNING().noquote()
                        << QStringLiteral("SettingsDialog: item [%1/%2] dependsOn '%3' unresolved")
                               .arg(cj)
                               .arg(jj)
                               .arg(item.dependsOn);
            }
            ++jj;
        }
        ++cj;
    }
}

} // namespace

// ===================== 分类列表条目文本（N136 §2.2） =====================
// 条目文本 = 分类标题（tr 后），未提交时在末尾追加「空格 + ●」后缀。宽度预算与
// refreshDerivedState 共用此拼接，保证预算串与实际显示串同源。仅本翻译单元使用
namespace {

QString categoryDisplayText(const QString &title, bool marked) {
    return marked ? title + QStringLiteral(" \u25CF") : title;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent, MpvController *controller,
                               const QString &pluginIniPath)
    : QDialog(parent), m_controller(controller), m_pluginIniPath(pluginIniPath) {
    // mpv.conf 路径：一切位置问 mpv 自己（N92：expand-path ~~home/ 取目录再拼文件名，
    // 见 MpvController::configDir），插件不再自行推导。取不到有效路径（空串）就根本不
    // open——m_confCache 保持无值，mpv 侧各项照常可改照常下发（写点本就有
    // if (m_confCache) 守卫），只是不落盘；回填读的是实时属性不经 conf 缓存，初值不受
    // 影响。绝不把空串喂给 open：那会读成空配置，Apply 时可能拿一份空配置去覆盖真配置
    m_mpvConfPath = m_controller->mpvConfPath();
    if (!m_mpvConfPath.isEmpty())
        m_confCache = MpvConf::open(m_mpvConfPath);

    setWindowTitle(tr("Settings"));
    resize(760, 520); // 固定初始尺寸；页内容超高走纵向滚动，拉大由用户手动
    buildUi();
    applyToControls();

    // 快照按持久值重建（mpv 取 conf 保存值；ini / 禁用取回填值）：打开即存在分歧
    // （菜单临时启停造成 conf 与运行期不合）时，由分歧本身点亮 Apply
    rebuildSnapshot();
    refreshDerivedState();

    // 按钮接线（Apply / OK / Cancel）
    connect(m_buttonBox->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this] { applyAll(); });
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, [this] {
        applyAll();
        accept();
    });
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::buildUi() {
    validateCatalog();

    auto *root = new QHBoxLayout(this);

    // 左侧分类列表：宽度一次预留，构造末尾按字体度量把宽度钉死（见下方注释）
    m_categoryList = new QListWidget(this);
    m_categoryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_categoryList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_categoryList->setIconSize(QSize(0, 0)); // 无图标
    m_categoryList->setSpacing(2);
    root->addWidget(m_categoryList);

    // 右侧容器（堆栈 + 底部按钮）
    auto *rightPane = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    m_pageStack = new QStackedWidget(rightPane);
    // 页内容进纵向滚动区（N146）：AI 页条目多、含多行编辑框，最满一页的最小高度
    // 不再反灌对话框——内容高于视口即滚动，初始 760x520，用户可拉大
    auto *pageScroll = new QScrollArea(rightPane);
    pageScroll->setWidget(m_pageStack);
    pageScroll->setWidgetResizable(true);
    pageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pageScroll->setFrameShape(QFrame::NoFrame);

    // 遍历目录动态生成页面：每分类一页、每设置项一组"标签 + 控件（+ 备注行）"。
    // 文案在构建控件此刻经 tr() 翻译（上下文即 SettingsDialog，既有 .ts 不失效）。
    // 各控件以 lambda 捕获自己的目录项指针接入 onControlChanged（static 目录
    // 生命周期内指针稳定，无需在控件上挂 id 属性做反查）
    // 目录文本字段（label/note/title）为 const char*：直接 tr()；
    // trS 仅供选项 label（QString，运行期构造）使用
    auto trS = [this](const QString &s) { return tr(s.toUtf8().constData()); };

    // 行首槽度量：方形按钮边长按样式小图标尺寸推导（不写死像素），槽宽恰为按钮宽，
    // 与内容之间的间距归各格外层布局的样式间距（标签行 / 整行 / 备注行同构，自动同列
    // 对齐）。全页一次预留：干净行放同宽空槽，按钮出现 / 消失零位移
    const int resetSide = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this) + 8;
    const int resetGutter = resetSide;

    for (const auto &cat : settingCatalog()) {
        m_categoryList->addItem(tr(cat.title));

        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        // 左边距收窄：行首槽贴分类面板，与右侧内容边距观感对称
        layout->setContentsMargins(4, 12, 12, 12);
        layout->setSpacing(8);

        // 表单布局：标签列宽度自动对齐到本页最宽标签；控件占标签右侧剩余宽度
        auto *form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(8);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        for (const auto &itemRef : cat.items) {
            const SettingItem *item = &itemRef;

            QWidget *control = nullptr;
            switch (item->widget) {
            case WidgetKind::Combo: {
                auto *combo = new QComboBox(page);
                const auto choices = item->choices ? item->choices() : QList<SettingChoice>{};
                for (const auto &c : choices)
                    combo->addItem(trS(c.label), c.value);
                connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                        [this, item] { onControlChanged(item); });
                control = combo;
                break;
            }
            case WidgetKind::EditableCombo: {
                // 可编辑下拉：逻辑值即文本；预设仅作快选（不翻译，后端标识符）
                auto *combo = new QComboBox(page);
                combo->setEditable(true);
                const auto choices = item->choices ? item->choices() : QList<SettingChoice>{};
                for (const auto &c : choices)
                    combo->addItem(c.label);
                connect(combo, &QComboBox::currentTextChanged, this,
                        [this, item] { onControlChanged(item); });
                control = combo;
                break;
            }
            case WidgetKind::SpinBox: {
                auto *spin = new QSpinBox(page);
                spin->setRange(item->range.min, item->range.max);
                connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                        [this, item] { onControlChanged(item); });
                control = spin;
                break;
            }
            case WidgetKind::CheckBox: {
                // 标签并入复选框，不再单出 QLabel
                auto *box = new QCheckBox(tr(item->label), page);
                connect(box, &QCheckBox::toggled, this, [this, item] { onControlChanged(item); });
                control = box;
                if (!item->id.isEmpty())
                    m_masters.insert(item->id, box);
                break;
            }
            case WidgetKind::LineEdit: {
                // 纯编辑框：逻辑值即文本；N146 起支持密码回显与占位串
                auto *edit = new QLineEdit(page);
                if (item->secret)
                    edit->setEchoMode(QLineEdit::Password);
                if (item->placeholder && *item->placeholder)
                    edit->setPlaceholderText(tr(item->placeholder));
                connect(edit, &QLineEdit::textChanged, this,
                        [this, item] { onControlChanged(item); });
                control = edit;
                break;
            }
            case WidgetKind::MultiLine: {
                // N146：多行文本（QPlainTextEdit）；逻辑值即整段文本，换行往返交给
                // QSettings 的 ini 转义。高度钉 5 行（提示词模板不长）：页高不再被
                // 默认 sizeHint 撑起
                auto *edit = new QPlainTextEdit(page);
                edit->setFixedHeight(edit->fontMetrics().lineSpacing() * 5 + 12);
                connect(edit, &QPlainTextEdit::textChanged, this,
                        [this, item] { onControlChanged(item); });
                control = edit;
                break;
            }
            case WidgetKind::FilterList: {
                // N42：滤镜列表编辑器，分组框为根——标题即目录项 label，不再单出
                // QLabel；预设数据源取自目录项
                auto *fl = new FilterListWidget(
                    tr(item->label),
                    item->filterPresets ? item->filterPresets() : QList<FilterPreset>{}, page);
                connect(fl, &FilterListWidget::changed, this,
                        [this, item] { onControlChanged(item); });
                control = fl;
                break;
            }
            case WidgetKind::ToolPath: {
                // N142：路径选择控件；候选发现要 ini 所在目录——与启动侧取配置文件
                // 父目录同一语义
                if (item->tool) {
                    auto *tool =
                        new ExternalToolPathCombo(externalToolSpec(*item->tool),
                                                  QFileInfo(m_pluginIniPath).absolutePath(), page);
                    connect(tool, &ExternalToolPathCombo::changed, this,
                            [this, item] { onControlChanged(item); });
                    control = tool;
                }
                break;
            }
            }
            // 目录没指定工具（validateCatalog 已记警告）：整行不建——控件、候选发现与
            // 过滤器都拿的是工具定义，没有定义就没有这一行
            if (!control)
                continue;

            m_controls.insert(item, control);

            // 行首槽（resetCell）：所有行统一让出重置按钮位，保证各页按钮同列对齐；
            // 干净行按钮隐藏、槽恒占位（空 QWidget，不进 Tab 链）
            auto *resetCell = new QWidget(page);
            // 宽高都钉死：宽保按钮出现零位移，高保 label 行不因按钮显隐而变高
            // （按钮比标签高，显隐会让该行标签垂直居中位上下漂移）
            resetCell->setFixedWidth(resetGutter);
            resetCell->setFixedHeight(resetSide);
            auto *resetLayout = new QHBoxLayout(resetCell);
            resetLayout->setContentsMargins(0, 0, 0, 0);
            resetLayout->setSpacing(0); // 槽宽恰为按钮宽，格内不再另加间距（间距归外层）
            auto *resetBtn = new QToolButton(resetCell);
            resetBtn->setAutoRaise(true);
            resetBtn->setFixedSize(resetSide, resetSide);
            resetBtn->setIconSize(QSize(resetSide - 8, resetSide - 8));
            resetBtn->setFocusPolicy(Qt::TabFocus); // 隐藏时 Qt 自动跳过，不占停靠点
            resetBtn->setToolTip(tr("Reset this setting"));
            resetBtn->setAccessibleName(tr("Reset this setting"));
            resetBtn->hide();
            resetLayout->addWidget(resetBtn, 0,
                                   item->widget == WidgetKind::FilterList ? Qt::AlignTop
                                                                          : Qt::AlignVCenter);
            resetLayout->addStretch(1);
            m_resetButtons.insert(item, resetBtn);
            connect(resetBtn, &QToolButton::clicked, this,
                    [this, item, resetBtn] { showResetMenu(item, resetBtn); });

            if (item->widget != WidgetKind::CheckBox && item->widget != WidgetKind::FilterList) {
                // 标签行：[行首槽][QLabel][stretch] 作标签列，控件列照旧——槽宽固定，
                // 各页标签仍经 QFormLayout 对齐到最宽标签
                auto *labelCell = new QWidget(page);
                auto *cellLayout = new QHBoxLayout(labelCell);
                cellLayout->setContentsMargins(0, 0, 0, 0);
                cellLayout->addWidget(resetCell);
                cellLayout->addWidget(new QLabel(tr(item->label)), 0, Qt::AlignVCenter);
                cellLayout->addStretch(1);
                form->addRow(labelCell, control);
            } else {
                // 标签并入复选框 / 分组框标题：整行容器 [行首槽][控件]；FilterList
                // 按钮顶对齐分组框标题行（resetCell 内已按项设对齐）
                auto *row = new QWidget(page);
                auto *rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->addWidget(resetCell, 0, Qt::AlignTop);
                rowLayout->addWidget(control, 1);
                form->addRow(row);
            }

            // 可选备注行：灰色小字（取 Disabled 文本色，随主题自适应），空则不占位；
            // 单参数 addRow 跨两列展示。必须开 wordWrap：QLabel 不换行时
            // minimumSizeHint ≈ 整行文本宽度，长备注会把对话框最小宽度撑爆
            // （N42 长备注实测撑到无法拖拽缩小）
            if (item->note && *item->note) { // 非空指针且有内容才显示
                auto *note = new QLabel(tr(item->note));
                note->setWordWrap(true);
                // N115：灰字走禁用态，随主题切换即时跟随（不再 pin 快照调色板）
                note->setEnabled(false);
                QFont nf = note->font();
                nf.setPointSize(qMax(nf.pointSize() - 1, 1));
                note->setFont(nf);
                // 备注行按行首槽宽缩进
                auto *noteRow = new QWidget(page);
                auto *noteLayout = new QHBoxLayout(noteRow);
                noteLayout->setContentsMargins(0, 0, 0, 0);
                auto *noteSpacer = new QWidget(noteRow);
                noteSpacer->setFixedWidth(resetGutter);
                noteLayout->addWidget(noteSpacer);
                noteLayout->addWidget(note, 1);
                form->addRow(noteRow);
            }

            m_pending.insert(item, {}); // 先占位，回填时覆写
        }
        layout->addLayout(form);
        layout->addStretch(1);
        m_pageStack->addWidget(page);
    }
    rightLayout->addWidget(pageScroll, 1);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, rightPane);
    rightLayout->addWidget(m_buttonBox);
    root->addWidget(rightPane, 1);

    // 切换分类同步堆栈
    connect(m_categoryList, &QListWidget::currentRowChanged, m_pageStack,
            &QStackedWidget::setCurrentIndex);
    m_categoryList->setCurrentRow(0);

    // 分类列表宽度一次预算：字体直接量「最长标题 + 后缀」的 advance，不经样式的
    // sizeFromContents（它报的尺寸与样式自绘的文本落点不同源，windows11 选中边饰的
    // 额外缩进恰好吃光后缀宽度，最长条目会溢出成 "…"）；余量写死 48，覆盖常见样式
    // 的文本缩进（含选中态额外缩进）。标记恒按后缀预留，出现 / 消失零宽度抖动。
    // 不设上限（上限随预算制有意取消，译名较长时列表可比现状宽）
    int maxTextWidth = 0;
    for (int i = 0; i < m_categoryList->count(); ++i)
        maxTextWidth = qMax(maxTextWidth, QFontMetrics(m_categoryList->font())
                                              .horizontalAdvance(categoryDisplayText(
                                                  m_categoryList->item(i)->text(), true)));
    const int frame = 2 * m_categoryList->frameWidth();
    m_categoryList->setFixedWidth(maxTextWidth + 48 + frame);

    retintResetIcons();
}

void SettingsDialog::fillItemFromBackend(const SettingItem *item) {
    QWidget *w = m_controls.value(item);
    if (!item || !w)
        return;

    if (!item->iniKey.isEmpty()) {
        // === 插件本体选项：从 INI 读 ===
        QSettings ini(m_pluginIniPath, QSettings::IniFormat);
        if (item->widget == WidgetKind::CheckBox) {
            const bool v = ini.value(item->iniKey, item->fallback).toBool();
            static_cast<QCheckBox *>(w)->setChecked(v);
            m_pending[item] = v;
        } else if (item->widget == WidgetKind::Combo) {
            auto *combo = static_cast<QComboBox *>(w);
            QVariant raw = ini.value(item->iniKey);
            if (!raw.isValid())
                raw = item->fallback;
            // INI 读回可能是字符串，对齐 fallback 类型后再匹配
            const QVariant v = (item->fallback.typeId() == QMetaType::Int)
                                   ? QVariant(raw.toInt())
                                   : QVariant(raw.toString().trimmed());
            int idx = combo->findData(v);
            if (idx < 0)
                raw = item->fallback; // 值不合法 → 取兜底再匹配
            if (idx < 0)
                idx = combo->findData(raw);
            combo->setCurrentIndex(idx >= 0 ? idx : 0); // 仍 miss → 第 0 项
            m_pending[item] = combo->currentData();
        } else if (item->widget == WidgetKind::SpinBox) {
            // 整数项：缺失或不可解析均落 fallback；setValue 自动钳制 [min,max]，
            // 钳制只作用于控件与内存，INI 中的越界旧值不回写
            auto *spin = static_cast<QSpinBox *>(w);
            bool ok = false;
            const int v = ini.value(item->iniKey, item->fallback).toInt(&ok);
            spin->setValue(ok ? v : item->fallback.toInt());
            m_pending[item] = spin->value();
        } else {
            // 其余形态（LineEdit / EditableCombo / MultiLine / ToolPath / FilterList）
            // 值即串，走与写值同一条通用路径：新形态只要 writeControlValue /
            // readControlValue 认得就自动进回填，不会因漏补一支而把 pending 停在构造
            // 期占位——那种漏补会让脏判定永远回不到净。CheckBox / Combo / SpinBox
            // 有类型对齐与索引回落的额外动作，留在上面
            writeControlValue(item, ini.value(item->iniKey, item->fallback));
            m_pending[item] = readControlValue(w);
        }
    } else if (!item->mpvKeys.isEmpty()) {
        // === mpv 选项（铁律）：仅从 MpvController 取，不读 mpv.conf ===
        switch (item->widget) {
        case WidgetKind::Combo: {
            // 枚举项：一次性读出全部管辖 key（按 mpvType 规范化），找与 authored
            // 赋值（同样过规范化）全 key 相等的选项——数值属性永不信任 mpv 字符串化
            auto *combo = static_cast<QComboBox *>(w);
            const auto choices = item->choices ? item->choices() : QList<SettingChoice>{};
            QList<QMap<QString, QString>> canon;
            canon.reserve(choices.size());
            for (const auto &c : choices) {
                QMap<QString, QString> a;
                for (auto it = c.mpvAssignments.cbegin(); it != c.mpvAssignments.cend(); ++it)
                    a.insert(it.key(), canonicalAuthoredMpvValue(item->mpvType, it.value()));
                canon.append(a);
            }
            QMap<QString, QString> current;
            bool allReadable = true;
            for (const QString &k : item->mpvKeys) {
                QString v;
                if (!readCanonicalMpvValue(m_controller, k, item->mpvType, v)) {
                    allReadable = false;
                    break;
                }
                current.insert(k, v);
            }
            int hit = -1;
            if (allReadable) {
                for (int i = 0; i < choices.size(); ++i) {
                    if (canon[i].isEmpty())
                        continue; // 占位不参与匹配
                    if (canon[i] == current) {
                        hit = i;
                        break;
                    }
                }
            }
            if (hit < 0) {
                // 任意组合不认识 / 属性不可读（用户手改 conf 等）→ 落首个占位项
                for (int i = 0; i < choices.size(); ++i) {
                    if (choices[i].mpvAssignments.isEmpty()) {
                        hit = i;
                        break;
                    }
                }
            }
            if (hit >= 0)
                combo->setCurrentIndex(hit);
            m_pending[item] = combo->currentData();
            break;
        }
        case WidgetKind::EditableCombo: {
            // 标量项：直接显示实际值（按 mpvType 规范化），不做预设匹配、无第 0 项兜底
            auto *combo = static_cast<QComboBox *>(w);
            QString v;
            if (!readCanonicalMpvValue(m_controller, item->mpvKeys[0], item->mpvType, v) ||
                v.isEmpty())
                v = item->fallback.toString();
            combo->setCurrentText(v);
            m_pending[item] = combo->currentText();
            break;
        }
        case WidgetKind::LineEdit: {
            // 纯编辑框：同 EditableCombo 的标量语义，显示实际值
            auto *edit = static_cast<QLineEdit *>(w);
            QString v;
            if (!readCanonicalMpvValue(m_controller, item->mpvKeys[0], item->mpvType, v) ||
                v.isEmpty())
                v = item->fallback.toString();
            edit->setText(v);
            m_pending[item] = edit->text();
            break;
        }
        case WidgetKind::SpinBox: {
            // mpvType 必为 Int/Double（目录自检保证）：规范串必为可解析数值，
            // 不再有 propertyDouble(-1.0) 哨兵值特判
            auto *spin = static_cast<QSpinBox *>(w);
            QString v;
            int n = item->fallback.toInt();
            if (readCanonicalMpvValue(m_controller, item->mpvKeys[0], item->mpvType, v))
                n = qRound(v.toDouble());
            spin->setValue(n); // 自动钳制 [min,max]
            m_pending[item] = spin->value();
            break;
        }
        case WidgetKind::CheckBox: {
            bool v = item->fallback.toBool();
            if (item->choices) {
                // 两态映射（N33 §2.1）：mpvType 必为 String，读回文本与两项赋值逐一
                // 精确比对，命中取该选项的逻辑值；读回为空或均不相等落 fallback
                QString raw;
                readCanonicalMpvValue(m_controller, item->mpvKeys[0], item->mpvType, raw);
                const auto choices = item->choices();
                for (const auto &c : choices) {
                    if (!raw.isEmpty() && c.mpvAssignments.value(item->mpvKeys[0]) == raw) {
                        v = c.value.toBool();
                        break;
                    }
                }
            } else {
                // mpvType 必为 Flag（目录自检保证）：读回规范化即 "yes"/"no"
                QString raw;
                if (readCanonicalMpvValue(m_controller, item->mpvKeys[0], item->mpvType, raw))
                    v = (raw == QLatin1String("yes"));
            }
            static_cast<QCheckBox *>(w)->setChecked(v);
            m_pending[item] = v;
            break;
        }
        case WidgetKind::FilterList: {
            // N42：node 结构化读（绕开转义串），映射为条目列表整体重建；pending
            // 存编译串（与 Apply 下发串同源），快照比对与 dirty 判定按串进行
            auto *fl = static_cast<FilterListWidget *>(w);
            fl->setEntries(m_controller->filterEntries(item->mpvKeys[0]));
            m_pending[item] = fl->compiledString();
            break;
        }
        case WidgetKind::ToolPath:
            break; // 仅 INI 后端（validateCatalog 保证），mpv 读回不触达
        case WidgetKind::MultiLine:
            break; // 仅 INI 后端（N146），mpv 读回不触达
        }
    }
    m_fillValue[item] = m_pending[item];
}

void SettingsDialog::applyFallback(const SettingItem *item) {
    QWidget *w = m_controls.value(item);
    if (!item || !w)
        return;
    w->setEnabled(false);
    // 落兜底与写值同一条路径：Combo 的索引回落、文本标量一族、ToolPath 的自动项
    // 都与 writeControlValue 逐字相同，不再各抄一份 switch（新增控件形态从此只补
    // writeControlValue / readControlValue 两处）。滤镜项是唯一真特例：禁用落空列表
    // = 无滤镜（空值语义与 mpv 属性空值同口径），不解析 fallback 串
    if (item->widget == WidgetKind::FilterList)
        static_cast<FilterListWidget *>(w)->setEntries({});
    else
        writeControlValue(item, item->fallback);
    m_pending[item] = readControlValue(w);
    // 禁用不变式：三值同落 fallback，dirty 与按钮可见性判据对禁用项恒为净
    m_fillValue[item] = m_pending[item];
    m_appliedSnapshot[item] = m_pending[item];
}

void SettingsDialog::applyToControls() {
    // 回填必须包裹在 m_blockApply 守卫内，防止回填过程触发 valueChanged
    // 污染 m_pending
    m_blockApply = true;

    // 第一遍：无依赖项（含联动主控）从后端正常回填
    for (const auto &cat : settingCatalog())
        for (const auto &item : cat.items)
            if (item.dependsOn.isEmpty())
                fillItemFromBackend(&item);

    // 第二遍：依赖项按主控状态分流——选中 → 启用并回填；未选中 → 禁用落
    // fallback（禁用时不回填，不读后端）
    for (const auto &cat : settingCatalog())
        for (const auto &item : cat.items) {
            if (item.dependsOn.isEmpty())
                continue;
            auto *masterCb = m_masters.contains(item.dependsOn)
                                 ? qobject_cast<QCheckBox *>(m_masters.value(item.dependsOn))
                                 : nullptr;
            if (masterCb && masterCb->isChecked()) {
                m_controls.value(&item)->setEnabled(true);
                fillItemFromBackend(&item);
            } else {
                applyFallback(&item);
            }
        }

    m_blockApply = false;
}

void SettingsDialog::refreshDependents(const QString &masterId) {
    // 刷新过程会改依赖项控件值并触发其信号，用 blockApply 短路防递归
    m_blockApply = true;
    auto *masterCb = m_masters.contains(masterId)
                         ? qobject_cast<QCheckBox *>(m_masters.value(masterId))
                         : nullptr;
    const bool masterOn = masterCb && masterCb->isChecked();
    for (const auto &cat : settingCatalog())
        for (const auto &item : cat.items) {
            if (item.dependsOn != masterId)
                continue;
            if (masterOn) {
                // 启用并重新回填：显示后端真实值，而非陈旧 fallback
                m_controls.value(&item)->setEnabled(true);
                fillItemFromBackend(&item);
            } else {
                // 禁用 + 复位 fallback + 丢弃该项未应用的修改；
                // 由此维持不变式"禁用项 pending == fallback == 快照 == fill"
                applyFallback(&item);
            }
        }
    // 联动改写了依赖项的 fill / 快照，整表重建一次（兜底取 fill，不吸收未提交编辑）
    rebuildSnapshot();
    m_blockApply = false;
}

QVariant SettingsDialog::readControlValue(QWidget *control) const {
    if (auto *tool = qobject_cast<ExternalToolPathCombo *>(control))
        return tool->value();
    if (auto *combo = qobject_cast<QComboBox *>(control)) {
        // 可编辑下拉：值取文本语义（用户可能输入自定义值）
        if (combo->isEditable())
            return combo->currentText();
        // 不可编辑下拉：兜底 data 无效时取 currentText（addItems 不设 data 的情况）
        const QVariant d = combo->currentData();
        return d.isValid() ? d : combo->currentText();
    }
    if (auto *spin = qobject_cast<QSpinBox *>(control))
        return spin->value();
    if (auto *edit = qobject_cast<QLineEdit *>(control))
        return edit->text();
    if (auto *multi = qobject_cast<QPlainTextEdit *>(control))
        return multi->toPlainText();
    if (auto *box = qobject_cast<QCheckBox *>(control))
        return box->isChecked();
    if (auto *fl = qobject_cast<FilterListWidget *>(control))
        return fl->compiledString(); // N42：逻辑值即编译串（与下发 / conf 写同源）
    return {};
}

void SettingsDialog::onControlChanged(const SettingItem *item) {
    if (m_blockApply || !item)
        return;
    QWidget *w = m_controls.value(item);
    if (!w)
        return;
    m_pending[item] = readControlValue(w);
    // 联动：主控 CheckBox 切换 → 刷新依赖项可用性与取值
    if (item->widget == WidgetKind::CheckBox && !item->id.isEmpty())
        refreshDependents(item->id);
    refreshDerivedState();
}

void SettingsDialog::refreshDerivedState() {
    // 三者不全等（pending != 快照 || fill != 快照）任一项成立即未提交；Apply 启用、
    // 行内重置按钮可见性、分类标记与 tooltip 共用这一次重算
    bool anyUncommitted = false;
    int categoryIndex = 0;
    for (const auto &cat : settingCatalog()) {
        bool categoryMarked = false;
        for (const auto &item : cat.items) {
            QWidget *w = m_controls.value(&item);
            const QVariant snap = m_appliedSnapshot.value(&item);
            const bool uncommitted =
                m_pending.value(&item) != snap || m_fillValue.value(&item) != snap;
            if (uncommitted) {
                anyUncommitted = true;
                categoryMarked = true;
            }
            // 按钮只在未提交且控件启用时进 Tab 链；禁用依赖项永不显示
            if (auto *btn = m_resetButtons.value(&item))
                btn->setVisible(uncommitted && w && w->isEnabled());
        }
        if (QListWidgetItem *catItem = m_categoryList->item(categoryIndex)) {
            // 文本整体由目录标题现拼，不从旧文本追加（本函数一轮内被多次调用）
            catItem->setText(categoryDisplayText(tr(cat.title), categoryMarked));
            catItem->setToolTip(categoryMarked ? tr("This category has uncommitted changes")
                                               : QString());
        }
        ++categoryIndex;
    }
    m_dirty = anyUncommitted;
    if (m_buttonBox && m_buttonBox->button(QDialogButtonBox::Apply))
        m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(m_dirty);
}

void SettingsDialog::applyResetChoice(const SettingItem *item, const QVariant &target) {
    QWidget *w = m_controls.value(item);
    if (!w)
        return;
    // 只改控件值：setEntries 不发 changed，m_blockApply 仍作双保险
    m_blockApply = true;
    writeControlValue(item, target);
    m_pending[item] = readControlValue(w);
    m_blockApply = false;
    refreshDerivedState();
}

void SettingsDialog::showResetMenu(const SettingItem *item, QToolButton *button) {
    QMenu menu(this);
    QAction *resetAction = menu.addAction(tr("Reset to Default"));
    QAction *discardAction = menu.addAction(tr("Discard Changes"));
    connect(resetAction, &QAction::triggered, this,
            [this, item] { applyResetChoice(item, defaultControlValue(item)); });
    connect(discardAction, &QAction::triggered, this, [this, item] {
        applyResetChoice(item, m_appliedSnapshot.value(item)); // 控件 ← 快照（2.8）
    });
    menu.exec(button->mapToGlobal(QPoint(0, button->height())));
}

void SettingsDialog::retintResetIcons() {
    const QColor tint = palette().color(QPalette::WindowText);
    const QIcon icon = Ui::themedIcon(App::iconPath(App::iconReset()), tint);
    for (auto *btn : m_resetButtons)
        btn->setIcon(icon);
}

void SettingsDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    // 主题 / 样式切换当场重染（全仓无运行期改字体处，分类列表宽度预算不重算）
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        retintResetIcons();
}

std::optional<QVariant> SettingsDialog::savedControlValue(const SettingItem *item) const {
    if (item->mpvKeys.isEmpty() || !m_confCache)
        return std::nullopt; // ini 项 / conf 不可用 → 无保存值
    for (const QString &k : item->mpvKeys) {
        if (!m_confCache->globalKeys().contains(k))
            return std::nullopt; // 任一管辖 key 缺失 → 无保存值（复合项缺一同样处理）
    }
    QMap<QString, QString> authored;
    for (const QString &k : item->mpvKeys)
        authored.insert(k, m_confCache->getGlobal(k));
    if (auto v = controlValueFromAuthoredKeys(item, authored))
        return v;
    return fallbackControlValue(item); // 两态未命中 / SpinBox 不可解析 → 目录兜底
}

QVariant SettingsDialog::fallbackControlValue(const SettingItem *item) const {
    QWidget *w = m_controls.value(item);
    switch (item->widget) {
    case WidgetKind::Combo: {
        // findData(fallback)，miss 落第 0 项 data——与 applyFallback 同型
        auto *combo = static_cast<QComboBox *>(w);
        const int idx = combo->findData(item->fallback);
        const int hit = idx >= 0 ? idx : 0;
        const QVariant d = combo->itemData(hit);
        return d.isValid() ? d : QVariant(combo->itemText(hit));
    }
    case WidgetKind::EditableCombo:
    case WidgetKind::MultiLine:
    case WidgetKind::LineEdit:
        return item->fallback.toString();
    case WidgetKind::SpinBox:
        return qBound(item->range.min, item->fallback.toInt(), item->range.max);
    case WidgetKind::CheckBox:
        return item->fallback.toBool();
    case WidgetKind::FilterList:
        return QStringLiteral(""); // 空串 = 无滤镜
    case WidgetKind::ToolPath:
        return item->fallback.toString(); // 空 = 自动
    }
    return {};
}

QVariant SettingsDialog::defaultControlValue(const SettingItem *item) {
    if (auto it = m_defaultCache.constFind(item); it != m_defaultCache.cend())
        return it.value();
    QVariant result;
    if (!item->iniKey.isEmpty()) {
        result = fallbackControlValue(item); // ini 默认 = 目录 fallback
        m_defaultAssignments[item] = {};
    } else {
        QMap<QString, QString> defaultsRaw;
        bool allOk = true;
        for (const QString &k : item->mpvKeys) {
            if (auto v = m_controller->optionDefaultValue(k))
                defaultsRaw.insert(k, *v);
            else {
                allOk = false; // 任一 key 不可用 → 整项回落目录 fallback
                break;
            }
        }
        // Flag 默认串形态非 yes/no 时与读回侧（propertyFlag 规范化）永不相等，写盘期
        // 删键会静默失效：按不可用处理，整项回落目录 fallback
        if (allOk && item->mpvType == MpvType::Flag) {
            for (const QString &v : std::as_const(defaultsRaw)) {
                if (v == QLatin1String("yes") || v == QLatin1String("no"))
                    continue;
                OXBOW_LOG_WARNING().noquote()
                    << QStringLiteral("SettingsDialog: option-info default '%1' for [%2] is not "
                                      "yes/no; falling back to catalog default")
                           .arg(v, item->mpvKeys.join(QStringLiteral(", ")));
                allOk = false;
                break;
            }
        }
        if (auto mapped = allOk ? controlValueFromAuthoredKeys(item, defaultsRaw) : std::nullopt) {
            result = *mapped;
            QMap<QString, QString> canon;
            for (auto a = defaultsRaw.cbegin(); a != defaultsRaw.cend(); ++a)
                canon.insert(a.key(), canonicalAuthoredMpvValue(item->mpvType, a.value()));
            m_defaultAssignments[item] = canon;
        } else {
            // option-info 不可用或映射不了 → 回落 fallback；比较源取 fallback 命中
            // 选项的逐 key 赋值（写盘期删除判定共用）
            result = fallbackControlValue(item);
            m_defaultAssignments[item] = mpvAssignmentsForValue(item, result);
        }
    }
    m_defaultCache.insert(item, result);
    return result;
}

QMap<QString, QString> SettingsDialog::mpvAssignmentsForValue(const SettingItem *item,
                                                              const QVariant &value) const {
    if (item->widget == WidgetKind::Combo) {
        const auto choices = item->choices ? item->choices() : QList<SettingChoice>{};
        for (const auto &c : choices) {
            if (c.value == value)
                return c.mpvAssignments; // 占位 → 空（调用方整项跳过）
        }
        return {};
    }
    if (item->widget == WidgetKind::CheckBox && item->choices) {
        const auto choices = item->choices();
        for (const auto &c : choices) {
            if (c.value.toBool() == value.toBool())
                return c.mpvAssignments;
        }
        return {};
    }
    // 标量（EditableCombo / SpinBox / 无 choices 的 CheckBox / LineEdit / FilterList）：
    // 单 key authored 串（FilterList 的 pending 即编译串）
    if (item->mpvKeys.isEmpty())
        return {};
    const QString k = item->mpvKeys[0];
    switch (item->mpvType) {
    case MpvType::Flag:
        return {{k, value.toBool() ? QStringLiteral("yes") : QStringLiteral("no")}};
    case MpvType::Int:
        return {{k, QString::number(value.toInt())}};
    case MpvType::Double:
        return {{k, QString::number(value.toDouble(), 'g', 15)}};
    case MpvType::String:
        return {{k, value.toString()}};
    }
    return {};
}

bool SettingsDialog::dispatchMpvAssignments(const SettingItem *item,
                                            const QMap<QString, QString> &assignments,
                                            QStringList &failedKeys) {
    bool ok = true;
    for (auto a = assignments.cbegin(); a != assignments.cend(); ++a) {
        if (!setTypedMpvProperty(m_controller, a.key(), item->mpvType, a.value())) {
            failedKeys << a.key();
            ok = false;
        }
    }
    return ok;
}

QMap<QString, QString> SettingsDialog::runtimeMpvAssignments(const SettingItem *item) const {
    // 任一 key 读取失败 → 空 map（调用方退回 pending assignments）；合法空值不与失败混同
    QMap<QString, QString> result;
    if (item->widget == WidgetKind::FilterList) {
        auto entries = m_controller->filterEntriesChecked(item->mpvKeys[0]);
        if (!entries)
            return {};
        result.insert(item->mpvKeys[0], FilterListWidget::compileEntries(*entries));
        return result;
    }
    for (const QString &k : item->mpvKeys) {
        QString v;
        if (!readCanonicalMpvValue(m_controller, k, item->mpvType, v))
            return {};
        result.insert(k, v);
    }
    return result;
}

void SettingsDialog::writeControlValue(const SettingItem *item, const QVariant &value) {
    QWidget *w = m_controls.value(item);
    if (!w)
        return;
    switch (item->widget) {
    case WidgetKind::Combo: {
        auto *combo = static_cast<QComboBox *>(w);
        const int idx = combo->findData(value);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        break;
    }
    case WidgetKind::EditableCombo:
        static_cast<QComboBox *>(w)->setCurrentText(value.toString());
        break;
    case WidgetKind::LineEdit:
        static_cast<QLineEdit *>(w)->setText(value.toString());
        break;
    case WidgetKind::MultiLine:
        static_cast<QPlainTextEdit *>(w)->setPlainText(value.toString());
        break;
    case WidgetKind::SpinBox:
        static_cast<QSpinBox *>(w)->setValue(value.toInt()); // setValue 自动钳制
        break;
    case WidgetKind::CheckBox:
        static_cast<QCheckBox *>(w)->setChecked(value.toBool());
        break;
    case WidgetKind::FilterList: {
        const QString s = value.toString();
        static_cast<FilterListWidget *>(w)->setEntries(
            s.isEmpty() ? QList<FilterEntry>{} : FilterListWidget::parseString(s));
        break;
    }
    case WidgetKind::ToolPath:
        static_cast<ExternalToolPathCombo *>(w)->setPath(value.toString());
        break;
    }
}

void SettingsDialog::rebuildSnapshot() {
    for (const auto &cat : settingCatalog()) {
        for (const auto &item : cat.items) {
            QWidget *w = m_controls.value(&item);
            QVariant snapshot;
            if (!item.iniKey.isEmpty()) {
                snapshot = m_fillValue.value(&item); // ini：无运行期对应，快照即回填值
            } else if (!w || !w->isEnabled()) {
                snapshot =
                    m_fillValue.value(&item); // 禁用不变式：pending == fallback == 快照 == fill
            } else {
                // mpv：conf 保存值优先；缺键 / conf 不可用落回填值
                snapshot = savedControlValue(&item).value_or(m_fillValue.value(&item));
            }
            m_appliedSnapshot[&item] = snapshot;
        }
    }
}

bool SettingsDialog::applyAll() {
    bool ok = true;
    bool mpvConfDirty = false;
    // N92：仅运行期——mpv 侧改动没进 conf 缓存（m_confCache 无值：配置目录不可用，或
    // mpv.conf 打不开），本轮发生过这类改动即置位，Apply 尾部如实告知不落盘
    bool mpvRuntimeOnly = false;
    QStringList changedIniKeys; // N35：本轮实际写入 / 删除的 ini 键（应用成功后随信号发出）
    QStringList failedMpvKeys;  // N42：String 下发失败的 key（合并为一条提示）
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        const SettingItem *item = it.key();
        if (!item)
            continue;
        // 禁用中的依赖项无条件跳过（不变式三值同落 fallback，恒净）
        QWidget *w = m_controls.value(item);
        if (!w || !w->isEnabled())
            continue;
        // 三者全等才跳过：pending == 快照但 fill != 快照（运行期分歧待收口）不跳过——
        // 菜单临时态固化与 Discard 恢复运行期都走这条通道
        const QVariant snap = m_appliedSnapshot.value(item);
        if (it.value() == snap && m_fillValue.value(item) == snap)
            continue;

        if (!item->iniKey.isEmpty()) {
            // === 插件本体选项：待写值等于默认 → 删键（读侧按缺键回落常量默认），
            // 不等 → 照常写 pending；删除键同样计入变更集合 ===
            QSettings s(m_pluginIniPath, QSettings::IniFormat);
            if (it.value() == defaultControlValue(item))
                s.remove(item->iniKey);
            else
                s.setValue(item->iniKey, it.value());
            changedIniKeys << item->iniKey;
        } else if (!item->mpvKeys.isEmpty()) {
            // === mpv 选项：pending 反查逐 key 赋值（占位整项跳过，不干涉用户意图）→
            // typed set 下发 → 逐 key 回读 → 写盘期默认值删除判定（2.9） ===
            const QMap<QString, QString> assignments = mpvAssignmentsForValue(item, it.value());
            if (assignments.isEmpty())
                continue;
            QStringList failedKeys;
            if (!dispatchMpvAssignments(item, assignments, failedKeys)) {
                failedMpvKeys << failedKeys;
                continue; // 下发失败：不写 conf / 不删键（避免持久与运行期不一致）
            }
            // 回读：以 mpv 实际生效值为写盘源——归一化后的表示与 conf 一致，重开不假脏
            QMap<QString, QString> confValues = runtimeMpvAssignments(item);
            if (confValues.isEmpty()) {
                // 回读失败退回 pending 的 authored 串（赋值本身即规范形）并记日志；
                // 不得把"读取失败"当成空串清空滤镜链
                OXBOW_LOG_WARNING().noquote()
                    << QStringLiteral("SettingsDialog: mpv readback failed for %1; writing "
                                      "authored values")
                           .arg(item->mpvKeys.join(QStringLiteral(", ")));
                confValues = assignments;
            }
            defaultControlValue(item); // 确保逐 key 默认串缓存就绪（含回落路径）
            const QMap<QString, QString> defaults = m_defaultAssignments.value(item);
            for (const QString &k : item->mpvKeys) {
                if (!m_confCache) {
                    mpvRuntimeOnly = true;
                    continue;
                }
                // 等默认 → 删键（全部出现；conf 缺键自然无操作、不新建行）；
                // 不等 → 回读写盘，setGlobal 同值短路：现值相同不置脏、免空 commit
                if (confValues.value(k) == defaults.value(k)) {
                    if (m_confCache->removeGlobal(k))
                        mpvConfDirty = true;
                } else if (m_confCache->setGlobal(k, confValues.value(k))) {
                    mpvConfDirty = true;
                }
            }
        }
    }
    if (!failedMpvKeys.isEmpty())
        QMessageBox::warning(this, tr("Settings"),
                             tr("Failed to apply the following mpv options: %1\n"
                                "These values were not written to mpv.conf.")
                                 .arg(failedMpvKeys.join(QStringLiteral(", "))));
    if (mpvConfDirty && m_confCache) {
        ok &= m_confCache->commit();
    } else if (mpvRuntimeOnly) {
        // N92：仅运行期——改动已照常下发，只是这一轮存不进 mpv.conf，如实告知
        // （文案不指认成因：目录不可用与文件打不开都落到这里）
        QMessageBox::information(this, tr("Settings"),
                                 tr("The mpv configuration directory is not writable, so these "
                                    "changes apply to this session only and are not saved."));
    }
    // 应用完成：N41 重读回填后按后端实际状态重建快照并刷新派生态——重读后 pending
    // 与快照恒等（mpv 项快照 = 回读归一后的保存值），不会出现"重读后立刻可应用"的抖动
    if (ok) {
        // N35：携本轮实际写入的 ini 键集合通知视图层（注册表重装配的触发源）；
        // 接收方按键过滤，无关 ini 变更零成本。放在重读之前：接收方若也动了 INI，
        // 重读正好一并收进来
        if (!changedIniKeys.isEmpty())
            emit pluginIniKeysChanged(changedIniKeys);
        applyToControls();
        rebuildSnapshot();
        refreshDerivedState();
    }
    return ok;
}
