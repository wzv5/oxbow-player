#pragma once

#include "pch.h"

#include "installer/DependencyCatalog.h"

// N150：安装器唯一碰文件系统的一层——落位 / 统一交换（旧目标 rename 进 backup、新内容
// 落位、删 backup）、卸载、启动清理，加上"下载落到哪、解压产物整理成 stage"的全部路径
// 与目录动作。全部路径相对插件目录解析；临时根 .oxbow-install/ 与目标同卷，rename 才原子。
// 空插件目录（反查失败）在本层单点挡下：拼出的相对路径会落到进程 CWD，那里不是安装根。
// 可离线单测（QTemporaryDir 当插件目录）
namespace DependencySwap {

struct SwapResult {
    bool ok = false;
    QString backupPath; // 成功且 backup 删不掉时非空：旧版留在此，下次启动随临时根删
    QString error;      // 失败原因（英文），ok 时为空
};

// —— 路径（一律相对插件目录，不创建）——
QString tempRoot(const QString &pluginDir);
QString stageRoot(const QString &pluginDir);
QString backupRoot(const QString &pluginDir);
QString extractRoot(const QString &pluginDir);
QString pluginPath(const QString &pluginDir, const QString &relative);
// 下载落点：模型 = stage/<id>/<目标文件名>（整包即目标）；压缩包 = 临时根下的 <id>.pkg
QString downloadTargetPath(const QString &pluginDir, const DependencyCatalog::Component &comp);
// 建出下载落点的父目录并返回该落点（整包即目标的形态要落进 stage/<id>，那一层得现建；
// 临时根在进执行页时刚被整个清掉）。空根与建不成都返回空串并给原因
QString prepareDownloadTarget(const QString &pluginDir, const DependencyCatalog::Component &comp,
                              QString *error);
// 该组件的解压目录（extract/<id>）与暂存目录（stage/<id>）：一一对应、互不重叠
QString extractDirFor(const QString &pluginDir, const DependencyCatalog::Component &comp);
QString stageDirFor(const QString &pluginDir, const DependencyCatalog::Component &comp);

// 解压前把该组件的解压目录清成空目录
bool prepareExtractDir(const QString &pluginDir, const DependencyCatalog::Component &comp);
// 解压产物丢弃（成功整理完与失败都要丢，否则残渣留到下次启动）
void discardExtractDir(const QString &pluginDir, const DependencyCatalog::Component &comp);

// 按目录表的解压规格把 extract/<id> 整理成最终形态，一律落到 stage/<id>：目录型整取、
// 文件型只取声明的那一个条目。staged 返回可直接交给 swapIntoPlace 的路径（目录型 =
// stage/<id>，文件型 = stage/<id>/<文件名>）。必需条目缺失（含解压层数不对）即失败——
// 宁可判失败也不让"解压成功"落进错层
bool stageExtracted(const QString &pluginDir, const DependencyCatalog::Component &comp,
                    QString *staged, QString *error);

// 统一交换：旧目标存在时先 rename 进 backup/<backupId>.<时间戳>（失败即旧内容原样、
// 该项失败）；stagedPath rename 到目标位，失败则把 backup 换回；成功后删 backup——
// 删不掉不算失败，路径记进结果
SwapResult swapIntoPlace(const QString &pluginDir, const QString &backupId,
                         const QString &relativeTarget, const QString &stagedPath);

// 卸载：旧目标 rename 进 backup 即算完成（目标已不在）；随后删 backup，删不掉不算失败。
// rename 失败（占用等）才是失败，原内容不动
SwapResult removeFromPluginDir(const QString &pluginDir, const QString &backupId,
                               const QString &relativeTarget);

// 启动清理：删整个临时根（含 backup / stage / extract 与残留包）。不做跨启动恢复；
// 删不掉的项记日志，返回 false 不阻断调用方
bool cleanupTempRoot(const QString &pluginDir);

} // namespace DependencySwap
