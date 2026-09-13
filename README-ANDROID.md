# Android 端（本分支）

这是 **ink-reader-esp** 仓库的 `android` 分支：墨水屏阅读器的 **Android 同伴端**源码
（基于开源阅读 Legado 的社区延续版 LegadoTeam/legado，GPL-3.0，见 `LICENSE`）。

固件（ESP8266）在 **`master` 分支**；两端通过 LUMI1 进度同步协议通信。

## 本分支相对上游做了什么

| 模块 | 内容 |
|---|---|
| `app/src/main/java/io/legado/app/esp/` | **ESP 进度同步**（16 个文件）：前台同步服务（HTTP 8384 + UDP 8390）、LUMI1 编解码、指纹校验、设备发现与绑定、进度快照/待确认队列、WebDAV 风格弹窗确认 |
| `app/src/main/java/io/legado/app/ui/main/device/` | 底栏「设备」页：同步开关/状态/刷新、设备 IP 绑定、前往水墨屏管理入口 |
| `app/src/main/java/io/legado/app/help/WatchMode.kt` `WatchUi.kt` | **手表模式**：首次启动自动识别小屏（`ro.build.characteristics=watch` 等）并固定，设置可改；顶部/底部避让物理圆角、底栏铺满+按钮组居中、阅读页脚避让 |
| `app/src/main/res/xml/pref_config_other.xml` | 其他设置 → 主界面：手表模式开关 |
| `app/src/main/res/menu/book_read.xml` + `ReaderMenuItem.kt` | 阅读菜单「更多选项」第一位 = **ESP 进度同步**（√ 显示状态，开关直控同步服务） |

## 协议与设备端

- 协议规范：固件分支 `docs/progress-lumi1.md`（LUMI1 载荷、UDP 发现/绑定、地址语义）
- 设备地址：STA 静态 `192.168.0.100`；配网热点 `192.168.4.1`；同步热点 `192.168.0.1/24`
- 本端监听：HTTP `8384`（GET/PUT `/progress`）、UDP `8390`（LUMIDISC/LUMIWHO/LUMIBIND/LUMIPING）

## 构建

```bash
# 需要 JDK 21 + Android SDK；minSdk 26
./gradlew :app:assembleDebug          # 调试包
./gradlew :app:assembleRelease -ParmOnly=true   # 仅 arm 的 release
```

机器相关配置（**未入库**，需按本机情况自建）：

- `local.properties`：`sdk.dir=...`
- `settings.gradle` 内的本地 maven 镜像（`m2local`，用于离线取 R8）按本机路径调整
- Gradle 用户目录：`GRADLE_USER_HOME`（可选）
