#include "pch.h"

#include "SubtitleBrowserDialog.h"

#include "AppearanceManager.h"
#include "Constants.h"
#include "Util.h"
#include "mpvbridge/MpvController.h"
#include "ui/ThemedIconEngine.h"

// 行模型：两列（有效起始时间 / 文本）。时间列显示"事件时间 + 当前 sub-delay"，
// 与进度条同一坐标系；延迟变化整列平移重算。定义在类前，与头文件的
// 前置声明同一实体（匿名命名空间会与之分家成两个类型）
class SubtitleBrowserModel : public QAbstractTableModel {
  public:
    enum Column { TimeColumn, TextColumn, ColumnCount };

    explicit SubtitleBrowserModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}

    void setEntries(const QList<SubtitleExtractor::SubtitleEntry> &entries) {
        beginResetModel();
        m_entries = entries;
        m_currentRow = -1; // 表已重建，旧当前行作废
        endResetModel();
    }

    void setDelay(double delay) {
        if (m_delay == delay)
            return;
        m_delay = delay;
        if (m_entries.isEmpty())
            return;
        emit dataChanged(index(0, TimeColumn), index(m_entries.size() - 1, TimeColumn),
                         {Qt::DisplayRole});
    }

    // 当前行变化只对旧行与新行发 dataChanged，不做整表刷新
    void setCurrentRow(int row) {
        const int old = std::exchange(m_currentRow, row);
        if (old == row)
            return;
        for (const int r : {old, row})
            if (r >= 0 && r < m_entries.size())
                emit dataChanged(index(r, 0), index(r, ColumnCount - 1));
    }

    int currentRow() const { return m_currentRow; }
    int entryCount() const { return m_entries.size(); }
    const SubtitleExtractor::SubtitleEntry *entryAt(int row) const {
        return row >= 0 && row < m_entries.size() ? &m_entries.at(row) : nullptr;
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : m_entries.size();
    }
    int columnCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : ColumnCount;
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole)
            return {};
        const auto &entry = m_entries.at(index.row());
        if (index.column() == TimeColumn) {
            const double end = entry.endSeconds + m_delay;
            if (!qIsFinite(end)) // 终点无上界（末块 end 缺失）不出右侧时间
                return formatOffsetTime(entry.startSeconds + m_delay);
            return formatOffsetTime(entry.startSeconds + m_delay) + timeSeparator() +
                   formatOffsetTime(end);
        }
        return entry.text;
    }

    // 负数时间（offset 后起点早于 0）：整个时间串前显示负号
    static QString formatOffsetTime(double seconds) {
        QString text = Util::formatTimeHMSms(qAbs(seconds));
        if (seconds < 0)
            text.prepend(u'-');
        return text;
    }
    static QString timeSeparator() { return QStringLiteral(" - "); }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        return section == TimeColumn ? tr("Time") : tr("Text");
    }

  private:
    QList<SubtitleExtractor::SubtitleEntry> m_entries;
    double m_delay = 0.0;
    int m_currentRow = -1;
};

// 绘制代理：配色全走归位后的外观出口——当前行正播蓝、四态行
// 前景、选中背景 selectionBackground，不写死色值
class SubtitleBrowserDelegate : public QStyledItemDelegate {
  public:
    explicit SubtitleBrowserDelegate(const SubtitleBrowserModel *model, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), m_model(model) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        const QPalette pal = option.palette;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        painter->save();
        painter->fillRect(option.rect, pal.color(QPalette::Base));
        if (selected)
            painter->fillRect(option.rect, AppearanceManager::selectionBackground(pal));
        painter->setPen(AppearanceManager::rowForeground(
            pal, false, index.row() == m_model->currentRow(), selected));
        constexpr int padding = 6;
        const QString text =
            painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(),
                                              Qt::ElideRight, option.rect.width() - padding * 2);
        painter->drawText(option.rect.adjusted(padding, 0, -padding, 0),
                          Qt::AlignVCenter | Qt::AlignLeading, text);
        painter->restore();
    }

  private:
    const SubtitleBrowserModel *m_model;
};

