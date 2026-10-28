#include "pch.h"

#include "MediaInfoHelper.h"

#include "ExternalTool.h"
#include "Logging.h"
#include "Util.h"

#ifndef Q_OS_WIN
#include <dlfcn.h> // dlopen / dladdr
#endif

#include "thirdparty/mediainfo/MediaInfoDLL.h"

namespace {

// ——N142 库状态单点——
// 就绪位是 GUI 线程 preload 写、工作线程纯读的可变状态，用原子量发布；
// 已加载路径只在 GUI 线程的 preload 内触碰，无需加锁
enum class LibState { NotTried, Failed, Ready };
std::atomic<LibState> g_libState{LibState::NotTried};
QString g_loadedPath; // 已加载模块的加载目标（POSIX 按名回退时为目标名本身）

// 路径等同判定：跟随平台文件系统语义（Windows 大小写不敏感）；空串按原样比较
bool samePath(const QString &a, const QString &b) {
    if (a.isEmpty() || b.isEmpty())
        return a == b;
#ifdef Q_OS_WIN
    return QString::compare(QDir::cleanPath(a), QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
}

// 基名等同判定：shim 只按目标名解析，改名副本不接受
bool sameTargetName(const QString &name, const QString &target) {
#ifdef Q_OS_WIN
    return name.compare(target, Qt::CaseInsensitive) == 0;
#else
    return name == target;
#endif
}

// preload 的加载目标。两个分支共用这一份解析口径，"已加载"分支要比的就是这次会加载什么
struct LoadTarget {
    QString path;                    // 交加载、也与 g_loadedPath 比对的目标；空 = 无目标
    bool unusableConfigured = false; // 显式路径无效（不存在或基名不符）：判不可用，不回落自动
};

// configured 为空 = 自动态：取候选表首项，POSIX 候选全空时退回目标名本身（与 shim 同参的按
// 名 dlopen）。非空 = 显式路径：相对写法以配置文件目录为基准（进程 CWD 随 mpv 从哪儿启动而
// 变，不可控），且一律绝对化——LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 对相对路径不可靠
LoadTarget resolveLoadTarget(const ExternalToolSpec &spec, const QString &configured,
                             const QString &configDir) {
    LoadTarget out;
    if (configured.isEmpty()) {
        const QStringList candidates = externalToolCandidates(spec, configDir);
        if (!candidates.isEmpty())
            // 候选表出表是给人看的形态（Windows 原生分隔符），加载目标这一侧统一归回正斜杠
            out.path = QDir::cleanPath(candidates.first());
#ifndef Q_OS_WIN
        else
            out.path = spec.targetFileName;
#endif
        return out;
    }

    const QFileInfo raw(configured);
    const QString absolute =
        raw.isAbsolute() || configDir.isEmpty()
            ? raw.absoluteFilePath()
            : QFileInfo(QDir(configDir).absoluteFilePath(configured)).absoluteFilePath();
    const QFileInfo info(absolute);
    if (!info.isFile() || !sameTargetName(info.fileName(), spec.targetFileName)) {
        out.unusableConfigured = true;
        return out;
    }
    out.path = QDir::cleanPath(info.absoluteFilePath());
    return out;
}

// 一次性读数：须在加载调用失败的紧后一步取，中间的任何 Win32 调用都会覆掉错误码
QString platformLoadError() {
#ifdef Q_OS_WIN
    return QStringLiteral("system error %1").arg(GetLastError());
#else
    const char *message = dlerror();
    return message ? QStringLiteral("dlopen: %1").arg(QString::fromUtf8(message))
                   : QStringLiteral("dlopen failed");
#endif
}

// toDouble/toLongLong 容错包装：空串 / 非数字视作未读到
bool toDouble(const std::wstring &ws, double *out) {
    if (ws.empty())
        return false;
    const QString s = QString::fromStdWString(ws);
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok)
        return false;
    *out = v;
    return true;
}

bool toLongLong(const std::wstring &ws, qint64 *out) {
    if (ws.empty())
        return false;
    const QString s = QString::fromStdWString(ws);
    bool ok = false;
    const qint64 v = s.toLongLong(&ok);
    if (!ok)
        return false;
    *out = v;
    return true;
}

QString toQString(const std::wstring &ws) {
    return ws.empty() ? QString() : QString::fromStdWString(ws);
}

MediaInfoHelper::Stream makeStream(MediaInfoDLL::MediaInfo &MI, MediaInfoDLL::stream_t kind,
                                   size_t i, MediaInfoHelper::Stream::Kind outKind) {
    using Stream = MediaInfoHelper::Stream;
    Stream st;
    st.kind = outKind;
    st.isDefault = toQString(MI.Get(kind, i, L"Default")).compare("yes", Qt::CaseInsensitive) == 0;
    st.title = toQString(MI.Get(kind, i, L"Title"));
    st.language = toQString(MI.Get(kind, i, L"Language/String"));
    const QString fmt = toQString(MI.Get(kind, i, L"Format"));
    if (!fmt.isEmpty())
        st.codec = fmt;
    else
        st.codec = toQString(MI.Get(kind, i, L"CodecID"));

    qint64 br = 0;
    if (toLongLong(MI.Get(kind, i, L"BitRate"), &br))
        st.bitrate = br;
    qint64 sz = 0;
    if (toLongLong(MI.Get(kind, i, L"StreamSize"), &sz))
        st.streamSize = sz;
    double dur = 0.0;
    if (toDouble(MI.Get(kind, i, L"Duration"), &dur))
        st.durationSeconds = dur / 1000.0;

    double v = 0.0;
    if (outKind == Stream::Kind::Video) {
        if (toDouble(MI.Get(kind, i, L"Width"), &v))
            st.width = int(v);
        if (toDouble(MI.Get(kind, i, L"Height"), &v))
            st.height = int(v);
        if (toDouble(MI.Get(kind, i, L"FrameRate"), &v))
            st.frameRate = v;
    } else if (outKind == Stream::Kind::Audio) {
        if (toDouble(MI.Get(kind, i, L"SamplingRate"), &v))
            st.sampleRate = int(v);
        if (toDouble(MI.Get(kind, i, L"Channel(s)"), &v))
            st.channelCount = int(v);
    }
    return st;
}

} // namespace

