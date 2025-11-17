# IceMoon Acoustic Field (IMAF)

**Real-time, Low-Cost Acoustic Field Simulation for Dynamic Audio in Unreal Engine 5.6+**

---

## ⚠️ Dependency & Compilation Warning

This plugin relies on the author's internal utility library (`IceMoonDataInterface`) and other utility components. **Therefore, external compilation is not supported,** as the goal is to showcase the system design, core algorithms, and run-time performance optimization techniques.

---

## 🚀 Core Value: Dynamic Acoustics for Runtime Environments

Traditional baked acoustic solutions fail in dynamic scenes. **IceMoon Acoustic Field (IMAF)** provides a lightweight, dynamic, and integrated simulation system specifically designed to handle **real-time changes** in:

- **Dynamic Sources:** Character footsteps, projectiles, moving geometry.
- **Moving Listeners:** Player camera, VR headsets.
- **Runtime Environment Changes:** Destructible geometry and dynamic level components.

IMAF prioritizes **practical runtime performance** and **workflow simplicity** over heavy acoustic middleware, focusing on the needs of real-world game production.

---

## ✨ System Highlights

### ✅ Low-Cost Dynamic Grid System

1.  **Multi-LOD Sparse Grid:** Uses 3 hierarchical grid layers with customizable multipliers (2x or 4x) for smooth or aggressive LOD transitions. Storage is sparse, allocating only for probed cells to minimize memory footprint.
2.  **Z-Axis Clamping:** Prevents acoustic contamination across floors by clamping the maximum cell height (Default: 4m).
3.  **Adaptive Query & Probe:**
    * **Async Probes:** Sound-source driven sampling (`AsyncFireProbes`) using Fibonacci sphere distribution and **asynchronous ray tracing** (Static/Stationary geometry only).
    * **Multi-LOD Weighted Query:** Combines all 3 LOD levels via weighted interpolation (50%/30%/20%) based on distance, time, and variance.
4.  **Physical Material Mapping:** Maps UE Physical Materials to custom data assets for per-surface **Occlusion, Diffusion, and Dampening** parameters.
5.  **Adaptive State Machine:** Optimizes CPU by switching update frequency based on source activity:
    * **Active** (<3s): Per-frame update.
    * **Dormant** (>10s): 1.0s interval update.

---

## 🛠️ Installation & Setup (For Review/Audit)

### Dependencies
- Unreal Engine **5.5+**
- **C++ Project** is required.
- **Internal Dependencies** (for full build): `IceMoonDataInterface`, `IceMoonBlueprintGPUMathUtilities`

### Steps
1.  Clone this repository into your project's **`Plugins/`** folder.
2.  Add the required dependency plugins (if available) to the appropriate locations.
3.  Regenerate Visual Studio project files and compile the module.

---

## 🚀 Quick Start: How to Use the System

### 1. Create Configuration Assets

| Asset Type | Description | Example Content |
| :--- | :--- | :--- |
| **Material Map** (`DA_IM_MaterialMap`) | Maps UE Physical Materials to acoustic properties. | `PM_Concrete: { Occlusion: 0.8, Diffusion: 0.3, ... }`<br>`PM_Wood: { Occlusion: 0.5, Diffusion: 0.6, ... }` |
| **Field Config** (`DA_IM_AcousticFieldConfig`) | Sets default reverb parameters and references the Material Map. | (References Material Map; sets default Decay Time, etc.) |

### 2. Add the Acoustic Field Actor

Drag the **`AIceMoonAcousticField`** Actor into the level and configure its spatial properties:

-   **`ConfigAsset`:** Reference your created Field Config Asset.
-   **`GridCellSize`:** Base size for LOD 0 (e.g., 200.0 cm).
-   **`LodMultiplier`:** 2 (for interiors) or 4 (for exteriors).
-   **`MaxCellHeightZ`:** Z-axis clamping height (e.g., 400.0 cm).
-   **`bDebug`:** `true` to enable visualization of the grid and probes.

### 3. Integrate with Sound Sources (Blueprints/C++)

The system requires two steps: **Firing Probes** (Sampling the environment) and **Querying** (Getting parameters for audio).

#### Blueprint Example (Character Footstep)

GetAcousticFieldActor → AcousticFieldGetSocketLocation "foot_l" → FootLocation// --- Step 1: Fire Probes (Async) ---3. AcousticField.AsyncFireProbes- Origin: FootLocation- NumTraces: 16 (sampling quality)- Radius: 2000.0 cm (sampling range)// --- Step 2: Query Parameters (Smoothed) ---4. AcousticField.QueryAcousticFieldSmooth- SourceObject: Self (to manage state)- SoundSlot: "FootL" (unique ID for the sound)- QueryLocation: FootLocation- SmoothSpeed: 3.0 s (Recommended for footsteps)→ ReverbParameters (FIM_AudioReverbParameters)Apply ReverbParameters to your Audio Component.
| SmoothSpeed Guideline | Use Case |
| :--- | :--- |
| **3.0-5.0s** | Discrete events (Footsteps, Impacts). |
| **0.5-1.0s** | Continuous loops (Engine sounds, Environmental loops). |

