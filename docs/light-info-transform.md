# Light Info: light space / 灯光空间

In EEVEE surface materials and NPR node trees, expand **Light Info → Transform**
to read the selected light object's evaluated world matrix. The panel is closed
by default. These outputs update with animation, parenting and constraints.

| Output | Meaning |
| --- | --- |
| Origin | World-space origin, including for Sun lights |
| X Axis / Y Axis / Z Axis | World-space local +X / +Y / +Z matrix columns |

Axes retain scale, negative scale and parent shear. They are **not unit vectors**.
Emission points along local **-Z**; the existing Direction output is normalized
and remains zero for Point lights. The existing Position output remains zero for
Sun lights. Use Origin when converting points for any light type.

Connect all four outputs to the matching inputs on **Basis Transform**:

1. Select **XYZ**, and disable **Orthonormalize** for exact object-space transforms.
2. **To Basis** converts world to light space; **From Basis** converts light to world.
3. Choose **Point** for positions, **Vector** for directions/displacements, or
   **Normal** for normalized inverse-transpose normal conversion.
4. Origin only affects Point. Convert an object/view-space input to world space
   before using a world-space light basis.

Orthonormalize intentionally removes axis scale and shear using Basis Transform's
existing orthogonalization. It defines a direction frame rather than reproducing
the inverse of a sheared object matrix. Singular bases use the existing Fallback
setting (Pass Through or Zero, determinant threshold 1e-8). An unassigned/deleted
or unavailable light outputs four zero vectors. A hidden light still supplies
its transform while its reference record is available.

No projection is applied: light space is not a spot projection, shadow-map UV,
or an area-light normalized rectangle. This feature does not add Cycles support.

## 中文说明

展开 **Light Info → 变换（Transform）**，将 **Origin、X Axis、Y Axis、Z Axis**
分别连接至 **Basis Transform** 的同名输入。选择 **XYZ**，关闭
**Orthonormalize（正交归一化）**，即可保留缩放、负缩放与父级剪切，得到精确灯光对象空间。

- **To Basis**：世界空间 → 灯光空间；**From Basis**：灯光空间 → 世界空间。
- **Point** 处理位置并使用原点；**Vector** 处理方向／位移；**Normal** 使用法线变换并归一化。
- 三轴是完整矩阵列，不是单位方向。局部 **+Z** 与发光方向 **-Z** 相反。
- 太阳灯的 **Origin** 提供真实原点；旧 **Position** 仍为零。点光的三轴有效；旧 **Direction** 仍为零。
- 输入若处于物体或视图空间，须先转到世界空间。开启正交归一化会移除缩放与剪切，
  不再等于完整对象矩阵的逆变换。零缩放／缺失目标沿用 Basis Transform 的既有回退。

节点读取求值后的变换，跟随动画、父级和约束，无需为每帧重建材质着色器。
动态灯光参数即使同名为 Origin，也使用独立标识符保存，不影响固定变换输出。