bool MediaInfoHelper::available() {
    // acquire：与 preload 的 release store 配对，工作线程读到就绪即看得到库已加载
    return g_libState.load(std::memory_order_acquire) == LibState::Ready;
}

void MediaInfoHelper::preload(const QString &configuredPath, const QString &configDir) {
    const ExternalToolSpec &spec = externalToolSpec(ExternalTool::MediaInfo);
    const QString configured = configuredPath.trimmed();
    const LoadTarget target = resolveLoadTarget(spec, configured, configDir);

    if (g_libState.load(std::memory_order_acquire) == LibState::Ready) {
        // 已加载：不卸载、不清状态，只在加载目标确有变化（含"发现不到了"）时提示下次启动
        if (target.unusableConfigured)
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("Configured MediaInfo library is not usable (missing or not "
                                  "named %1); the loaded one stays in use. loaded: %2, "
                                  "requested: %3")
                       .arg(spec.targetFileName, g_loadedPath, configured);
        else if (!samePath(target.path, g_loadedPath))
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("MediaInfo library is already loaded; this request takes "
                                  "effect on next start. loaded: %1, next: %2")
                       .arg(g_loadedPath, target.path.isEmpty()
                                              ? QStringLiteral("(none discovered)")
                                              : target.path);
        return;
    }

    if (target.unusableConfigured) {
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("Configured MediaInfo library is not usable (missing or not named "
                              "%1): %2")
                   .arg(spec.targetFileName, configured);
        g_libState.store(LibState::Failed, std::memory_order_relaxed);
        return;
    }
    if (target.path.isEmpty()) {
        OXBOW_LOG_INFO() << "MediaInfo library not found in plugin dir, config dir or PATH";
        g_libState.store(LibState::Failed, std::memory_order_relaxed);
        return;
    }

    // 加载：句柄永久保留，任何路径都不卸载（同名 DLL 不能并存，卸载即换不回）
#ifdef Q_OS_WIN
    const std::wstring widePath = target.path.toStdWString();
    HMODULE handle =
        LoadLibraryExW(widePath.c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    void *handle = dlopen(target.path.toUtf8().constData(), RTLD_LAZY);
#endif
    if (!handle) {
        const QString why = platformLoadError();
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("Failed to load MediaInfo library: %1 (%2)").arg(target.path, why);
        g_libState.store(LibState::Failed, std::memory_order_relaxed);
        return;
    }

    // 探针：触发 shim 的按名解析，以 IsReady 判定最终可用
    MediaInfoDLL::MediaInfo probe;
    if (!probe.IsReady()) {
        OXBOW_LOG_WARNING().noquote()
            << QStringLiteral("MediaInfo shim cannot resolve the loaded library by name: %1")
                   .arg(target.path);
        g_libState.store(LibState::Failed, std::memory_order_relaxed);
        return;
    }

    // shim 实际命中的模块与加载目标比对：不一致 = 同进程已有同名模块先占了按名解析
    if (QFileInfo(target.path).isAbsolute()) {
        QString shimPath;
#ifdef Q_OS_WIN
        // shim 内 MediaInfo_Module 是 static HMODULE（extern "C" 全局区，不在
        // MediaInfoDLL 命名空间）
        if (MediaInfo_Module) {
            std::wstring buf(MAX_PATH, L'\0');
            DWORD n = 0;
            while ((n = GetModuleFileNameW(MediaInfo_Module, buf.data(),
                                           static_cast<DWORD>(buf.size()))) == buf.size())
                buf.resize(buf.size() * 2);
            shimPath = QString::fromWCharArray(buf.data(), static_cast<int>(n));
        }
#else
        Dl_info info{};
        // MediaInfo_New 在探针构造期已被解析，指向库内函数
        if (MediaInfo_New && dladdr(reinterpret_cast<void *>(MediaInfo_New), &info) &&
            info.dli_fname)
            shimPath = QString::fromUtf8(info.dli_fname);
#endif
        if (!shimPath.isEmpty() && !samePath(shimPath, target.path))
            OXBOW_LOG_WARNING().noquote()
                << QStringLiteral("MediaInfo shim resolved to a different module than requested;"
                                  " shim: %1, requested: %2")
                       .arg(QDir::cleanPath(shimPath), target.path);
    }

    g_loadedPath = target.path;
    g_libState.store(LibState::Ready, std::memory_order_release);
}

MediaInfoHelper::Data MediaInfoHelper::parse(const QString &path) {
    Data out;
    if (path.isEmpty())
        return out;

    // N142：未就绪直接放弃——绝不让 shim 的按名加载在工作线程被惰性触发
    if (g_libState.load(std::memory_order_acquire) != LibState::Ready)
        return out;

    // N18：仅对本地路径解析；网络/流路径（http:// / rtmp:// / ...）无本地文件
    // 实体，Open 必然失败且会触发 IO 等待；提前返回空 Data 让 UI 优雅降级
    if (Util::Url::isNetworkStream(path))
        return out;

    MediaInfoDLL::MediaInfo MI;
    if (!MI.IsReady())
        return out;
    MI.Option(L"ParseSpeed", L"0");     // 0=完整模式（最快且全字段）；上游默认即此
    MI.Option(L"Timeout_Get", L"5000"); // 单 Get 超时守卫

    const std::wstring wpath(path.toStdWString());
    // MediaInfo 上游 Open 返回值语义：返回 Count_Get(Stream_General)（即
    // General 流数）， 单文件通常为 1；返回 0 才代表路径为空 /
    // 文件无法访问——切勿视为"非 0 即失败"
    if (MI.Open(wpath) == 0)
        return out;

    // ——General——
    auto getGeneral = [&](const wchar_t *name) {
        return MI.Get(MediaInfoDLL::Stream_General, 0, name);
    };
    out.format = toQString(getGeneral(L"Format"));
    double dur = 0.0;
    if (toDouble(getGeneral(L"Duration"), &dur))
        out.duration = dur / 1000.0;
    out.title = toQString(getGeneral(L"Title"));
    out.album = toQString(getGeneral(L"Album"));
    out.performer = toQString(getGeneral(L"Performer"));
    const QString recorded = toQString(getGeneral(L"Recorded_Date"));
    if (!recorded.isEmpty())
        out.recordedDate = Util::parseReleaseDate(recorded);
    else {
        const QString tagged = toQString(getGeneral(L"Tagged_Date"));
        if (!tagged.isEmpty())
            out.recordedDate = Util::parseReleaseDate(tagged);
    }

    // ——多流——
    using Stream = MediaInfoHelper::Stream;
    for (size_t i = 0; i < MI.Count_Get(MediaInfoDLL::Stream_Video); ++i)
        out.streams.append(makeStream(MI, MediaInfoDLL::Stream_Video, i, Stream::Kind::Video));
    for (size_t i = 0; i < MI.Count_Get(MediaInfoDLL::Stream_Audio); ++i)
        out.streams.append(makeStream(MI, MediaInfoDLL::Stream_Audio, i, Stream::Kind::Audio));
    for (size_t i = 0; i < MI.Count_Get(MediaInfoDLL::Stream_Text); ++i)
        out.streams.append(makeStream(MI, MediaInfoDLL::Stream_Text, i, Stream::Kind::Text));

    // ——章节：MP4/MKV 章节在 MediaInfo 上游按"索引条目"暴露
    // 取法：先读 Chapters_Pos_Begin / Chapters_Pos_End 得章节范围，
    // 再以 (size_t)pos 为第三参数（不是字段名）经 MediaInfo_GetI 拿整章内容——
    // shim 的 size_t 重载 Get(kind, sn, size_t Parameter, InfoKind=Info_Text)
    // 直接对应底层 MediaInfo_GetI，避免 wstring 字段名误用
    auto takeChapters = [&](MediaInfoDLL::stream_t kind) {
        if (MI.Count_Get(kind) == 0)
            return;
        bool okB = false, okE = false;
        const size_t begin = toQString(MI.Get(kind, 0, L"Chapters_Pos_Begin")).toUShort(&okB);
        const size_t end = toQString(MI.Get(kind, 0, L"Chapters_Pos_End")).toUShort(&okE);
        if (!okB || !okE || end <= begin)
            return;
        for (size_t pos = begin; pos < end; ++pos) {
            // 标题：Info_Text 模式（按 Pos 整数查）——返回章节标题字符串
            const QString title = toQString(MI.Get(kind, 0, pos, MediaInfoDLL::Info_Text));
            // 时间：Info_Name 模式（按 Pos 整数查）——返回 "HH:MM:SS.fff"
            // 时间戳字符串
            const QString stamp = toQString(MI.Get(kind, 0, pos, MediaInfoDLL::Info_Name));
            // 解析 HH:MM:SS.fff → 秒
            double secs = 0.0;
            const QStringList parts = stamp.split(QLatin1Char(':'));
            if (parts.size() == 3) {
                bool ok = false;
                const double h = parts[0].toDouble(&ok);
                ok = ok;
                const double m = parts[1].toDouble(&ok);
                ok = ok;
                const double s = parts[2].toDouble(&ok);
                if (ok)
                    secs = h * 3600.0 + m * 60.0 + s;
            }
            if (title.isEmpty() && secs == 0.0)
                continue;
            Chapter ch;
            ch.title = title;
            ch.timeSeconds = secs;
            out.chapters.append(ch);
        }
    };
    takeChapters(MediaInfoDLL::Stream_Other);
    if (out.chapters.isEmpty())
        takeChapters(MediaInfoDLL::Stream_Menu);

    MI.Close();
    return out;
}