SubtitleBrowserDialog::SubtitleBrowserDialog(MpvController *controller,
                                             SubtitleExtractor *extractor, QWidget *parent)
    : QDialog(parent), m_controller(controller), m_extractor(extractor) {
    setWindowTitle(tr("Subtitle Browser"));
    setAttribute(Qt::WA_QuitOnClose, false); // 退出收口只归 MPV_EVENT_SHUTDOWN
    resize(560, 480);

    connect(
        m_extractor, &SubtitleExtractor::finished, this,
        [this](const QList<SubtitleExtractor::SubtitleEntry> &entries) { applyEntries(entries); });
    connect(m_extractor, &SubtitleExtractor::failed, this,
            [this](SubtitleExtractor::Failure reason) { applyFailure(reason); });
    connect(m_extractor, &SubtitleExtractor::progress, this, [this](double ratio) {
        if (m_maskProgress->maximum() == 0)
            m_maskProgress->setRange(0, 100); // ffmpeg 报出总时长，忙碌态切百分比
        m_maskProgress->setValue(int(ratio * 100));
    });

    m_model = new SubtitleBrowserModel(this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setItemDelegate(new SubtitleBrowserDelegate(m_model, m_view));
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection); // 多选 + 全选，配合右键复制
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setShowGrid(false);
    m_view->setWordWrap(false);
    m_view->verticalHeader()->hide();
    // 表头可见才有列宽拖拽：时间列可拖，文本列占满余量
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->horizontalHeader()->setSectionResizeMode(SubtitleBrowserModel::TimeColumn,
                                                     QHeaderView::Interactive);
    m_view->horizontalHeader()->setSectionResizeMode(SubtitleBrowserModel::TextColumn,
                                                     QHeaderView::Stretch);
    m_view->setColumnWidth(SubtitleBrowserModel::TimeColumn, 190);
    // 行高照播放列表：字号高 + 上下 4px 边距（PlaylistDelegate 同式）
    m_view->verticalHeader()->setDefaultSectionSize(m_view->fontMetrics().height() + 8);
    // 右键菜单：全选 / 复制（仅复制选中行的字幕内容）。右键点行就地选中该行
    // （标准列表行为）——复制入口因此对"没先左键选中"的用法也可用
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex underCursor = m_view->indexAt(pos);
        if (underCursor.isValid() && !m_view->selectionModel()->isSelected(underCursor))
            m_view->selectRow(underCursor.row());
        QMenu menu(this);
        QAction *all = menu.addAction(tr("Select All"));
        QAction *copy = menu.addAction(tr("Copy"));
        copy->setEnabled(m_view->selectionModel()->hasSelection());
        QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
        if (chosen == all)
            m_view->selectAll();
        else if (chosen == copy)
            copySelectedTexts();
    });
    // 双击条目才 seek：单击只做选中，避免浏览滚动时的误跳转
    connect(m_view, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto *entry = m_model->entryAt(index.row());
        if (!entry)
            return;
        m_controller->commandAsync(QStringList{
            QStringLiteral("seek"), QString::number(entry->startSeconds + m_delay, 'f', 3),
            QStringLiteral("absolute")});
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    buildToolbar(layout);
    layout->addWidget(m_view, 1);

    // 遮罩最后创建并置顶；几何在 resizeEvent 跟随
    m_mask = new QWidget(this);
    m_mask->setVisible(false);
    auto *maskLayout = new QVBoxLayout(m_mask);
    maskLayout->addStretch(1);
    m_maskLabel = new QLabel(m_mask);
    m_maskLabel->setAlignment(Qt::AlignCenter);
    m_maskLabel->setWordWrap(true);
    maskLayout->addWidget(m_maskLabel);
    m_maskProgress = new QProgressBar(m_mask);
    m_maskProgress->setVisible(false);
    maskLayout->addWidget(m_maskProgress);
    m_maskCancel = new QPushButton(tr("Cancel"), m_mask);
    maskLayout->addWidget(m_maskCancel, 0, Qt::AlignCenter);
    maskLayout->addStretch(1);
    // 直接关闭对话框等同于取消：取消按钮走同一收口（关 = 取消）
    connect(m_maskCancel, &QPushButton::clicked, this, &QDialog::close);

    // 推送折叠：单次 0 间隔定时器把一批 time-pos 合并成一次处理
    m_timePosCoalescer.setSingleShot(true);
    connect(&m_timePosCoalescer, &QTimer::timeout, this,
            [this] { computeCurrentRow(m_pendingTimePos); });

    retintIcons();
    // observe 注册即推过初值，但那早于本对话框出生——构造时现读一次实际值
    setSubDelay(m_controller->propertyDouble(QStringLiteral("sub-delay"), 0.0));
    m_pendingTimePos = m_controller->propertyDouble(QStringLiteral("time-pos"), 0.0);
}

