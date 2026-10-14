#include "pch.h"

#include "SettingsDialog.h"

#include "Constants.h"
#include "FilterListWidget.h"
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
    FilterList
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
    QString id;                        // 可选：联动主控项标识（仅 CheckBox 主控需要）
    QString dependsOn;                 // 可选：指向某 CheckBox 项 id（单层依赖，禁用不回填）
    MpvType mpvType = MpvType::String; // mpv 属性类型提示（默认 String；INI 后端忽略；
                                       // 带 choices 的 CheckBox 固定 String）
    // FilterList 专用（N42）：预设菜单数据源；其余控件忽略
    std::function<QList<FilterPreset>()> filterPresets;
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
bool readCanonicalMpvValue(MpvController *c, const QString &key, MpvType type, QString &out) {
    switch (type) {
    case MpvType::String:
        out = c->propertyString(key); // String 型：空串是合法值，失败同样表现为空
        return true;
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

// 写路径：按类型提示走 typed set（消灭"数值当字符串下发再由 mpv 解析"环节）
void setTypedMpvProperty(MpvController *c, const QString &key, MpvType type, const QString &raw) {
    switch (type) {
    case MpvType::String:
        c->setProperty(key, raw);
        break;
    case MpvType::Flag:
        c->setProperty(key, raw == QLatin1String("yes"));
        break;
    case MpvType::Int:
        c->setProperty(key, int(raw.toLongLong()));
        break;
    case MpvType::Double:
        c->setProperty(key, raw.toDouble());
        break;
    }
}

// 音量步进四档（滚轮 / 方向键共用；INI 后端，写 int）
QList<SettingChoice> stepChoices() {
    QList<SettingChoice> l;
    for (int s : {1, 2, 5, 10})
        l.append({QString::number(s), s, {}});
    return l;
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

// 硬件解码：标量项（可编辑下拉），choices 仅作快选预设，逻辑值即文本本身。
// auto-copy（N42）：把解码帧拷回系统内存——视频滤镜（vf）在原生硬解下不可用
// （硬件帧无法进 libavfilter 软件滤镜链），copy 模式是启用滤镜的标准解法
QList<SettingChoice> hwdecPresets() {
    return {{QStringLiteral("auto")},      {QStringLiteral("auto-safe")},
            {QStringLiteral("auto-copy")}, {QStringLiteral("d3d11va")},
            {QStringLiteral("dxva2")},     {QStringLiteral("no")}};
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
    static const QList<SettingCategory> catalog = {
        // ① 基本 —— 插件本体选项（写 oxbow.ini）
        {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Basic"),
         .items =
             {
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Language"),
                  .widget = WidgetKind::Combo,
                  .iniKey = "language",
                  .choices = languageChoices,
                  .note = QT_TRANSLATE_NOOP("SettingsDialog", "Takes effect after restart")},
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Wheel Step"),
                  .widget = WidgetKind::Combo,
                  .iniKey = "volumeWheelStep",
                  .choices = stepChoices,
                  .fallback = 2, // 维护者指定：滚轮步进默认 2
                  .note = QT_TRANSLATE_NOOP("SettingsDialog", "For adjusting volume")},
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Arrow Key Step"),
                  .widget = WidgetKind::Combo,
                  .iniKey = "volumeKeyStep",
                  .choices = stepChoices,
                  .fallback = 5, // 对齐现版：miss 兜底 5 而非第 0 项
                  .note = QT_TRANSLATE_NOOP("SettingsDialog", "For adjusting volume")},
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Recursive Depth Limit"),
                  .widget = WidgetKind::SpinBox,
                  .iniKey = "recursiveDepthLimit",
                  .fallback = App::recursiveDepthLimitDefault(),
                  .range = {0, App::recursiveDepthLimitMax()},
                  .note = QT_TRANSLATE_NOOP("SettingsDialog",
                                            "For directory enumeration including subdirectories")},
             }},
        // ② 播放 —— mpv 选项（写 mpv.conf + 下发）
        {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Playback"),
         .items =
             {
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Loop Playback"),
                  .widget = WidgetKind::Combo,
                  .mpvKeys = {"loop-file", "loop-playlist"},
                  .choices = loopChoices,
                  .fallback = QString() /*占位*/},
             }},
        // ③ 视频 —— mpv 选项
        {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Video"),
         .items =
             {
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Hardware Decoding"),
                  .widget = WidgetKind::EditableCombo,
                  .mpvKeys = {"hwdec"},
                  .choices = hwdecPresets,
                  .fallback = "auto"},
                 // N42：视频滤镜列表（与音频页同构）。备注两条合一（原生硬解下 CPU
                 // 滤镜不可用——mpv 非 copy 硬解不自动回读硬件帧，需 copy 模式或
                 // 软解；尺寸类滤镜的导航器已知不一致——修正属 N40，按已知项口径
                 // 如实告知，不承诺修好）
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
                 // N42：原单行文本框升级为滤镜列表编辑器（空值语义不变：无滤镜）
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Audio Filters"),
                  .widget = WidgetKind::FilterList,
                  .mpvKeys = {"af"},
                  .note = QT_TRANSLATE_NOOP(
                      "SettingsDialog",
                      "loudnorm works internally at 192 kHz in dynamic mode and raises the "
                      "output sample rate; add aresample=48000 when a fixed rate is needed"),
                  .filterPresets = audioFilterPresets},
             }},
        // ⑤ 网络 —— N35：远端浏览链路（WebDAV 枚举）所用 client 的缺省参数（写 oxbow.ini）；N37
        // 起并列 mpv 侧播放证书开关
        // （写 mpv.conf + 下发 tls-verify）——两条链路的证书校验各自独立，
        // 只关一侧就会出现"能列出、播不了"（或反之），两开关互不联动；
        // 并列 mpv 侧 ytdl hook 开关（写 mpv.conf + 下发 ytdl）
        {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Network"),
         .items =
             {
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Remote Request Timeout"),
                  .widget = WidgetKind::SpinBox,
                  .iniKey = "remoteTimeoutSec",
                  .fallback = App::remoteTimeoutSecDefault(),
                  .range = {1, 120}},
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Ignore HTTPS Certificate Errors"),
                  .widget = WidgetKind::CheckBox,
                  .iniKey = "ignoreHttpsErrors",
                  .fallback = App::ignoreHttpsErrorsDefault(),
                  // N37：澄清只管浏览侧（列表与元数据请求），不影响播放
                  .note = QT_TRANSLATE_NOOP("SettingsDialog",
                                            "Applies to playlist and metadata requests only; "
                                            "playback is governed by the mpv-side option below")},
                 // N37：播放侧（mpv / ffmpeg 发请求）证书开关。fallback 为未勾选——
                 // 只在属性不可读或取值未命中两项赋值时兜底；mpv 内置默认 tls-verify=no
                 // 且可实时读写，首次打开呈现为已勾选，保持勾选不改变任何现状
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog",
                                             "Ignore HTTPS Certificate Errors (mpv side)"),
                  .widget = WidgetKind::CheckBox,
                  .mpvKeys = {"tls-verify"},
                  .choices = tlsVerifyChoices,
                  .fallback = false,
                  .mpvType = MpvType::String},
                 // mpv 侧 ytdl hook 开关（ytdl 选项，bool）：勾选 = ytdl=yes（mpv
                 // 内置默认即启用，可实时读写——ytdl hook 脚本逐文件加载时现读该
                 // 选项，改动对后续播放立即生效）；无 choices 走 Flag 直通两态
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Enable youtube-dl hook script"),
                  .widget = WidgetKind::CheckBox,
                  .mpvKeys = {"ytdl"},
                  .fallback = true,
                  .mpvType = MpvType::Flag},
             }},
        // ⑥ 播放上报 —— N74：报告者开关组（粒度 = 报告者种类，ini 键与注册表的键
        // 一一对应；分类不设总开关）。全部写 oxbow.ini，应用成功经既有
        // pluginIniKeysChanged 通知主窗改驱动器启用表并切闸门，实时生效、无需重启
        {.title = QT_TRANSLATE_NOOP("SettingsDialog", "Playback Reporting"),
         .items =
             {
                 {.label = QT_TRANSLATE_NOOP("SettingsDialog", "Report playback to Jellyfin"),
                  .widget = WidgetKind::CheckBox,
                  .iniKey = jellyfinSwitch().iniKey, // 键名与缺省同引上报账目行（N99 单点）
                  .fallback = jellyfinSwitch().defaultValue,
                  .note = QT_TRANSLATE_NOOP(
                      "SettingsDialog",
                      "Sends watch position to the signed-in Jellyfin server so resume "
                      "points and watched marks stay up to date")},
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
        qWarning("SettingsDialog: two-state CheckBox item [%d/%d] must declare "
                 "MpvType::String",
                 ci, ii);
    if (item.mpvKeys.size() != 1)
        qWarning("SettingsDialog: two-state CheckBox item [%d/%d] must govern exactly one "
                 "key",
                 ci, ii);
    if (item.fallback.typeId() != QMetaType::Bool)
        qWarning("SettingsDialog: two-state CheckBox item [%d/%d] fallback must be bool", ci, ii);
    if (choices.size() != 2) {
        qWarning("SettingsDialog: two-state CheckBox item [%d/%d] needs exactly two "
                 "choices, got %d",
                 ci, ii, int(choices.size()));
        return; // 后续检查依赖"恰两项"
    }
    const bool v0 = choices[0].value.toBool();
    const bool v1 = choices[1].value.toBool();
    if (choices[0].value.typeId() != QMetaType::Bool ||
        choices[1].value.typeId() != QMetaType::Bool || v0 == v1)
        qWarning("SettingsDialog: two-state CheckBox item [%d/%d] choices must carry one "
                 "true and one false bool value",
                 ci, ii);
    QString texts[2];
    for (int k = 0; k < 2; ++k) {
        const QMap<QString, QString> &a = choices[k].mpvAssignments;
        const bool shapeOk =
            a.size() == 1 && item.mpvKeys.size() == 1 && a.firstKey() == item.mpvKeys[0];
        if (!shapeOk) {
            qWarning("SettingsDialog: two-state CheckBox item [%d/%d] choice %d must "
                     "assign the single governed key",
                     ci, ii, k);
            continue;
        }
        texts[k] = a.first();
        if (texts[k].isEmpty())
            qWarning("SettingsDialog: two-state CheckBox item [%d/%d] choice %d assigns "
                     "empty text",
                     ci, ii, k);
    }
    if (!texts[0].isEmpty() && texts[0] == texts[1])
        qWarning("SettingsDialog: two-state CheckBox item [%d/%d] choices must assign "
                 "distinct texts",
                 ci, ii);
}

// 目录自检（构造期一次，仅 qWarning 不中断；R4 §4.3.6）
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
                qWarning("SettingsDialog: catalog item [%d/%d] must have exactly "
                         "one backend (iniKey xor mpvKeys)",
                         ci, ii);
            if (item.widget == WidgetKind::Combo && !item.choices)
                qWarning("SettingsDialog: Combo item [%d/%d] missing choices provider", ci, ii);
            if (hasMpv) {
                // mpvType 与控件语义配套检查
                if (item.widget == WidgetKind::CheckBox) {
                    // 有 choices 走两态映射（N33 §2.1）；无 choices 仍要求 Flag
                    if (item.choices)
                        validateTwoStateCheckBox(item, ci, ii);
                    else if (item.mpvType != MpvType::Flag)
                        qWarning("SettingsDialog: mpv CheckBox item [%d/%d] must declare "
                                 "MpvType::Flag when it carries no choices",
                                 ci, ii);
                }
                if (item.widget == WidgetKind::SpinBox && item.mpvType != MpvType::Int &&
                    item.mpvType != MpvType::Double)
                    qWarning("SettingsDialog: mpv SpinBox item [%d/%d] must declare "
                             "MpvType::Int or Double",
                             ci, ii);
                if (item.widget == WidgetKind::FilterList) {
                    // N42：滤镜列表恰管辖一个 key、String 型下发；INI 后端无意义
                    if (item.mpvKeys.size() != 1)
                        qWarning("SettingsDialog: FilterList item [%d/%d] must govern "
                                 "exactly one key",
                                 ci, ii);
                    if (item.mpvType != MpvType::String)
                        qWarning("SettingsDialog: FilterList item [%d/%d] must declare "
                                 "MpvType::String",
                                 ci, ii);
                    if (!item.iniKey.isEmpty())
                        qWarning("SettingsDialog: FilterList item [%d/%d] must be an mpv "
                                 "item, not INI",
                                 ci, ii);
                }
                if (item.widget == WidgetKind::Combo) {
                    // mpv 枚举项必须有且仅有一个占位项（R3 §3.7 语义依赖）
                    int placeholders = 0;
                    const auto choices = item.choices ? item.choices() : QList<SettingChoice>{};
                    for (const auto &c : choices)
                        if (c.mpvAssignments.isEmpty())
                            ++placeholders;
                    if (placeholders != 1)
                        qWarning("SettingsDialog: mpv enum item [%d/%d] needs exactly one "
                                 "placeholder choice, got %d",
                                 ci, ii, placeholders);
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
                                qWarning("SettingsDialog: mpv enum item [%d/%d] assignment "
                                         "'%s' not parseable as declared MpvType",
                                         ci, ii, qPrintable(it.value()));
                        }
                }
            }
            // 两态映射仅属 mpv 后端：项级校验必须在 hasMpv 分支之外，
            // 否则 INI 项触达不到
            if (item.widget == WidgetKind::CheckBox && item.choices && !hasMpv)
                qWarning("SettingsDialog: INI CheckBox item [%d/%d] must not carry "
                         "choices",
                         ci, ii);
            if (item.widget == WidgetKind::SpinBox) {
                const int f = item.fallback.toInt();
                if (f < item.range.min || f > item.range.max)
                    qWarning("SettingsDialog: SpinBox fallback out of range [%d/%d]", ci, ii);
            }
            if (!item.id.isEmpty()) {
                if (item.widget != WidgetKind::CheckBox)
                    qWarning("SettingsDialog: id '%s' on item [%d/%d] must be a "
                             "CheckBox master",
                             qPrintable(item.id), ci, ii);
                if (ids.contains(item.id))
                    qWarning("SettingsDialog: duplicate master id '%s' on item [%d/%d]",
                             qPrintable(item.id), ci, ii);
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
                    qWarning("SettingsDialog: CheckBox item [%d/%d] must not depend "
                             "on others (single-level dependency)",
                             cj, jj);
                if (!ids.contains(item.dependsOn))
                    qWarning("SettingsDialog: item [%d/%d] dependsOn '%s' unresolved", cj, jj,
                             qPrintable(item.dependsOn));
            }
            ++jj;
        }
        ++cj;
    }
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
    resize(760, 520); // 默认窗口尺寸（内容较小时避免对话框过小）
    buildUi();
    applyToControls();

    // 初始化快照：回填后的值即为初始已应用值。
    m_appliedSnapshot = m_pending;
    // 初始 Apply 按钮禁用；修改任一控件后 updateApplyButtonState() 启用
    m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);

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

    // 左侧分类列表（固定宽度 140，纯文本）
    m_categoryList = new QListWidget(this);
    m_categoryList->setFixedWidth(140);
    m_categoryList->setIconSize(QSize(0, 0)); // 无图标
    m_categoryList->setSpacing(2);
    root->addWidget(m_categoryList);

    // 右侧容器（堆栈 + 底部按钮）
    auto *rightPane = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    m_pageStack = new QStackedWidget(rightPane);

    // 遍历目录动态生成页面：每分类一页、每设置项一组"标签 + 控件（+ 备注行）"。
    // 文案在构建控件此刻经 tr() 翻译（上下文即 SettingsDialog，既有 .ts 不失效）。
    // 各控件以 lambda 捕获自己的目录项指针接入 onControlChanged（static 目录
    // 生命周期内指针稳定，无需在控件上挂 id 属性做反查）
    // 目录文本字段（label/note/title）为 const char*：直接 tr()；
    // trS 仅供选项 label（QString，运行期构造）使用
    auto trS = [this](const QString &s) { return tr(s.toUtf8().constData()); };
    for (const auto &cat : settingCatalog()) {
        m_categoryList->addItem(tr(cat.title));

        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(12, 12, 12, 12);
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
                // 纯编辑框：逻辑值即文本
                auto *edit = new QLineEdit(page);
                connect(edit, &QLineEdit::textChanged, this,
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
            }

            m_controls.insert(item, control);
            if (item->widget != WidgetKind::CheckBox && item->widget != WidgetKind::FilterList) {
                // 标签与控件同行；QFormLayout 保证各标签列左对齐到最宽标签
                form->addRow(new QLabel(tr(item->label)), control);
            } else {
                // 标签并入复选框 / 分组框标题，独占整行
                form->addRow(control);
            }

            // 可选备注行：灰色小字（取 Disabled 文本色，随主题自适应），空则不占位；
            // 单参数 addRow 跨两列展示。必须开 wordWrap：QLabel 不换行时
            // minimumSizeHint ≈ 整行文本宽度，长备注会把对话框最小宽度撑爆
            // （N42 长备注实测撑到无法拖拽缩小）
            if (item->note && *item->note) { // 非空指针且有内容才显示
                auto *note = new QLabel(tr(item->note));
                note->setWordWrap(true);
                QPalette pal = note->palette();
                pal.setColor(QPalette::WindowText,
                             pal.color(QPalette::Disabled, QPalette::WindowText));
                note->setPalette(pal);
                QFont nf = note->font();
                nf.setPointSize(qMax(nf.pointSize() - 1, 1));
                note->setFont(nf);
                form->addRow(note);
            }

            m_pending.insert(item, {}); // 先占位，回填时覆写
        }
        layout->addLayout(form);
        layout->addStretch(1);
        m_pageStack->addWidget(page);
    }
    rightLayout->addWidget(m_pageStack, 1);

    m_buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Ok | QDialogButtonBox::Cancel, rightPane);
    rightLayout->addWidget(m_buttonBox);
    root->addWidget(rightPane, 1);

    // 切换分类同步堆栈
    connect(m_categoryList, &QListWidget::currentRowChanged, m_pageStack,
            &QStackedWidget::setCurrentIndex);
    m_categoryList->setCurrentRow(0);
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
        } else if (item->widget == WidgetKind::LineEdit) {
            auto *edit = static_cast<QLineEdit *>(w);
            const QString v = ini.value(item->iniKey, item->fallback).toString();
            edit->setText(v);
            m_pending[item] = v;
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
        }
    }
}

