#!/bin/bash

# 登录对话框中文化
sed -i 's/"Aegisy Login"/"Aegisy 登录"/g' src/login_dialog.cpp
sed -i 's/"Welcome to Aegisy Client"/"欢迎使用 Aegisy 客户端"/g' src/login_dialog.cpp
sed -i 's/"Please login with your Aegisy account"/"请使用您的 Aegisy 账号登录"/g' src/login_dialog.cpp
sed -i 's/"Email:"/"邮箱:"/g' src/login_dialog.cpp
sed -i 's/"Password:"/"密码:"/g' src/login_dialog.cpp
sed -i 's/"Remember Me"/"记住我"/g' src/login_dialog.cpp
sed -i 's/"Login"/"登录"/g' src/login_dialog.cpp
sed -i 's/"Cancel"/"取消"/g' src/login_dialog.cpp
sed -i 's/"Logging in..."/"登录中..."/g' src/login_dialog.cpp
sed -i 's/"Login failed"/"登录失败"/g' src/login_dialog.cpp
sed -i 's/"Please enter email and password"/"请输入邮箱和密码"/g' src/login_dialog.cpp
sed -i 's/"Invalid Input"/"输入无效"/g' src/login_dialog.cpp
sed -i 's/"Login Failed"/"登录失败"/g' src/login_dialog.cpp

echo "登录对话框中文化完成"
