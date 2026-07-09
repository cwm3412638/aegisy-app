#!/bin/bash

# 窗口标题中文化
sed -i 's/setWindowTitle("Aegisy Client")/setWindowTitle("Aegisy 客户端")/g' src/main_window.cpp
sed -i 's/setWindowTitle("Aegisy Client - Login")/setWindowTitle("Aegisy 客户端 - 登录")/g' src/login_dialog.cpp
sed -i 's/setWindowTitle("API Keys Management")/setWindowTitle("API Keys 管理")/g' src/api_keys_dialog.cpp
sed -i 's/setWindowTitle("Environment Configuration")/setWindowTitle("环境配置")/g' src/env_config_dialog.cpp
sed -i 's/setWindowTitle("Environment Manager")/setWindowTitle("环境管理")/g' src/env_manager_dialog.cpp

echo "窗口标题中文化完成"
