#pragma once

#include "pch.h"

#include "ExternalTool.h"

// N142：外部工具路径选择控件——可编辑下拉（首项为自动项）+ 浏览按钮。
// 取值按文本语义（不依赖 currentIndex 是否被编辑动作重置），候选与自定义输入
// 共用同一条规则。
class ExternalToolPathCombo : public QWidget {
    Q_OBJECT
  public:
    // configDir 供候选发现使用（ini 所在目录；启动侧与设置对话框语义一致）；插件目录
    // 取 App::paths() 的出口在构造期定格（N147），与 configDir 一起注入 ExternalTool
    // 那两只函数（它们只吃参数、不读单点）
    ExternalToolPathCombo(const ExternalToolSpec &spec, const QString &configDir,
                          QWidget *parent = nullptr);

    // 逻辑值：自动 = 空串；编辑框文本去空白后为空、或等于自动项文案均视为自动，
    // 其余返回文本本身
    QString value() const;

    // 回填：空串选自动项并清空编辑框；非空按 data 精确匹配候选项，未命中以
    // 自定义文本显示。不触发 changed()（pending 更新由调用方负责）
    void setPath(const QString &path);

  signals:
    void changed();

  private:
    void browse();

    ExternalToolSpec m_spec;
    QString m_pluginDir;
    QString m_configDir;
    QString m_autoLabel;
    QComboBox *m_combo = nullptr;
};
