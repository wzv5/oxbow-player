#pragma once

#include "pch.h"

// N179：script-playlist 协议的纯解析层，不碰控件、mpv 与实体
namespace ScriptListSpec {

// 入站名与动词
inline constexpr char kInboundName[] = "script-playlist";
// 入站动词
inline constexpr char kVerbCreate[] = "create";
inline constexpr char kVerbLayer[] = "layer";
// 回执 reason：协议 token，不进 UI
inline constexpr char kReasonBadArgs[] = "bad-args";
inline constexpr char kReasonBadId[] = "bad-id";
inline constexpr char kReasonExists[] = "exists";
inline constexpr char kEventCreated[] = "created";
inline constexpr char kEventRejected[] = "rejected";
inline constexpr char kEventNeedLayer[] = "need-layer";
inline constexpr char kEventCancelLayer[] = "cancel-layer";

// 数据源 id：字符集 [A-Za-z0-9_.-]，1-64 个字符
bool isValidListId(const QString &id);

// 出站消息名：oxbow-script-playlist:<event>
QString eventName(const QString &event);

// layer 应答的中立结构（实体铸造归提供者）

// 容器；title 是层级显示名，与 metadata.title 的媒体标题无关
struct ContainerData {
    QString id;
    QString title;
    QString path; // 纯展示，空 = 未知
};

// 条目；未给的键一律留空或 nullopt
struct ItemData {
    QString url;
    QString displayName;
    QString filename;
    QVariantMap options; // mpv 选项名 → 标量文本
    // file 三键（unix 秒；nullopt = 未给）
    std::optional<qint64> fileSize;
    std::optional<qint64> fileCreated;
    std::optional<qint64> fileModified;
    // metadata 六键：空串 / 无效时间即未给
    QString metaFormat;
    std::optional<double> metaDuration;
    QDateTime metaRecordedDate;
    QString metaTitle;
    QString metaAlbum;
    QString metaPerformer;
};

// 一层解析结果；valid = false 即整层非法
struct LayerData {
    bool valid = false;
    // 脚本自定的整层失败文案；非空即整层失败，解析在此收口，containers / items 恒空
    QString error;
    QList<ContainerData> containers;
    QList<ItemData> items;
    int skippedCount = 0; // 被跳过的条目数；warning 只在根层给出
};

// 解析 layer 应答：error 非空即止于文案、内容不解析；坏类型按未给，容器 id 非法或同层重复丢弃
LayerData parseLayer(const QByteArray &json);

} // namespace ScriptListSpec
