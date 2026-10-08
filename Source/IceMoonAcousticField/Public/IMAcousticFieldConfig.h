// AcousticIdentifier/Source/IceMoonAcousticField/Public/IMAcousticFieldConfig.h

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "IMAcousticTypes.h"
#include "IMAcousticFieldConfig.generated.h"

class UIMMaterialMap;

/**
 * 声场配置预设 - 定义场景特定的声学参数
 * 包括：默认混响、Wet计算参数等
 * 可创建多个预设：室内、室外、地下城等，供不同关卡使用
 */
UCLASS(BlueprintType)
class ICEMOONACOUSTICFIELD_API UIMAcousticFieldConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	// ========================================
	// 材质库引用
	// ========================================

	/** 物理材质库（全局共享，留空则使用项目默认） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|1. Material Library",
		meta=(Tooltip="物理材质到音频响应的映射库（可选，留空则使用全局默认）"))
	TObjectPtr<UIMMaterialMap> MaterialLibrary;

	// ========================================
	// 默认混响配置
	// ========================================

	/** 查询失败时使用的默认混响参数（通常代表室外/开放空间） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|2. Default Reverb",
		meta=(Tooltip="查询失败或无数据时的默认混响（例如室外环境）"))
	FIM_AudioReverbParameters DefaultReverbParameters;

	// ========================================
	// Wet 计算参数配置
	// ========================================

	/** Wet计算参数组 - 控制混响强度的计算逻辑 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|3. Wet Calculation",
		meta=(Tooltip="Wet计算参数（封闭度、命中率、靠墙检测等）"))
	FIM_WetCalculationParameters WetCalculationParameters;

	// ========================================
	// 元数据（可选）
	// ========================================

	/** 配置预设名称（如：室内、室外、地下城等） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|4. Metadata",
		meta=(Tooltip="此配置的名称/用途描述"))
	FString ConfigName = "Default";

	/** 配置描述 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|4. Metadata",
		meta=(Tooltip="配置的详细说明", MultiLine=true))
	FString ConfigDescription;

	// ========================================
	// 辅助访问函数
	// ========================================

	/** 获取Wet封闭距离阈值 */
	FORCEINLINE float GetWetClosedDistanceThreshold() const { return WetCalculationParameters.WetClosedDistanceThreshold; }
	/** 获取Wet开放距离阈值 */
	FORCEINLINE float GetWetOpenDistanceThreshold() const { return WetCalculationParameters.WetOpenDistanceThreshold; }
	/** 获取Wet低命中率阈值 */
	FORCEINLINE float GetWetHitRateLow() const { return WetCalculationParameters.WetHitRateLow; }
	/** 获取Wet高命中率阈值 */
	FORCEINLINE float GetWetHitRateHigh() const { return WetCalculationParameters.WetHitRateHigh; }
	/** 获取Wet靠墙距离阈值 */
	FORCEINLINE float GetWetNearWallDistance() const { return WetCalculationParameters.WetNearWallDistance; }
	/** 获取Wet靠墙增强量 */
	FORCEINLINE float GetWetNearWallBoost() const { return WetCalculationParameters.WetNearWallBoost; }

	// ========================================
	// 构造函数 - 设置合理的默认值
	// ========================================
	UIMAcousticFieldConfig()
	{
		// 默认混响（室外环境）
		DefaultReverbParameters.Wet = 0.05f;
		DefaultReverbParameters.Delay = 0.02f;
		DefaultReverbParameters.Decay = 0.3f;
		DefaultReverbParameters.Density = 0.2f;
		DefaultReverbParameters.Diffusion = 0.4f;
		DefaultReverbParameters.Dampening = 0.6f;
	}
};
