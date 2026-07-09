#!/bin/bash

# 主窗口中文化
sed -i 's/"User: Not logged in"/"用户: 未登录"/g' src/main_window.cpp
sed -i 's/"User: "/"用户: "/g' src/main_window.cpp
sed -i 's/": Configured"/": 已配置"/g' src/main_window.cpp
sed -i 's/": Not configured"/": 未配置"/g' src/main_window.cpp
sed -i 's/"Logout Confirmation"/"退出登录确认"/g' src/main_window.cpp
sed -i 's/"Are you sure you want to logout?"/"确定要退出登录吗？"/g' src/main_window.cpp

echo "主窗口中文化完成"
