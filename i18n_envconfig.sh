#!/bin/bash

# 环境配置对话框中文化
sed -i 's/"Environment Configuration"/"环境配置"/g' src/env_config_dialog.cpp
sed -i 's/"Configure AI Applications"/"配置 AI 应用"/g' src/env_config_dialog.cpp
sed -i 's/"Apply your Aegisy API configuration to Claude Desktop, Cursor, and Continue.dev"/"将您的 Aegisy API 配置应用到 Claude Desktop、Cursor 和 Continue.dev"/g' src/env_config_dialog.cpp
sed -i 's/"Configuration"/"配置"/g' src/env_config_dialog.cpp
sed -i 's/"Target Applications"/"目标应用"/g' src/env_config_dialog.cpp
sed -i 's/"Claude Desktop"/"Claude Desktop"/g' src/env_config_dialog.cpp
sed -i 's/"Cursor Editor"/"Cursor 编辑器"/g' src/env_config_dialog.cpp
sed -i 's/"Continue.dev"/"Continue.dev"/g' src/env_config_dialog.cpp
sed -i 's/"Log:"/"日志:"/g' src/env_config_dialog.cpp
sed -i 's/"📦 Backup Current"/"📦 备份当前配置"/g' src/env_config_dialog.cpp
sed -i 's/"♻️ Restore Backup"/"♻️ 恢复备份"/g' src/env_config_dialog.cpp
sed -i 's/"✓ Apply Configuration"/"✓ 应用配置"/g' src/env_config_dialog.cpp
sed -i 's/"Invalid Input"/"输入无效"/g' src/env_config_dialog.cpp
sed -i 's/"Please enter an API Key."/"请输入 API Key。"/g' src/env_config_dialog.cpp
sed -i 's/"Please enter a Base URL."/"请输入 Base URL。"/g' src/env_config_dialog.cpp
sed -i 's/"No Target Selected"/"未选择目标"/g' src/env_config_dialog.cpp
sed -i 's/"Please select at least one application to configure."/"请至少选择一个应用进行配置。"/g' src/env_config_dialog.cpp
sed -i 's/"Confirm Configuration"/"确认配置"/g' src/env_config_dialog.cpp
sed -i 's/"This will modify your application configurations"/"这将修改您的应用配置"/g' src/env_config_dialog.cpp
sed -i 's/"A backup will be created automatically."/"将自动创建备份。"/g' src/env_config_dialog.cpp
sed -i 's/"Do you want to proceed?"/"是否继续？"/g' src/env_config_dialog.cpp
sed -i 's/"Success"/"成功"/g' src/env_config_dialog.cpp
sed -i 's/"Configuration applied successfully!"/"配置应用成功！"/g' src/env_config_dialog.cpp
sed -i 's/"Note: You may need to restart the applications for changes to take effect."/"注意：您可能需要重启应用以使更改生效。"/g' src/env_config_dialog.cpp
sed -i 's/"Creating backup..."/"创建备份..."/g' src/env_config_dialog.cpp
sed -i 's/"Configuring Claude Desktop..."/"配置 Claude Desktop..."/g' src/env_config_dialog.cpp
sed -i 's/"Configuring Cursor..."/"配置 Cursor..."/g' src/env_config_dialog.cpp
sed -i 's/"Configuring Continue.dev..."/"配置 Continue.dev..."/g' src/env_config_dialog.cpp
sed -i 's/"✓ Configuration applied successfully!"/"✓ 配置应用成功！"/g' src/env_config_dialog.cpp
sed -i 's/"✗ Configuration failed"/"✗ 配置失败"/g' src/env_config_dialog.cpp
sed -i 's/"No Backup"/"无备份"/g' src/env_config_dialog.cpp
sed -i 's/"No backup available to restore."/"没有可恢复的备份。"/g' src/env_config_dialog.cpp
sed -i 's/"No Backups"/"无备份"/g' src/env_config_dialog.cpp
sed -i 's/"No backup directory found."/"未找到备份目录。"/g' src/env_config_dialog.cpp
sed -i 's/"No backups available to restore."/"没有可恢复的备份。"/g' src/env_config_dialog.cpp
sed -i 's/"Select Backup"/"选择备份"/g' src/env_config_dialog.cpp
sed -i 's/"Choose a backup to restore:"/"选择要恢复的备份:"/g' src/env_config_dialog.cpp
sed -i 's/"Confirm Restore"/"确认恢复"/g' src/env_config_dialog.cpp
sed -i 's/"This will restore configurations from"/"这将从以下位置恢复配置"/g' src/env_config_dialog.cpp
sed -i 's/"Current configurations will be overwritten."/"当前配置将被覆盖。"/g' src/env_config_dialog.cpp
sed -i 's/"Restore Complete"/"恢复完成"/g' src/env_config_dialog.cpp
sed -i 's/"Successfully restored %1 configuration"/"成功恢复 %1 个配置文件"/g' src/env_config_dialog.cpp
sed -i 's/"Restore Failed"/"恢复失败"/g' src/env_config_dialog.cpp
sed -i 's/"No configuration files were found in the selected backup."/"在选定的备份中未找到配置文件。"/g' src/env_config_dialog.cpp
sed -i 's/"Partial Restore"/"部分恢复"/g' src/env_config_dialog.cpp
sed -i 's/"Some configuration files could not be restored."/"某些配置文件无法恢复。"/g' src/env_config_dialog.cpp
sed -i 's/"Check the log for details."/"请查看日志了解详情。"/g' src/env_config_dialog.cpp
sed -i 's/"Backup Complete"/"备份完成"/g' src/env_config_dialog.cpp
sed -i 's/"Backup created successfully!"/"备份创建成功！"/g' src/env_config_dialog.cpp
sed -i 's/"Location:"/"位置:"/g' src/env_config_dialog.cpp
sed -i 's/"Backup Failed"/"备份失败"/g' src/env_config_dialog.cpp
sed -i 's/"Some files could not be backed up."/"某些文件无法备份。"/g' src/env_config_dialog.cpp

echo "环境配置对话框中文化完成"
