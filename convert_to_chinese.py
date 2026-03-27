#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re
import os

files_to_convert = ["docs/scene-extensions.md", "docs/extended-nodes.md", "docs/npr-workflow.md", "docs/interface-guide.md"]


def convert_to_chinese_only(filepath):
    """将双语选项卡文件转换为纯中文"""
    if not os.path.exists(filepath):
        return f"✗ {filepath} - 文件不存在"

    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    # 新策略：使用正则表达式删除 === "English" === 段落
    # 1. 删除 === "English" === 之后到下一个 === 或其他 HTML 标记的所有内容
    content = re.sub(r'=== "English"\n\n(.*?)(?:=== (?!"中文)|$)', "", content, flags=re.DOTALL)

    # 2. 删除 === "中文" === 标记，保留内容
    content = re.sub(r'=== "中文"\n\n', "", content)

    # 3. 清理多余空行（超过两个连续空行则替换为两个）
    content = re.sub(r"\n\n\n+", "\n\n", content)

    with open(filepath, "w", encoding="utf-8") as f:
        f.write(content)

    return f"✓ {filepath} - {len(content)} 字符"


for file in files_to_convert:
    result = convert_to_chinese_only(file)
    print(result)

print("\n✅ 所有文件转换完成！")
