import re
import os

files_to_convert = ["docs/scene-extensions.md", "docs/extended-nodes.md", "docs/npr-workflow.md", "docs/interface-guide.md"]

for filepath in files_to_convert:
    if os.path.exists(filepath):
        with open(filepath, "r", encoding="utf-8") as f:
            content = f.read()

        # Remove English sections
        content = re.sub(r'=== "English"\n\n(.*?)(?:===|\Z)', "", content, flags=re.DOTALL)

        # Remove Chinese tab markers
        content = re.sub(r'=== "中文"\n\n', "", content)

        # Clean up extra blank lines
        content = re.sub(r"\n\n\n+", "\n\n", content)

        with open(filepath, "w", encoding="utf-8") as f:
            f.write(content)

        print(f"✓ {filepath} - completed")
    else:
        print(f"✗ {filepath} - not found")

print("\n✅ All files converted!")
