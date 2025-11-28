// AcousticIdentifier/Source/IceMoonAcousticField/Private/IMAcousticFieldActor.cpp

#include "IMAcousticFieldActor.h"
#include "DA_IM_MaterialMap.h"
#include "DA_IM_AcousticFieldConfig.h"
#include "IMMathUtils.h"
#include "IM_Common/Public/Gameplay/IMWorldUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Runtime/PhysicsCore/Public/PhysicalMaterials/PhysicalMaterial.h" 

static TAutoConsoleVariable<int32> CVar_DebugLevelStat(
	TEXT("Icemoon.AcousticField.debug"),
	0, // 默认关闭  
	TEXT(" 1是输出事件驱动Debug 2是输出TickDebug"),
	ECVF_RenderThreadSafe
);
#define ICEMOON_LOG_DEBUG_TICK(Format, ...) if (CVar_DebugLevelStat.GetValueOnAnyThread() >= 2) UE_LOG(LogTemp, Log, TEXT("[DEBUG-TICK][%s:%d] ") TEXT(Format), ANSI_TO_TCHAR(__FILE__), __LINE__, ##__VA_ARGS__);
#define ICEMOON_LOG_DEBUG(Format, ...) if (CVar_DebugLevelStat.GetValueOnAnyThread() >= 1) UE_LOG(LogTemp, Log, TEXT("[DEBUG][%s:%d] ") TEXT(Format), ANSI_TO_TCHAR(__FILE__), __LINE__, ##__VA_ARGS__);
#define ICEMOON_LOG_WARN(Format, ...) UE_LOG(LogTemp, Warning, TEXT("[WARN][%s:%d] ") TEXT(Format), ANSI_TO_TCHAR(__FILE__), __LINE__, ##__VA_ARGS__)
#define ICEMOON_LOG_ERROR(Format, ...) UE_LOG(LogTemp, Error, TEXT("[ERROR][%s:%d] ") TEXT(Format), ANSI_TO_TCHAR(__FILE__), __LINE__, ##__VA_ARGS__)

static TWeakObjectPtr<AIceMoonAcousticField> GWorldAcousticActor; // 静态实例指针，用于快速访问

DEFINE_STAT(STAT_IMAcousticField_Tick);
DEFINE_STAT(STAT_IMAcousticField_TraceCallback);
DEFINE_STAT(STAT_IMAcousticField_Query);

AIceMoonAcousticField::AIceMoonAcousticField()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}
void AIceMoonAcousticField::BeginPlay()
{
	Super::BeginPlay();
	GWorldAcousticActor = this;

	int32 LodNum = LodCellSizes.Num();
	if (LodNum < 2 )  UE_LOG(LogTemp, Error, TEXT("IMAcousticField: LodCellSizes is empty! System will not work."));
	AcousticGridArray.SetNum(LodNum);
	CellSubBitMaskArray.SetNum(LodNum - 1);
	LodCellSizesZ.SetNum(LodNum);

	// 使用可配置的LOD倍增器生成cell尺寸
	// LodMultiplier = 2: 渐进式，适合室内
	// LodMultiplier = 4: 激进式，适合室外
	for (int i = 0; i < LodCellSizes.Num(); i++)
	{
		LodCellSizes[i] = FMath::Pow(static_cast<float>(LodMultiplier), i) * GridCellSize;

		// 计算Z轴钳制后的尺寸
		LodCellSizesZ[i] = (MaxCellHeightZ > 0.0f) ? FMath::Min(LodCellSizes[i], MaxCellHeightZ) : LodCellSizes[i];

		// Log LOD配置信息（仅Editor）
		#if WITH_EDITOR
		const float CellSizeM = LodCellSizes[i] / 100.0f; // 转换为米
		const float ClampedHeightM = LodCellSizesZ[i] / 100.0f; // Z轴钳制后的尺寸
		UE_LOG(LogTemp, Log, TEXT("IMAcousticField: LOD %d -> XY: %.2fm, Z: %.2fm (钳制=%d)"),
			i, CellSizeM, ClampedHeightM, (MaxCellHeightZ > 0.0f && LodCellSizes[i] > MaxCellHeightZ));
		#endif
	}
}
void AIceMoonAcousticField::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_IMAcousticField_Tick);
	Super::Tick(DeltaTime);

//状态机
	float LastQueryTimeOffset = GetWorld()->GetTimeSeconds() - LastQueryTime;
	int32 CurrentState = -1;
	if (LastQueryTimeOffset < ActiveStateThreshold)
	{
		CurrentState = 0; // 活跃状态
	}else if (LastQueryTimeOffset > DormantStateThreshold)
	{
		CurrentState = 2; // 休眠状态
	}else
	{
		CurrentState = 1; // 中等状态
	}
	//状态切换 修改tick
	switch (CurrentState)
	{
		case 0:
		PrimaryActorTick.TickInterval = 0.0f; // 0.0f 表示每帧Tick
		break;
		case 1:
		PrimaryActorTick.TickInterval = 0.1f;
		break;
	case 2:
		PrimaryActorTick.TickInterval = 1.0f;
		break;
	}
#if WITH_EDITOR
	if(CVar_DebugLevelStat.GetValueOnGameThread() > 0){
		// 显示状态和统计信息
		FString DebugText = FString::Printf(TEXT("IMAcoustic Field State: %d | Active Cells: %d"),
			static_cast<int32>(CurrentState),
			AcousticGridArray[0].Num());
		GEngine->AddOnScreenDebugMessage(-1, 0.f, FColor::Green, DebugText);

		// 显示所有LOD层级的cell统计
		for (int32 LodIndex = 0; LodIndex < AcousticGridArray.Num(); ++LodIndex)
		{
			FString LodStats = FString::Printf(TEXT("  LOD %d: %d cells (CellSize: %.1fcm)"),
				LodIndex,
				AcousticGridArray[LodIndex].Num(),
				LodCellSizes[LodIndex]);
			GEngine->AddOnScreenDebugMessage(-1, 0.f, FColor::Cyan, LodStats);
		}

		// 可视化所有存在的cell（使用不同颜色区分LOD）
		TArray<FColor> LodColors = {
			FColor::Green,   // LOD 0 - 最细致
			FColor::Yellow,  // LOD 1 - 中等
			FColor::Orange   // LOD 2 - 最粗糙
		};

		for (int32 LodIndex = 0; LodIndex < AcousticGridArray.Num(); ++LodIndex)
		{
			const float CellSize = LodCellSizes[LodIndex];
			const float CellSizeZ = LodCellSizesZ[LodIndex]; // Z轴钳制后的尺寸
			FColor DrawColor = LodIndex < LodColors.Num() ? LodColors[LodIndex] : FColor::White;

			for (const auto& CellPair : AcousticGridArray[LodIndex])
			{
				const FIntVector& GridCoord = CellPair.Key;
				const FIM_GridAudioCell& Cell = CellPair.Value;

				const FIM_AudioReverbParameters ReverbParams = CalculateCellReverbParameters(FVector::ZeroVector, Cell);
				DrawColor.A = static_cast<uint8>(FMath::Clamp(ReverbParams.Wet * 200.0f + 20.0f, 20.0f, 220.0f));
				
				// 计算cell的中心位置（Z轴使用钳制后的尺寸）
				const FVector CellCenter(
					(GridCoord.X + 0.5f) * CellSize,
					(GridCoord.Y + 0.5f) * CellSize,
					(GridCoord.Z + 0.5f) * CellSizeZ
				);
				const FVector CellExtent(CellSize * 0.5f, CellSize * 0.5f, CellSizeZ * 0.5f);

				// 绘制cell边界框
				DrawDebugBox(GetWorld(), CellCenter, CellExtent, DrawColor, false, -1.0f, 0, 2.0f);

				// 在cell中心绘制一个小球，表示平均命中位置
				//DrawDebugSphere(GetWorld(), Cell.RayRes.AveHitLocation, 10.0f, 8, DrawColor, false, -1.0f, 0, 2.0f);

				// 绘制cell信息（probe数量）
				const FString CellInfo = FString::Printf(TEXT("P:%d"), Cell.RayRes.ProbeCount);
				DrawDebugString(GetWorld(), CellCenter, CellInfo, nullptr, DrawColor, 0.0f, true);
			}
		}
	}
