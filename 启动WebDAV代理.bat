@echo off
rem 墨水屏进度同步 PC 代理桥 (http://<本机IP>:8080/dav -> https://dav.jianguoyun.com/dav)
rem 作用: ESP8266 内存不足无法直连 HTTPS, 由本机代理完成 TLS 转发。
rem 用法: 双击运行, 保持窗口开着即可 (墨水屏同步时本机须在线)。
cd /d "%~dp0"
echo 启动 WebDAV 代理: http://本机IP:8080/dav  -^>  https://dav.jianguoyun.com/dav
echo 关闭本窗口即停止代理。
echo.
python webdav_proxy.py 8080
echo.
echo 代理已退出。
pause
