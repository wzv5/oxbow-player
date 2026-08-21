#pragma once

#include <QTabWidget>
#include <QStringList>
#include <QVariantList>
#include <QString>

class QListWidget;
class QListWidgetItem;

// 右侧播放列表：每个 tab 对应一个播放列表，tab 标题为列表名。
// 约定：第一个 tab 名为"默认"，不可删除；其余 tab 持久化保存到 INI 配置文件。
// 只有一个 tab 是"正在播放列表"（与 mpv 播放列表同步），初始为第一个 tab；
// 双击任意 tab 中的条目会把该 tab 整个载入 mpv 并播放对应条目，同时该 tab 成为新的同步目标。
class PlaylistWidget : public QTabWidget
{
    Q_OBJECT
public:
    explicit PlaylistWidget(const QString &configFile = QString(), QWidget *parent = nullptr);

    // 用 mpv 播放列表内容刷新"正在播放"tab，并高亮当前条目
    void setPlaylist(const QVariantList &items);
    // 向当前 tab 添加文件（触发 filesAdded 信号）
    void addFiles(const QStringList &files);

    QStringList filesOf(int tabIndex) const;
    int playingTabIndex() const { return m_playingTab; }
    bool currentIsPlayingTab() const { return currentIndex() == m_playingTab; }

    // 切换到指定 tab 并选中第 row 行
    void selectItem(int tabIndex, int row);
    // 当前 tab 中选中的行号，无选中项返回 -1
    int currentSelectedRow() const;
    // 转移“正在播放”同步目标到指定 tab（双击条目或会话恢复载入时调用）
    void markPlayingTab(int tabIndex) { m_playingTab = tabIndex; }

    // 持久化：从配置文件恢复非默认播放列表 / 保存除默认外的所有列表
    void loadPlaylists();
    void savePlaylists();

signals:
    // 双击条目：请求播放 files 中第 index 个；发出前已将该 tab 标记为正在播放列表
    void playRequested(const QStringList &files, int index);
    // 当前 tab 收到新文件（菜单添加或拖放）
    void filesAdded(const QStringList &files);
    // "正在播放"tab 请求移除指定行 / 清空（需同步到 mpv 播放列表，由 MainWindow 执行；
    // 本地 tab 的删除直接在界面完成，不发信号）
    void removeRequested(const QList<int> &rows);
    void clearRequested();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    QListWidget *createList();
    QListWidget *listAt(int index) const;

    int m_playingTab = 0;
    QString m_configFile;
};