#endif
	if (CurrentState >= 2) return;

	//如果激活则 启动 清理grid的cell计划  TODO目前写的随意
	if (GetWorld()->TimeSince(LastCleanupTime) > 5.0f)
	{
		const float CurrentTime = GetWorld()->GetTimeSeconds();
		TickTrimAudioFieldForLod( CurrentTime );
		LastCleanupTime = CurrentTime;
	}
}


AIceMoonAcousticField* AIceMoonAcousticField::GetAcousticFieldActor(const UObject* WorldContextObject)
{
	// 1. 安全获取 World (防御性原则：Context 可能无效)
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return nullptr;

	// 2. 缓存命中检查 (关键修正：必须校验 World 是否匹配，防止 PIE 跨世界引用错误)
	if (GWorldAcousticActor.IsValid())
	{
		// 如果缓存的 Actor 属于当前 World，直接返回
		if (GWorldAcousticActor->GetWorld() == World){ return GWorldAcousticActor.Get();
		}else{
			// 缓存失效或属于其他 World (如从 PIE 切回 Editor)，重置
			GWorldAcousticActor.Reset();
		}
	}

	// 3. 场景查找 (慢速路径)
	AIceMoonAcousticField* ResultActor = Cast<AIceMoonAcousticField>(
		UGameplayStatics::GetActorOfClass(World, AIceMoonAcousticField::StaticClass())
	);

	// 4. 不存在则创建 (新增逻辑)
	if (!ResultActor)
	{
		// 防御性：避免在构建脚本(Construction Script)或不安全的时机生成
		if (!World->IsGameWorld())
		{
			// 这里的 Log 很重要，避免在预览界面意外生成垃圾对象
			UE_LOG(LogTemp, Warning, TEXT("[IM] GetAcousticFieldActor: Attempting to spawn in non-game world context."));
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = FName("IM_AutoCreatedAcousticField_Singleton"); // 具名以便调试
		SpawnParams.ObjectFlags = RF_Transient; // 如果该 Actor 不需要随关卡保存，加上此标记

		// 核心创建接口
		ResultActor = World->SpawnActor<AIceMoonAcousticField>(
			AIceMoonAcousticField::StaticClass(), 
			FVector::ZeroVector, 
			FRotator::ZeroRotator, 
			SpawnParams
		);

		UE_LOG(LogTemp, Log, TEXT("[IM] Created new AcousticField Actor in World: %s"), *World->GetName());
	}

	// 5. 更新全局缓存
	if (ResultActor)
	{
		GWorldAcousticActor = ResultActor;
	}

	return ResultActor;
}


void AIceMoonAcousticField::AsyncFireProbes( FVector Origin, int32 NumTraces, float Radius, FVector Direction, float ConeDegree, int RandomSeed)
{
	UWorld* World = GetWorld();
	if (!World || NumTraces <= 0) return;
	FTraceDelegate TraceDelegate;
	TraceDelegate.BindUObject(this, &AIceMoonAcousticField::OnAsyncTraceComplete);
	TArray<FVector> SampleDirections;
	IMMathUtils::GetFibonacciSphereSamples(SampleDirections, NumTraces, Direction, ConeDegree, true, RandomSeed);

	FCollisionQueryParams Params;
	Params.bReturnPhysicalMaterial = true;
	Params.MobilityType = EQueryMobilityType::Static; // 只查询 Static Mobility 的对象

	for (const FVector& RandomDir : SampleDirections)
	{
		const FVector Start = Origin + RandomDir * 3.0f;//开始位置 偏移3.0cm 为了防止大部分box墙面是000 1,0,0等固定整数正好在 阀值位置的困境
		const FVector End = Origin + RandomDir * Radius;

#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			DrawDebugLine(World, Origin, Origin + RandomDir * 50.0f, FColor::Blue, false, 0.5f);
		}
#endif
		// 【修复】改用 ByChannel + MobilityType 方案：
		// - Visibility 通道：大部分静态几何体默认 Block，查询组件默认 Ignore
		// - MobilityType::Static：只查询 Mobility=Static 的对象，自动过滤动态查询组件
		// 优势：不需要手动维护忽略列表，配置更清晰
		World->AsyncLineTraceByChannel(
			EAsyncTraceType::Single,
			Start,
			End,
			ECC_Visibility, // 使用 Visibility 通道代替 ObjectType 查询
			Params,
			FCollisionResponseParams::DefaultResponseParam, // ByChannel 需要此参数
			&TraceDelegate
		);
	}
}
void AIceMoonAcousticField::OnAsyncTraceComplete(const FTraceHandle& TraceHandle, FTraceDatum& TraceDatum)
{
	SCOPE_CYCLE_COUNTER(STAT_IMAcousticField_TraceCallback);

	// 核心修复：检查 OutHits 是否为空（未命中时为空）
	if (TraceDatum.OutHits.Num() > 0)
	{
		// 命中：正常处理所有命中结果
		for (int i = 0; i < TraceDatum.OutHits.Num(); ++i)
		{
			const FHitResult& Hit = TraceDatum.OutHits[i];
#if WITH_EDITOR
			if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
			{
				AActor* HitActor = Hit.GetActor();
				const FString ActorName = HitActor ? HitActor->GetName() : TEXT("None");
				const float Dist = Hit.Distance;
				UE_LOG(LogTemp, Log, TEXT("  [命中] %s (%.1fcm)"), *ActorName, Dist);
				DrawDebugLine(GetWorld(), Hit.TraceStart, Hit.ImpactPoint, FColor::Yellow, false, 0.5f);
			}
#endif
			AddProbeFromHitResultOnlayWorldStatic(Hit);
		}
	}else{
		// 未命中：OutHits 为空，手动创建未命中的 HitResult
		FHitResult MissResult;
		MissResult.TraceStart = TraceDatum.Start;
		MissResult.TraceEnd = TraceDatum.End;
		MissResult.bBlockingHit = false;
#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("  [未命中] 射向开阔空间"));
			DrawDebugLine(GetWorld(), TraceDatum.Start, TraceDatum.End, FColor::Red, false, 0.5f);
		}
#endif
		// 添加未命中记录（只增加 ProbeCount，不增加 RayHitCount）
		AddProbeFromHitResultOnlayWorldStatic(MissResult);
	}
}
void AIceMoonAcousticField::AddProbeFromHitResultOnlayWorldStatic(const FHitResult& HitResult)
{
	// 未命中的射线也需要记录（代表开阔空间）
	// 不要过早return，让AddAudioFieldForLod处理所有情况

	if (HitResult.bBlockingHit)
	{
		AActor* HitActor = HitResult.GetActor();
		if (HitActor)
		{
			if (HitActor->GetRootComponent()->Mobility == EComponentMobility::Movable) // Static 和 Stationary 则添加
			{
				// TODO: 灰名单容错机制（中优先级）- 临时屏蔽错误配置的Movable Actor
#if WITH_EDITOR
				if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
				{
					UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: 忽略可移动物体 - %s"), *HitActor->GetName());
				}
#endif
				return; // 组件必须静态
			}
		}
	}

	// 无论命中与否都记录到声场
	AddAudioFieldForLod(HitResult);
}
void AIceMoonAcousticField::AddAudioFieldForLod(const FHitResult& HitResult)
{
	const FVector StartPos = HitResult.TraceStart;

	// 判断射线是否有效命中（必须是非移动组件）
	const bool bIsValidHit = HitResult.bBlockingHit;

	// 获取命中相关数据（仅在命中时使用）
	const FVector HitLocation = bIsValidHit ? HitResult.ImpactPoint : FVector::ZeroVector;
	const float Distance = bIsValidHit ? (StartPos - HitLocation).Length() : 0.0f;
	const FIM_AudioMaterialResponse AudioData = bIsValidHit ? GetAudioResponseForMaterial(HitResult.PhysMaterial.Get()) : FIM_AudioMaterialResponse();
	const float DirecitonVar = bIsValidHit ? HitResult.ImpactNormal.Dot(HitResult.Normal) : 0.0f;
	const float CurrentTime = GetWorld()->GetTimeSeconds();

	if (AcousticGridArray.IsEmpty()) return;

	// --- 3. 写入LOD 0并向上递归更新掩码 ---
	for (int32 LodIndex = 0; LodIndex < LodCellSizes.Num(); ++LodIndex)
	{
		const float CellSize = LodCellSizes[LodIndex];
		const float CellSizeZ = LodCellSizesZ[LodIndex]; // Z轴钳制后的尺寸

		// 计算网格坐标（Z轴使用钳制后的尺寸）
		const FIntVector GridCoord(
			FMath::FloorToInt(StartPos.X / CellSize),
			FMath::FloorToInt(StartPos.Y / CellSize),
			FMath::FloorToInt(StartPos.Z / CellSizeZ)
		);

		// 写入主数据（bIsValidHit = false 时只增加 ProbeCount，不记录材质）
		const bool bIsNewCell = !AcousticGridArray[LodIndex].Contains(GridCoord);
		AcousticGridArray[LodIndex].FindOrAdd(GridCoord).AddProbeData(HitLocation, Distance, bIsValidHit, DirecitonVar, AudioData, CurrentTime);
#if WITH_EDITOR
		// 只输出新建 Cell 的信息（使用不同的标记便于区分）
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Display, TEXT("  [新建Cell] LOD%d @ %s (总数:%d)"),
				LodIndex, *GridCoord.ToString(), AcousticGridArray[LodIndex].Num());
		}
