// AcousticIdentifier/Source/IceMoonAcousticField/Public/IMAcousticFieldActor.h

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IM_AcousticTypes.h"
#include "IM_Common/Public/Gameplay/IMSystemStats.h"

#include "IMAcousticFieldActor.generated.h"

DECLARE_CYCLE_STAT_EXTERN(TEXT("IMAcousticField Tick"), STAT_IMAcousticField_Tick, STATGROUP_IMSystem, ICEMOONACOUSTICFIELD_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("IMAcousticField Callback"), STAT_IMAcousticField_TraceCallback, STATGROUP_IMSystem, ICEMOONACOUSTICFIELD_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("IMAcousticField Query"), STAT_IMAcousticField_Query, STATGROUP_IMSystem, ICEMOONACOUSTICFIELD_API)

struct FHitResult;
struct FTraceDatum; // <-- 需要为回调函数前向声明

/**
 * 关卡单例Actor，负责管理整个世界的声学场数据。 核心:低开销/解决声源动态/不同相机位置导致的动态音效问题(音频遮蔽 环境声学)  辅助用于返回碰撞对象位置发现等用于其他用途
 * 弥补传统烘焙声学方案无法响应实时环境变化的缺陷， 更加的便携易用性

    AIM 插件 - 核心特性状态
    ✅ 已实现
        1. 关卡单例Actor + 3层LOD网格系统（2x/4x可配置倍增器）
        2. 静态世界原则：只接受Static/Stationary几何体数据
        3. 多LOD加权查询（50%-30%-20%组合插值）
        4. 物理材质→音频响应映射（DA_IM_MaterialMap + DA_IM_AcousticFieldConfig）
        5. 音源驱动的按需探测（AsyncFireProbes）
        6. 事件驱动的缓存失效（InvalidateAcousticRegion）
        7. 系统休眠状态机（Active/Moderate/Dormant自适应Tick频率）
        8. "免费"射线数据注入（AddProbeFromHitResultOnlayWorldStatic）
        9. 平滑查询系统（QueryAcousticFieldSmooth，自适应指数平滑）
        10. 自适应搜索半径（基于LOD倍增器动态调整）
        11. Z轴钳制参数（MaxCellHeightZ，防止跨楼层污染）

    🚧 待实现（按优先级排序）
        [高优] GPU SDF空间连续性检测系统（详见IM_AcousticTypes.h的TODO文档）
            - Cell创建时异步GPU验证，拒绝空间不连续的Cell
            - 查询时GPU路径遮挡检测，降低被墙遮挡Cell的权重
            - 解决当前方差检测无法精确判断跨房间Cell的问题
        [中优] 灰名单容错机制
            - 临时屏蔽错误配置为Movable的Actor（防止性能黑洞）
        [中优] 集成AudioReverbVolume回退
            - 无探针数据时查询UE内置混响体积作为后备
        [低优] 数据聚合后台任务
            - 将密集探针聚合为集群探针，优化长期运行性能

    ❌ 已废弃
        - 监听者驱动的背景探测（基于相机的持续探针）：不符合按需采样设计
        - 声学寻路/隧道效应：技术成本高，暂不实现
        - 提前通知/Token模式：对工作流破坏性大
        - Cell内sub-point法线/入射角存储：复杂度高，SDF方案更优
 */
UCLASS()
class ICEMOONACOUSTICFIELD_API AIceMoonAcousticField : public AActor
{
    GENERATED_BODY()
    
public:
    AIceMoonAcousticField();

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

public:
    // 获取单例控制器
    UFUNCTION(BlueprintPure, Category = "IM|Acoustics", meta = (WorldContext = "WorldContextObject"))
    static AIceMoonAcousticField* GetAcousticFieldActor(const UObject* WorldContextObject);

    /**
     * [按需探测] 请求在指定位置进行一次小规模、高优先级的探测。
     * 用于填充远程音源等缓存未命中的区域。
     * @param NumTraces 发射的射线数量。
     */
    UFUNCTION(BlueprintCallable, Category = "IM|Tracing")
    void AsyncFireProbes(FVector Origin, int32 NumTraces = 3, float Radius = 2000.0f, FVector ConeDirection = FVector::UpVector, float ConeDegree = 179.99,  int RandomSeed = -1); 
    
    // 临近查询 如果有多个则插值
    UFUNCTION(BlueprintCallable, Category = "IM|Acoustics")
    bool QueryAcousticField(FVector QueryLocation, FIM_AudioReverbParameters& OutResponse);