void SubtitleBrowserDialog::buildToolbar(QVBoxLayout *layout) {
    auto *bar = new QHBoxLayout;
    // 图标按钮：正方形（24×24，图标 16，与播放列表底栏同度量），语义靠 tooltip
    auto makeButton = [this, bar](const QString &tooltip) {
        auto *button = new QToolButton(this);
        button->setToolTip(tooltip);
        button->setFocusPolicy(Qt::NoFocus);
        // 与主窗控制栏按钮同度量（36×36 / 图标 24，见 MainWindow 底栏）
        button->setIconSize(QSize(24, 24));
        button->setFixedSize(36, 36);
        bar->addWidget(button);
        return button;
    };
    // 符号约定写死在规格：sub-delay 为正 = 字幕延后出现
    m_earlierButton = makeButton(tr("Show subtitles 0.5s earlier"));
    connect(m_earlierButton, &QToolButton::clicked, this,
            [this] { m_controller->setProperty(QStringLiteral("sub-delay"), m_delay - 0.5); });
    m_offsetEdit = new QLineEdit(this);
    m_offsetEdit->setToolTip(tr("Subtitle offset in seconds"));
    // 按常见量级取宽：显示 "999.999" 即可，多数调整不会超出
    m_offsetEdit->setFixedWidth(
        m_offsetEdit->fontMetrics().horizontalAdvance(QStringLiteral("999.999")) + 16);
    m_offsetEdit->setAlignment(Qt::AlignRight);
    connect(m_offsetEdit, &QLineEdit::textEdited, this, [this] { m_offsetDirty = true; });
    connect(m_offsetEdit, &QLineEdit::returnPressed, this, &SubtitleBrowserDialog::commitOffset);
    connect(m_offsetEdit, &QLineEdit::editingFinished, this,
            &SubtitleBrowserDialog::updateOffsetDisplay);
    bar->addWidget(m_offsetEdit);
    m_laterButton = makeButton(tr("Show subtitles 0.5s later"));
    connect(m_laterButton, &QToolButton::clicked, this,
            [this] { m_controller->setProperty(QStringLiteral("sub-delay"), m_delay + 0.5); });
    m_resetButton = makeButton(tr("Reset subtitle offset"));
    connect(m_resetButton, &QToolButton::clicked, this,
            [this] { m_controller->setProperty(QStringLiteral("sub-delay"), 0.0); });
    m_locateButton = makeButton(tr("Scroll to the current subtitle line"));
    connect(m_locateButton, &QToolButton::clicked, this, &SubtitleBrowserDialog::locateCurrentRow);

    m_searchBox = new QLineEdit(this);
    m_searchBox->setPlaceholderText(tr("Search"));
    m_searchBox->setClearButtonEnabled(true);
    m_searchAction = m_searchBox->addAction(QIcon(), QLineEdit::TrailingPosition);
    connect(m_searchBox, &QLineEdit::returnPressed, this, &SubtitleBrowserDialog::searchNext);
    bar->addWidget(m_searchBox, 1); // 搜索框以伸展因子占满余量

    layout->addLayout(bar);
}

