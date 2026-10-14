#pragma once

#include "pch.h"

// N42：滤镜列表条目模型（af / vf 共用）。
// text 是自由文本：@label: 标签、命名 / 位置参数、lavfi 图一律原样承载，
// 编辑器不做参数结构化拆分，因此不存在"表达不了 / 往返丢信息"的情形；
// enabled = 勾选态，取消勾选编译为 "!" 前缀（参数保留，mpv 照常记录禁用位）。
struct FilterEntry {
    bool enabled = true;
    QString text;
};

// N42：滤镜预设（一组条目，1..n 条），插入即整体追加为勾选态新条目；只作插入
// 模板，与条目模型无耦合。预置数据定义在 SettingsDialog.cpp（与设置目录同处，
// 翻译上下文归属 SettingsDialog，同 hwdecPresets 的归属方式），本类型仅为其载体。
// 滤镜名一律实测可用（不含 lavfi 壳：单条 ffmpeg 滤镜原生名直接可用）；增益档
// 必须带 dB 后缀（裸数字是线性倍数，volume=6 实为 +15.6 dB）
struct FilterPreset {
    const char *label;   // 未翻译原文（英文），菜单构建时经 tr() 翻译
    const char *name;    // 可翻译的 @name: 标签（插入时经 SettingsDialog 上下文翻译，
                         // 首条自动挂标签；空 = 不挂）。须以 QT_TRANSLATE_NOOP 标记
                         // 原文（翻译点按变量取原文，lupdate 无法自行提取）；
                         // 标签语法约束：不得含 , = : [ ] 与首尾空白，
                         // 翻译文案需自行遵守
    QStringList entries; // 插入的条目全文（不含 ! 前缀）；Custom 为单个空条目待用户填写
};

// N42：音视频滤镜列表编辑控件（af / vf 共用同一类）。
// 根容器为分组框（标题即目录项 label），框内：两列条目树（列 1 勾选 = 启停，
// 列 2 条目全文就地编辑、等宽字体；@label: 是全文的一部分，不单开列）、按钮行
// （预设插入 / 删除 / 上移 / 下移 / 复制整串 / 粘贴整串）、只读编译预览行。
// 条目为"纯文本 + 预设插入"：编辑器不解释参数语义、不校验滤镜名，合法性由
// Apply 时 mpv 下发结果裁决；顶层逗号（只能是条目分隔符）就地标红不阻断。
// 重建（回填 / 粘贴）保持当前选中行（N41 §2.8 的跨条目要求，本控件承担）。
// 预览行与 readControlValue 同走 compileEntries，所见即所下发。
//
// 滤镜串的编译 / 解析纯函数以静态成员收口在本类（规格 §2.5 原定 Util，实施期
// 维护者裁决移入：它们只服务于滤镜串场景，无跨场景通用性）；node 读侧映射
// （MpvController::filterEntries）与结构校验（hasTopLevelComma）亦经此取用。
class FilterListWidget : public QGroupBox {
    Q_OBJECT
  public:
    FilterListWidget(const QString &title, const QList<FilterPreset> &presets,
                     QWidget *parent = nullptr);

    // ——滤镜串编译 / 解析（纯函数，af / vf object settings list 语法）——
    // 语法：["@"<label>":"] ["!"] <filter-name> ["=" <参数> [":" <参数>]...]，
    // 条目以 "," 分隔；参数值可用 [...] 包裹（平衡嵌套引号）或
    // %<十进制长度>%<原文>（按长度引号）

    // 编译：条目列表 → 串。条目全文先去首尾空白，空白后为空的条目一律丢弃
    // （F5：空条目是顶层语法错误）；! 位（@ 开头含顶层冒号则在冒号之后，否则
    // 段首）上已带的手打 ! 归一进勾选位——多个也一并归一，只输出一个 !，不产生
    // !!name（mpv 语义下该位的 ! 只可能是禁用标记，归一无损）。全部条目被丢弃
    // 时返回空串（语义 = 无滤镜，与 LineEdit 时代的空值口径一致）
    static QString compileEntries(const QList<FilterEntry> &entries);

    // 解析（粘贴整串）：按顶层逗号切分（[ ] 深度栈 + %n% 引号整段跳过——mpv
    // 读回与本编辑器对"含不配平中括号且含逗号"值的编译输出都用 %n% 形态，漏掉
    // 它会把这类值拆错、打破复制 / 粘贴往返），每段只剥启用位 !（定位规则与
    // 编译侧同源，连续多个一并归一），余下整段即 text（@label: 是 text 的一部分，
    // 不剥离）。不解释参数语义、不校验滤镜名；空段保留为空条目（可见即可控，
    // 编译时才丢弃）；最坏情形只是条目边界不理想，绝不丢内容
    static QList<FilterEntry> parseString(const QString &s);

    // 参数值引号选择：值含 , = : / 首尾空白 / 以 [ 开头（mpv 把值首的 [ 一律
    // 当引号开启，裸写会被误读）时必须引号包裹——中括号配平用 [...]（可读、
    // 与手写形态一致，F9/F15）；不配平回退 %<长度>%<原文>（[...] 读到"最后一个
    // 未配对的 ]"，无法表达不配平值，F15）。长度按 UTF-8 字节计（mpv 内部按
    // C 字符串解析），写侧与扫描侧同口径（见 cpp 的 quotedCharSpan）。
    // 其余值原样返回（裸写）
    static QString quoteParamValue(const QString &value);

    // 结构校验（控件就地标红用）：条目全文是否含顶层逗号——顶层逗号只能是
    // 条目分隔符，值中的逗号必须用引号包裹
    static bool hasTopLevelComma(const QString &text);

    // N43：菜单显示文案——`名称: 参数`（半角冒号 + 一个空格）。名称 = 条目的
    // @label: 标签前缀（预置插入时挂的 @<预置名>:），参数 = 该前缀之后的条目
    // 全文（name 与 = 之后的参数串）；无标签前缀时省略名称部分、原样返回条目
    // 全文。标签前缀定位与编译 / 粘贴侧同源（labelColonPos），不另起扫描；
    // 前缀后无正文的畸形条目（@ln:）同样原样返回，不产生空尾段
    static QString entryDisplayText(const QString &text);

    // 当前条目列表（勾选态 → enabled，列 2 文本 → text）
    QList<FilterEntry> entries() const;
    // 整体重建条目（回填 / 粘贴整串）：不发 changed()（回填在 m_blockApply 守护内
    // 本也不会污染 pending，此处双保险）；保持当前选中行，本无选中则不强行选中
    void setEntries(const QList<FilterEntry> &entries);
    // 编译串（只读预览行内容与 Apply 下发串同源）
    QString compiledString() const;

  signals:
    // 任何用户侧变更（勾选 / 编辑 / 增删 / 移动 / 粘贴）统一发出
    void changed();

  private:
    // 追加若干勾选态条目（预设插入入口），滚动并选中新末条
    void appendEntries(const QStringList &texts);
    // 派生态统一刷新：顶层逗号标红、预览行、按钮启停。内部以 m_blockChanged
    // 守护对条目数据的写回（标红改 ForegroundRole 会再次触发 itemChanged）
    void refreshDerived();
    void updateButtons();

    QTreeWidget *m_tree = nullptr;
    QToolButton *m_addButton = nullptr;
    QToolButton *m_removeButton = nullptr;
    QToolButton *m_upButton = nullptr;
    QToolButton *m_downButton = nullptr;
    QLineEdit *m_preview = nullptr;
    QList<FilterPreset> m_presets;
    bool m_blockChanged = false;
};