#endif
		// 如果不是最顶层，就需要更新其父层级的掩码
		if (LodIndex < CellSubBitMaskArray.Num())
		{
			// 计算在下一层(父层级)的坐标
			const FIntVector ParentGridCoord(
				FMath::FloorToInt(static_cast<float>(GridCoord.X) / LodFactor),
				FMath::FloorToInt(static_cast<float>(GridCoord.Y) / LodFactor),
				FMath::FloorToInt(static_cast<float>(GridCoord.Z) / LodFactor)
			);
            
			// 计算在本父单元格内的局部3D坐标
			const FIntVector LocalCoord = GridCoord - ParentGridCoord * LodFactor;

			// 将3D局部坐标转换为1D索引 (0-63)
			const uint64 LocalIndex = LocalCoord.X + (LocalCoord.Y * LodFactor) + (LocalCoord.Z * LodFactor * LodFactor);

			if (LocalIndex < 64)
			{
				// 获取对应LOD的掩码Map
				TMap<FIntVector, uint64>& MaskMap = CellSubBitMaskArray[LodIndex];
                
				// 使用位或操作来设置对应的bit为1
				MaskMap.FindOrAdd(ParentGridCoord) |= (1ULL << LocalIndex);
			}
		}
	}
}

void AIceMoonAcousticField::InvalidateAcousticRegion(const FBox& ChangedBounds)
{
	if (AcousticGridArray.IsEmpty() || CellSubBitMaskArray.IsEmpty()) return;

	for (int32 LodIndex = 0; LodIndex < LodCellSizes.Num(); ++LodIndex)
	{
		const float CellSize = LodCellSizes[LodIndex];
		const float CellSizeZ = LodCellSizesZ[LodIndex]; // Z轴钳制后的尺寸
		TMap<FIntVector, FIM_GridAudioCell>& CurrentGrid = AcousticGridArray[LodIndex];

		// (这是一个简化的实现，更精确的实现需要迭代Box内的所有单元格)
		for (auto It = CurrentGrid.CreateIterator(); It; ++It)
		{
			const FIntVector& GridCoord = It.Key();
			const FVector CellCenter(
				(GridCoord.X + 0.5f) * CellSize,
				(GridCoord.Y + 0.5f) * CellSize,
				(GridCoord.Z + 0.5f) * CellSizeZ
			);
			if (ChangedBounds.IsInside(CellCenter))
			{
				// --- 开始修改 ---
				const FIntVector RemovedGridCoord = It.Key();
				
				// 1. 移除Cell
				It.RemoveCurrent();

				// 2. 更新父级的Mask (如果这不是最顶层LOD)
				if (LodIndex < CellSubBitMaskArray.Num())
				{
					const FIntVector ParentGridCoord(
						FMath::FloorToInt(static_cast<float>(RemovedGridCoord.X) / LodFactor),
						FMath::FloorToInt(static_cast<float>(RemovedGridCoord.Y) / LodFactor),
						FMath::FloorToInt(static_cast<float>(RemovedGridCoord.Z) / LodFactor)
					);

					if (uint64* Mask = CellSubBitMaskArray[LodIndex].Find(ParentGridCoord))
					{
						const FIntVector LocalCoord = RemovedGridCoord - ParentGridCoord * LodFactor;
						const uint64 LocalIndex = LocalCoord.X + (LocalCoord.Y * LodFactor) + (LocalCoord.Z * LodFactor * LodFactor);

						if (LocalIndex < 64)
						{
							// 将对应的bit清零
							*Mask &= ~(1ULL << LocalIndex);
						}
						
						if (*Mask == 0u)
						{
							CellSubBitMaskArray[LodIndex].Remove(ParentGridCoord);
						}
					}
				}
				// --- 结束修改 ---
			}
		}
	}
}
FIM_AudioMaterialResponse AIceMoonAcousticField::GetAudioResponseForMaterial(const UPhysicalMaterial* PhysMaterial) const
{
	// 从ConfigAsset的MaterialLibrary获取
	UDA_IM_MaterialMap* MatLib = nullptr;
	if (ConfigAsset && ConfigAsset->MaterialLibrary)
	{
		MatLib = ConfigAsset->MaterialLibrary;
	}

	if (!MatLib)
	{
		UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: No MaterialLibrary configured in ConfigAsset"));
		return FIM_AudioMaterialResponse();
	}

	if (!PhysMaterial)
	{
		return MatLib->Fallback_MaterialResponse;
	}

	if (const FIM_AudioMaterialResponse* FoundResponse = MatLib->MaterialMap.Find(PhysMaterial))
	{
		return *FoundResponse;
	}

	return MatLib->Fallback_MaterialResponse;
}