bool SubtitleBrowserDialog::event(QEvent *event) {
    // 外观事件重染搜索装饰图标，与播放列表同款
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        retintIcons();
    return QDialog::event(event);
}

void SubtitleBrowserDialog::retintIcons() {
    // 五枚图标的唯一赋值点（外观事件重染与初值共用，与播放列表底栏同款）
    QColor tint = palette().color(QPalette::Text);
    m_earlierButton->setIcon(Ui::themedIcon(App::iconPath(App::iconRewind()), tint));
    m_laterButton->setIcon(Ui::themedIcon(App::iconPath(App::iconFastForward()), tint));
    m_resetButton->setIcon(Ui::themedIcon(App::iconPath(App::iconReset()), tint));
    m_locateButton->setIcon(Ui::themedIcon(App::iconPath(App::iconLocatePlaying()), tint));
    tint.setAlpha(128);
    m_searchAction->setIcon(Ui::themedIcon(App::iconPath(App::iconSearch()), tint));
}

void SubtitleBrowserDialog::showEvent(QShowEvent *event) {
    QDialog::showEvent(event);
    refresh(); // 打开面板那一刻按当前状态立即走一遍取数流程（缓存命中即复用）
}

void SubtitleBrowserDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    if (m_mask)
        m_mask->setGeometry(rect());
}

void SubtitleBrowserDialog::setTracks(const QString &mediaPath,
                                      const QList<MpvController::TrackInfo> &tracks) {
    m_mediaPath = mediaPath;
    m_tracks = tracks;
    m_idle = false;
    refresh();
}

void SubtitleBrowserDialog::setMediaIdle() {
    m_idle = true;
    refresh();
}

void SubtitleBrowserDialog::setTimePos(double seconds) {
    m_pendingTimePos = seconds;
    if (!m_timePosCoalescer.isActive())
        m_timePosCoalescer.start(); // 心跳停止时自然不再触发
}

void SubtitleBrowserDialog::setSubDelay(double delay) {
    m_delay = delay;
    m_model->setDelay(delay);
    updateOffsetDisplay();
    computeCurrentRow(m_pendingTimePos); // 暂停期没有心跳推送，换行判定就地重算
}

// 重取触发点的统一收口：面板不可见一律只存态不取数；停止态清空列表
// 只留原因串；有媒体则选当前主选文本字幕轨，同一请求已在飞或已出表即不动
void SubtitleBrowserDialog::refresh() {
    if (!isVisible())
        return;
    const MpvController::TrackInfo *current = nullptr;
    if (!m_idle) {
        for (const auto &track : m_tracks) {
            if (SubtitleExtractor::isBrowsableTextSubtitle(track)) {
                current = &track;
                break;
            }
        }
    }
    if (!current) {
        m_extractor->cancel();
        m_model->setEntries({});
        m_currentRow = -1;
        m_entriesGeneration++;
        m_searchAnchor = -1;
        // 停止态与"选中的不是文本字幕轨"同提示：此刻没有可浏览的文本字幕轨
        showMask(tr("No usable text subtitle track"), false);
        return;
    }
    if (m_extractor->isCurrentRequest(*current, m_mediaPath))
        return;
    // 缓存命中与直读路径会同步发 finished（applyEntries 在本调用栈内执行），
    // 故先把遮罩切到加载态再发起；总时长由 ffmpeg 自己报，报不出就保持忙碌态
    showLoadingMask();
    m_extractor->start(*current, m_mediaPath);
}

void SubtitleBrowserDialog::showLoadingMask() {
    showMask(tr("Loading subtitles"), true);
    m_maskProgress->setRange(0, 0); // 总时长未知：不定态忙碌条，progress 信号到位再切
    m_maskProgress->show();
}

void SubtitleBrowserDialog::showMask(const QString &message, bool cancelable) {
    m_maskLabel->setText(message);
    m_maskCancel->setVisible(cancelable);
    m_maskCancel->setEnabled(true);
    m_maskProgress->setVisible(false); // 进度只属加载态，由 showLoadingMask 单独打开
    m_mask->setGeometry(rect());
    m_mask->raise();
    m_mask->setVisible(true);
}

