#pragma once

#include "pch.h"

// N89：input.conf 保真读写（与 MpvConf 同建制、并行独立）。
// - 四种行：Empty / Comment / KeyValue / Directive（变更记录 1：无 Section——
//   input.conf 无整行区头语法，唯一的区语法是行内 {区名} 前缀，归 Directive）
// - 行原样保留（缩进 / 中间空白 / 行尾空白 / CR）；只有被编辑的行按字段重组，
//   其余行逐字节回写；逐行的 \r 留在 raw 里，换行符本身按文件级形态再现（原文件
//   末尾有无换行 + 是否 CRLF），故追加与删除都不会让别的行的行尾符漂移
// - 被行首 "# " 注释掉的绑定行认领为 disabled 的 KeyValue 行；认领判据与
//   disableLine 的写出严格互逆（断言守，限键名合法的行——键名反解不出的行按
//   变更记录 2 不被认领），#f ignore 等不规整形态同样命中
// - Directive（default-bindings start / <键> {区名} <命令>）按异常数据处理：
//   认出、原样回写、不显示、不参与任何判据。default-bindings stop 不存在该
//   语法（变更记录 3），按"键名解析不出来的行"照常显示
// - 不认识 mpv：键名合法性经注入的判定器（变更记录 2，对话框接 MpvKeyName
//   反解）；不做同键分组与归一化（归 MpvBindingDialog 经 MpvKeyName 完成）
struct InputConfLine {
    enum class Kind { Empty, Comment, KeyValue, Directive };
    Kind kind = Kind::Empty;
    QString raw; // 原行内容（含行尾 \r；不含换行符本身；未编辑行逐字节回写）
    // KeyValue 专用：
    bool disabled = false; // 被行首 "# "（或 "#<空白>" 不规整形态）注释掉的绑定行
    QString indent;        // 行首缩进（禁用在其后插 "# "；启用时正文左移到它之后）
    QString key;           // 键名原文（未归一化；解不出合法键名的行同样存原文）
    QString command;       // 命令串整体（含多命令 "a cmd1 ; cmd2"；不含行末注释）
    QString comment;       // 行末注释（引号外首个 "#" 之后的文本，不含该 "#"）
};

class InputConf {
  public:
    // 键名合法性判定器：仅用于认领判据（变更记录 2）。由调用方注入——对话框接
    // MpvKeyName 的反解判定，单元测试给简表判定；本类不认识 mpv。
    using KeyValidator = std::function<bool(const QString &)>;

    InputConf() = default; // std::optional 需要默认构造；空 path + 空 lines
    // 加载现有文件；不存在则返回空 lines 的实例（commit 时创建）。
    // 文件存在但读不到（不存在 / 打不开 / 权限）返回 nullopt——调用方须按"不可编辑"
    // 处置：读不到却照样写盘，等于用新内容盖掉一份没能读进内存的原文件。
    static std::optional<InputConf> open(const QString &path, const KeyValidator &validator);

    const QList<InputConfLine> &lines() const { return m_lines; }

    // —— 行级编辑（index 指 lines() 下标；类型不符属调用方违约，撞断言）——
    // 活动 KeyValue 行 → 行首缩进之后插入固定的 "# "，其后正文一字节不改
    void disableLine(int index);
    // 认领行 → 删掉认领时实际剥掉的那一段（"#"+其后一段连续空白），正文左移到原缩进后
    void enableLine(int index);
    void removeLine(int index);
    // 就地重组该行（仅 KeyValue 行；命令串按不透明文本原样写入，引号 / 转义由
    // 调用方按 input.conf 语法自理，变更记录 4）
    void setLineFields(int index, const QString &key, const QString &command,
                       const QString &comment);
    // 末尾追加一条活动自定义行，返回其下标
    int appendLine(const QString &key, const QString &command, const QString &comment);

    // 写盘（QSaveFile 原子写，UTF-8 无 BOM）
    bool commit();

  private:
    bool parse(); // 读失败交 false（open 据此回 nullopt）
    QString serialize() const;
    // 单行分类：Empty / Comment（认领不中）/ KeyValue（含认领命中的禁用行）/ Directive
    void classify(InputConfLine &l) const;
    // 认领解析：判 "#..." 行是否被禁用的绑定行（行首缩进 → 剥 "#" 与其后一段
    // 连续空白 → 剩余部分先排除两类 Directive 的注释形态，再解析成 <键> <命令>，
    // 键名经判定器认合法）。命中时填充 line（KeyValue + disabled）并返回 true。
    bool claimDisabled(const QString &raw, InputConfLine *line) const;

    QString m_path;
    KeyValidator m_validator;
    QList<InputConfLine> m_lines;
    // 换行符按文件级形态记：逐行的 \r 留在 raw 里，换行符本身只有一条规则——末行可
    // 缺（原文件无末尾换行时原样再现）。故追加 / 删除不改动他行的行尾状态。
    bool m_trailingNewline = true;         // 当前末行是否后随换行符
    bool m_originalTrailingNewline = true; // 原文件的末行终止态（删掉末行即回它）
    bool m_crlf = false;                   // 原文件用 CRLF（新增行随它）
};
