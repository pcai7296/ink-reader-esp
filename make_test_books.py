#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 LittleFS 阅读测试书 (T1/T2/T3)。

用途: 用户按 docs/reader-lfs-migration.md §10 用现有 Web 文件管理把生成的 txt 上传进 LittleFS。
内容: UTF-8 中文正文 + 标准章节标题 (第N章 ……), 用于同时验证 正文排版 / .i1 页表 / .z1 章节识别。

用法:
  python make_test_books.py --out build/test_books
  # 生成 T1_300k.txt (~300KB) / T2_1m2.txt (~1.2MB) / T3_1m8.txt (~1.8MB, 贴近 LittleFS 上限)
"""
import argparse
import os

CHAPTER_WORDS = ["风起", "夜行", "旧梦", "青石", "长街", "孤灯", "寒山", "渡口", "剑鸣", "归途"]
BODY = ("他沿着长街慢慢走着，风从屋檐下穿过，带起细碎的尘土。"
        "远处传来打更的声音，一下又一下，落在寂静的夜里。"
        "他忽然想起很多年前的那个清晨，也是这样的天色，也是这样的风。"
        "记忆像潮水一样漫上来，又慢慢退去，只留下潮湿的痕迹。"
        "前面转角处有一盏灯还亮着，昏黄的光落在地上，像一小片温暖的海。"
        "他停下脚步，把手拢在袖子里，静静地站了一会儿。")


def make_book(path, target_bytes):
    """生成测试书: 每个段落带唯一编号, 保证相邻页文字明显不同。
    (旧版每段都是同一句 → 相邻页看起来一模一样, 会被误判为"卡在固定页")"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    written = 0
    chapter = 0
    para = 0
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        while written < target_bytes:
            chapter += 1
            head = "第%d章 %s\n\n" % (chapter, CHAPTER_WORDS[(chapter - 1) % len(CHAPTER_WORDS)])
            fh.write(head)
            written += len(head.encode("utf-8"))
            for _ in range(12):
                para += 1
                line = "　　【%06d】" % para + BODY[:40] + "（第%d段/第%d章）\n\n" % (para, chapter)
                fh.write(line)
                written += len(line.encode("utf-8"))
                if written >= target_bytes:
                    break
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join("build", "test_books"))
    args = ap.parse_args()
    plan = [("T1_300k.txt", 300 * 1024), ("T2_1m2.txt", 1200 * 1024), ("T3_1m8.txt", 1800 * 1024)]
    for name, size in plan:
        p = os.path.join(args.out, name)
        got = make_book(p, size)
        print("%-14s %8d bytes  -> %s" % (name, got, p))


if __name__ == "__main__":
    main()