#### C++ Example

```cpp
// --- Firing Probes ---
AIceMoonAcousticField* Field = AIceMoonAcousticField::GetAcousticFieldActor(this);
if (Field) 
{
    FVector FootLocation = GetMesh()->GetSocketLocation(Foot);
    Field->AsyncFireProbes(FootLocation, 16, 2000.0f);
}

// --- Querying Parameters ---
FIM_AudioReverbParameters Reverb;
Field->QueryAcousticFieldSmooth(this, Foot, FootLocation, Reverb, 3.0f);

// Application logic follows...
ApplyReverbToFootstep(Reverb); 
📐 Core Concepts: Parameter CalculationThe system calculates various parameters based on probe hits, distance, and time:ParameterCalculation MethodPhysical MeaningWetEnclosure × Hit Rate × Uniformity × Wall EnhancementOverall Reverb StrengthDelay(Camera ↔ Reflection + Source ↔ Reflection) / Speed of SoundEarly Reflection Arrival Time (seconds)Decay$0.161 \times \text{Volume} / \text{Average Absorption}$RT60 Decay Time (seconds)DiffusionMaterial Scattering × 0.7 + Direction Variance × 0.3Surface Irregularity and Scattering🏎️ Performance ConsiderationsMemory UsagePer Cell: $\sim 140$ bytes (Sparse allocation).Medium Level (100m x 100m): $\sim 0.7-2.1$ MB.CPU Performance (Adaptive Tick + Query)Active State (Low Latency): Per-frame tick ($\sim 0.01$ms).Dormant State (Low Overhead): $1.0$ second interval update.Probe Firing: Asynchronous (non-blocking) on worker threads.Scalability SuggestionsScenarioGridCellSize (cm)LodMultiplierNumTracesTight Interior100216-24Open World40048-12📖 API ReferenceC++// Get the world singleton actor
AIceMoonAcousticField* GetAcousticFieldActor(UObject* WorldContextObject);

// Fire probes (Asynchronous, non-blocking)
void AsyncFireProbes(FVector Origin, int32 NumTraces = 16, float Radius = 2000.0f, ...);

// Smooth query (Recommended for footsteps/discrete events)
bool QueryAcousticFieldSmooth(
    UObject* SourceObject,
    FName SoundSlot,
    FVector QueryLocation,
    FIM_AudioReverbParameters& OutResponse,
    float SmoothSpeed = 3.0f
);
📜 Licensing and MaintenanceThis project is provided free under the BSD 3-Clause License.No Feature Requests: This project does not accept feature requests or functional enhancement Issues. Relevant Issues will be automatically closed.Bugs Only: The Issues section is strictly for reporting engine compatibility problems or critical bugs.Contributors submitting PRs will be credited in the Credits.md file.If used in a commercial project, mentioning "IceMoon" in the game's credits is appreciated.Made with ❄️ by IceMoon | UE 5.6+ | BSD 3-Clause


# IceMoon Acoustic Field

**实时、低成本的声学场模拟系统，用于 Unreal Engine 5.6 的动态音频**

**此插件依赖于个人内部库 IceMoonDataInterface，因此无法在外部环境中编译。当然你可以吧关键位置删掉 就一个stat统计 和 一个编辑器/pie打包都可靠的GetCameraPosition工具函数**
---

## 核心价值

传统烘焙声学方案无法响应运行时环境变化。**IceMoonAcousticField** 提供轻量级、动态、易集成的声学模拟系统，专为以下场景设计：

- **动态声源**（角色脚步、抛射物、动态物体）
- **移动的听者**（移动相机、VR头显）
- **运行时环境变化**（可破坏环境、动态几何体）

与重量级声学中间件不同，IMAF 专注于**实用的运行时性能**和**工作流简洁性**。

---

## ✨ 主要特性

### ✅ 核心系统

1. **多 LOD 网格系统**
   - 3 层分层网格，可配置倍增器（2x 或 4x）
   - 室内：`2m → 4m → 8m`（2x，细腻过渡）
   - 室外：`2m → 8m → 32m`（4x，激进 LOD）
   - 稀疏存储（仅分配有数据的单元格）

2. **Z 轴高度钳制**
   - 防止跨楼层声学污染
   - 可配置最大单元格高度（默认：4m）

3. **按需探针发射**
   - 声源驱动采样（`AsyncFireProbes`）
   - 斐波那契球分布，均匀覆盖
   - 异步射线检测（仅 Static/Stationary 几何体）

4. **多 LOD 加权查询**
   - 组合 3 个 LOD 层级，权重固定（50% / 30% / 20%）
   - 基于距离 + 时间 + 方差的插值

5. **物理材质 → 音频响应映射**
   - 基于数据资产的材质库（`DA_IM_MaterialMap`）
   - 每种材质的遮蔽、扩散、阻尼参数

6. **平滑查询系统**（`QueryAcousticFieldSmooth`）
   - 用于离散采样事件的指数平滑（AnimNotify、脚步）
   - 根据 Wet 参数变化幅度自适应调整速度
   - 每个声源 + 每个槽位缓存

7. **自适应状态机**
   - 活跃（<3s）：每帧
   - 中等（3-10s）：0.1s 间隔
   - 休眠（>10s）：1.0s 间隔

---

## 🛠️ 安装

### 依赖
- Unreal Engine **5.5+**
- **C++ 项目**
- **依赖插件**：`IceMoonDataInterface`、`IceMoonBlueprintGPUMathUtilities`

### 步骤

1. 克隆到 `Plugins/` 文件夹
2. 添加 IceMoonDataInterface IceMoonBlueprintGPUMathUtilities 插件
3. 重新生成项目文件并编译

---

## 🚀 快速开始

### 1. 创建配置资产

**材质库**（`DA_IM_MaterialMap`）：
```
MaterialMap:
  - PM_Concrete: { Occlusion: 0.8, Diffusion: 0.3, LowDamp: 0.6, HighDamp: 0.7 }
  - PM_Wood:     { Occlusion: 0.5, Diffusion: 0.6, LowDamp: 0.4, HighDamp: 0.5 }
