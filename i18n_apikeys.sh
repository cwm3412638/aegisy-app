#!/bin/bash

# API Keys 对话框中文化
sed -i 's/"API Keys Management"/"API Keys 管理"/g' src/api_keys_dialog.cpp
sed -i 's/"Total: 0 keys"/"总计: 0 个 Key"/g' src/api_keys_dialog.cpp
sed -i 's/"Total: %1 keys"/"总计: %1 个 Key"/g' src/api_keys_dialog.cpp
sed -i 's/"🔄 Refresh"/"🔄 刷新"/g' src/api_keys_dialog.cpp
sed -i 's/"📋 Copy Key"/"📋 复制 Key"/g' src/api_keys_dialog.cpp
sed -i 's/"✓ Set as Active"/"✓ 设为活跃"/g' src/api_keys_dialog.cpp
sed -i 's/"Close"/"关闭"/g' src/api_keys_dialog.cpp
sed -i 's/"Name"/"名称"/g' src/api_keys_dialog.cpp
sed -i 's/"Status"/"状态"/g' src/api_keys_dialog.cpp
sed -i 's/"Quota"/"配额"/g' src/api_keys_dialog.cpp
sed -i 's/"Used"/"已用"/g' src/api_keys_dialog.cpp
sed -i 's/"Usage %"/"使用率"/g' src/api_keys_dialog.cpp
sed -i 's/"Created"/"创建时间"/g' src/api_keys_dialog.cpp
sed -i 's/"Loading API keys..."/"加载 API Keys..."/g' src/api_keys_dialog.cpp
sed -i 's/"No Selection"/"未选择"/g' src/api_keys_dialog.cpp
sed -i 's/"Please select an API key first."/"请先选择一个 API Key。"/g' src/api_keys_dialog.cpp
sed -i 's/"✓ API key copied to clipboard!"/"✓ API Key 已复制到剪贴板！"/g' src/api_keys_dialog.cpp
sed -i 's/"✓ Key.*is now active!"/"✓ Key '%1' 已设为活跃！"/g' src/api_keys_dialog.cpp
sed -i 's/"✓ Loaded %1 API keys"/"✓ 已加载 %1 个 API Keys"/g' src/api_keys_dialog.cpp
sed -i 's/"✗ Error: %1"/"✗ 错误: %1"/g' src/api_keys_dialog.cpp
sed -i 's/"Error"/"错误"/g' src/api_keys_dialog.cpp
sed -i 's/"Failed to load API keys"/"加载 API Keys 失败"/g' src/api_keys_dialog.cpp
sed -i 's/"Unlimited"/"无限制"/g' src/api_keys_dialog.cpp

echo "API Keys 对话框中文化完成"