bool AIceMoonAcousticField::QueryAcousticField(FVector QueryLocation, FIM_AudioReverbParameters& OutResponse)
{
	SCOPE_CYCLE_COUNTER(STAT_IMAcousticField_Query);
	LastQueryTime = GetWorld()->GetTimeSeconds();

	if (AcousticGridArray.IsEmpty())
	{
		OutResponse = FIM_AudioReverbParameters();
		return false;
	}

#if WITH_EDITOR
	if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
	{
		// 绘制查询位置
		DrawDebugSphere(GetWorld(), QueryLocation, 20.0f, 12, FColor::Magenta, false, 0.5f, 0, 3.0f);
	}
#endif

	// ========== 新的多LOD加权组合查询系统 ==========
	// 策略：每个LOD层级都做插值查询，然后按固定权重组合
	// LOD 0 (高精度): 50% 权重
	// LOD 1 (中精度): 30% 权重
	// LOD 2 (低精度): 20% 权重

	const int32 TopLod = LodCellSizes.Num() - 1;

#if WITH_EDITOR
	if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
	{
		// [调试] 显示LOD配置和坐标转换过程
		UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: [多LOD加权查询] QueryLocation=%s"), *QueryLocation.ToString());
		for (int32 i = 0; i < LodCellSizes.Num(); ++i)
		{
			const float CellSize = LodCellSizes[i];
			const float CellSizeZ = LodCellSizesZ[i];
			const FIntVector GridCoord(
				FMath::FloorToInt(QueryLocation.X / CellSize),
				FMath::FloorToInt(QueryLocation.Y / CellSize),
				FMath::FloorToInt(QueryLocation.Z / CellSizeZ)
			);
			const bool bExists = AcousticGridArray[i].Contains(GridCoord);
			UE_LOG(LogTemp, Warning, TEXT("  LOD %d: CellXY=%.1fcm, CellZ=%.1fcm, GridCoord=%s, 存在=%d"),
				i, CellSize, CellSizeZ, *GridCoord.ToString(), bExists);
		}
	}
#endif

	// 累积多个LOD的结果
	FIM_AudioReverbParameters AccumulatedResponse;
	AccumulatedResponse.Wet = 0.0f;
	AccumulatedResponse.Delay = 0.0f;
	AccumulatedResponse.Decay = 0.0f;
	AccumulatedResponse.Density = 0.0f;
	AccumulatedResponse.Diffusion = 0.0f;
	AccumulatedResponse.Dampening = 0.0f;
	float TotalWeight = 0.0f;
	bool bFoundData = false;

	// 固定LOD权重配比（从精确到粗糙）
	TArray<float> LodWeights = { 0.5f, 0.3f, 0.2f }; // LOD 0, 1, 2

	// 从最粗糙到最精确查询（确保有兜底数据）
	for (int32 LodIndex = TopLod; LodIndex >= 0; --LodIndex)
	{
		FIM_AudioReverbParameters LodResponse;
		int32 Cells = 0, Probes = 0, Hits = 0;

		// 每个LOD都做插值查询（搜索周围格子）
		if (InterpolateAtLod(LodIndex, QueryLocation, LodResponse, &Cells, &Probes, &Hits))
		{
			// 获取该LOD的固定权重
			const float LodWeight = (LodIndex < LodWeights.Num()) ? LodWeights[LodIndex] : 0.1f;

			// 累加加权结果
			AccumulatedResponse.Wet += LodResponse.Wet * LodWeight;
			AccumulatedResponse.Delay += LodResponse.Delay * LodWeight;
			AccumulatedResponse.Decay += LodResponse.Decay * LodWeight;
			AccumulatedResponse.Density += LodResponse.Density * LodWeight;
			AccumulatedResponse.Diffusion += LodResponse.Diffusion * LodWeight;
			AccumulatedResponse.Dampening += LodResponse.Dampening * LodWeight;
			TotalWeight += LodWeight;
			bFoundData = true;

#if WITH_EDITOR
			if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
			{
				const float HitRate = Probes > 0 ? static_cast<float>(Hits) / static_cast<float>(Probes) : 0.0f;
				UE_LOG(LogTemp, Log, TEXT("IMAcousticField: -> LOD %d: W=%.2f, Wet=%.2f [C=%d, P=%d, H=%d, HR=%.2f]"),
					LodIndex, LodWeight, LodResponse.Wet, Cells, Probes, Hits, HitRate);
			}
#endif
		}
#if WITH_EDITOR
		else if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: -> LOD %d 无数据"), LodIndex);
		}
#endif
	}

	// 归一化结果
	if (bFoundData && TotalWeight > 0.001f)
	{
		// 注意：由于权重总和可能不是1.0（某些LOD可能无数据），需要归一化
		const float NormalizeFactor = 1.0f / TotalWeight;
		OutResponse.Wet = AccumulatedResponse.Wet * NormalizeFactor;
		OutResponse.Delay = AccumulatedResponse.Delay * NormalizeFactor;
		OutResponse.Decay = AccumulatedResponse.Decay * NormalizeFactor;
		OutResponse.Density = AccumulatedResponse.Density * NormalizeFactor;
		OutResponse.Diffusion = AccumulatedResponse.Diffusion * NormalizeFactor;
		OutResponse.Dampening = AccumulatedResponse.Dampening * NormalizeFactor;

#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("IMAcousticField: [最终结果] Wet=%.2f (归一化因子=%.2f)"),
				OutResponse.Wet, NormalizeFactor);
		}
#endif
	}
	else
	{
		// 没有任何数据，返回默认值
		OutResponse = FIM_AudioReverbParameters();
		bFoundData = false;
#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0) UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: -> 所有LOD均无数据，返回默认值"));
#endif
	}

	return bFoundData;
}
// 使用lod 这个查询只会查询最稀疏版本
bool AIceMoonAcousticField::GetAcousticFieldExtentCells(int32 LodIndex, FVector QueryLocation, float SearchRadius,
	TArray<FIM_GridAudioCell>& OutCells)
{
	OutCells.Empty();
	const float CellSize = LodCellSizes[LodIndex];
	const float CellSizeZ = LodCellSizesZ[LodIndex]; // Z轴钳制后的尺寸

	if (SearchRadius <= 0.0) return false;

	// 计算搜索范围（Z轴使用钳制后的尺寸）
	const FIntVector MinGrid(
		FMath::FloorToInt((QueryLocation.X - SearchRadius) / CellSize),
		FMath::FloorToInt((QueryLocation.Y - SearchRadius) / CellSize),
		FMath::FloorToInt((QueryLocation.Z - SearchRadius) / CellSizeZ)
	);
	const FIntVector MaxGrid(
		FMath::FloorToInt((QueryLocation.X + SearchRadius) / CellSize),
		FMath::FloorToInt((QueryLocation.Y + SearchRadius) / CellSize),
		FMath::FloorToInt((QueryLocation.Z + SearchRadius) / CellSizeZ)
	);
	for (int32 x = MinGrid.X; x <= MaxGrid.X; ++x)
	{
		for (int32 y = MinGrid.Y; y <= MaxGrid.Y; ++y)
		{
			for (int32 z = MinGrid.Z; z <= MaxGrid.Z; ++z)
			{
				if (const FIM_GridAudioCell* Cell = AcousticGridArray[LodIndex].Find(FIntVector(x,y,z)))
				{
					if (FVector::Dist(QueryLocation, Cell->RayRes.AveHitLocation) <= SearchRadius)
					{
						OutCells.Add(*Cell);
					}
				}
			}
		}
	}
	return !OutCells.IsEmpty();
}

