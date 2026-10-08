// AcousticIdentifier/Source/AcousticIdentifier/Public/IMAcousticTypes.h

#pragma once

#include "CoreMinimal.h"
//#include "Runtime/Engine/Classes/Engine/NetSerialization.h"   //需要PhysicsCore
#include "IMAcousticTypes.generated.h"

//--------------------------------------------------------------------------------------------------
// 音频材质响应 - 这是音效设计师真正关心的参数
//--------------------------------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FIMAudioMaterialResponse
{
	GENERATED_BODY()
	FIMAudioMaterialResponse()
	: Occlusion(0.3f),
	Diffusion(0.5f),
	LowDampening(0.5f),
	HighDampening(0.5f)
	{}

	// [0-1] 此材质声音完全阻挡的程度. 0=完全通透, 1=完全遮蔽   暂时用不上 可能用于一些声音吸收
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Occlusion = 0.3f;

	// 是否光滑平坦 可以理解为除粗糙度
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Diffusion = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LowDampening = 0.5f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustic Response", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HighDampening = 0.5f;
};
USTRUCT(BlueprintType)
struct FIMAudioRayResponse
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	float SumDistance = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	float AveVariance = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	float MinDistance = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	float SumHitDirectionVariance = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	FVector AveHitLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	int32 RayHitCount = 0;
	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	int32 ProbeCount = 0;  
};