void SubtitleBrowserDialog::hideMask() { m_mask->setVisible(false); }

void SubtitleBrowserDialog::applyEntries(const QList<SubtitleExtractor::SubtitleEntry> &entries) {
    m_entriesGeneration++;
    m_searchAnchor = -1;
    m_currentRow = -1;
    m_model->setEntries(entries);
    hideMask();
    // 唯一定位时机：新条目表就绪即自动滚动到当前行一次并高亮；此后播放过程不再
    // 自动滚动
    computeCurrentRow(m_pendingTimePos);
    locateCurrentRow();
}

void SubtitleBrowserDialog::applyFailure(SubtitleExtractor::Failure reason) {
    m_entriesGeneration++;
    m_searchAnchor = -1;
    m_currentRow = -1;
    m_model->setEntries({});
    switch (reason) {
    case SubtitleExtractor::Failure::NotLocal:
        showMask(tr("Only local media and local subtitle files are supported"), false);
        break;
    case SubtitleExtractor::Failure::FfmpegMissing:
        showMask(tr("FFmpeg not found. Set its path in Settings, under External Tools."), false);
        break;
    case SubtitleExtractor::Failure::FetchFailed:
        showMask(tr("Failed to extract subtitles"), false);
        break;
    }
}

// 当前行两级优化：事件坐标系 t = time-pos - sub-delay，行区间
// [start, end)。一级：命中缓存行 O(1)；二级：按方向探测相邻若干行；仍不命中才二分
// （大幅跳转 / 源乱序；重叠区间向后多探几行）。只换文字颜色，不滚动
void SubtitleBrowserDialog::computeCurrentRow(double timePos) {
    const int count = m_model->entryCount();
    if (count == 0) {
        m_currentRow = -1;
        m_model->setCurrentRow(-1);
        return;
    }
    const double t = timePos - m_delay;
    auto covers = [this, t](int row) {
        const auto *entry = m_model->entryAt(row);
        return entry && entry->startSeconds <= t && t < entry->endSeconds;
    };
    int row = -1;
    if (m_currentRow >= 0 && m_currentRow < count && covers(m_currentRow)) {
        row = m_currentRow; // 命中缓存即返回（常态走这一条）
    } else if (m_currentRow >= 0) {
        // 播放推进只向前，先向后探；seek 则前后各探一段
        for (int i = m_currentRow + 1, end = qMin(count, m_currentRow + 1 + 32); i < end; ++i)
            if (covers(i)) {
                row = i;
                break;
            }
        if (row < 0) {
            for (int i = m_currentRow - 1, end = qMax(-1, m_currentRow - 32); i > end; --i)
                if (covers(i)) {
                    row = i;
                    break;
                }
        }
    }
    if (row < 0) {
        // 按起点二分到候选行，再对向前的重叠区间各探若干行
        int lo = 0, hi = count; // 第一个 start > t 的下标
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (m_model->entryAt(mid)->startSeconds <= t)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (int i = lo - 1, end = qMax(-1, lo - 1 - 32); i > end; --i)
            if (i >= 0 && covers(i)) {
                row = i;
                break;
            }
    }
    m_currentRow = row;
    m_model->setCurrentRow(row);
}

void SubtitleBrowserDialog::locateCurrentRow() {
    const int count = m_model->entryCount();
    if (count == 0)
        return; // 空表无行可定位
    int row = m_currentRow;
    if (row < 0) {
        // 无当前行（时间不落在任何条目区间）：滚动并选中到最接近当前时间的行
        const double t = m_pendingTimePos - m_delay;
        int lo = 0, hi = count; // 第一个 start > t 的下标
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (m_model->entryAt(mid)->startSeconds <= t)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == 0)
            row = 0; // 时间在首行之前 → 第一行
        else if (lo == count)
            row = count - 1; // 时间在末行之后 → 最后一行
        else
            row = (t - m_model->entryAt(lo - 1)->startSeconds <=
                           m_model->entryAt(lo)->startSeconds - t
                       ? lo - 1
                       : lo);
    }
    m_view->selectRow(row);
    m_view->scrollTo(m_model->index(row, 0), QAbstractItemView::PositionAtCenter);
}