/*
	Wet   查询box  难以计算推荐用空间混响box来做  是所有反射声（早期反射 + 混响尾音）的集合

	delay  射线  找最短(相机->反射碰撞位置 + 发生源->反射碰撞位置) / 声速 * 艺术加工    *0.666~1.33 或者 找最短射线   声速343m/s
	Decay 衰减60dB时间  射线+材质  就是声音留存时间    房间尺寸*材质硬度  0.161 * V / A    V计算为半box  A是全部面积积分后的吸收率   (low + high)/2
	Density  射线    命中点方差*反射体数量   注:AverageVariance在1次射线的时候需要调整写一些东西       就是算 命中率 命中率0则给个0.几数值

	Diffusion   材质   它模拟的是房间表面的不规则性。高扩散意味着声音在各个角度均匀散射，回音听起来模糊平滑。低扩散意味着反射集中，听起来像回声 (Echo)。   MaterialScatter∗0.7+DirectionVariance∗0.3
	Dampening   材质  高频材质吸收。 它模拟的是高频声音被地毯、窗帘等软材质更快吸收的现象。

	Gain   不管    纯粹的工程控制。它就是混音师调整音量用的，用于确保混响不会太大或太小
	Bandwidth  不管   很多时候，低频混响会使混音变得泥泞（Muddy），高频混响会很刺耳。Bandwidth 允许工程师切掉不需要的频率，与声场物理无关。
*/
FIM_AudioReverbParameters AIceMoonAcousticField::CalculateCellReverbParameters(const FVector QueryPos, const FIM_GridAudioCell& CellResults)
{
	if (CellResults.RayRes.RayHitCount == 0) return FIM_AudioReverbParameters();
	FIM_AudioReverbParameters Reverb;
	/*
	Wet(0.0f),
	Delay(0.0f),
	Decay(0.5f),
	Density(0.5f),
	Diffusion(0.5f),
	Dampening(0.5f)
	 */

	// ========== 实时计算 Wet（混响强度） ==========
	// 基于：1.空间封闭度 2.墙壁覆盖率 3.空间均匀性

	// 获取Wet计算参数
	const FIM_WetCalculationParameters WetParams = GetWetCalculationParameters();

	// 关键修复：未命中射线代表开阔空间（>20m），必须参与平均距离计算
	const int32 MissCount = CellResults.RayRes.ProbeCount - CellResults.RayRes.RayHitCount;
	const float MaxProbeDistanceCm = 2000.0f; // 探测最大距离（默认20m = 2000cm）

	// 1. 空间封闭度（基于加权平均距离 - 包含未命中射线）
	// 公式：AvgDist = (命中射线总距离 + 未命中数量 × 最大距离) / 总探测数
	const float WeightedSumDistanceCm = CellResults.RayRes.SumDistance + static_cast<float>(MissCount) * MaxProbeDistanceCm;
	const float AvgDistanceCm = WeightedSumDistanceCm / static_cast<float>(CellResults.RayRes.ProbeCount);
	const float AvgDistanceM = AvgDistanceCm * 0.01f; // 转换为米

	// 距离映射：封闭阈值内（小房间）→1.0, 开放阈值外（开阔空间）→0.0
	const float ClosureFactor = IMMathUtils::Remap_Sat<float>(
		WetParams.WetOpenDistanceThreshold,
		WetParams.WetClosedDistanceThreshold,
		0.0f, 1.0f, AvgDistanceM);

	// 2. 墙壁覆盖率（射线命中率）
	const float HitRate = static_cast<float>(CellResults.RayRes.RayHitCount) / static_cast<float>(CellResults.RayRes.ProbeCount);
	// 平滑插值：命中率阈值控制混响强弱
	const float HitRateFactor = IMMathUtils::Smoothstep_Sat<float>(
		WetParams.WetHitRateLow,
		WetParams.WetHitRateHigh,
		HitRate);

	// 3. 空间均匀性（包含未命中射线的方差影响）
	// 计算真实方差：考虑未命中射线（它们距离固定为MaxProbeDistanceCm）
	// 方差 = Σ(xi - μ)² / N，其中μ是加权平均距离
	float TotalVarianceCm2 = 0.0f;
	if (CellResults.RayRes.RayHitCount > 0)
	{
		// 命中射线的方差贡献（使用存储的平均方差 × 命中数量）
		const float HitAvgDistCm = CellResults.RayRes.SumDistance / static_cast<float>(CellResults.RayRes.RayHitCount);
		// 重新计算命中射线相对于加权平均的方差
		const float HitDeviationCm = HitAvgDistCm - AvgDistanceCm;
		TotalVarianceCm2 += CellResults.RayRes.AveVariance * CellResults.RayRes.RayHitCount; // 命中射线内部方差
		TotalVarianceCm2 += HitDeviationCm * HitDeviationCm * CellResults.RayRes.RayHitCount; // 命中组整体偏移
	}
	if (MissCount > 0)
	{
		// 未命中射线的方差贡献（它们都是MaxProbeDistanceCm）
		const float MissDeviationCm = MaxProbeDistanceCm - AvgDistanceCm;
		TotalVarianceCm2 += MissDeviationCm * MissDeviationCm * MissCount;
	}
	const float EffectiveVarianceCm2 = TotalVarianceCm2 / static_cast<float>(CellResults.RayRes.ProbeCount);
	const float UniformityFactor = 1.0f / (1.0f + EffectiveVarianceCm2 * 0.0001f); // 调节系数

	// 4. 最短距离修正（靠墙时增强混响）
	// 如果最短距离很近，说明靠近墙壁，应该增强混响
	const float MinDistM = CellResults.RayRes.MinDistance * 0.01f;
	const float NearWallBoost = IMMathUtils::Remap_Sat<float>(
		2.0f,
		WetParams.WetNearWallDistance,
		0.0f,
		WetParams.WetNearWallBoost,
		MinDistM);

	// 综合计算Wet（指数饱和映射）
	float WetRaw = ClosureFactor * HitRateFactor * UniformityFactor + NearWallBoost;

	// 指数饱和曲线：Wet = Max * (1 - e^(-k*x))
	// 特点：低值时接近线性响应，高值时自然饱和到MaxWetValue
	// SaturationRate控制曲线陡峭度（2.5为推荐值，越大越陡）
	const float SaturationRate = 2.5f;
	Reverb.Wet = WetParams.MaxWetValue * (1.0f - FMath::Exp(-SaturationRate * WetRaw));
	
	FVector3d camPos = IMWorldUtils::GetCameraLocation(this);
	//FVector camPos = FVector();
	float mindis = CellResults.RayRes.MinDistance;
	FVector hitPos = CellResults.RayRes.AveHitLocation;
	hitPos = (hitPos - QueryPos).GetSafeNormal() * mindis + QueryPos;
	float hitToCameraDis = (hitPos - camPos).Length();
	const float AudioSpeed = 34300.0f;
	Reverb.Delay = (hitToCameraDis + mindis) / 34300.0;
	//Reverb.Delay *= 1000.0f;  //秒变毫秒   修改了 默认用秒做单位

	// 使用加权平均距离计算空间体积（已包含未命中射线的开阔空间影响）
	// AvgDistanceM已在前面计算（包含未命中射线）
	float VolueSize = AvgDistanceM * AvgDistanceM * 0.5 * AvgDistanceM; //半box空间
	float avgDampening = (CellResults.MatRes.LowDampening + CellResults.MatRes.HighDampening) * 0.5;
	avgDampening = FMath::Lerp(0.1, 1.0, avgDampening);  //暂时写这个
	Reverb.Decay = VolueSize * 0.161 / avgDampening;
	Reverb.Decay = FMath::Clamp(0.01, 1.0, Reverb.Decay);

	float RayHitRate = (float)CellResults.RayRes.RayHitCount / (float)CellResults.RayRes.ProbeCount;
	Reverb.Density = FMath::Clamp(0.1, 1.0, RayHitRate);

	Reverb.Diffusion = CellResults.MatRes.Diffusion;
	Reverb.Dampening = CellResults.MatRes.HighDampening;
	return Reverb;
}