//--------------------------------------------------------------------------------------------------
// 网格单元
//--------------------------------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FIMGridAudioCell
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	FIMAudioRayResponse RayRes;
    
	UPROPERTY(BlueprintReadOnly, Category = "AIM|Cell")
	FIMAudioMaterialResponse MatRes;
    
	float LastUpdateTime = 0.0f;
    
	// ========================================================================================================
	// TODO: [高优先级] GPU SDF空间连续性检测系统 - 完整架构方案
	// ========================================================================================================
	//
	// 【问题背景】
	// 当前方案：使用方差检测空间一致性（简单但不够精确）
	// 核心限制：
	//   - 方差检测无法精确判断Cell是否横跨多个房间/墙体
	//   - 32m LOD 2 Cell可能跨越多个房间，导致混响数据污染
	//   - 稀疏采样（每秒2-3个脚步射线）需要"大胆假设"墙位置
	//
	// 【未来方案】集成GPU SDF查询系统
	//   1. **Cell创建时验证**：
	//      - 射线采样时构建/更新局部SDF场
	//      - Cell创建时异步提交GPU任务查询Cell Box范围的空间连续性
	//      - 只在SDF<0区域（封闭空间）创建/激活Cell
	//      - 空间不连续的Cell标记为Invalid（仅存储方向掩码，不参与插值）
	//
	//   2. **查询时路径遮挡检测**：
	//      - 在InterpolateAtLod权重计算前，GPU SDF查询 QueryLocation → Cell.AveHitLocation 的路径
	//      - 如果路径SDF积分穿过0（有墙） → 遮挡，权重×0.1
	//      - 如果Cell内SDF方差大 → 空间不一致，权重×0.3
	//
	//   3. **性能优化**：
	//      - GPU并行处理NearbyCells，单次Compute Shader调用批量验证
	//      - 参考：IceMoonDataInterface的CSSDFUtils.ush
	//
	// 【GPU延迟问题及解决策略】
	// 核心挑战：GPU→CPU readback需要2-3+帧延迟，AnimNotify无异步回调
	//
	// **策略1: 延迟激活状态机（核心方案）**
	//   - 添加ECellValidationState枚举: Pending, Validating, Valid, Invalid, Expired
	//   - Cell创建流程：
	//       1. Cell创建时立即存储为Pending状态
	//       2. 提交GPU SDF任务，状态→Validating
	//       3. 2-3帧后GPU回读完成：
	//          - 空间连续 → Valid（参与插值计算）
	//          - 空间不连续 → Invalid（降级为仅存储方向掩码，不参与查询）
	//   - 查询时跳过Pending/Validating状态的Cell，使用其他LOD或邻近Cell
	//   优势：不阻塞射线添加流程，异步验证，渐进式数据质量提升
	//
	// **策略2: CPU预过滤 + GPU精确验证（混合方案）**
	//   - 第一阶段：CPU快速过滤（0延迟）
	//       - 使用现有方差+命中率做启发式判断
	//       - 方差阈值（如2.0m²）：高方差→可能跨墙
	//       - 命中率阈值（如0.5）：低命中率→开放空间
	//   - 第二阶段：GPU精确验证（异步，低优先级）
	//       - CPU预过滤通过的Cell可以立即参与插值（权重×0.5降级）
	//       - 后台提交GPU验证任务
	//       - 验证失败后自动降级Cell状态
	//   优势：脚步音效立即可用（不能等3帧），GPU后台提升数据质量
	//
	// **策略3: 批量验证 + 帧分摊（性能优化）**
	//   - 每帧验证预算：最多验证N个Cell（如10个）
	//   - 维护TArray<FIMGridAudioCell*> PendingValidationQueue
	//   - 单次Compute Shader批量处理整个队列
	//   优势：避免单帧大量GPU任务，平滑性能曲线
	//
	// **策略4: 空间邻域传播（启发式加速）**
	//   - 如果8邻域中有3个以上的Valid Cell，且方差相似
	//   - 则认为当前Cell也可能是Valid，降低验证优先级
	//   优势：大幅减少GPU验证任务量，适合室内连续空间
	//
	// **策略5: 懒验证 + LRU缓存（按需策略）**
	//   - 只在查询时才提交GPU验证（而非创建时）
	//   - 第一次被查询时标记为Validating，跳过该Cell使用其他数据
	//   - 后续查询时GPU结果已回读，可正常使用
	//   优势：只验证真正被使用的Cell，适合大型开放世界
	//
	// 【推荐实现优先级】
	//   短期（立即可用）：
	//     1. 添加ECellValidationState枚举到FIMGridAudioCell
	//     2. 修改查询逻辑跳过Pending/Validating状态
	//     3. 实现CPU预过滤函数（0延迟，立即可用）
	//   中期（异步优化）：
	//     4. 添加GPU验证队列 + 批量验证Tick
	//     5. 实现空间邻域传播减少GPU任务量
	//   长期（性能优化）：
	//     6. 懒验证策略，只验证查询附近的Cell
	//
	// ========================================================================================================

	void AddProbeData(const FVector& NewProbeHitLocation, float NewProbeDistance, bool HitValid, float HitDirectionVariance, const FIMAudioMaterialResponse& NewProbeResponse, const float GameTime){
		if (!HitValid)
		{
			RayRes.ProbeCount++;
			return;
		}
		if (RayRes.RayHitCount == 0)
		{
			MatRes = NewProbeResponse;
			RayRes.SumDistance = NewProbeDistance;
			RayRes.AveHitLocation = NewProbeHitLocation;
			RayRes.MinDistance = NewProbeDistance; // 初始化最短距离

			// 单样本方差：使用距离的20%作为初始不确定度（启发式）
			// 这表示单样本的低可信度，避免方差为0导致的错误高置信度
			const float InitialUncertainty = NewProbeDistance * 0.2f; // 20%不确定度
			RayRes.AveVariance = InitialUncertainty * InitialUncertainty; // 方差 = 标准差²
		}else{  // 使用平均算法更新所有值
			const float OldTotalWeight = static_cast<float>(RayRes.RayHitCount);
			const float NewTotalWeight = static_cast<float>(RayRes.RayHitCount + 1);

			// 更新平均值
			RayRes.AveHitLocation = (RayRes.AveHitLocation * OldTotalWeight + NewProbeHitLocation) / NewTotalWeight;
			float AverageDistance = RayRes.SumDistance / OldTotalWeight;
			float NewVariance = abs(NewProbeDistance - AverageDistance);
			NewVariance *= NewVariance;
			RayRes.AveVariance = (RayRes.AveVariance * OldTotalWeight + NewVariance) / NewTotalWeight; //理论上应该是全部查询后计算平均后再算方差但没有这些数据 使用当前版本
			RayRes.SumDistance += NewProbeDistance;

			// 更新最短距离（取最小值）
			RayRes.MinDistance = FMath::Min(RayRes.MinDistance, NewProbeDistance);

			// 更新聚合响应 (对每个成员都这样做)
			MatRes.Occlusion = (MatRes.Occlusion * OldTotalWeight + NewProbeResponse.Occlusion) / NewTotalWeight;
			MatRes.Diffusion = (MatRes.Diffusion * OldTotalWeight + NewProbeResponse.Diffusion) / NewTotalWeight;
			MatRes.LowDampening = (MatRes.LowDampening * OldTotalWeight + NewProbeResponse.LowDampening) / NewTotalWeight;
			MatRes.HighDampening = (MatRes.HighDampening * OldTotalWeight + NewProbeResponse.HighDampening) / NewTotalWeight;
		}
		RayRes.ProbeCount++;
		RayRes.RayHitCount++;
		// 更新时间戳 (需要从外部传入World)
		LastUpdateTime = GameTime;
	}
};

