# IceMoonAcousticField V2 实用版

当前状态：实现及自动验收中，尚未交付用户试听。旧 v1 查询性能、旧 P0 实验和局部静态审核均不代表 V2 完成。

## 支持范围

- UE 5.8、Windows、单听者、单声道 SoundWave（包括 Procedural）、立体声耳机。
- 静态网格及实例、静态声学材质烘焙；声源和听者可移动。
- 插件内部使用 Steam Audio SDK 4.8.1；发行包来源、逐文件哈希和许可证位于 `Source/ThirdParty/SteamAudio`。
- 环境混响是听者位置驱动的烘焙卷积近似，不是任意声源与听者组合的完整波动 IR。

## 在实际关卡中烘焙

1. 在 Windows 音频设置中选择 **IceMoon Acoustic Field** 空间化插件，重启编辑器使音频设备重新初始化。
2. 放置 `AIMAcousticBakeVolume`。一个音频设备只允许一个活动声场；设置轴对齐、单位缩放的 Bake Bounds，改变 Box Extent 调整范围。
3. 在 Materials 中明确映射真实网格材质及三频段吸收系数、散射系数。系数取值 0–1；缺失、重复或无效映射会失败。
4. 按顺序执行 **Inspect Scene → Generate Probes → Show Probe Coverage → Bake**。检查 Scene Issues；静态必需几何未导出不能报成功。
5. 烘焙成功后保存关卡，使 Baked Field 引用持久化。资产保存于插件 `Bakes` 目录；重新启动后可检查并显示已保存探针。

导出读取实际 LOD0 网格、实例变换和材质。没有可读 CPU 几何的数据会明确失败。编辑器可视化组件和引擎默认辅助对象会列为排除；移动对象不参加静态烘焙。需要明确排除的组件添加 `IMAcousticIgnore`，必须参加的组件添加 `IMAcousticRequired`；两标签冲突会失败。不得用排除标签掩盖必需墙体或楼板。

场景指纹包含实际几何、变换、声学系数、探针参数及配方版本。编辑器定期检查变更；播放开始时再次验证。取消、失败或烘焙中改变场景不会替换上一份完整资产。取消等待当前 SDK 计算阶段返回。

## 接入声音

1. 给声源 Actor 添加实际 `UAudioComponent` 与 `UIMAcousticSourceComponent`，把后者的 Audio Component 指向前者。
2. 使用单声道 SoundWave，启用空间化并选择 HRTF；衰减设置的空间化插件数组只配置一个 `UIMAcousticSpatializationSettings`。
3. 关闭 UE 原生距离衰减、距离低通、遮挡、Listener Focus 和 Reverb Send；清除冲突的空间化、遮挡、混响或 Source Data Override 插件配置。
4. 听者和受管声源必须在 Bake Bounds 内。无效接入会在声场 Status 中报告。

直达声和绕墙声取自同一份未遮挡的单声道输入。SDK 负责各路径的传播效果与耳机空间化。环境混响的每路输入施加一次声源距离增益后汇入专用 Submix；不要再追加 v1 Wet/Delay/Decay 驱动或第二套原生混响发送。

## 试听开关

- **Enable V2**：总开关。关闭时输出等功率、居中的干声参考，并重置间接历史；该参考不冒充 UE 原生 HRTF。
- **Direct Route / Path Route / Reverb Route**：分通路试听，静音仍保留卷积历史，便于在相同状态下比较；这三个按钮不代表 CPU 绕过。
- **Reverb Wet Gain**：播放开始时绑定的湿声音量。改变后重新开始播放。

缺失、过期、越界或失效结果使用明确的干声降级并关闭无效间接效果，降级块不计为 V2 成功。位于 Bake Bounds 内并不保证有可见探针覆盖；无法确认有效覆盖时关闭环境混响并报告原因，不延用旧位置的混响。场景静态烘焙不支持运行中动态门、破坏或材质重烘。

## 验证入口

所有关卡与烘焙资产在插件 Content 内；测试不调用旧白模的重建或清空入口。

| 自动测试 | 作用 |
|---|---|
| `IceMoon.AcousticField.W1.DirectPath` | 一墙一门真实 UE 分通路录音、声源/听者同时移动及转头方向 |
| `IceMoon.AcousticField.W2.Inventory` | 只读清点现有白模 |
| `IceMoon.AcousticField.W2.BakeExisting` | 实际白模烘焙、几何保持、错图/损坏/材质过期/取消反例 |
| `IceMoon.AcousticField.W2.ColdLoad` | 新进程加载资产、指纹和探针预览验证 |
| `IceMoon.AcousticField.W3.Pressure16x600` | 16 个真实 AudioComponent、600 秒移动及回调计时 |
| `IceMoon.AcousticField.W3.Diagnostic16x30` | 30 秒故障定位；不替代 600 秒验收，无渲染模式仅供诊断 |
| `IceMoon.AcousticField.W3.Lifecycle` | 连续 PIE、开关、越界、声源销毁/复用及静音录音 |

原始证据位于运行工程 `Saved/AcousticV2`。原生 SDK 测试在 `Tests`：Smoke、Decay、Paths 各自保存源文件哈希、日志、录音或路径段。原生结果不替代 UE 录音或用户试听。

配方2使用 4096 射线、64 次反弹、2 秒卷积数据。配方1的两次反弹无法区分高低吸收的衰减斜率，已由反例否决；版本变化要求重新烘焙。当前仍须完成全部运行、压力和最终独立审核，之后才进入用户试听。没有自动合并或同步授权。