bool AIceMoonAcousticField::InterpolateAtLod(const int32 LodIndex, const FVector QueryLocation, FIM_AudioReverbParameters& OutInterpolatedResponse, int32* OutCells, int32* OutProbes, int32* OutHits)
{
	// TODO: [高优先级] GPU SDF空间连续性检测系统
	// 详细架构方案和GPU延迟处理策略请查看：IM_AcousticTypes.h:75-155
	// 当前使用方差检测，无法精确判断cell是否横跨多个房间/墙体

	TArray<FIM_GridAudioCell> NearbyCells;

	// ========== 自适应搜索半径 ==========
	// 根据LOD倍增器调整搜索策略：
	// - 2x倍增器 (2-4-8m): 每个LOD间隔较小，需要更紧凑的搜索半径
	// - 4x倍增器 (2-8-32m): LOD 2很大，需要缩小搜索半径避免污染
	float SearchRadiusFactor;
	if (LodMultiplier == 2)
	{
		// 2x倍增器：紧凑模式（适合室内）
		// LOD 0 (2m): 2.0x = 4m  - 精确局部查询
		// LOD 1 (4m): 1.5x = 6m  - 中等范围
		// LOD 2 (8m): 1.0x = 8m  - 保守范围，避免跨房间
		if (LodIndex == 0)
			SearchRadiusFactor = 2.0f;
		else if (LodIndex == 1)
			SearchRadiusFactor = 1.5f;
		else
			SearchRadiusFactor = 1.0f;
	}
	else // LodMultiplier == 4
	{
		// 4x倍增器：激进模式（适合室外）
		// LOD 0 (2m): 3.0x = 6m  - 扩大搜索以弥补cell稀疏性
		// LOD 1 (8m): 1.5x = 12m - 中等范围
		// LOD 2 (32m): 0.6x = 19.2m - 严格限制，避免远距离污染
		if (LodIndex == 0)
			SearchRadiusFactor = 3.0f;
		else if (LodIndex == 1)
			SearchRadiusFactor = 1.5f;
		else
			SearchRadiusFactor = 0.6f;
	}

	const float SearchRadius = LodCellSizes[LodIndex] * SearchRadiusFactor;
	if (!GetAcousticFieldExtentCells(LodIndex, QueryLocation, SearchRadius, NearbyCells)) // 搜索1.5倍格子大小的范围
	{
		OutInterpolatedResponse = FIM_AudioReverbParameters();
#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: InterpolateAtLod LOD %d: No nearby cells found within %.1fcm"), LodIndex, SearchRadius);
		}
#endif
		return false;
	}

	FIM_AudioReverbParameters AccumulatedResponse; //必须为0然后按权重计算后的叠加来算 而非初始化为正确值
	AccumulatedResponse.Wet = 0.0f;
	AccumulatedResponse.Delay = 0.0f;
	AccumulatedResponse.Decay = 0.0f;
	AccumulatedResponse.Density = 0.0f;
	AccumulatedResponse.Diffusion = 0.0f;
	AccumulatedResponse.Dampening = 0.0f;
	float TotalWeight = 0.0f;

	// 统计插值使用的探针信息
	int32 TotalProbes = 0;
	int32 TotalHits = 0;
	int32 CellsUsed = 0;

	for (const FIM_GridAudioCell& Cell : NearbyCells)
	{
		//注意这里是距离平方 1 4 16 查询周围1.5倍单元格  16最大可能查询到3格=48米外 4800*4800=23040000的点 1.0f / DistanceSqr直接炸了别说Confidence方差也是一个巨幅缩减的玩意
		const float DistanceSqr = FVector::DistSquared(QueryLocation, Cell.RayRes.AveHitLocation) / 10000.0f;   // /10000 转换为米单位平方
		const float MinDistanceSqr = 1.0f;
		if (DistanceSqr < MinDistanceSqr) // 如果点1m内 就直接用这个单元格的数据
		{
			OutInterpolatedResponse = CalculateCellReverbParameters(QueryLocation, Cell);
#if WITH_EDITOR
			if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
			{
				const float HitRate = Cell.RayRes.ProbeCount > 0
					? static_cast<float>(Cell.RayRes.RayHitCount) / static_cast<float>(Cell.RayRes.ProbeCount)
					: 0.0f;
				UE_LOG(LogTemp, Log, TEXT("IMAcousticField: -> Direct cell hit (dist < 1m) [Probes=%d, Hits=%d, HitRate=%.2f]"),
					Cell.RayRes.ProbeCount, Cell.RayRes.RayHitCount, HitRate);
				// 绘制直接命中的cell
				DrawDebugLine(GetWorld(), QueryLocation, Cell.RayRes.AveHitLocation, FColor::White, false, 0.5f, 0, 4.0f);
				//DrawDebugSphere(GetWorld(), Cell.RayRes.AveHitLocation, 15.0f, 8, FColor::White, false, 0.5f, 0, 2.0f);
			}
#endif
			return true;
		}

		const float TimeSinceUpdate = GetWorld()->GetTimeSeconds() - Cell.LastUpdateTime;
		const float TimeWeight = IMMathUtils::Remap_Sat<float>(5.0, 30.0, 1.0, 0.2, TimeSinceUpdate); //时间权重
		const float ClampedVariance = Cell.RayRes.AveVariance / 2000.0;  // todo 有问题 1m平方就 10000了
		const float Confidence = 1.0f / (1.0f + ClampedVariance *  0.0001f); // 方差越大，可信度越低 说明空间均匀性很差  todo 默认先0.0001f后面调整 暴露出来
		const float Weight = (1.0f / DistanceSqr) * TimeWeight * Confidence;

		const float MinWeight = 1e-6f; // 最小权重阈值
		if (Weight < MinWeight)
		{
			UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: Skipping cell 权重错误请检查 (%.8f < %.8f)"), Weight, MinWeight);
			UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: Skipping cell 距离权重 %.8f 时间权重 %.8f  方差权重%.8f"), (1.0f / DistanceSqr), TimeWeight, Confidence);
			continue; // 跳过权重过小的单元格
		}

		FIM_AudioReverbParameters currnetParameter = CalculateCellReverbParameters(QueryLocation, Cell);
		// 累加所有参数
		AccumulatedResponse.Wet +=  currnetParameter.Wet * Weight;
		AccumulatedResponse.Delay += currnetParameter.Delay * Weight;
		AccumulatedResponse.Decay += currnetParameter.Decay * Weight;
		AccumulatedResponse.Density += currnetParameter.Density * Weight;
		AccumulatedResponse.Diffusion += currnetParameter.Diffusion * Weight;
		AccumulatedResponse.Dampening += currnetParameter.Dampening * Weight;
		TotalWeight += Weight;

		// 统计探针信息
		TotalProbes += Cell.RayRes.ProbeCount;
		TotalHits += Cell.RayRes.RayHitCount;
		CellsUsed++;
#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			// 绘制插值使用的cell（使用半透明颜色，线条粗细反映权重）
			const float LineThickness = FMath::Clamp(Weight * 2.0f, 0.5f, 3.0f);
			FColor InterpColor = FColor::Cyan;
			InterpColor.A = static_cast<uint8>(FMath::Clamp(Weight * 255.0f, 30.0f, 200.0f));
			DrawDebugLine(GetWorld(), QueryLocation, Cell.RayRes.AveHitLocation, InterpColor, false, 0.5f, 0, LineThickness);
			//DrawDebugSphere(GetWorld(), Cell.RayRes.AveHitLocation, 8.0f, 6, InterpColor, false, 0.5f, 0, 1.0f);
		}