//--------------------------------------------------------------------------------------------------
// Wet 计算参数 - 用于控制混响强度的计算逻辑
//--------------------------------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FIM_WetCalculationParameters
{
	GENERATED_BODY()
	FIM_WetCalculationParameters()
	: WetClosedDistanceThreshold(2.0f),
	WetOpenDistanceThreshold(15.0f),
	WetHitRateLow(0.2f),
	WetHitRateHigh(0.8f),
	WetNearWallDistance(0.3f),
	WetNearWallBoost(0.2f)
	{}

	/** 封闭空间的平均距离阈值（米）：小于此值认为是封闭房间 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "1.0", ClampMax = "5.0"))
	float WetClosedDistanceThreshold = 2.0f;

	/** 开放空间的平均距离阈值（米）：大于此值认为是开阔区域 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "5.0", ClampMax = "30.0"))
	float WetOpenDistanceThreshold = 15.0f;

	/** 命中率低阈值：低于此值混响减弱 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WetHitRateLow = 0.2f;

	/** 命中率高阈值：高于此值混响增强 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WetHitRateHigh = 0.8f;

	/** 靠墙距离阈值（米）：小于此值时增强混响 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "0.2", ClampMax = "2.0"))
	float WetNearWallDistance = 0.3f;

	/** 靠墙混响增强量：靠近墙壁时额外增加的Wet值 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float WetNearWallBoost = 0.2f;

	/** Wet最大值：Wet的理论最大输出值（使用平滑压缩映射，而非硬截断） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wet Calculation", meta = (ClampMin = "0.3", ClampMax = "1.0"))
	float MaxWetValue = 0.6f;
};

//--------------------------------------------------------------------------------------------------
// 音频混响参数 - 最终应用到音频系统的混响效果参数
//--------------------------------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FIM_AudioReverbParameters
{
	GENERATED_BODY()
	FIM_AudioReverbParameters()
	: Wet(0.0f),
	Delay(0.02f),
	Decay(0.5f),
	Density(0.2f),
	Diffusion(0.4f),
	Dampening(0.6f)
	{}
    
	// 场空间查询混合强度
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reverb Parameters", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Wet = 0.0f;

	// 延迟时间（秒）- 射线找最短(相机->反射碰撞位置 + 发生源->反射碰撞位置) / 声速 * 艺术加工
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reverb Parameters", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float Delay = 0.02f;

	// 衰减60dB时间（秒）- 声音留存时间，房间尺寸*材质硬度 0.161 * V / A
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reverb Parameters", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float Decay = 0.5f;

	// 密度 - 命中点方差*反射体数量（命中率）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reverb Parameters", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Density = 0.2f;

	// 扩散 - 房间表面的不规则性。高扩散=声音均匀散射（模糊平滑），低扩散=反射集中（回声）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reverb Parameters", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Diffusion = 0.4f;

	// 阻尼 - 高频材质吸收（地毯、窗帘等软材质更快吸收高频）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Reverb Parameters", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Dampening = 0.6f;

	//Gain   不管    纯粹的工程控制。它就是混音师调整音量用的，用于确保混响不会太大或太小
	//Bandwidth  不管   很多时候，低频混响会使混音变得泥泞（Muddy），高频混响会很刺耳。Bandwidth 允许工程师切掉不需要的频率，与声场物理无关。
};