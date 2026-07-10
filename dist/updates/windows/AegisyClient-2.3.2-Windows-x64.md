# Aegisy Client 2.3.2 for Windows

- 修复安装后缺少 `zlib1_.dll` 导致程序无法启动的问题。
- Windows 安装包现在会收集 OpenSSL 目录中的完整运行时 DLL。
- 打包过程中增加启动冒烟测试，缺少动态库时会直接停止生成安装包。
