#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Multi-language MkDocs build script
Builds both Chinese and English versions and deploys to GitHub Pages
"""

import os
import shutil
import subprocess
import sys


def run_command(cmd, description):
    """Run a shell command and report status"""
    print(f"\n{'='*60}")
    print(f"[{description}]")
    print(f"{'='*60}")
    print(f"Command: {cmd}\n")

    result = subprocess.run(cmd, shell=True, cwd=os.getcwd())

    if result.returncode != 0:
        print(f"❌ {description} 失败!")
        return False
    print(f"✅ {description} 成功!")
    return True


def build_multi_language():
    """Build both Chinese and English versions"""

    site_dir = "site"

    # Clean previous build
    if os.path.exists(site_dir):
        shutil.rmtree(site_dir)
        print(f"✓ 清理旧的构建目录: {site_dir}")

    os.makedirs(site_dir, exist_ok=True)

    # Build Chinese version (root)
    if not run_command("mkdocs build -f mkdocs.yml -d site", "构建中文版本"):
        return False

    # Build English version (en subfolder)
    # First we need to temporarily create the en folder structure
    if not run_command("mkdocs build -f mkdocs.en.yml -d site/en", "构建英文版本"):
        return False

    print("\n" + "=" * 60)
    print("✅ 所有语言版本构建成功!")
    print("=" * 60)

    # Verify structure
    print("\n📂 网站目录结构:")
    for root, dirs, files in os.walk(site_dir):
        level = root.replace(site_dir, "").count(os.sep)
        indent = " " * 2 * level
        print(f"{indent}{os.path.basename(root)}/")
        subindent = " " * 2 * (level + 1)
        for file in files[:3]:  # Show first 3 files
            print(f"{subindent}{file}")
        if len(files) > 3:
            print(f"{subindent}... +{len(files)-3} more files")
        if level > 2:  # Limit depth
            break

    return True


def main():
    """Main entry point"""
    print(
        """
╔════════════════════════════════════════════════════════════╗
║      Blender 5.1 NPR Port - Multi-Language Build          ║
║           中英文双语网站构建工具                            ║
╚════════════════════════════════════════════════════════════╝
    """
    )

    if not build_multi_language():
        sys.exit(1)

    print("\n✅ 构建完成！")
    print("📍 下一步: 部署到 GitHub Pages")
    print("   - 确保 site/ 目录已生成")
    print("   - GitHub 会自动检测 gh-pages 分支的更新")

    return 0


if __name__ == "__main__":
    sys.exit(main())