#endif
	}

	if (TotalWeight > 1e-5f)
	{
		AccumulatedResponse.Wet =  AccumulatedResponse.Wet / TotalWeight;
		AccumulatedResponse.Delay = AccumulatedResponse.Delay / TotalWeight;
		AccumulatedResponse.Decay = AccumulatedResponse.Decay / TotalWeight;
		AccumulatedResponse.Density = AccumulatedResponse.Density / TotalWeight;
		AccumulatedResponse.Diffusion = AccumulatedResponse.Diffusion / TotalWeight;
		AccumulatedResponse.Dampening = AccumulatedResponse.Dampening / TotalWeight;
		OutInterpolatedResponse = AccumulatedResponse;

		// 返回统计信息（可选）
		if (OutCells) *OutCells = CellsUsed;
		if (OutProbes) *OutProbes = TotalProbes;
		if (OutHits) *OutHits = TotalHits;

		return true;
	}else
	{
		// 确保数值正常 数值爆炸了 数值错误再钳制也没用
		UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: TotalWeight <= 1e-5f error"));
	}
	return false;
}

void AIceMoonAcousticField::TickTrimAudioFieldForLod(const float GameTime)
{
	float CleanupAge = 7.5f; // 基础清理时间 (例如: 7.5 * 2 = 15s for LOD 0)

	if (AcousticGridArray.IsEmpty() || CellSubBitMaskArray.IsEmpty()) return;

	// 必须用索引循环，才能在清理时访问对应的Mask数组
	for (int32 LodIndex = 0; LodIndex < AcousticGridArray.Num(); ++LodIndex)
	{
		TMap<FIntVector, FIM_GridAudioCell>& CurrentGrid = AcousticGridArray[LodIndex];
		CleanupAge *= 2.0f;  // LOD 0: 15s, LOD 1: 30s, LOD 2: 60s

		for (auto It = CurrentGrid.CreateIterator(); It; ++It)
		{
			//计时还有效则退出
			if (GameTime - It->Value.LastUpdateTime < CleanupAge) continue;


			// 1. 移除Cell数据
			It.RemoveCurrent();
#if WITH_EDITOR
			// if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)UE_LOG(LogTemp, Log, TEXT("IMAcousticField: 有声场元素移除 LOD %d - REMOVING Cell at Coord %s (Age: %.2fs > Max: %.2fs)."), LodIndex, *(It.Key()).ToString(), GameTime - It->Value.LastUpdateTime, CleanupAge);
#endif
			// 2. CellSubBitMaskArray支持 0 1下标
			if (LodIndex >= CellSubBitMaskArray.Num()) continue;

			const FIntVector RemovedGridCoord = It.Key();
			// 计算此Cell在父层级的坐标
			const FIntVector ParentGridCoord(
				FMath::FloorToInt(static_cast<float>(RemovedGridCoord.X) / LodFactor),
				FMath::FloorToInt(static_cast<float>(RemovedGridCoord.Y) / LodFactor),
				FMath::FloorToInt(static_cast<float>(RemovedGridCoord.Z) / LodFactor)
			);

			// 在父LOD的Mask Map中查找
			if (uint64* Mask = CellSubBitMaskArray[LodIndex].Find(ParentGridCoord))
			{
				// 计算此Cell在父Cell内的局部索引 (0-63)
				const FIntVector LocalCoord = RemovedGridCoord - ParentGridCoord * LodFactor;
				const uint64 LocalIndex = LocalCoord.X + (LocalCoord.Y * LodFactor) + (LocalCoord.Z * LodFactor * LodFactor);

				if (LocalIndex < 64)
				{
					// 使用位与非 (AND NOT) 操作，将对应的bit清零
					*Mask &= ~(1ULL << LocalIndex);
				}

				// (可选优化): 如果这个Mask的所有bit都为0了，可以从Map中移除，节省内存
				if (*Mask == 0u)
				{
					CellSubBitMaskArray[LodIndex].Remove(ParentGridCoord);
				}
			}
		}
	}

	// 清理超过30秒未使用的平滑查询缓存
	for (auto It = SmoothQueryCache.CreateIterator(); It; ++It)
	{
		if (GameTime - It.Value().LastQueryTime > 30.0f)
		{
#if WITH_EDITOR
			if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
			{
				UE_LOG(LogTemp, Log, TEXT("IMAcousticField: 清理平滑查询缓存 ID=%s (Age: %.2fs)"),
					*It.Key().ToString(), GameTime - It.Value().LastQueryTime);
			}
#endif
			It.RemoveCurrent();
		}
	}
}

bool AIceMoonAcousticField::QueryAcousticFieldSmooth(
	UObject* SourceObject,
	FName SoundSlot,
	FVector QueryLocation,
	FIM_AudioReverbParameters& OutResponse,
	float SmoothSpeed)
{
#if WITH_EDITOR
	if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("IMAcousticField: 查询开始 --------------------"));

	}
