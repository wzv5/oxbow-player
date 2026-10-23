#include "ui/OpenUrlDialog.h"

#include "Constants.h"
#include "Util.h"
#include "streams/StreamTransit.h"

namespace {
// 输入区下限：宽够容一条长 URL，高 6 行——够贴一份多行链接，不占满屏
constexpr int kEditMinWidth = 420;
constexpr int kEditMinLines = 6;
} // namespace

OpenUrlDialog::OpenUrlDialog(const QString &title, QWidget *parent) : QDialog(parent) {
    setWindowTitle(title);

    // QPlainTextEdit 没有 placeholder 槽，说明只能走标签
    auto *hint = new QLabel(tr("Enter file paths or URLs, one per line."), this);
    hint->setWordWrap(true);

    m_edit = new QPlainTextEdit(this);
    // 不自动换行：软换行会把一条长 URL 显示成两行，读起来像两条
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_edit->setMinimumWidth(kEditMinWidth);
    m_edit->setMinimumHeight(m_edit->fontMetrics().lineSpacing() * kEditMinLines);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // Browse… 不入按钮盒：Qt 在 Windows 下把 ActionRole 排在 RejectRole 之前（会得
    // "确定 / 浏览 / 取消"）；紧邻按钮盒右侧又与盒内间距不等。放最左与 OK / Cancel
    // 分成两簇，也合它"只改输入区、不关对话框"的身份
    auto *browseBtn = new QPushButton(tr("Browse…"), this);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(browseBtn);
    buttonRow->addStretch();
    buttonRow->addWidget(m_buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addWidget(m_edit, 1);
    layout->addLayout(buttonRow);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseBtn, &QPushButton::clicked, this, &OpenUrlDialog::browse);
    connect(m_edit, &QPlainTextEdit::textChanged, this, &OpenUrlDialog::refreshOkState);
    refreshOkState();
}

// N131：定形在这里补形态（相对串按 CWD 绝对化 + 转原生分隔符 / 收敛 http 书写）——
// 用户在对话框里明确输入，输什么算什么；拖放那两处只收已定形串，见 N132 §2.2。
// N133：转接串先丢再定形——toIdentity 首行会 unwrap，先定形就把 oxbow:// 折成直发
// 身份落进列表，绕过提供者端点与凭据，播必败却看不出原因
QStringList OpenUrlDialog::entries() const {
    QStringList out;
    for (const QString &seg : Util::Url::splitEntryText(m_edit->toPlainText())) {
        if (StreamTransit::isTransitUri(seg))
            continue;
        out.push_back(Util::Url::toNative(Util::Url::toIdentity(seg)));
    }
    return out;
}

void OpenUrlDialog::browse() {
    const QString patterns = Util::extensionPatterns(App::defaultMediaExtensions()).join(u' ');
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Open File"), QString(), tr("Media files (%1);;All files (*)").arg(patterns));
    if (files.isEmpty())
        return;
    // 整框替换而非追加：一条一行，与输入区本身的排布同形
    m_edit->setPlainText(files.join(u'\n'));
    QTextCursor cursor = m_edit->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_edit->setTextCursor(cursor);
}

void OpenUrlDialog::refreshOkState() {
    // 判据必须与 entries() 同一套（含转接过滤），否则"只剩转接串"时确定可点却点了没反应
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!entries().isEmpty());
}
