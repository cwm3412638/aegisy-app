#!/bin/bash

# 环境管理对话框中文化
sed -i 's/"Environment Manager"/"环境管理"/g' src/env_manager_dialog.cpp
sed -i 's/"Environments"/"环境列表"/g' src/env_manager_dialog.cpp
sed -i 's/"Environment Details"/"环境详情"/g' src/env_manager_dialog.cpp
sed -i 's/"➕ Add"/"➕ 新建"/g' src/env_manager_dialog.cpp
sed -i 's/"✏️ Edit"/"✏️ 编辑"/g' src/env_manager_dialog.cpp
sed -i 's/"🗑️ Delete"/"🗑️ 删除"/g' src/env_manager_dialog.cpp
sed -i 's/"✓ Activate Environment"/"✓ 激活环境"/g' src/env_manager_dialog.cpp
sed -i 's/"Select an environment to view details"/"选择一个环境以查看详情"/g' src/env_manager_dialog.cpp
sed -i 's/"No environments configured. Click.*Add.*to create one."/"未配置环境。点击.*新建.*创建一个。"/g' src/env_manager_dialog.cpp
sed -i 's/"Cannot Delete"/"无法删除"/g' src/env_manager_dialog.cpp
sed -i 's/"Cannot delete the active environment"/"无法删除活跃的环境"/g' src/env_manager_dialog.cpp
sed -i 's/"Please activate another environment first."/"请先激活其他环境。"/g' src/env_manager_dialog.cpp
sed -i 's/"Confirm Delete"/"确认删除"/g' src/env_manager_dialog.cpp
sed -i 's/"Are you sure you want to delete the environment"/"确定要删除环境"/g' src/env_manager_dialog.cpp
sed -i 's/"This action cannot be undone."/"此操作无法撤销。"/g' src/env_manager_dialog.cpp
sed -i 's/"✓ Environment.*deleted"/"✓ 环境 '%1' 已删除"/g' src/env_manager_dialog.cpp
sed -i 's/"✗ Failed to delete environment"/"✗ 删除环境失败"/g' src/env_manager_dialog.cpp
sed -i 's/"Confirm Activation"/"确认激活"/g' src/env_manager_dialog.cpp
sed -i 's/"Activate environment"/"激活环境"/g' src/env_manager_dialog.cpp
sed -i 's/"This will apply the configuration to selected applications."/"这将应用配置到选定的应用。"/g' src/env_manager_dialog.cpp
sed -i 's/"✓ Environment.*activated!"/"✓ 环境 '%1' 已激活！"/g' src/env_manager_dialog.cpp
sed -i 's/"✗ Failed to activate environment"/"✗ 激活环境失败"/g' src/env_manager_dialog.cpp
sed -i 's/"Environment Activated"/"环境已激活"/g' src/env_manager_dialog.cpp
sed -i 's/"Environment.*is now active!"/"环境 '%1' 现已激活！"/g' src/env_manager_dialog.cpp
sed -i 's/"Note: You may need to restart your applications"/"注意：您可能需要重启应用"/g' src/env_manager_dialog.cpp
sed -i 's/"for the changes to take effect."/"以使更改生效。"/g' src/env_manager_dialog.cpp
sed -i 's/"✓ Environment.*updated"/"✓ 环境 '%1' 已更新"/g' src/env_manager_dialog.cpp
sed -i 's/"✗ Failed to update environment"/"✗ 更新环境失败"/g' src/env_manager_dialog.cpp
sed -i 's/"✓ Environment.*created"/"✓ 环境 '%1' 已创建"/g' src/env_manager_dialog.cpp
sed -i 's/"Edit Environment"/"编辑环境"/g' src/env_manager_dialog.cpp
sed -i 's/"New Environment"/"新建环境"/g' src/env_manager_dialog.cpp
sed -i 's/"Create New Environment"/"创建新环境"/g' src/env_manager_dialog.cpp
sed -i 's/"Save"/"保存"/g' src/env_manager_dialog.cpp
sed -i 's/"Create"/"创建"/g' src/env_manager_dialog.cpp
sed -i 's/"e.g., Production, Development, Testing"/"例如：生产环境、开发环境、测试环境"/g' src/env_manager_dialog.cpp

echo "环境管理对话框中文化完成"