#endif
	if (!SourceObject)
	{
		UE_LOG(LogTemp, Warning, TEXT("IMAcousticField::QueryAcousticFieldSmooth - SourceObject is null"));
		if (ConfigAsset)
		{
			OutResponse = ConfigAsset.Get()->DefaultReverbParameters;
		}else
		{
			OutResponse = FIM_AudioReverbParameters();
		}
		return false;
	}

	// 生成唯一查询ID: "ObjectUniqueID_SoundSlot"
	// 支持Actor或Component，推荐使用Component（从AnimNotify直接传入SkeletalMeshComponent）
	const FName QueryID = FName(FString::Printf(TEXT("%d_%s"),
		SourceObject->GetUniqueID(),
		*SoundSlot.ToString()));

	// 查询当前原始数据
	FIM_AudioReverbParameters RawResponse;
	const bool bFoundData = QueryAcousticField(QueryLocation, RawResponse);

	// 确定目标混响参数（找到数据用查询结果，否则用配置的默认值）
	const FIM_AudioReverbParameters TargetResponse = bFoundData ? RawResponse : GetDefaultReverbParameters();

	const float CurrentTime = GetWorld()->GetTimeSeconds();

	// 查找缓存
	FAcousticQueryCache* Cache = SmoothQueryCache.Find(QueryID);

	if (!Cache)
	{
		// 首次查询，直接使用目标值并缓存
		FAcousticQueryCache NewCache;
		NewCache.LastResult = TargetResponse;
		NewCache.LastQueryTime = CurrentTime;
		NewCache.LastQueryLocation = QueryLocation;
		SmoothQueryCache.Add(QueryID, NewCache);

		OutResponse = TargetResponse;
		return true;
	}

	// 计算时间增量（限制最大1.0秒，防止暂停后的跳变）
	const float DeltaTime = FMath::Clamp(CurrentTime - Cache->LastQueryTime, 0.0f, 1.0f);

	// 问题：固定SmoothSpeed对大幅度变化响应太慢（如从封闭区域→开阔空间） 解决：根据Wet变化幅度自适应调整平滑速度
	const float WetChange = FMath::Abs(TargetResponse.Wet - Cache->LastResult.Wet);

	// 自适应因子：小变化→1.0（慢速平滑），大变化→0.25（快速响应，4倍加速）
	const float AdaptiveFactor = IMMathUtils::Remap_Sat<float>(0.075f, 0.2f, 1.0f, 0.25f, WetChange);
	const float EffectiveSmoothSpeed = SmoothSpeed * AdaptiveFactor;

	// 指数平滑：Alpha = 1 - e^(-Δt / τ)
	// SmoothSpeed (τ) 表示"时间常数"，即达到63.2%收敛所需的时间
	// - 对于离散采样（脚步声等）：推荐 3.0-5.0 秒（需要多次查询才完全过渡）
	// - 对于连续采样（环境音等）：推荐 0.5-1.0 秒（快速响应）
	// - 自适应调整：大变化时自动加快响应速度
	const float Alpha = 1.0f - FMath::Exp(-DeltaTime / FMath::Max(EffectiveSmoothSpeed, 0.01f));

	// 平滑插值
	OutResponse.Wet = FMath::Lerp(Cache->LastResult.Wet, TargetResponse.Wet, Alpha);
	OutResponse.Delay = FMath::Lerp(Cache->LastResult.Delay, TargetResponse.Delay, Alpha);
	OutResponse.Decay = FMath::Lerp(Cache->LastResult.Decay, TargetResponse.Decay, Alpha);
	OutResponse.Density = FMath::Lerp(Cache->LastResult.Density, TargetResponse.Density, Alpha);
	OutResponse.Diffusion = FMath::Lerp(Cache->LastResult.Diffusion, TargetResponse.Diffusion, Alpha);
	OutResponse.Dampening = FMath::Lerp(Cache->LastResult.Dampening, TargetResponse.Dampening, Alpha);

	// 更新缓存
	Cache->LastResult = OutResponse;
	Cache->LastQueryTime = CurrentTime;
	Cache->LastQueryLocation = QueryLocation;

#if WITH_EDITOR
	if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("IMAcousticField: 查询FName Key:%s, QueryLocation:%s"), *QueryID.ToString(), *QueryLocation.ToString());

		// 输出自适应平滑信息（当发生显著变化时）
		if (WetChange > 0.03f)
		{
			UE_LOG(LogTemp, Log, TEXT("IMAcousticField: [平滑] Target=%.2f, Current=%.2f, Δ=%.3f → Factor=%.3f, Speed=%.2fs, Alpha=%.3f"),
				TargetResponse.Wet, Cache->LastResult.Wet, WetChange, AdaptiveFactor, EffectiveSmoothSpeed, Alpha);
		}

		// 检测显著变化（激进加速中）
		if (WetChange > 0.15f)
		{
			UE_LOG(LogTemp, Warning, TEXT("IMAcousticField: [显著变化] Wet: %.2f → %.2f (Δ=%.3f) - 激进加速中 (×%.1f)"),
				Cache->LastResult.Wet, TargetResponse.Wet, WetChange, 1.0f/AdaptiveFactor);
		}
	}
#endif

	return true; // 总是返回true，因为总有平滑后的有效值
}

// ========================================
// 配置获取辅助函数（优先Override → ConfigAsset → 默认值）
// ========================================

FIM_AudioReverbParameters AIceMoonAcousticField::GetDefaultReverbParameters() const
{
	if (bOverrideDefaultReverb)
	{
		return DefaultReverbParameters_Override;
	}
	if (ConfigAsset)
	{
		return ConfigAsset->DefaultReverbParameters;
	}
	// 最终回退到硬编码默认值
	FIM_AudioReverbParameters Fallback;
	Fallback.Wet = 0.0f;
	Fallback.Delay = 0.02f;
	Fallback.Decay = 0.3f;
	Fallback.Density = 0.2f;
	Fallback.Diffusion = 0.4f;
	Fallback.Dampening = 0.6f;
	return Fallback;
}

FIM_WetCalculationParameters AIceMoonAcousticField::GetWetCalculationParameters() const
{
	if (bOverrideWetCalculation)
	{
		return WetCalculationParameters_Override;
	}
	if (ConfigAsset)
	{
		return ConfigAsset->WetCalculationParameters;
	}
	// 最终回退到默认值
	return FIM_WetCalculationParameters();
}

void AIceMoonAcousticField::ClearSmoothQueryCache(UObject* SourceObject, FName SoundSlot)
{
	if (!SourceObject)
	{
		UE_LOG(LogTemp, Warning, TEXT("IMAcousticField::ClearSmoothQueryCache - SourceObject is null"));
		return;
	}

	if (SoundSlot == NAME_None)
	{
		// 清理该对象的所有缓存（遍历查找所有匹配的ID）
		const FString ObjectIDPrefix = FString::Printf(TEXT("%d_"), SourceObject->GetUniqueID());
		int32 RemovedCount = 0;

		for (auto It = SmoothQueryCache.CreateIterator(); It; ++It)
		{
			if (It.Key().ToString().StartsWith(ObjectIDPrefix))
			{
				It.RemoveCurrent();
				RemovedCount++;
			}
		}

#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("IMAcousticField: ClearSmoothQueryCache - 清理对象所有缓存: %s, 移除数量=%d"),
				*SourceObject->GetName(), RemovedCount);
		}
#endif
	}
	else
	{
		// 清理特定槽位的缓存
		const FName QueryID = FName(FString::Printf(TEXT("%d_%s"),
			SourceObject->GetUniqueID(),
			*SoundSlot.ToString()));

		const int32 RemovedCount = SmoothQueryCache.Remove(QueryID);

#if WITH_EDITOR
		if(CVar_DebugLevelStat.GetValueOnGameThread() > 0)
		{
			UE_LOG(LogTemp, Log, TEXT("IMAcousticField: ClearSmoothQueryCache - ID=%s, 移除=%d"),
				*QueryID.ToString(), RemovedCount);
		}
#endif
	}
}