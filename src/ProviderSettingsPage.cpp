#include "pch.h"

#include "ProviderSettingsPage.h"

// 基类只承载接口：构造转发父窗口，三个纯虚由各提供者的页面实现
ProviderSettingsPage::ProviderSettingsPage(QWidget *parent) : QWidget(parent) {}
