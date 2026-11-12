#pragma once

#include "pch.h"

namespace App {

// 进程级路径事实（N147）：核心插件目录与 oxbow.ini 落点的唯一出口。入口在
// QApplication 建好、setApplicationName 之后 initializePaths() 一次算定，先于一切
// 线程出生，此后任意线程只读；不做惰性补算。未初始化即取用属编程错误：Debug 断言，
// Release 返回空值。
class AppPaths {
  public:
    const QString &pluginDir() const; // 核心模块所在目录；反查失败照填空串
    const QString &iniFile() const;   // oxbow.ini 绝对路径（可写探针选定）
    const QString &iniDir() const;    // iniFile 的父目录

  private:
    friend void initializePaths();
    friend const AppPaths &paths();
    static AppPaths &instance();

    bool m_initialized = false;
    QString m_pluginDir;
    QString m_iniFile;
    QString m_iniDir;
};

void initializePaths();
const AppPaths &paths();

} // namespace App
