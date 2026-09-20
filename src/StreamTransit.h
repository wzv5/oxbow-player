#pragma once

#include "pch.h"

// N52 L1：mpvqt:// 转接 URI 语法唯一持有者（协议名与 scheme 常量只住在这里）。
//
// URI 形态（沿用 N51 已实证结论，逐字不改）：
//   mpvqt://<listUuid>/<条目段>
// 条目段 = 条目身份整体 QUrl::toPercentEncoding（unreserved 之外全编码，'/' 编成
// %2F）——整段只占一个路径段，段尾保留原扩展名，不挂显示名段，不追求可读性；
// fromPercentEncoding 严格互逆。
//
// 分层定位：本层是叶子。只依赖 QtCore 字符串运算，不认识 Util、不认识提供者、
// 不认识 mpv、不认识传输。允许依赖方：Util（折叠点）、MpvController（逐文件
// 选项）、StreamRelay（open 期 unwrap）。
//
// 前缀长度纪律（N51 坑 1）：一律由字符串运算得出，禁止手算字面量；派生量只在
// 本层 .cpp 内部运算，任何其他文件不得复制该数值。
namespace StreamTransit {

// —— 协议名与 scheme 常量唯一持有者（N52 §2.2）——
// L4 注册协议名取 kProtocol；URI 判定与铸造取 kScheme。二者同源，禁止在本层
// 之外复制协议名或手算前缀长度。
inline constexpr char kProtocol[] = "mpvqt";  // mpv_stream_cb_add_ro 的注册名
inline constexpr char kScheme[] = "mpvqt://"; // URI 前缀 = kProtocol + "://"

// 仅前缀判定（大小写不敏感，与 Util::isHttpUrl 同款串前缀口径，不解析 QUrl）。
bool isTransitUri(const QString &path);

// 铸造转接 URI：mpvqt://<uuid>/<percent-encoded identity>。
// 入参应为无凭据身份（由调用方——基类下发点——先 stripUrlCredentials 保证，
// 本层不依赖 Util、不代剥）；uuid 为不含路径分隔符的列表身份。
// 凭据双保险：Debug 用 Q_ASSERT 守候；Release 下检出 userinfo 形态即拒绝铸造
// （返回空串，调用方按 open 必失败处理），绝不产出含凭据的 URI。
// 身份不含 "://"（条目 id、本地路径等）时无 userinfo 可言，不受影响。
QString mint(const QString &uuid, const QString &identity);

// 还原条目身份：unwrap(mint(u, i)) == i。
// 非本协议、authority 空（uuid 段缺失）、无条目段、解出空串一律原样返回。
// 幂等：可安全重复施加——这是还原折叠点（Util::resolveSourcePath）能常驻
// 首行的前提。
QString unwrap(const QString &path);

// 逐文件 loadfile 选项（N52 §2.10）：凡走本协议的流，mpv 的 cache auto 判定
// 恒判本地（stream_cb 协议不在宿主内置网络协议名单），须逐文件申报 cache=yes。
// 非本协议返回空 map（调用方走既有无选项通道）。
// 申报口径按 scheme 而非按来源：cache=yes 治的是"mpv 认不出本协议是网络流"，
// 与传输种类无关，故留在 L1 前缀判定，不升成 ByteStream 的虚函数。
QVariantMap perFileOptions(const QString &path);

} // namespace StreamTransit
