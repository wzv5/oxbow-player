#pragma once

#include "pch.h"

// N150：安装器钉死源串的唯一清单。照 AiSubtitleCatalog 的分工——常量式源文案
// 带 QT_TRANSLATE_NOOP 标注、取值函数 translate：lupdate 只认这两个抽取点，裸字面量经
// 取值函数转手它一概看不见（漏了就编译过、英文显影过、.ts 里根本没这条）。标注里的上下文
// 一律写字符串字面量（lupdate 按字面量认上下文），取值侧同一串收在 wizText / catalogText。
namespace InstallerText {

inline QString wizText(const char *source) {
    return QCoreApplication::translate("DependencyInstallerWizard", source);
}
inline QString wizText(const char *source, const QString &arg1) {
    return QCoreApplication::translate("DependencyInstallerWizard", source).arg(arg1);
}
inline QString catalogText(const char *source) {
    return QCoreApplication::translate("DependencyCatalog", source);
}

// —— 入口与窗口 ——
constexpr const char *kWizardTitle =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Install Optional Dependencies");

// —— 第一页 ——
constexpr const char *kPageSelectTitle =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Select Components");
constexpr const char *kPageSelectSubtitle =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard",
                      "Choose which optional dependencies to install, reinstall, or remove.");
constexpr const char *kColumnName = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Name");
constexpr const char *kColumnLatest =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Latest Version");
constexpr const char *kColumnInstalled =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Installed Version");
constexpr const char *kColumnSize = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Download Size");
constexpr const char *kColumnDescription =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Description");
constexpr const char *kHintCheckStates = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard",
    "Checked = install or reinstall. Unchecked = remove or leave uninstalled. Half-checked = "
    "keep as is.");
constexpr const char *kHintInstallLocation =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Install Location: %1");
constexpr const char *kHintClosingPlayer = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard", "Closing the player interrupts an ongoing install.");
constexpr const char *kStatusChecking =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Checking\u2026");
constexpr const char *kStatusNewVersions =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "New versions available");
constexpr const char *kCellFailed = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Failed");
constexpr const char *kCellInstalled = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Installed");
constexpr const char *kTipVersionFailed =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Latest version lookup failed: %1");
constexpr const char *kTipSizeFailed =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Download size lookup failed: %1");

// —— 按钮与第二页 ——
constexpr const char *kButtonInstall = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Install");
constexpr const char *kButtonClose = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Close");
constexpr const char *kButtonCancelling =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Cancelling\u2026");
constexpr const char *kPageExecuteTitle =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Installing");
constexpr const char *kConfirmCancelTitle =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Cancel Installation");
constexpr const char *kConfirmCancelBody =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Do you want to cancel the ongoing "
                                                   "installation?");

// —— 确认框 ——
constexpr const char *kGroupRemoved =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Will be removed:");
constexpr const char *kGroupInstalled =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Will be installed:");
constexpr const char *kGroupSkipped =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Unavailable items that will be skipped:");
constexpr const char *kGroupContinue = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Continue?");
constexpr const char *kWarnDiskSpace = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard",
    "Warning: the plugin drive may not have enough free space (need about %1, available %2).");
constexpr const char *kDialogTitleNotWritable =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Plugin Directory Not Writable");
constexpr const char *kDialogBodyNotWritable = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard", "Oxbow cannot write to the plugin directory:\n%1\n\n%2");

// —— 步骤行与摘要 ——
constexpr const char *kStepDownloading =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Downloading %1");
constexpr const char *kStepExtracting =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Extracting %1");
constexpr const char *kStepInstalling =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Installing %1");
constexpr const char *kStepReinstalling =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Reinstalling %1");
constexpr const char *kStepRemoving = QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Removing %1");
constexpr const char *kSummaryLine = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard", "%1 installed, %2 removed, %3 failed, %4 skipped");

// —— 钉死的日志串 ——
constexpr const char *kLogSkipped =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Skipped (unavailable): %1");
constexpr const char *kLogLegacyWhisper = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard", "Legacy whisper/ folder detected (not migrated)");
constexpr const char *kLogDownloadRetry =
    QT_TRANSLATE_NOOP("DependencyInstallerWizard", "Download retry %1 of %2: %3");
constexpr const char *kLogHostSwitch = QT_TRANSLATE_NOOP(
    "DependencyInstallerWizard", "Host %1 failed before any byte (%2), switching to %3: %4");

// —— 字符占位（不进翻译表：— 与 … 是字符不是文案）——
inline QString emptyCell() { return QStringLiteral("\u2014"); }
inline QString pendingCell() { return QStringLiteral("\u2026"); }

} // namespace InstallerText