    /**
     * [平滑查询] 带指数平滑的声场查询，专为AnimNotify等离散采样事件设计
     * 自动缓存每个声源的上次查询结果，平滑过渡避免混响参数跳变
     * @param SourceObject 声源对象（可以是Actor或Component，推荐用Component）
     * @param SoundSlot 声音槽位名称（如 "FootL", "FootR", "Jump"）
     * @param QueryLocation 查询位置
     * @param OutResponse 平滑后的混响参数
     * @param SmoothSpeed 平滑时间常数（秒，默认5.0）
     *        - 表示达到63.2%收敛所需时间，3倍时间达到95%收敛
     *        - 离散采样（脚步）推荐 3.0-5.0 秒
     *        - 连续采样（环境音）推荐 0.5-1.0 秒
     *        - 值越大响应越慢，需要更多次查询才完全过渡
     * @return 总是返回true（失败时平滑到默认值）
     */
    UFUNCTION(BlueprintCallable, Category = "IM|Acoustics") 
    bool QueryAcousticFieldSmooth(
        UObject* SourceObject,
        FName SoundSlot,
        FVector QueryLocation,
        FIM_AudioReverbParameters& OutResponse,
        float SmoothSpeed = 3.0f);

    /**
     * [清理缓存] 手动清理指定对象的平滑查询缓存
     * @param SourceObject 要清理的对象（Actor或Component）
     * @param SoundSlot 声音槽位（NAME_None表示清理该对象的所有槽位）
     */
    UFUNCTION(BlueprintCallable, Category = "IM|Acoustics")
    void ClearSmoothQueryCache(UObject* SourceObject, FName SoundSlot = NAME_None);

    // 读取周围声音混淆系数等处理的参数函数
    UFUNCTION(BlueprintCallable, Category = "IM|Tracing")
    bool GetAcousticFieldExtentCells(int32 LodIndex, FVector QueryLocation, float SearchRadius, TArray<FIM_GridAudioCell>& OutCells);

//辅助函数
    // 一些发声设备他在发声时候会使用一个碰撞检测 那么这个免费的射线就因该存储用于 其他地方的返回需求   需要调用者明确知晓这个射线是静态碰撞查询到的
    UFUNCTION(BlueprintCallable, Category = "IM|Acoustics")
    void AddProbeFromHitResultOnlayWorldStatic(const FHitResult& HitResult);

    // 静态区域更新 剔除区域的声学数据
    UFUNCTION(BlueprintCallable, Category = "IM|Acoustics")
    void InvalidateAcousticRegion(const FBox& ChangedBounds);

    //物理材质 查声学 音效设置
    UFUNCTION(BlueprintCallable, Category = "IM|Acoustics", BlueprintPure)
    FIM_AudioMaterialResponse GetAudioResponseForMaterial(const UPhysicalMaterial* PhysMaterial) const;

    // ========================================
    // 配置资产引用
    // ========================================

    /** 声场配置预设（包含材质库引用、默认混响、Wet参数等） */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|Configuration",
        meta=(Tooltip="声场配置预设资产（必需）"))
    TObjectPtr<class UDA_IM_AcousticFieldConfig> ConfigAsset;
    
    // ========================================
    // 网格配置
    // ========================================

    /** 存储网格密度 - 基础网格单元大小（LOD 0） */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|Configuration",
        meta=(Tooltip="基础网格单元大小（厘米）LOD0的cell大小", ClampMin="50.0", ClampMax="500.0"))
    float GridCellSize = 200.0f;

    /** LOD层级倍增系数 - 控制LOD之间的密度差异
     * 2 = 渐进式 (2m, 4m, 8m) - 适合室内场景，更细腻的过渡
     * 4 = 激进式 (2m, 8m, 32m) - 适合大型室外场景
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|Configuration",
        meta=(Tooltip="LOD倍增系数: 2=室内优化(2-4-8m), 4=室外优化(2-8-32m)", ClampMin="2", ClampMax="4"))
    int32 LodMultiplier = 2;

    /** Z轴最大单元格高度钳制（厘米）- 防止垂直方向跨越多个楼层
     * 典型楼层高度约400cm，建议设置为400-500cm
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|Configuration",
        meta=(Tooltip="Z轴最大cell高度（厘米），避免跨楼层。0=不钳制", ClampMin="0.0", ClampMax="1000.0"))
    float MaxCellHeightZ = 400.0f;

    // ========================================
    // 性能配置
    // ========================================

    /** 活跃状态时间阈值（秒）- 最后一次查询后小于此时间则每帧Tick */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|Performance",
        meta=(Tooltip="最后查询后小于此时间 → 活跃状态（每帧Tick）", ClampMin="1.0", ClampMax="10.0"))
    float ActiveStateThreshold = 3.0f;

    /** 休眠状态时间阈值（秒）- 最后一次查询后大于此时间则降低Tick频率 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IM|Performance",
        meta=(Tooltip="最后查询后大于此时间 → 休眠状态（降低Tick频率）", ClampMin="5.0", ClampMax="30.0"))
    float DormantStateThreshold = 10.0f;

    // ========================================
    // 实例覆盖选项（可选）
    // ========================================

    /** 是否覆盖配置资产中的默认混响参数 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IM|Configuration|Override",
        meta=(Tooltip="勾选后可覆盖ConfigAsset中的默认混响参数（用于特殊关卡微调）"))
    bool bOverrideDefaultReverb = false;

    /** 覆盖的默认混响参数 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IM|Configuration|Override",
        meta=(EditCondition="bOverrideDefaultReverb", EditConditionHides, Tooltip="此实例特定的默认混响参数"))
    FIM_AudioReverbParameters DefaultReverbParameters_Override;

    /** 是否覆盖配置资产中的Wet计算参数 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IM|Configuration|Override",
        meta=(Tooltip="勾选后可覆盖ConfigAsset中的Wet计算参数"))
    bool bOverrideWetCalculation = false;

    /** 覆盖的Wet计算参数（使用结构体统一管理） */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "IM|Configuration|Override",
        meta=(EditCondition="bOverrideWetCalculation", EditConditionHides, Tooltip="此实例特定的Wet计算参数"))
    FIM_WetCalculationParameters WetCalculationParameters_Override;
    