// 右键复制：仅复制选中行的字幕内容，按行序拼接
void SubtitleBrowserDialog::copySelectedTexts() {
    QList<int> rows;
    for (const QModelIndex &index : m_view->selectionModel()->selectedRows())
        rows << index.row();
    if (rows.isEmpty())
        return;
    std::sort(rows.begin(), rows.end());
    QStringList texts;
    texts.reserve(rows.size());
    for (int row : rows)
        texts << m_model->entryAt(row)->text;
    QGuiApplication::clipboard()->setText(texts.join(u'\n'));
}

// 搜索推进（照播放列表语义）：对条目文本做不区分大小写的包含匹配，
// 只找下一条不过滤列表；查询串相同且锚点有效（行号在范围内 + 该行文本仍包含查询串 +
// 条目表代数未变）才续搜，否则从头起搜；回绕命中与全表无命中都 beep 一次；
// 命中后选中并居中滚动，不夺焦、不触发 seek
void SubtitleBrowserDialog::searchNext() {
    const QString query = m_searchBox->text();
    if (query.isEmpty()) {
        m_searchQuery.clear();
        m_searchAnchor = -1;
        return; // 空串回车不动作：不回绕、不发声、不报错
    }
    const int count = m_model->entryCount();
    const bool resume = m_searchQuery == query && m_generationAtAnchor == m_entriesGeneration &&
                        m_searchAnchor >= 0 && m_searchAnchor < count &&
                        m_model->entryAt(m_searchAnchor)->text.contains(query, Qt::CaseInsensitive);
    const int last = count - 1;
    const int wrapEnd = resume ? m_searchAnchor : last;
    auto contains = [this, &query](int row) {
        return m_model->entryAt(row)->text.contains(query, Qt::CaseInsensitive);
    };
    int hit = -1;
    for (int r = resume ? m_searchAnchor + 1 : 0; r <= last; ++r) {
        if (contains(r)) {
            hit = r;
            break;
        }
    }
    if (hit < 0) {
        // 只有"顺着往下直接命中的那一段"不发声：一旦进入回绕段就响一声，故回绕
        // 命中与全表无命中都响、且同一次回车至多一声
        QApplication::beep();
        if (resume) { // 新查询的第一段已是整表一趟，无需再扫
            for (int r = 0; r <= wrapEnd; ++r) {
                if (contains(r)) {
                    hit = r;
                    break;
                }
            }
        }
    }
    if (hit < 0)
        return; // 全表无命中：选中与滚动都不动，锚点保留（下次回车等价重试）
    m_view->selectRow(hit);
    m_view->scrollTo(m_model->index(hit, 0), QAbstractItemView::PositionAtCenter);
    m_searchQuery = query;
    m_generationAtAnchor = m_entriesGeneration;
    m_searchAnchor = hit;
}

// 偏移提交：回车生效，非法输入与写入失败一律忽略并
// 回落实际值
void SubtitleBrowserDialog::commitOffset() {
    m_offsetDirty = false;
    QString text = m_offsetEdit->text();
    text.replace(u',', u'.');
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !qIsFinite(value))
        return;
    if (m_controller->setProperty(QStringLiteral("sub-delay"), value))
        setSubDelay(value); // 写入成功先乐观落账，observe 推送随后对账
}

// 偏移显示：持焦且有未提交输入时不回写（否则半截输入被 observe
// 推送覆盖）；失焦未回车即重置为当前实际值（不保留半截输入）
void SubtitleBrowserDialog::updateOffsetDisplay() {
    if (m_offsetEdit->hasFocus() && m_offsetDirty)
        return;
    m_offsetEdit->setText(QString::number(m_delay));
}