void SettingsDialog::applyFallback(const SettingItem *item) {
    QWidget *w = m_controls.value(item);
    if (!item || !w)
        return;
    w->setEnabled(false);
    switch (item->widget) {
    case WidgetKind::Combo: {
        auto *combo = static_cast<QComboBox *>(w);
        const int idx = combo->findData(item->fallback);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
        break;
    }
    case WidgetKind::EditableCombo:
        static_cast<QComboBox *>(w)->setCurrentText(item->fallback.toString());
        break;
    case WidgetKind::LineEdit:
        static_cast<QLineEdit *>(w)->setText(item->fallback.toString());
        break;
    case WidgetKind::SpinBox:
        static_cast<QSpinBox *>(w)->setValue(item->fallback.toInt());
        break;
    case WidgetKind::CheckBox:
        static_cast<QCheckBox *>(w)->setChecked(item->fallback.toBool());
        break;
    case WidgetKind::FilterList:
        // 禁用落兜底：空列表 = 无滤镜（空值语义，与 mpv 属性空值口径一致）
        static_cast<FilterListWidget *>(w)->setEntries({});
        break;
    }
    m_pending[item] = readControlValue(w);
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
                // 由此维持不变式"禁用项 pending == fallback == snapshot"
                applyFallback(&item);
            }
        }
    m_blockApply = false;
}