private:
    /** 异步射线检测完成后的回调函数 */
    void OnAsyncTraceComplete(const FTraceHandle& TraceHandle, FTraceDatum& TraceDatum);
    
    // TODO  工具函数 对于全部周围函数计算声场遮蔽系数 理论上 应该是随机周围偏移采样
    // 我原版写的是每秒射线基于 法线法向当作z计算新的xy  然后 lerp(x -x y -y,随机方向, .035) 射 四个射线   *20m最大场 根据这几个射线的距离来  计算 回响延迟时间
    // 首先是回响延迟   =  平均射线长度 * 2来回 / 声速30cm/s  * 0.7艺术变化(因为平均大概率比中位数大一点)
    // 然后  低波 情况 大概率是由 物理材质决定了  但我没写这套
    // 然后 wet level      我是用射线距离 remap 6m ~20m 回归线性0~1 然后 累加  最后平均
    // 然后 density      我是根据平均值 和  每个射线的 平方  然后把这值 loge 得到一个新值 使用 remap min15 max10 反映映射回 density  
    // 然后 dry level    gain   bandwidth  diffusion  dampening  decay 都没使用   基本除了延迟
    /**
     * [内部工具] 根据一组射线检测结果，计算出一个综合的声学响应。
     * 这是你注释中描述的算法的实现入口。
     * @param HitResults 一批发声源周围的射线检测结果。
     * @return 计算出的FAIM_AudioMaterialResponse。
     */
    FIM_AudioReverbParameters CalculateCellReverbParameters(const FVector QueryPos,const FIM_GridAudioCell& CellResults);

    void AddAudioFieldForLod(const FHitResult& HitResult);
    bool InterpolateAtLod(const int32 LodIndex, const FVector QueryLocation, FIM_AudioReverbParameters& OutInterpolatedResponse, int32* OutCells = nullptr, int32* OutProbes = nullptr, int32* OutHits = nullptr);
    void TickTrimAudioFieldForLod(const float GameTime);

    // 配置获取辅助函数（优先使用Override，否则使用ConfigAsset）
    FIM_AudioReverbParameters GetDefaultReverbParameters() const;
    FIM_WetCalculationParameters GetWetCalculationParameters() const;

    // --- 内部状态 ---
    TArray< TMap<FIntVector, FIM_GridAudioCell> > AcousticGridArray;
    // CellSubBitMaskArray[0] 存储 LOD 1 对 LOD 0 的掩码
    // CellSubBitMaskArray[1] 存储 LOD 2 对 LOD 1 的掩码
    TArray< TMap<FIntVector, uint64> > CellSubBitMaskArray;
    TArray<float> LodCellSizes = { 100.0f, 400.0f, 1600.0f };
    TArray<float> LodCellSizesZ; // Z轴钳制后的尺寸（应用MaxCellHeightZ）
    int32 LodFactor = 4;

    // --- 平滑查询缓存 ---
    // 查询缓存结构
    struct FAcousticQueryCache
    {
        FIM_AudioReverbParameters LastResult;
        float LastQueryTime;
        FVector LastQueryLocation;

        FAcousticQueryCache()
            : LastResult()
            , LastQueryTime(0.0f)
            , LastQueryLocation(FVector::ZeroVector)
        {}
    };
    // 缓存Map：Key为 "ActorUniqueID_SoundSlot"
    TMap<FName, FAcousticQueryCache> SmoothQueryCache;

    //存储射线事件
    TArray<FTraceHandle> ActiveTraceHandles;
    FRandomStream ProbeRandomStream; //连续性随机种子用于保持均匀随机方向
    float LastQueryTime = -1000.0;
    float LastCleanupTime = 0.0;

};