Fallback: { 0.5, 0.5, 0.5, 0.5 }
```

**声场配置**（`DA_IM_AcousticFieldConfig`）：
- 引用材质库
- 设置默认混响参数

### 2. 添加声场 Actor

拖拽 `AIceMoonAcousticField` 到关卡，配置：
```
ConfigAsset: [你的配置资产]
GridCellSize: 200.0 cm
LodMultiplier: 2 (室内) 或 4 (室外)
MaxCellHeightZ: 400.0 cm
bDebug: true (启用可视化)
```

### 3. 从声源发射探针

**蓝图示例**（角色脚步）：
```
1. GetAcousticFieldActor → AcousticField
2. GetSocketLocation "foot_l" → FootLocation
3. AcousticField.AsyncFireProbes
   - Origin: FootLocation
   - NumTraces: 16
   - Radius: 2000.0 cm
4. AcousticField.QueryAcousticFieldSmooth
   - SourceObject: Self
   - SoundSlot: "FootL"
   - QueryLocation: FootLocation
   - SmoothSpeed: 3.0 s
   → ReverbParameters
5. 应用到音频组件
```

**C++ 示例**：
```cpp
void AMyCharacter::PlayFootstepSound(FName Foot)
{
    AIceMoonAcousticField* Field = AIceMoonAcousticField::GetAcousticFieldActor(this);
    if (!Field) return;

    FVector FootLocation = GetMesh()->GetSocketLocation(Foot);
    Field->AsyncFireProbes(FootLocation, 16, 2000.0f);

    FIM_AudioReverbParameters Reverb;
    Field->QueryAcousticFieldSmooth(this, Foot, FootLocation, Reverb, 3.0f);

    // 应用混响参数到你的音频系统
    ApplyReverbToFootstep(Reverb);
}
```

---

## 📐 核心概念

### Z 轴高度钳制

**问题**：大单元格（8m × 8m × 8m）跨越多层，导致跨楼层声学污染

**解决方案**：`MaxCellHeightZ = 400cm`
- XY 维度：跟随 `LodCellSizes`（2m, 4m, 8m...）
- Z 维度：钳制为 `min(LodCellSizes[i], MaxCellHeightZ)`
- 结果：LOD 2 变为 8m × 8m × **4m**

### 混响参数计算

| 参数 | 计算方法 | 物理意义 |
|------|---------|---------|
| **Wet** | 封闭度 × 命中率 × 均匀性 × 靠墙增强 | 混响强度 |
| **Delay** | (相机↔反射 + 声源↔反射) / 声速 | 早期反射到达时间（秒）|
| **Decay** | 0.161 × 体积 / 平均吸收 | RT60 衰减时间（秒）|
| **Density** | 命中率 | 反射密度 |
| **Diffusion** | 材质散射 × 0.7 + 方向方差 × 0.3 | 表面不规则性 |
| **Dampening** | 材质高频吸收 | 高频衰减 |

---

## 📖 API 参考

### 核心函数

```cpp
// 获取世界单例
AIceMoonAcousticField* GetAcousticFieldActor(UObject* WorldContextObject);

