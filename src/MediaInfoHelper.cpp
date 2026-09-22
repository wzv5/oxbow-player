#include "pch.h"

#include "MediaInfoHelper.h"

#include "Util.h"

#include "thirdparty/mediainfo/MediaInfoDLL.h"

namespace {

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
    // shim 内 MediaInfo_Module 是 static HMODULE；构造临时实例触发
    // MediaInfoDLL_Load
    static const bool kLoaded = [] {
        MediaInfoDLL::MediaInfo probe;
        return probe.IsReady();
    }();
    return kLoaded;
}

MediaInfoHelper::Data MediaInfoHelper::parse(const QString &path) {
    Data out;
    if (path.isEmpty())
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