QVariant SettingsDialog::readControlValue(QWidget *control) const {
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
    updateApplyButtonState();
}

void SettingsDialog::updateApplyButtonState() {
    // dirty：任一控件值与快照不同
    bool anyDirty = false;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        const auto snapIt = m_appliedSnapshot.find(it.key());
        if (snapIt == m_appliedSnapshot.end() || snapIt.value() != it.value()) {
            anyDirty = true;
            break;
        }
    }
    m_dirty = anyDirty;
    if (m_buttonBox && m_buttonBox->button(QDialogButtonBox::Apply))
        m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(m_dirty);
}

bool SettingsDialog::applyAll() {
    bool ok = true;
    bool mpvConfDirty = false;
    // N92：仅运行期——mpv 侧改动没进 conf 缓存（m_confCache 无值：配置目录不可用，或
    // mpv.conf 打不开），本轮发生过这类改动即置位，Apply 尾部如实告知不落盘
    bool mpvRuntimeOnly = false;
    QStringList changedIniKeys; // N35：本轮实际写入的 ini 键（应用成功后随信号发出）
    QStringList failedMpvKeys;  // N42：String 下发失败的 key（合并为一条提示）
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        const SettingItem *item = it.key();
        if (!item)
            continue;
        const QVariant &val = it.value();
        // 禁用中的依赖项无条件跳过（防御性；正常禁用项 pending==fallback==
        // snapshot，不产生 dirty）
        QWidget *w = m_controls.value(item);
        if (!w || !w->isEnabled())
            continue;
        // 与快照比对：未变则跳过下发与写盘
        const auto snapIt = m_appliedSnapshot.find(item);
        if (snapIt != m_appliedSnapshot.end() && snapIt.value() == val)
            continue;

        if (!item->iniKey.isEmpty()) {
            // === 插件本体选项：写 oxbow.ini ===
            QSettings s(m_pluginIniPath, QSettings::IniFormat);
            s.setValue(item->iniKey, val);
            changedIniKeys << item->iniKey;
        } else if (!item->mpvKeys.isEmpty() && item->widget == WidgetKind::Combo) {
            // === mpv 枚举项：按 value 反查选项；占位（assignments 空）不下发不写 conf ===
            const auto choices = item->choices ? item->choices() : QList<SettingChoice>{};
            const SettingChoice *hit = nullptr;
            for (const auto &c : choices) {
                if (c.value == val) {
                    hit = &c;
                    break;
                }
            }
            if (!hit || hit->mpvAssignments.isEmpty())
                continue;
            for (auto a = hit->mpvAssignments.cbegin(); a != hit->mpvAssignments.cend(); ++a) {
                // 下发按 mpvType 走 typed set；conf 写保留 authored 字符串
                // （mpv.conf 本就是文本格式）
                setTypedMpvProperty(m_controller, a.key(), item->mpvType, a.value());
                if (m_confCache) {
                    m_confCache->setGlobal(a.key(), a.value());
                    mpvConfDirty = true;
                } else {
                    mpvRuntimeOnly = true;
                }
            }
        } else if (!item->mpvKeys.isEmpty() && item->widget == WidgetKind::CheckBox &&
                   item->choices) {
            // === mpv CheckBox 两态映射（N33 §2.1）：勾选态反查选项，写其赋值全集；
            // 字符串下发（flag 属性由 mpv 自行解析文本），conf 写 authored 字符串 ===
            const bool on = val.toBool();
            const auto choices = item->choices();
            for (const auto &c : choices) {
                if (c.value.toBool() != on)
                    continue;
                for (auto a = c.mpvAssignments.cbegin(); a != c.mpvAssignments.cend(); ++a) {
                    m_controller->setProperty(a.key(), a.value());
                    if (m_confCache) {
                        m_confCache->setGlobal(a.key(), a.value());
                        mpvConfDirty = true;
                    } else {
                        mpvRuntimeOnly = true;
                    }
                }
                break;
            }
        } else if (!item->mpvKeys.isEmpty()) {
            // === mpv 标量项（EditableCombo/SpinBox/无 choices 的 CheckBox/LineEdit）：
            // 按 mpvType 走 typed set 下发；conf 写用字符串形式（mpv.conf 本就是文本格式） ===
            const QString k = item->mpvKeys[0];
            QString confValue;
            bool setOk = true;
            switch (item->mpvType) {
            case MpvType::Flag:
                confValue = val.toBool() ? QStringLiteral("yes") : QStringLiteral("no");
                m_controller->setProperty(k, val.toBool());
                break;
            case MpvType::Int:
                confValue = QString::number(val.toInt());
                m_controller->setProperty(k, val.toInt());
                break;
            case MpvType::Double:
                confValue = QString::number(val.toDouble(), 'g', 15);
                m_controller->setProperty(k, val.toDouble());
                break;
            case MpvType::String:
                confValue = val.toString();
                setOk = m_controller->setProperty(k, confValue);
                break;
            }
            // N42：String 下发失败按项级跳过——不写该项 conf（避免污染持久配置），
            // key 记入合并提示；其余项照常，ok 仍以 conf 提交结果为准；N41 重读
            // 随后把失败项控件拉回后端真值（重读后 pending == 快照，无假 dirty）
            if (!setOk) {
                failedMpvKeys << k;
                continue;
            }
            if (m_confCache) {
                m_confCache->setGlobal(k, confValue);
                mpvConfDirty = true;
            } else {
                mpvRuntimeOnly = true;
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
    // 应用完成：更新快照为当前已应用值；Apply 按钮随之禁用
    if (ok) {
        m_appliedSnapshot = m_pending;
        m_dirty = false;
        if (m_buttonBox && m_buttonBox->button(QDialogButtonBox::Apply))
            m_buttonBox->button(QDialogButtonBox::Apply)->setEnabled(false);
        // N35：携本轮实际写入的 ini 键集合通知视图层（注册表重装配的触发源）；
        // 接收方按键过滤，无关 ini 变更零成本
        if (!changedIniKeys.isEmpty())
            emit pluginIniKeysChanged(changedIniKeys);

        // N41：Apply 成功后立即重读回填，显示后端实际状态（mpv 归一化 / 下发
        // 失败 / INI 写盘失败均如实呈现）。放在 ini 键变更信号之后：接收方若
        // 也动了 INI，重读正好一并收进来。重读复用完整回填路径（内部自带
        // m_blockApply 守卫），随后以重读结果重建已应用快照并刷新按钮状态——
        // 重读后 pending 与快照恒等，不会出现"重读后立刻可应用"的抖动
        applyToControls();
        m_appliedSnapshot = m_pending;
        updateApplyButtonState();
    }
    return ok;
}