// 发射探针（异步，非阻塞）
void AsyncFireProbes(FVector Origin, int32 NumTraces = 16, float Radius = 2000.0f, ...);

// 查询声学参数（即时，使用缓存数据）
bool QueryAcousticField(FVector QueryLocation, FIM_AudioReverbParameters& OutResponse);

// 平滑查询（推荐用于离散事件）
bool QueryAcousticFieldSmooth(
    UObject* SourceObject,
    FName SoundSlot,
    FVector QueryLocation,
    FIM_AudioReverbParameters& OutResponse,
    float SmoothSpeed = 3.0f
);
```

**SmoothSpeed 指南**：
- **3.0-5.0s**：离散事件（脚步、冲击）
- **0.5-1.0s**：连续循环（环境音、引擎）

---

## ⚙️ 配置参数

| 参数 | 默认值 | 描述 |
|-----|-------|------|
| `GridCellSize` | 200 cm | LOD 0 基础单元格大小 |
| `LodMultiplier` | 2 | LOD 缩放：2（室内）、4（室外）|
| `MaxCellHeightZ` | 400 cm | Z 轴钳制（0 = 禁用）|
| `ActiveStateThreshold` | 3.0 s | 活跃状态超时 |
| `DormantStateThreshold` | 10.0 s | 休眠状态超时 |
| `bDebug` | false | 启用可视化 + 详细日志 |

---

## 🏎️ 性能考虑

### 内存使用
- **每单元格**：~140 字节（稀疏分配）
- **小室内**（10m × 10m × 3m）：~50-200 单元格 = **7-28 KB**
- **中等关卡**（100m × 100m × 10m）：~5,000-15,000 单元格 = **0.7-2.1 MB**

### CPU 性能
- **Tick 成本 + 查询成本**（自适应）：
  - 活跃：每帧（~0.01ms）
  - 中等：0.1s 间隔
  - 休眠：1.0s 间隔
- **探针发射**：异步（非阻塞） 工作线程隐藏开销

### 可扩展性建议

| 场景 | GridCellSize | LodMultiplier | MaxCellHeightZ | NumTraces |
|-----|-------------|--------------|----------------|-----------|
| 狭窄室内 | 100 cm | 2 | 300 cm | 16-24 |
| 标准室内 | 200 cm | 2 | 400 cm | 12-16 |
| 大厅 | 300 cm | 2 | 600 cm | 16-24 |
| 室外 | 200 cm | 4 | 400 cm | 12-16 |
| 开放世界 | 400 cm | 4 | 500 cm | 8-12 |

---

## 🧪 调试

### 启用可视化

设置 `bDebug = true` → 查看彩色盒子（绿色=LOD0，黄色=LOD1，橙色=LOD2）

### 常见问题

1. **没有单元格出现**：检查 `AsyncFireProbes` 是否调用，验证命中 Static/Stationary 几何体
2. **跨楼层污染**：减少 `MaxCellHeightZ`（尝试 300-400cm）
3. **混响太强/弱**：调整 `WetCalculationParameters`，检查材质响应
4. **卡顿/不连续**：使用 `QueryAcousticFieldSmooth`，增加 `SmoothSpeed`
5. **性能问题**：增加 `GridCellSize`，使用 `LodMultiplier = 4`，减少 `NumTraces`

### 性能分析

控制台输入：
```
stat IMSystem
```

---

## 🤝 贡献

### Bug 报告与功能请求

- **Bug 报告**：欢迎提交 issue（附上 UE 版本、复现步骤）
- **功能请求**：**不接受**（作者个人工具库政策）
  - 功能相关的 PR 欢迎提交但可能不会合并

### Pull Request

**接受的 PR**：
- Bug 修复
- 性能改进
- 文档改进
- 引擎版本兼容性更新

**PR 指南**：
1. 遵循 UE 编码标准
2. 在 UE 5.6 上测试
3. 更新 README（如果添加公共 API）
4. 贡献者将被添加到 `Credits.md`

---

## 📜 许可证

**BSD 3-Clause License**

- 本库使用 **BSD 3-Clause 许可** 免费提供
- **本项目不接受任何功能请求 (Feature Requests) 或功能增强的 Issues**，相关 Issues 将被自动关闭
- Issues 区严格用于报告引擎兼容性问题或关键 Bug
- 提交 PR 的贡献者，其名字将被加入插件的 Credits.md 文件中，以示感谢
- 如果您在商业项目中使用本插件，我非常感谢您能在游戏致谢名单中提及 IceMoon 贡献

---

**Made with ❄️ by IceMoon | UE 5.6+ | BSD 3-Clause**
