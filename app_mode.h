#pragma once
// app_mode.h — AppMode 枚举**唯一定义**（2026-09-12 审查 #6）
//
// 历史问题: 该枚举曾在 globals.h 与 ink-reader-esp.ino 各写一份（数值相同, 靠注释提醒
// "新增模式必须同步两处"）—— 加 APP_STATS 时就踩过：两份容易漂移, 且漂移是静默的
// （.ino 内部按 APP_* 常量判断, 其他 .cpp 按 globals.h 版本, 数值一旦不一致就出现
// "某个界面被当成另一个界面"的灵异 bug）。
//
// 现在: 定义为唯一来源, globals.h 与 ink-reader-esp.ino 都 include 本文件。
// 新增界面只在**这里**加成员, 并同步:
//   ① ink-reader-esp.ino 的 loop() 分发分支; ② 需要跨 TU 判断处的 switch/if;
//   ③ saveSleepRecord/唤醒恢复映射（若有方向/界面语义）; ④ AGENTS.md 界面清单。
enum AppMode { APP_HOME = 0, APP_BROWSER = 1, APP_READER = 2, APP_CHAPTERS = 3,
               APP_NETWORK = 4, APP_CLOCK_CONNECT = 5, APP_CLOCK = 6,
               APP_WEATHER = 7, APP_SETTINGS = 8, APP_BMP = 9,
               APP_MARKS = 10, APP_CLOCK_DISGUISE = 11, APP_STATS = 12 };
