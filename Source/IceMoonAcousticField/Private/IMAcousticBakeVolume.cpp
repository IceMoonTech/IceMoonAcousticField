#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeIdentity.h"
#include "IMAcousticGrassCapture.h"
#include "IMAcousticLandscapeCells.h"
#include "IMAcousticBakeAsset.h"
#include "IMAcousticSourceComponent.h"
#include "IMAcousticMetaSound.h"
#include "IMAcousticPresenceParameters.h"
#include "MetasoundSource.h"
#include "Sound/AudioBus.h"
#include "UObject/StrongObjectPtr.h"
#include "IMAcousticSimulationWorker.h"
#include "IMAcousticReverbPreset.h"
#include "IMAcousticBakeRecipe.h"
#include "IMAcousticCoordinates.h"
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "IMAcousticOperationMemorySampler.h"
#include "Misc/AutomationTest.h"
#endif
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/Guid.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "AudioDevice.h"
#include "AudioDeviceManager.h"
#include "AudioMixerDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Sound/SoundSubmix.h"
#include "Components/AudioComponent.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Brush.h"
#include "GameFramework/PhysicsVolume.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include <algorithm>
#if WITH_EDITOR
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#endif

namespace IMAcousticBakeVolumePrivate
{
std::atomic<uint64> NextWorldEpoch{1};

// Q5 upstream timing is an explicit process flag. The default path performs no
// timing log and keeps the same worker, capacity, lease, and audio sequence.
bool IMBakeVolumeUpstreamTimingEnabled()
{
	static const bool bEnabled = FParse::Param(FCommandLine::Get(), TEXT("IMAcousticUpstreamTiming"));
	return bEnabled;
}

uint64 IMBakeVolumeUpstreamPhysicalBytes()
{
	return IMBakeVolumeUpstreamTimingEnabled() ? FPlatformMemory::GetStats().UsedPhysical : 0;
}

void IMLogBakeVolumeUpstreamTiming(const TCHAR* Event, double StartedAt, uint64 Bytes,
	uint32 DeviceId, uint64 Epoch, uint64 Detail = 0, uint64 Auxiliary = 0,
	uint64 PhysicalBefore = 0)
{
	if (!IMBakeVolumeUpstreamTimingEnabled()) return;
	const double Now = FPlatformTime::Seconds();
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	const uint64 PhysicalDelta = PhysicalBefore > 0 && Memory.UsedPhysical >= PhysicalBefore
		? Memory.UsedPhysical - PhysicalBefore : 0;
	UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticUpstreamTiming event=%s wall_s=%.9f elapsed_ms=%.3f bytes=%llu physical_delta_bytes=%llu device=%u epoch=%llu gt_frame=%llu detail=%llu auxiliary=%llu"),
		Event, Now, StartedAt > 0.0 ? (Now - StartedAt) * 1000.0 : 0.0,
		static_cast<unsigned long long>(Bytes), static_cast<unsigned long long>(PhysicalDelta),
		DeviceId, static_cast<unsigned long long>(Epoch),
		static_cast<unsigned long long>(GFrameCounter),
		static_cast<unsigned long long>(Detail), static_cast<unsigned long long>(Auxiliary));
}

// Per-component diagnostics cost measurable time on large scenes, so opt in when
// investigating geometry. This switch changes logging only; every instance is
// still exported and hashed for bake identity and Editor invalidation.
TAutoConsoleVariable<int32> CVarIMAcousticDetailedGeometryLog(
	TEXT("IM.AcousticField.DetailedGeometryLog"), 0,
	TEXT("Emit per-component acoustic geometry diagnostics during scene capture."),
	ECVF_Default);
// Exaggeration strength S of the exaggerated presence version (mapping and the accepted S = 1 values: IMAcousticPresenceParameters.h).
// Read each publish on the game thread, clamped to the console/Details range; physical sources are never affected.
TAutoConsoleVariable<float> CVarIMAcousticPresenceStrength(
	TEXT("IM.AcousticField.PresenceStrength"), 1.f,
	TEXT("Exaggeration strength of the exaggerated presence version: 0 = physical path, 1 = accepted exaggerated values, up to 2."),
	ECVF_Default);
struct FIMBakeJob
{
	std::atomic<bool> Cancelled{false};
	std::atomic<bool> Done{false};
	bool Success=false;
	FIMAcousticBakeData Data;
	std::string Error;
	FString Fingerprint;
	FString WorldPackage;
	FString MetadataJson;
	TFuture<void> Future;
};

void IMHashMaterialIdentity(FSHA1& Hash, const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	const int32 Length=Utf8.Length();
	Hash.Update(reinterpret_cast<const uint8*>(&Length),sizeof(Length));
	if(Length)Hash.Update(reinterpret_cast<const uint8*>(Utf8.Get()),Length);
}

// This cache belongs to one synchronous GT CaptureScene call. It never survives
// an Editor tick, so property notifications are not used to decide freshness.
// Cache eviction changes read cost only: the current component still gets every
// vertex/index. The 16 MiB limit covers retained vector payload capacities, not
// the whole capture or the one oversized component's required working buffer.
constexpr uint64 IMMeshLocalCachePayloadBudgetBytes=16ull*1024ull*1024ull;

struct FIMMeshLocalReadGeometry
{
	std::vector<FVector3f> SourcePositions;
	std::vector<FVector> LocalPositions;
	std::vector<uint32> Indices;
	bool AllLocalPositionsFinite=true;

	uint64 PayloadCapacityBytes() const
	{
		return uint64(SourcePositions.capacity())*sizeof(FVector3f)
			+uint64(LocalPositions.capacity())*sizeof(FVector)
			+uint64(Indices.capacity())*sizeof(uint32);
	}
};

struct FIMMeshLocalReadKey
{
	const FPositionVertexBuffer* Positions=nullptr;
	const FRawStaticIndexBuffer* Indices=nullptr;
	bool operator==(const FIMMeshLocalReadKey& Other) const
	{return Positions==Other.Positions&&Indices==Other.Indices;}
};
uint32 GetTypeHash(const FIMMeshLocalReadKey& Key)
{return HashCombine(PointerHash(Key.Positions),PointerHash(Key.Indices));}

void IMReadMeshLocalGeometry(const FPositionVertexBuffer& InPositions,
	const FRawStaticIndexBuffer& InIndices,FIMMeshLocalReadGeometry& Out)
{
	check(IsInGameThread());
	Out.SourcePositions.resize(InPositions.GetNumVertices());
	Out.LocalPositions.resize(InPositions.GetNumVertices());
	Out.AllLocalPositionsFinite=true;
	for(uint32 Vertex=0;Vertex<InPositions.GetNumVertices();++Vertex)
	{
		Out.SourcePositions[Vertex]=InPositions.VertexPosition(Vertex);
		Out.LocalPositions[Vertex]=FVector(Out.SourcePositions[Vertex]);
		Out.AllLocalPositionsFinite&=!Out.LocalPositions[Vertex].ContainsNaN();
	}
	Out.Indices.resize(InIndices.GetArrayView().Num());
	for(size_t Index=0;Index<Out.Indices.size();++Index)
		Out.Indices[Index]=InIndices.GetIndex(uint32(Index));
}

class FIMMeshLocalReadCache
{
public:
	explicit FIMMeshLocalReadCache(uint64 InPayloadBudgetBytes=IMMeshLocalCachePayloadBudgetBytes)
		:PayloadBudgetBytes(InPayloadBudgetBytes){}

	const FIMMeshLocalReadGeometry& Read(const FPositionVertexBuffer& InPositions,
		const FRawStaticIndexBuffer& InIndices,TUniquePtr<FIMMeshLocalReadGeometry>& OutUncached)
	{
		check(IsInGameThread());
		OutUncached.Reset();
		const FIMMeshLocalReadKey Key{&InPositions,&InIndices};
		if(const auto* Found=Entries.Find(Key))
		{
			++Hits;
			return **Found;
		}
		++Misses;
		auto Geometry=MakeUnique<FIMMeshLocalReadGeometry>();
		IMReadMeshLocalGeometry(InPositions,InIndices,*Geometry);
		const uint64 Bytes=Geometry->PayloadCapacityBytes();
		if(Bytes>PayloadBudgetBytes)
		{
			++OversizedReads;
			OutUncached=MoveTemp(Geometry);
			return *OutUncached;
		}
		if(RetainedPayloadBytes>PayloadBudgetBytes-Bytes)
		{
			Entries.Reset();RetainedPayloadBytes=0;++Evictions;
		}
		const auto* Result=Geometry.Get();
		Entries.Add(Key,MoveTemp(Geometry));
		RetainedPayloadBytes+=Bytes;
		PeakRetainedPayloadBytes=FMath::Max(PeakRetainedPayloadBytes,RetainedPayloadBytes);
		return *Result;
	}

	uint64 Hits=0,Misses=0,Evictions=0,OversizedReads=0;
	uint64 RetainedPayloadBytes=0,PeakRetainedPayloadBytes=0;
private:
	uint64 PayloadBudgetBytes=0;
	TMap<FIMMeshLocalReadKey,TUniquePtr<FIMMeshLocalReadGeometry>> Entries;
};

void IMHashMeshLocalBytes(FSHA1& Hash,const void* Data,uint64 Bytes)
{
	const auto* Cursor=static_cast<const uint8*>(Data);
	// FSHA1 takes uint32 lengths. Chunking preserves the old concatenated byte
	// stream, including for a buffer larger than one API length can represent.
	while(Bytes)
	{
		const uint32 Count=uint32(FMath::Min<uint64>(Bytes,MAX_uint32));
		Hash.Update(Cursor,Count);Cursor+=Count;Bytes-=Count;
	}
}
void IMHashMeshLocalSource(FSHA1& Hash,const FIMMeshLocalReadGeometry& Geometry)
{
	// Deliberately no new counts, digest composition or padding: the legacy
	// identity stream was each FVector3f followed by each normalized uint32.
	IMHashMeshLocalBytes(Hash,Geometry.SourcePositions.data(),
		uint64(Geometry.SourcePositions.size())*sizeof(FVector3f));
	IMHashMeshLocalBytes(Hash,Geometry.Indices.data(),
		uint64(Geometry.Indices.size())*sizeof(uint32));
}
void IMHashMeshLocalPositions(FSHA1& Hash,const FIMMeshLocalReadGeometry& Geometry)
{
	IMHashMeshLocalBytes(Hash,Geometry.LocalPositions.data(),
		uint64(Geometry.LocalPositions.size())*sizeof(FVector));
}

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticCaptureLocalHashParity,
	"IceMoon.AcousticField.G2.CaptureLocalHashParity",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticCaptureLocalHashParity::RunTest(const FString&)
{
	const TArray<FVector3f> InputPositions={
		FVector3f(-0.0f,0.0f,0.125f),FVector3f(1.5f,-3.25f,0.0f),FVector3f(1.0e-20f,8.0f,-0.5f)};
	const TArray<uint32> InputIndices={0,2,1};
	FPositionVertexBuffer Positions;Positions.Init(InputPositions,true);
	FRawStaticIndexBuffer Indices16(true),Indices32(true);
	Indices16.SetIndices(InputIndices,EIndexBufferStride::Force16Bit);
	Indices32.SetIndices(InputIndices,EIndexBufferStride::Force32Bit);
	auto FinishHash=[](FSHA1& Hash)
	{Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20);};
	auto ScalarIdentity=[&](const FRawStaticIndexBuffer& Indices)
	{
		FSHA1 Hash;
		for(uint32 Vertex=0;Vertex<Positions.GetNumVertices();++Vertex)
		{const FVector3f Value=Positions.VertexPosition(Vertex);Hash.Update(reinterpret_cast<const uint8*>(&Value),sizeof(Value));}
		for(int32 Index=0;Index<Indices.GetArrayView().Num();++Index)
		{const uint32 Value=Indices.GetIndex(Index);Hash.Update(reinterpret_cast<const uint8*>(&Value),sizeof(Value));}
		for(int32 Instance=0;Instance<2;++Instance)
		{
			Hash.Update(reinterpret_cast<const uint8*>(&Instance),sizeof(Instance));
			for(uint32 Vertex=0;Vertex<Positions.GetNumVertices();++Vertex)
			{const FVector Value(Positions.VertexPosition(Vertex));Hash.Update(reinterpret_cast<const uint8*>(&Value),sizeof(Value));}
		}
		return FinishHash(Hash);
	};
	auto BulkIdentity=[&](const FIMMeshLocalReadGeometry& Geometry)
	{
		FSHA1 Hash;IMHashMeshLocalSource(Hash,Geometry);
		for(int32 Instance=0;Instance<2;++Instance)
		{Hash.Update(reinterpret_cast<const uint8*>(&Instance),sizeof(Instance));IMHashMeshLocalPositions(Hash,Geometry);}
		return FinishHash(Hash);
	};
	FIMMeshLocalReadCache Cache;
	TUniquePtr<FIMMeshLocalReadGeometry> Working;
	const auto& First=Cache.Read(Positions,Indices16,Working);
	const FString Original=BulkIdentity(First);
	const uint64 OnePayload=First.PayloadCapacityBytes();
	TestEqual(TEXT("16-bit normalization equals legacy scalar stream"),Original,ScalarIdentity(Indices16));
	TestEqual(TEXT("same buffer reuse preserves identity"),BulkIdentity(Cache.Read(Positions,Indices16,Working)),Original);
	TestEqual(TEXT("one reused read"),Cache.Hits,uint64(1));
	TestEqual(TEXT("32-bit normalization equals legacy scalar stream"),BulkIdentity(Cache.Read(Positions,Indices32,Working)),ScalarIdentity(Indices32));
	TestEqual(TEXT("16/32 storage widths normalize identically"),ScalarIdentity(Indices32),Original);

	// Direct CPU mutation intentionally fires no UObject/property notification.
	// A new capture cache must read the new bytes; no persistent dirty token exists.
	const FVector3f SavedPosition=Positions.VertexPosition(1);
	Positions.VertexPosition(1).X+=0.125f;
	FIMMeshLocalReadCache NextCapture;
	const FString Edited=BulkIdentity(NextCapture.Read(Positions,Indices16,Working));
	TestNotEqual(TEXT("next capture sees notification-free vertex edit"),Edited,Original);
	TestEqual(TEXT("edited bytes still match scalar oracle"),Edited,ScalarIdentity(Indices16));
	Positions.VertexPosition(1)=SavedPosition;
	FIMMeshLocalReadCache RestoredCapture;
	TestEqual(TEXT("exact vertex restore returns original identity"),BulkIdentity(RestoredCapture.Read(Positions,Indices16,Working)),Original);

	TArray<uint32> ChangedIndices=InputIndices;Swap(ChangedIndices[1],ChangedIndices[2]);
	Indices32.SetIndices(ChangedIndices,EIndexBufferStride::Force32Bit);
	FIMMeshLocalReadCache IndexEditCapture;
	TestNotEqual(TEXT("next capture sees direct index edit"),BulkIdentity(IndexEditCapture.Read(Positions,Indices32,Working)),Original);
	TestEqual(TEXT("index edit normalized bytes match scalar oracle"),BulkIdentity(IndexEditCapture.Read(Positions,Indices32,Working)),ScalarIdentity(Indices32));
	Indices32.SetIndices(InputIndices,EIndexBufferStride::Force32Bit);

	FIMMeshLocalReadCache TinyCache(OnePayload-1);
	TestEqual(TEXT("oversized uncached read retains complete identity"),BulkIdentity(TinyCache.Read(Positions,Indices16,Working)),Original);
	TestEqual(TEXT("oversized payload is not retained"),TinyCache.RetainedPayloadBytes,uint64(0));
	TestEqual(TEXT("oversized path observed"),TinyCache.OversizedReads,uint64(1));
	FPositionVertexBuffer OtherPositions;OtherPositions.Init(InputPositions,true);
	FIMMeshLocalReadCache EvictingCache(OnePayload);
	TestEqual(TEXT("first bounded entry complete"),BulkIdentity(EvictingCache.Read(Positions,Indices16,Working)),Original);
	TestEqual(TEXT("eviction replacement complete"),BulkIdentity(EvictingCache.Read(OtherPositions,Indices16,Working)),Original);
	TestEqual(TEXT("budget eviction observed"),EvictingCache.Evictions,uint64(1));
	TestTrue(TEXT("retained payload stays within budget"),EvictingCache.PeakRetainedPayloadBytes<=OnePayload);
	return true;
}
#endif

// GT-only resolver. The SDK receives copied coefficients and geometry, never
// UObjects. A physical profile controls acoustic participation explicitly;
// render blend mode and asset names are not runtime classification rules.
struct FIMAcousticMaterialTable
{
	bool Physical=false;
	TMap<UMaterialInterface*,int32> RenderIndices;
	TMap<UPhysicalMaterial*,int32> PhysicalIndices;
	TArray<IPLMaterial> Native;
	TArray<bool> Solid;
	TArray<bool> SupportsProbes;
	TArray<FString> Identities;

	bool Build(const AIMAcousticBakeVolume& Volume,FString& Failure)
	{
		check(IsInGameThread());
		if(Volume.MaterialDrive!=EIMAcousticMaterialDrive::PhysicalMaterial
			&&Volume.MaterialDrive!=EIMAcousticMaterialDrive::LegacyRenderMaterial)
		{Failure=TEXT("Unknown acoustic material drive mode.");return false;}
		Physical=Volume.MaterialDrive==EIMAcousticMaterialDrive::PhysicalMaterial;
		auto Add=[&](const FVector& Absorption,float Scattering,bool bSolid,bool bSupport,const FString& Identity)
		{
			IPLMaterial Material{};
			for(int32 Band=0;Band<3;++Band)
			{
				const double Value=Absorption[Band];
				if(!FMath::IsFinite(Value)||Value<0||Value>1)
				{
					Failure=FString::Printf(TEXT("Acoustic profile %s: absorption band %d must be finite and within [0,1]."),*Identity,Band);
					return false;
				}
				Material.absorption[Band]=float(Value);
				// Preserve the approved wall/direct/path contract. Transmission
				// cannot make native baked visibility porous, so NonSolid leaf
				// cards are classified before scene submission instead.
				Material.transmission[Band]=0.0f;
			}
			if(!FMath::IsFinite(Scattering)||Scattering<0||Scattering>1)
			{
				Failure=FString::Printf(TEXT("Acoustic profile %s: scattering must be finite and within [0,1]."),*Identity);
				return false;
			}
			if(!bSolid&&bSupport)
			{
				Failure=FString::Printf(TEXT("Acoustic profile %s: non-solid geometry cannot support probes; disable probe support."),*Identity);
				return false;
			}
			Material.scattering=Scattering;
			Native.Add(Material);Solid.Add(bSolid);SupportsProbes.Add(bSupport);Identities.Add(Identity);
			return true;
		};
		if(Physical)
		{
			for(const auto& Mapping:Volume.PhysicalMaterials)
			{
				UPhysicalMaterial* Asset=Mapping.PhysicalMaterial.Get();
				if(!Asset){Failure=FString::Printf(TEXT("PhysicalMaterials[%d] has no PhysicalMaterial asset."),Native.Num());return false;}
				if(PhysicalIndices.Contains(Asset)){Failure=TEXT("Duplicate acoustic physical material: ")+Asset->GetPathName();return false;}
				if(!Add(Mapping.Absorption,Mapping.Scattering,Mapping.bSolidGeometry,Mapping.bSupportsProbes,IMStableBakeObjectPath(Asset)))return false;
				PhysicalIndices.Add(Asset,Native.Num()-1);
			}
		}
		else
		{
			for(const auto& Mapping:Volume.Materials)
			{
				UMaterialInterface* Asset=Mapping.Material.Get();
				if(!Asset){Failure=FString::Printf(TEXT("Legacy Materials[%d] has no render material asset."),Native.Num());return false;}
				if(RenderIndices.Contains(Asset)){Failure=TEXT("Duplicate legacy acoustic render material: ")+Asset->GetPathName();return false;}
				if(!Add(Mapping.Absorption,Mapping.Scattering,true,true,IMStableBakeObjectPath(Asset)))return false;
				RenderIndices.Add(Asset,Native.Num()-1);
			}
		}
		if(Native.IsEmpty()){Failure=TEXT("Acoustic material mapping is empty for the selected drive mode.");return false;}
		return true;
	}

	int32 ResolveMesh(UStaticMeshComponent* Component,int32 Slot,int32 Instance) const
	{
		check(IsInGameThread());
		UMaterialInterface* Render=Component->GetMaterial(Slot);
		if(!Physical)
		{const int32* Index=RenderIndices.Find(Render);return Index?*Index:INDEX_NONE;}
		// ISM/HISM share section slots; a live instance body may carry an
		// override when no explicit component override was authored. The
		// component override has priority by the PM-drive contract; querying a
		// simple-body default must never erase the actual section-slot PM.
		UPhysicalMaterial* Asset=Component->BodyInstance.GetPhysMaterialOverride();
		if(!Asset)
			if(FBodyInstance* Body=Component->GetBodyInstance(NAME_None,false,Instance))
				Asset=Body->GetPhysMaterialOverride();
		if(!Asset&&Render)Asset=Render->GetPhysicalMaterial();
		const int32* Index=PhysicalIndices.Find(Asset);
		return Index?*Index:INDEX_NONE;
	}
};

// Capture-local GT diagnostics. The selected profile index is the resolver's
// actual result, including instance/component overrides; transient grass object
// names never determine the producer, material role, or acoustic classification.
struct FIMGeometrySectionContribution
{
	const UPrimitiveComponent* Component=nullptr;
	FString ComponentPath;
	FString ProducerIdentity;
	FString MeshAssetPath;
	FString RenderMaterialPath;
	int32 SectionIndex=INDEX_NONE;
	int32 MaterialSlot=INDEX_NONE;
	int32 ProfileIndex=INDEX_NONE;
	uint64 IncludedInstances=0;
	uint64 SolidTriangles=0;
	uint64 NonSolidTriangles=0;
};

void IMLogSectionContributions(const TArray<FIMGeometrySectionContribution>& Rows,
	const FIMAcousticMaterialTable& Materials,const FString& CaptureId,
	const FString& WorldPackage,const FString& Fingerprint,
	int32 SceneSolidTriangles,int32 SceneNonSolidTriangles)
{
	check(IsInGameThread());
	uint64 MeshSolidTriangles=0,MeshNonSolidTriangles=0;
	for(const auto& Row:Rows)
	{
		const int32 Profile=Row.ProfileIndex;
		const auto& Native=Materials.Native[Profile];
		MeshSolidTriangles+=Row.SolidTriangles;
		MeshNonSolidTriangles+=Row.NonSolidTriangles;
		FString Json;
		auto W=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		W->WriteObjectStart();
		W->WriteValue(TEXT("schema"),1);
		W->WriteValue(TEXT("capture_id"),CaptureId);
		W->WriteValue(TEXT("world"),WorldPackage);
		W->WriteValue(TEXT("fingerprint"),Fingerprint);
		W->WriteValue(TEXT("component_path"),Row.ComponentPath);
		W->WriteValue(TEXT("producer_identity"),Row.ProducerIdentity);
		W->WriteValue(TEXT("mesh_asset_path"),Row.MeshAssetPath);
		W->WriteValue(TEXT("section_index"),Row.SectionIndex);
		W->WriteValue(TEXT("material_slot"),Row.MaterialSlot);
		W->WriteValue(TEXT("render_material_path"),Row.RenderMaterialPath);
		W->WriteValue(TEXT("profile_index"),Profile);
		W->WriteValue(TEXT("profile_identity"),Materials.Identities[Profile]);
		W->WriteValue(TEXT("physical_material_drive"),Materials.Physical);
		W->WriteValue(TEXT("solid_geometry"),Materials.Solid[Profile]);
		W->WriteValue(TEXT("supports_probes"),Materials.SupportsProbes[Profile]);
		W->WriteArrayStart(TEXT("absorption"));
		for(int32 Band=0;Band<3;++Band)W->WriteValue(Native.absorption[Band]);
		W->WriteArrayEnd();
		W->WriteValue(TEXT("scattering"),Native.scattering);
		W->WriteValue(TEXT("included_instance_contributions"),Row.IncludedInstances);
		W->WriteValue(TEXT("solid_triangles_emitted"),Row.SolidTriangles);
		W->WriteValue(TEXT("non_solid_triangles_excluded"),Row.NonSolidTriangles);
		W->WriteObjectEnd();W->Close();
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticSectionContribution %s"),*Json);
	}
	FString Json;
	auto W=TJsonWriterFactory<TCHAR,TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	W->WriteObjectStart();
	W->WriteValue(TEXT("schema"),1);
	W->WriteValue(TEXT("capture_id"),CaptureId);
	W->WriteValue(TEXT("world"),WorldPackage);
	W->WriteValue(TEXT("fingerprint"),Fingerprint);
	W->WriteValue(TEXT("section_rows"),Rows.Num());
	W->WriteValue(TEXT("mesh_solid_triangles"),MeshSolidTriangles);
	W->WriteValue(TEXT("mesh_non_solid_triangles"),MeshNonSolidTriangles);
	W->WriteValue(TEXT("scene_solid_triangles"),SceneSolidTriangles);
	W->WriteValue(TEXT("scene_non_solid_triangles"),SceneNonSolidTriangles);
	W->WriteValue(TEXT("scope"),TEXT("mesh sections; landscape is included only in complete scene totals"));
	W->WriteObjectEnd();W->Close();
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticSectionContributionSummary %s"),*Json);
}

void IMCompactAcousticVertices(FIMAcousticSceneInput& Geometry)
{
	std::vector<int32> Remap(Geometry.Vertices.size(),INDEX_NONE);
	std::vector<IPLVector3> Used;
	Used.reserve(std::min(Geometry.Vertices.size(),Geometry.Triangles.size()*size_t(3)));
	for(auto& Triangle:Geometry.Triangles)
		for(int32 Corner=0;Corner<3;++Corner)
		{
			const int32 Old=Triangle.indices[Corner];
			int32& New=Remap[Old];
			if(New==INDEX_NONE){New=int32(Used.size());Used.push_back(Geometry.Vertices[Old]);}
			Triangle.indices[Corner]=New;
		}
	Geometry.Vertices=std::move(Used);
}

FString BakeMetadata(const AIMAcousticBakeVolume& Volume,const FIMAcousticSceneInput& Geometry,const FVector& Origin)
{
	FString Json;auto W=TJsonWriterFactory<>::Create(&Json);
	W->WriteObjectStart();W->WriteValue(TEXT("recipe_version"),IMAcousticRecipe::Version);
	W->WriteValue(TEXT("sdk_version"),UIMAcousticBakeAsset::SDKVersion);W->WriteValue(TEXT("triangles"),int32(Geometry.Triangles.size()));
	W->WriteValue(TEXT("probe_count"),int32(Geometry.Probes.size()));
	W->WriteValue(TEXT("probe_spacing_cm"),Volume.ProbeSpacingCm);W->WriteValue(TEXT("probe_height_cm"),Volume.ProbeHeightCm);
	auto Vector=[&W](const TCHAR* Name,const FVector& V){W->WriteArrayStart(Name);for(int I=0;I<3;++I)W->WriteValue(V[I]);W->WriteArrayEnd();};
	Vector(TEXT("bounds_origin_cm"),Origin);Vector(TEXT("bounds_extent_cm"),Volume.BakeBounds->GetUnscaledBoxExtent());
	W->WriteValue(TEXT("material_drive"),Volume.MaterialDrive==EIMAcousticMaterialDrive::PhysicalMaterial?TEXT("physical_material"):TEXT("legacy_render_material"));
	W->WriteValue(TEXT("non_solid_triangles"),Volume.NonSolidTriangles);
	W->WriteValue(TEXT("non_solid_sections"),Volume.NonSolidSections);
	W->WriteArrayStart(TEXT("materials"));
	if(Volume.MaterialDrive==EIMAcousticMaterialDrive::PhysicalMaterial)
	for(const auto& M:Volume.PhysicalMaterials)
	{
		W->WriteObjectStart();W->WriteValue(TEXT("asset_path"),M.PhysicalMaterial->GetPathName());
		Vector(TEXT("absorption"),M.Absorption);W->WriteValue(TEXT("scattering"),M.Scattering);
		W->WriteValue(TEXT("solid_geometry"),M.bSolidGeometry);W->WriteValue(TEXT("supports_probes"),M.bSupportsProbes);W->WriteObjectEnd();
	}
	else for(const auto& M:Volume.Materials)
	{
		W->WriteObjectStart();W->WriteValue(TEXT("asset_path"),M.Material->GetPathName());
		Vector(TEXT("absorption"),M.Absorption);W->WriteValue(TEXT("scattering"),M.Scattering);W->WriteObjectEnd();
	}
	W->WriteArrayEnd();W->WriteArrayStart(TEXT("probes_m"));
	for(const auto& P:Geometry.Probes)
	{W->WriteArrayStart();W->WriteValue(P.center.x);W->WriteValue(P.center.y);W->WriteValue(P.center.z);W->WriteValue(P.radius);W->WriteArrayEnd();}
	W->WriteArrayEnd();W->WriteObjectStart(TEXT("reflection"));
	W->WriteValue(TEXT("num_rays"),IMAcousticRecipe::ReverbNumRays);W->WriteValue(TEXT("num_bounces"),IMAcousticRecipe::ReverbNumBounces);
	W->WriteValue(TEXT("num_diffuse_samples"),IMAcousticRecipe::ReverbNumDiffuse);
	W->WriteValue(TEXT("irradiance_min_m"),IMAcousticRecipe::IrradianceMinM);
	W->WriteValue(TEXT("sim_duration_s"),IMAcousticRecipe::ReverbSimDurationS);W->WriteValue(TEXT("saved_duration_s"),IMAcousticRecipe::ReverbSavedDurationS);
	W->WriteValue(TEXT("order"),FIMAcousticAudioFrame::Order);W->WriteValue(TEXT("type"),TEXT("CONVOLUTION"));W->WriteObjectEnd();
	W->WriteObjectStart(TEXT("path"));W->WriteValue(TEXT("num_samples"),IMAcousticRecipe::PathNumSamples);
	W->WriteValue(TEXT("radius_m"),IMAcousticRecipe::PathRadiusM);W->WriteValue(TEXT("threshold"),IMAcousticRecipe::PathThreshold);
	W->WriteValue(TEXT("vis_range_m"),IMAcousticRecipe::PathVisRangeM);W->WriteValue(TEXT("path_range_m"),IMAcousticRecipe::PathRangeM);
	W->WriteObjectEnd();W->WriteObjectEnd();W->Close();return Json;
}

// A bake-bound listener is not automatically an acoustically valid listener:
// the editor/PIE camera can be placed inside a closed wall slab while still
// remaining inside BakeBounds. In that state the SDK's nearest-probe lookup can
// select a probe on either side of the slab and produce a large, discontinuous
// reverb change. Test the exported acoustic triangles themselves so this guard
// follows the same geometry contract as the bake.
bool PointInsideAcousticTriangleRange(const FIMAcousticSceneInput& Geometry, int32 StartTriangle, int32 EndTriangle, const IPLVector3& Point)
{
	const int32 TriangleCount = static_cast<int32>(Geometry.Triangles.size());
	const int32 Start = FMath::Clamp(StartTriangle, 0, TriangleCount);
	const int32 End = FMath::Clamp(EndTriangle, Start, TriangleCount);
	if (End <= Start) return false;

	const FVector P(Point.x, Point.y, Point.z);
	const FVector Directions[] = {
		FVector(1.0f, 0.371f, 0.173f).GetSafeNormal(),
		FVector(0.271f, 1.0f, 0.133f).GetSafeNormal(),
		FVector(0.163f, 0.239f, 1.0f).GetSafeNormal()};
	int32 InsideVotes = 0;
	for (const FVector& Direction : Directions)
	{
		int32 Hits = 0;
		for (int32 TriangleIndex = Start; TriangleIndex < End; ++TriangleIndex)
		{
			const IPLTriangle& Triangle = Geometry.Triangles[TriangleIndex];
			const int32 IA = Triangle.indices[0];
			const int32 IB = Triangle.indices[1];
			const int32 IC = Triangle.indices[2];
			if (IA < 0 || IB < 0 || IC < 0
				|| size_t(IA) >= Geometry.Vertices.size()
				|| size_t(IB) >= Geometry.Vertices.size()
				|| size_t(IC) >= Geometry.Vertices.size())
			{
				continue;
			}
			const FVector A(Geometry.Vertices[IA].x, Geometry.Vertices[IA].y, Geometry.Vertices[IA].z);
			const FVector B(Geometry.Vertices[IB].x, Geometry.Vertices[IB].y, Geometry.Vertices[IB].z);
			const FVector C(Geometry.Vertices[IC].x, Geometry.Vertices[IC].y, Geometry.Vertices[IC].z);
			const FVector E1 = B - A;
			const FVector E2 = C - A;
			const FVector H = FVector::CrossProduct(Direction, E2);
			const double Det = FVector::DotProduct(E1, H);
			if (FMath::Abs(Det) < 1.e-9) continue;
			const double InvDet = 1.0 / Det;
			const FVector S = P - A;
			const double U = InvDet * FVector::DotProduct(S, H);
			if (U <= 1.e-7 || U >= 1.0 - 1.e-7) continue;
			const FVector Q = FVector::CrossProduct(S, E1);
			const double V = InvDet * FVector::DotProduct(Direction, Q);
			if (V <= 1.e-7 || U + V >= 1.0 - 1.e-7) continue;
			const double T = InvDet * FVector::DotProduct(E2, Q);
			if (T > 1.e-7) ++Hits;
		}
		if ((Hits & 1) != 0) ++InsideVotes;
	}
	return InsideVotes >= 2;
}

bool PointInsideAcousticGeometry(const FIMAcousticSceneInput& Geometry, const TArray<TPair<int32, int32>>& TriangleRanges, const IPLVector3& Point)
{
	// Test each exported static-mesh instance independently. A whole-scene
	// parity pass is not valid when separate walls, floors, or open doorways
	// cancel one another's ray crossings. A hollow box remains hollow because
	// its own triangle range is tested as one topology, while the solid wall
	// shell around that opening still rejects a listener placed in the shell.
	for (const TPair<int32, int32>& Range : TriangleRanges)
	{
		if (PointInsideAcousticTriangleRange(Geometry, Range.Key, Range.Value, Point)) return true;
	}
	return false;
}

IPLMatrix4x4 ToSDKDynamicTransform(const FTransform& Transform, const FVector& Origin)
{
	IPLMatrix4x4 Matrix{};
	// Instance visibility (validation raycasts) treats the instance matrix as
	// rigid: non-uniform component scale must live in the sub-scene vertices
	// (see IMBuildDynamicMeshSnapshot), never in this basis. Translation stays
	// in meters in column 3 (C API row-major Xeon order).
	const FQuat Rigid = Transform.GetRotation();
	const FVector UEAxes[3] = { FVector::RightVector, FVector::UpVector, -FVector::ForwardVector };
	for (int32 Column = 0; Column < 3; ++Column)
	{
		const IPLVector3 Axis = ToSDKDirection(Rigid.RotateVector(UEAxes[Column]));
		Matrix.elements[0][Column] = Axis.x;
		Matrix.elements[1][Column] = Axis.y;
		Matrix.elements[2][Column] = Axis.z;
	}
	const IPLVector3 Translation = ToSDKPosition(Transform.GetLocation(), Origin);
	Matrix.elements[0][3] = Translation.x;
	Matrix.elements[1][3] = Translation.y;
	Matrix.elements[2][3] = Translation.z;
	Matrix.elements[3][3] = 1.0f;
	return Matrix;
}

// Coplanar-triangle merge for dynamic snapshots: Steam Audio pathing conducts
// through internally-subdivided panels (a 2x2-split box face leaks bit-identical
// energy to no door at all, while the same extents as clean tris seal; evidence
// inc112/inc117/inc118), so each coplanar connected patch must reach the SDK as
// a minimal triangulation with no interior edges. Operates in place; any ambiguity
// keeps the original triangles (fail-safe: coverage never shrinks). GT context
// only (allocates); the static bake path is untouched (static walls seal today).
struct FIMPlaneKey
{
	int32 Axis = 0;
	FIntVector Nq = FIntVector::ZeroValue;
	int32 Dq = 0;
	int32 Mat = 0;
	bool operator==(const FIMPlaneKey& O) const { return Axis == O.Axis && Nq == O.Nq && Dq == O.Dq && Mat == O.Mat; }
};
uint32 GetTypeHash(const FIMPlaneKey& K)
{
	uint32 H = ::GetTypeHash(K.Axis);
	H = HashCombine(H, ::GetTypeHash(K.Nq.X));
	H = HashCombine(H, ::GetTypeHash(K.Nq.Y));
	H = HashCombine(H, ::GetTypeHash(K.Nq.Z));
	H = HashCombine(H, ::GetTypeHash(K.Mat));
	return HashCombine(H, ::GetTypeHash(K.Dq));
}
double PolyArea2(const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
	return (double(B.X) - A.X) * (C.Y - A.Y) - (double(B.Y) - A.Y) * (C.X - A.X);
}
bool PointStrictlyInTri(const FVector2D& Q, const FVector2D& A, const FVector2D& B, const FVector2D& C)
{
	const double S1 = PolyArea2(Q, A, B);
	const double S2 = PolyArea2(Q, B, C);
	const double S3 = PolyArea2(Q, C, A);
	return S1 > 1e-9 && S2 > 1e-9 && S3 > 1e-9;
}
bool EarClip(const TArray<FVector2D>& P, TArray<int32>& OutFlat)
{
	TArray<int32> V;
	for (int32 I = 0; I < P.Num(); ++I) { V.Add(I); }
	int32 Guard = V.Num() * V.Num() + 1;
	while (V.Num() > 3 && Guard-- > 0)
	{
		bool bClipped = false;
		for (int32 I = 0; I < V.Num(); ++I)
		{
			const int32 Ip = (I + V.Num() - 1) % V.Num();
			const int32 In = (I + 1) % V.Num();
			const FVector2D& A = P[V[Ip]];
			const FVector2D& B = P[V[I]];
			const FVector2D& C = P[V[In]];
			if (PolyArea2(A, B, C) <= 1e-9) { continue; }
			bool bInside = false;
			for (int32 J = 0; J < V.Num(); ++J)
			{
				if (J == I || J == Ip || J == In) { continue; }
				if (PointStrictlyInTri(P[V[J]], A, B, C)) { bInside = true; break; }
			}
			if (bInside) { continue; }
			OutFlat.Add(V[Ip]); OutFlat.Add(V[I]); OutFlat.Add(V[In]);
			V.RemoveAt(I); bClipped = true; break;
		}
		if (!bClipped) { return false; }
	}
	if (V.Num() != 3) { return false; }
	OutFlat.Add(V[0]); OutFlat.Add(V[1]); OutFlat.Add(V[2]);
	return true;
}
void MergeCoplanarDynamicTris(TArray<IPLVector3>& Verts, TArray<IPLTriangle>& Tris, TArray<int32>& MatIds)
{
	const int32 TriCount = Tris.Num();
	if (TriCount < 2 || MatIds.Num() != TriCount || Verts.Num() <= 0) { return; }
	for (int32 I = 0; I < TriCount; ++I)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			if (Tris[I].indices[K] < 0 || Tris[I].indices[K] >= Verts.Num()) { return; }
		}
	}
	TMap<FVector, int32> WeldId;
	TArray<IPLVector3> Welded;
	TArray<int32> Remap;
	Remap.SetNum(Verts.Num());
	for (int32 I = 0; I < Verts.Num(); ++I)
	{
		const FVector P(Verts[I].x, Verts[I].y, Verts[I].z);
		if (const int32* Found = WeldId.Find(P)) { Remap[I] = *Found; }
		else { Remap[I] = Welded.Num(); WeldId.Add(P, Welded.Num()); Welded.Add(Verts[I]); }
	}
	struct FIMWTri { int32 V[3]; int32 Mat; FVector N; float D; };
	TArray<FIMWTri> Work;
	Work.Reserve(TriCount);
	for (int32 I = 0; I < TriCount; ++I)
	{
		const int32 A = Remap[Tris[I].indices[0]];
		const int32 B = Remap[Tris[I].indices[1]];
		const int32 C = Remap[Tris[I].indices[2]];
		if (A == B || B == C || C == A) { continue; }
		const FVector VA(Welded[A].x, Welded[A].y, Welded[A].z);
		const FVector VB(Welded[B].x, Welded[B].y, Welded[B].z);
		const FVector VC(Welded[C].x, Welded[C].y, Welded[C].z);
		const FVector N = FVector::CrossProduct(VB - VA, VC - VA);
		if (N.SizeSquared() < 1e-12) { continue; }
		FIMWTri T; T.V[0] = A; T.V[1] = B; T.V[2] = C; T.Mat = MatIds[I];
		T.N = N.GetSafeNormal(); T.D = -FVector::DotProduct(T.N, VA);
		Work.Add(T);
	}
	if (Work.Num() < 1) { return; }
	TMap<FIMPlaneKey, TArray<int32>> Groups;
	for (int32 I = 0; I < Work.Num(); ++I)
	{
		const FVector& N = Work[I].N;
		const float AX = FMath::Abs(N.X), AY = FMath::Abs(N.Y), AZ = FMath::Abs(N.Z);
		FIMPlaneKey K;
		K.Axis = (AX >= AY && AX >= AZ) ? 0 : ((AY >= AZ) ? 1 : 2);
		K.Nq = FIntVector(FMath::RoundToInt(N.X * 4096.0f), FMath::RoundToInt(N.Y * 4096.0f), FMath::RoundToInt(N.Z * 4096.0f));
		K.Dq = FMath::RoundToInt(Work[I].D * 10000.0f);
		K.Mat = Work[I].Mat;
		Groups.FindOrAdd(K).Add(I);
	}
	TArray<IPLTriangle> NewTris;
	TArray<int32> NewMats;
	NewTris.Reserve(TriCount);
	NewMats.Reserve(TriCount);
	auto EmitOriginal = [&](int32 WI)
	{
		IPLTriangle T{};
		T.indices[0] = Work[WI].V[0]; T.indices[1] = Work[WI].V[1]; T.indices[2] = Work[WI].V[2];
		NewTris.Add(T); NewMats.Add(Work[WI].Mat);
	};
	TArray<int32> VisitMark;
	VisitMark.SetNumZeroed(Work.Num());
	int32 VisitGen = 1;
	for (auto& KV : Groups)
	{
		const TArray<int32>& Members = KV.Value;
		TMap<int64, TArray<int32>> EdgeTris;
		for (int32 WI : Members)
		{
			for (int32 E = 0; E < 3; ++E)
			{
				const int32 U = Work[WI].V[E], W = Work[WI].V[(E + 1) % 3];
				const int64 Key = ((int64)FMath::Min(U, W) << 32) | (uint32)FMath::Max(U, W);
				EdgeTris.FindOrAdd(Key).Add(WI);
			}
		}
		for (int32 Seed : Members)
		{
			if (VisitMark[Seed] == VisitGen) { continue; }
			TArray<int32> Comp;
			TArray<int32> Stack; Stack.Add(Seed); VisitMark[Seed] = VisitGen;
			while (Stack.Num() > 0)
			{
				const int32 Cur = Stack.Pop();
				Comp.Add(Cur);
				for (int32 E = 0; E < 3; ++E)
				{
					const int32 U = Work[Cur].V[E], W = Work[Cur].V[(E + 1) % 3];
					const int64 Key = ((int64)FMath::Min(U, W) << 32) | (uint32)FMath::Max(U, W);
					if (const TArray<int32>* Adj = EdgeTris.Find(Key))
					{
						for (int32 NX : *Adj)
						{
							if (VisitMark[NX] != VisitGen) { VisitMark[NX] = VisitGen; Stack.Add(NX); }
						}
					}
				}
			}
			if (Comp.Num() < 2) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
			TMap<int64, int32> Use;
			for (int32 WI : Comp)
			{
				for (int32 E = 0; E < 3; ++E)
				{
					const int32 U = Work[WI].V[E], W = Work[WI].V[(E + 1) % 3];
					const int64 Key = ((int64)FMath::Min(U, W) << 32) | (uint32)FMath::Max(U, W);
					Use.FindOrAdd(Key)++;
				}
			}
			TMap<int32, TArray<int32>> Adj;
			bool bManifold = true;
			for (auto& EK : Use)
			{
				if (EK.Value != 1) { continue; }
				const int32 U = int32(EK.Key >> 32), W = int32(EK.Key & 0xFFFFFFFF);
				Adj.FindOrAdd(U).Add(W); Adj.FindOrAdd(W).Add(U);
			}
			for (auto& AK : Adj) { if (AK.Value.Num() != 2) { bManifold = false; break; } }
			if (!bManifold || Adj.Num() == 0) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
			TArray<int32> Loop;
			int32 Start = INT32_MAX;
			for (auto& AK : Adj) { Start = FMath::Min(Start, AK.Key); }
			int32 Prev = -1, Cur = Start;
			for (int32 Step = 0; Step <= Adj.Num(); ++Step)
			{
				Loop.Add(Cur);
				const TArray<int32>& NB = Adj.FindChecked(Cur);
				const int32 Next = (NB[0] != Prev) ? NB[0] : NB[1];
				Prev = Cur; Cur = Next;
				if (Cur == Start) { break; }
			}
			if (Cur != Start || Loop.Num() < 3) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
			for (int32 Pass = 0; Pass < 4 && Loop.Num() > 3; ++Pass)
			{
				bool bDropped = false;
				for (int32 I = 0; I < Loop.Num(); ++I)
				{
					const int32 Ip = (I + Loop.Num() - 1) % Loop.Num();
					const int32 In = (I + 1) % Loop.Num();
					const FVector A(Welded[Loop[Ip]].x, Welded[Loop[Ip]].y, Welded[Loop[Ip]].z);
					const FVector B(Welded[Loop[I]].x, Welded[Loop[I]].y, Welded[Loop[I]].z);
					const FVector C(Welded[Loop[In]].x, Welded[Loop[In]].y, Welded[Loop[In]].z);
					const FVector AB = B - A, BC = C - B;
					const float L1 = AB.Size(), L2 = BC.Size();
					if (L1 < 1e-9 || L2 < 1e-9) { continue; }
					const float SinA = FVector::CrossProduct(AB, BC).Size() / (L1 * L2);
					const float Dist = FVector::CrossProduct(B - A, C - A).Size() / (C - A).Size();
					if (SinA < 1e-6 && Dist < 1e-6 && FVector::DotProduct(AB, BC) > 0.0f)
					{
						Loop.RemoveAt(I); bDropped = true; break;
					}
				}
				if (!bDropped) { break; }
			}
			if (Loop.Num() < 3) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
			const int32 Axis = KV.Key.Axis;
			TArray<FVector2D> Pts;
			Pts.Reserve(Loop.Num());
			for (int32 VI : Loop)
			{
				const IPLVector3& P = Welded[VI];
				Pts.Add(Axis == 0 ? FVector2D(P.y, P.z) : (Axis == 1 ? FVector2D(P.x, P.z) : FVector2D(P.x, P.y)));
			}
			double LoopArea = 0.0;
			for (int32 I = 0; I < Pts.Num(); ++I) { LoopArea += PolyArea2(Pts[I], Pts[(I + 1) % Pts.Num()], Pts[(I + 2) % Pts.Num()]); }
			if (FMath::Abs(LoopArea) < 1e-9) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
			if (LoopArea < 0.0)
			{
				for (int32 LI = 0, LJ = Loop.Num() - 1; LI < LJ; ++LI, --LJ) { Loop.Swap(LI, LJ); }
				Pts.Reset();
				for (int32 VI : Loop)
				{
					const IPLVector3& P = Welded[VI];
					Pts.Add(Axis == 0 ? FVector2D(P.y, P.z) : (Axis == 1 ? FVector2D(P.x, P.z) : FVector2D(P.x, P.y)));
				}
			}
			TArray<int32> Flat;
			if (!EarClip(Pts, Flat)) { for (int32 WI : Comp) { EmitOriginal(WI); } continue; }
			const int32 RefA = Work[Comp[0]].V[0], RefB = Work[Comp[0]].V[1], RefC = Work[Comp[0]].V[2];
			auto ProjOf = [&](int32 VI) -> FVector2D
			{
				const IPLVector3& P = Welded[VI];
				return Axis == 0 ? FVector2D(P.y, P.z) : (Axis == 1 ? FVector2D(P.x, P.z) : FVector2D(P.x, P.y));
			};
			const double RefSign = PolyArea2(ProjOf(RefA), ProjOf(RefB), ProjOf(RefC));
			const double OutSign = PolyArea2(Pts[Flat[0]], Pts[Flat[1]], Pts[Flat[2]]);
			const bool bSwap = (RefSign < 0.0) != (OutSign < 0.0);
			for (int32 I = 0; I + 2 < Flat.Num(); I += 3)
			{
				IPLTriangle T{};
				T.indices[0] = Loop[Flat[I]];
				T.indices[1] = Loop[bSwap ? Flat[I + 2] : Flat[I + 1]];
				T.indices[2] = Loop[bSwap ? Flat[I + 1] : Flat[I + 2]];
				NewTris.Add(T); NewMats.Add(Work[Comp[0]].Mat);
			}
		}
		++VisitGen;
	}
	if (NewTris.Num() < 1) { return; }
	Verts = MoveTemp(Welded);
	Tris = MoveTemp(NewTris);
	MatIds = MoveTemp(NewMats);
}
bool BuildDynamicMeshSnapshot(UStaticMeshComponent* Component, int32 InstanceIndex,
	const FVector& Origin, const FIMAcousticMaterialTable& MaterialTable,
	FIMAcousticDynamicMeshSnapshot& Out, FString& Failure)
{
	// Dynamic spline deformation needs its own geometry-change hash. Reject an
	// explicitly supplied blocker until that contract exists; raw LOD0 would
	// put an acoustic obstacle somewhere other than the rendered spline.
	if (Cast<USplineMeshComponent>(Component))
	{
		Failure = TEXT("Dynamic spline mesh blocker is unsupported.");
		return false;
	}
	if (!IsValid(Component) || !Component->GetStaticMesh() || !Component->GetStaticMesh()->GetRenderData()
		|| Component->GetStaticMesh()->GetRenderData()->LODResources.IsEmpty())
	{
		Failure = TEXT("Dynamic geometry has no CPU mesh data.");
		return false;
	}
	UStaticMesh* Mesh = Component->GetStaticMesh();
	const FStaticMeshLODResources& LOD = Mesh->GetRenderData()->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	const auto IndexView = LOD.IndexBuffer.GetArrayView();
	if (Positions.GetNumVertices() == 0 || !Positions.GetVertexData() || IndexView.Num() == 0)
	{
		Failure = FString::Printf(TEXT("Dynamic mesh CPU geometry unavailable: %s"), *Mesh->GetPathName());
		return false;
	}
	FTransform Transform = Component->GetComponentTransform();
	if (UInstancedStaticMeshComponent* Instances = Cast<UInstancedStaticMeshComponent>(Component))
	{
		// This registration owns one rigid obstacle, not a component-wide
		// instance stream. Never accept instance zero as the entire component.
		if (Instances->GetInstanceCount() != 1 || InstanceIndex != 0)
		{
			Failure = TEXT("Registered dynamic ISM/HISM requires exactly one instance; component-wide dynamic instances are unsupported.");
			return false;
		}
		if (!Instances->GetInstanceTransform(InstanceIndex, Transform, true))
		{
			Failure = TEXT("Dynamic instance transform unavailable.");
			return false;
		}
	}
	if (Transform.ContainsNaN() || FMath::IsNearlyZero(Transform.GetDeterminant()))
	{
		Failure = TEXT("Dynamic instance has invalid or singular transform.");
		return false;
	}
	Out = {};
	Out.Key = (static_cast<uint64>(Component->GetUniqueID()) << 32)
		| static_cast<uint64>(InstanceIndex + 1);
	// World scale lives in the vertices (meters), not in the instance basis:
	// visibility raycasts treat instances as rigid, so a scaled basis would
	// silently shrink the obstacle (W3 closed-door leak). Scale edits change
	// the hash so the simulator recreates instead of reusing stale geometry.
	const FVector MeshScale = Transform.GetScale3D();
	Out.Transform = ToSDKDynamicTransform(Transform, Origin);
	Out.Materials.Append(MaterialTable.Native);
	FSHA1 GeometryIdentity;
	IMHashMaterialIdentity(GeometryIdentity,IMStableBakeObjectPath(Mesh));
	for(int32 Index=0;Index<MaterialTable.Identities.Num();++Index)
	{
		IMHashMaterialIdentity(GeometryIdentity,MaterialTable.Identities[Index]);
		const uint8 Roles=(MaterialTable.Solid[Index]?1:0)|(MaterialTable.SupportsProbes[Index]?2:0);
		GeometryIdentity.Update(&Roles,sizeof(Roles));
	}
	Out.Vertices.Reserve(Positions.GetNumVertices());
	for (uint32 VertexIndex = 0; VertexIndex < Positions.GetNumVertices(); ++VertexIndex)
	{
		const FVector RawVertex(Positions.VertexPosition(VertexIndex));
		const FVector ScaledVertex(RawVertex.X * MeshScale.X, RawVertex.Y * MeshScale.Y, RawVertex.Z * MeshScale.Z);
		Out.Vertices.Add(ToSDKDirection(ScaledVertex * 0.01f));
		GeometryIdentity.Update(reinterpret_cast<const uint8*>(&RawVertex),sizeof(RawVertex));
	}
	// UE faces wind CW; IMToSDKDirection (Y,Z,-X, det -1) already converts to
	// SDK-CCW-outward, so swap only when the instance itself mirrors (det<0).
	// Swapping det>0 wound the H1 door inward (VOL -0.594) and conducted path
	// audio through the sealed aperture (inc108; falsifiable by VOL sign).
	const bool Flip = Transform.GetDeterminant() < 0.0f;
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		const int32 MaterialIndex=MaterialTable.ResolveMesh(Component,Section.MaterialIndex,InstanceIndex);
		if (MaterialIndex==INDEX_NONE)
		{
			Failure = FString::Printf(TEXT("Dynamic acoustic material missing: %s section %d"),
				*Component->GetPathName(), Section.MaterialIndex);
			return false;
		}
		GeometryIdentity.Update(reinterpret_cast<const uint8*>(&Section.MaterialIndex),sizeof(Section.MaterialIndex));
		GeometryIdentity.Update(reinterpret_cast<const uint8*>(&MaterialIndex),sizeof(MaterialIndex));
		if(!MaterialTable.Solid[MaterialIndex])continue;
		for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
		{
			IPLTriangle Triangle{};
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 Index = LOD.IndexBuffer.GetIndex(Section.FirstIndex + TriangleIndex * 3 + Corner);
				if (Index >= Positions.GetNumVertices())
				{
					Failure = TEXT("Dynamic mesh index exceeds vertex buffer.");
					return false;
				}
				Triangle.indices[Corner] = static_cast<IPLint32>(Index);
			}
			if (Flip) { std::swap(Triangle.indices[1], Triangle.indices[2]); }
			Out.Triangles.Add(Triangle);
			Out.MaterialIndices.Add(MaterialIndex);
		}
	}
	{
		const int32 IMergeInTris = Out.Triangles.Num();
		const int32 IMergeInVerts = Out.Vertices.Num();
		MergeCoplanarDynamicTris(Out.Vertices, Out.Triangles, Out.MaterialIndices);
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMergeDynamic in_tris=%d out_tris=%d in_verts=%d out_verts=%d mesh=%s"),
			IMergeInTris, Out.Triangles.Num(), IMergeInVerts, Out.Vertices.Num(), *Mesh->GetPathName());
	}
	if (Out.Triangles.IsEmpty())
	{
		// An explicit NonSolid profile removes this obstacle from the next
		// snapshot; the worker then removes its previous instance, if any.
		if(MaterialTable.Physical)return true;
		Failure=FString::Printf(TEXT("Dynamic mesh has no supported triangles: %s"),*Mesh->GetPathName());return false;
	}
	auto HashArray=[&GeometryIdentity](const auto& Values)
	{
		const int32 Count=Values.Num();GeometryIdentity.Update(reinterpret_cast<const uint8*>(&Count),sizeof(Count));
		if(Count)GeometryIdentity.Update(reinterpret_cast<const uint8*>(Values.GetData()),Count*sizeof(Values[0]));
	};
	HashArray(Out.Vertices);HashArray(Out.Triangles);HashArray(Out.MaterialIndices);HashArray(Out.Materials);
	GeometryIdentity.Final();uint8 Digest[20];GeometryIdentity.GetHash(Digest);
	FMemory::Memcpy(&Out.GeometryHash,Digest,sizeof(Out.GeometryHash));
	return true;
}
}

struct FIMAcousticFieldRuntime
{
	FIMAcousticSceneInput Geometry;
	TArray<bool> ProbeSupportMaterials;
	TArray<TPair<int32, int32>> GeometryTriangleRanges;
	FIMAcousticGrassGenerationState GrassGeneration;
	IPLProbeGenerationParams ProbeParams{};
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	bool bProfileEditorValidationForTest=false;
	// Test owner retains the sampler and detaches it before destruction. Only
	// the GT ticker uses this pointer; ordinary Editor/runtime paths keep null.
	FIMAcousticOperationMemorySampler* EditorValidationMemorySamplerForTest=nullptr;
	int32 ProfiledEditorValidationTicks=0;
	double ProfiledEditorCaptureMs=0.0;
	double ProfiledEditorAssetValidationMs=0.0;
	double ProfiledEditorMaxTickMs=0.0;
#endif
	FString Fingerprint;
	FString LastDynamicGeometryFailure;
	FString ProbeFingerprint;
	FVector Origin=FVector::ZeroVector;
	TSharedPtr<IMAcousticBakeVolumePrivate::FIMBakeJob,ESPMode::ThreadSafe> Job;
	TUniquePtr<FIMAcousticSimulationWorker> Worker;
	TSharedPtr<FIMAcousticDeviceBridge,ESPMode::ThreadSafe> Bridge;
	TSharedPtr<FIMAcousticReverbPool,ESPMode::ThreadSafe> Reverb;
	FIMAcousticMetaSoundContextPtr MetaSoundContext;
	TStrongObjectPtr<UAudioBus> MetaSoundBus;
	TSet<uint64> MetaSoundRegisteredSources;
	bool MetaSoundAudibleReported = false;
	FAudioDeviceHandle DeviceHandle;
	FTSTicker::FDelegateHandle BakeTicker;
	FTSTicker::FDelegateHandle ValidationTicker;
	uint64 Epoch=0;
	uint64 ListenerSpatialVersion=0;
	FVector LastListenerLocation=FVector::ZeroVector;
	FQuat LastListenerRotation=FQuat::Identity;
	bool bHasListenerPose=false;
	double NextSnapshot=0;
	double PreviousSnapshot=0;
	bool SceneValid=false;
	bool ApertureTransitEnabled=false;
	FVector ApertureTransitCenterUE=FVector::ZeroVector;
	FVector ApertureTransitHalfExtentUE=FVector::ZeroVector;
};

AIMAcousticBakeVolume::AIMAcousticBakeVolume()
{
	PrimaryActorTick.bCanEverTick=true;
	BakeBounds=CreateDefaultSubobject<UBoxComponent>(TEXT("BakeBounds"));
	SetRootComponent(BakeBounds);
	BakeBounds->SetBoxExtent(FVector(500,500,250));
	BakeBounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Runtime=MakeUnique<FIMAcousticFieldRuntime>();
}
AIMAcousticBakeVolume::~AIMAcousticBakeVolume() = default;

void AIMAcousticBakeVolume::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
#if WITH_EDITOR
	if(!Runtime||Runtime->ValidationTicker.IsValid()||!GetWorld()||GetWorld()->IsGameWorld()||IsTemplate())return;
	// Asset/instance edits and actor insertion can bypass this actor's property
	// callbacks. Re-export in the editor at a bounded cadence so all supported
	// geometry changes invalidate the same fingerprint, even without realtime.
	TWeakObjectPtr<AIMAcousticBakeVolume> WeakOwner(this);
	Runtime->ValidationTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakOwner,LastValidationSeconds=FPlatformTime::Seconds()](float) mutable
	{
		auto* Owner=WeakOwner.Get();if(!Owner||!Owner->GetWorld())return false;
		// Automation helpers pump the core ticker with synthetic deltas, so the 1s ticker interval alone does not bound
		// real-time cost; a full scene re-export per pump would starve unrelated tests' waits.
		const double NowSeconds=FPlatformTime::Seconds();
		if(NowSeconds-LastValidationSeconds<1.0)return true;
		LastValidationSeconds=NowSeconds;
		if(Owner->GetWorld()->IsGameWorld()||(GEditor&&GEditor->PlayWorld)||Owner->Runtime->Job||!Owner->BakedField)return true;
		const FString PreviousStatus=Owner->Status;FString Error;
#if WITH_DEV_AUTOMATION_TESTS
		const bool bProfile=Owner->Runtime->bProfileEditorValidationForTest;
		const double TickStartedAt=bProfile?FPlatformTime::Seconds():0.0;
		auto* MemorySampler=bProfile?Owner->Runtime->EditorValidationMemorySamplerForTest:nullptr;
		const uint64 TickMemoryId=MemorySampler?MemorySampler->BeginOperation(TEXT("scheduled_editor_tick")):0;
		const uint64 CaptureMemoryId=MemorySampler?MemorySampler->BeginOperation(TEXT("scheduled_editor_scene_capture")):0;
#endif
		const bool bCaptured=Owner->CaptureScene();
#if WITH_DEV_AUTOMATION_TESTS
		const double CaptureEndedAt=bProfile?FPlatformTime::Seconds():0.0;
		if(MemorySampler)MemorySampler->EndOperation(CaptureMemoryId);
		const uint64 ValidationMemoryId=MemorySampler?MemorySampler->BeginOperation(TEXT("scheduled_editor_bound_asset_validation")):0;
#endif
		bool bBakeValid=false;
		if(!bCaptured)Owner->Status=TEXT("Bake stale: scene export is incomplete. See Scene Issues.");
		else
		{
			bBakeValid=Owner->BakedField->Validate(Owner->GetWorld()->GetOutermost()->GetName(),Owner->Runtime->Fingerprint,Error);
			if(!bBakeValid)Owner->Status=Error;
			else
			{
				// Both fingerprint mismatches and incomplete scene exports mark
				// the bake stale. A successful recapture must clear either status.
				const bool bWasStale = PreviousStatus.StartsWith(TEXT("Acoustic bake is stale"))
					|| PreviousStatus.StartsWith(TEXT("Bake stale:"));
				Owner->Status = bWasStale ? TEXT("Bake valid for current scene.") : PreviousStatus;
			}
		}
#if WITH_DEV_AUTOMATION_TESTS
		if(MemorySampler)
		{
			MemorySampler->EndOperation(ValidationMemoryId);
			MemorySampler->EndOperation(TickMemoryId);
		}
		if(bProfile)
		{
			const double TickEndedAt=FPlatformTime::Seconds();
			const double CaptureMs=(CaptureEndedAt-TickStartedAt)*1000.0;
			const double ValidationMs=(TickEndedAt-CaptureEndedAt)*1000.0;
			const double TotalMs=(TickEndedAt-TickStartedAt)*1000.0;
			++Owner->Runtime->ProfiledEditorValidationTicks;
			Owner->Runtime->ProfiledEditorCaptureMs+=CaptureMs;
			Owner->Runtime->ProfiledEditorAssetValidationMs+=ValidationMs;
			Owner->Runtime->ProfiledEditorMaxTickMs=FMath::Max(Owner->Runtime->ProfiledEditorMaxTickMs,TotalMs);
			UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticG2EditorTicker capture_ms=%.3f asset_validate_ms=%.3f total_ms=%.3f captured=%d bound_asset_valid=%d triangles=%d"),
				CaptureMs,ValidationMs,TotalMs,bCaptured?1:0,bBakeValid?1:0,Owner->ExportedTriangles);
		}
#endif
		return true;
	}),1.0f);
#endif
}

bool AIMAcousticBakeVolume::CaptureScene()
{
	check(IsInGameThread());
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	const double IMCaptureStarted=FPlatformTime::Seconds();
	uint64 IMObsLandscapeBlocks=0,IMObsLandscapeHeightSamples=0,IMObsLandscapeMaterialCells=0;
	uint64 IMObsLandscapeValidationCells=0,IMObsLandscapeCellsVisited=0,IMObsLandscapeVisibilityHoles=0;
	uint64 IMObsLandscapeRegionCulled=0,IMObsLandscapeCellsIncluded=0;
	uint64 IMObsMeshComponents=0,IMObsUniqueMeshes=0,IMObsMeshPositionHashes=0,IMObsMeshIndexHashes=0;
	uint64 IMObsInstancedMeshComponents=0,IMObsMeshInstanceVisits=0,IMObsMeshTransformFailures=0,IMObsMeshInvalidTransforms=0;
	uint64 IMObsMeshInstancesOutside=0,IMObsMeshInstancesIncluded=0,IMObsMeshInvalidSplineInstances=0;
	uint64 IMObsMeshVertices=0,IMObsMeshSplineVertices=0,IMObsMeshSectionResolves=0,IMObsMeshTrianglesEmitted=0;
	uint64 IMObsCompactionCalls=0,IMObsCompactionVerticesIn=0,IMObsCompactionVerticesOut=0,IMObsCompactionTriangles=0;
	double IMObsLandscapeReadSeconds=0.0,IMObsLandscapeCellWorkSeconds=0.0;
	double IMObsMeshLODHashSeconds=0.0,IMObsMeshInstanceExpansionSeconds=0.0;
	double IMObsMeshComponentSeconds=0.0,IMObsMeshVisitCullBookkeepingSeconds=0.0,IMObsCompactionSeconds=0.0;
	TSet<UStaticMesh*> IMObsSeenMeshes;
#endif
	const bool bDetailedGeometryLog=IMAcousticBakeVolumePrivate::CVarIMAcousticDetailedGeometryLog.GetValueOnGameThread()!=0;
	TArray<IMAcousticBakeVolumePrivate::FIMGeometrySectionContribution> IMSectionContributions;
	const FString IMContributionCaptureId=bDetailedGeometryLog?FGuid::NewGuid().ToString(EGuidFormats::Digits):FString();
	Runtime->SceneValid=false;
	auto PreviousProbes=MoveTemp(Runtime->Geometry.Probes);
	GeneratedProbes=0;
	Runtime->Geometry={};
	Runtime->GeometryTriangleRanges.Reset();
	SceneIssues.Reset();ExportedTriangles=0;NonSolidTriangles=0;NonSolidSections=0;
	Runtime->ProbeSupportMaterials.Reset();
	if (!GetWorld() || !BakeBounds || !FMath::IsFinite(ProbeSpacingCm) || ProbeSpacingCm<25
		|| !FMath::IsFinite(ProbeHeightCm) || ProbeHeightCm<25)
	{ Status=TEXT("Invalid world, bounds or probe configuration.");return false; }
	const FTransform BoundsTransform=BakeBounds->GetComponentTransform();
	if(!BoundsTransform.GetRotation().Equals(FQuat::Identity) || !BoundsTransform.GetScale3D().Equals(FVector::OneVector))
	{ Status=TEXT("Bake bounds must be axis-aligned with unit scale; edit Box Extent for size.");return false; }
	Runtime->Origin=BoundsTransform.GetLocation();
	const FBox Region=BakeBounds->Bounds.GetBox();
	bool Failed=false;
	IMAcousticBakeVolumePrivate::FIMAcousticMaterialTable MaterialTable;
	FString MaterialFailure;
	if(!MaterialTable.Build(*this,MaterialFailure))
	{SceneIssues.Add(MaterialFailure);Status=MaterialFailure;return false;}
	for(const auto& Material:MaterialTable.Native)Runtime->Geometry.Materials.push_back(Material);
	Runtime->ProbeSupportMaterials=MaterialTable.SupportsProbes;
	FSHA1 SourceIdentity;
	if(MaterialTable.Physical)
	{
		// Identity is independent of numeric equality: exchanging two PM assets,
		// changing a role, or moving a NonSolid instance must stale the bake.
		IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,TEXT("physical-material-rules-v1"));
		for(int32 Index=0;Index<MaterialTable.Identities.Num();++Index)
		{
			IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,MaterialTable.Identities[Index]);
			const uint8 Roles=(MaterialTable.Solid[Index]?1:0)|(MaterialTable.SupportsProbes[Index]?2:0);
			SourceIdentity.Update(&Roles,sizeof(Roles));
		}
	}
	FIMAcousticGrassCapture GrassCapture;
	FString GrassFailure;
	if(!IMCaptureAcousticGrassRegion(GetWorld(),Region,Runtime->GrassGeneration,GrassCapture,GrassFailure))
	{SceneIssues.Add(GrassFailure);Status=GrassFailure;return false;}
	if(MaterialTable.Physical&&!GrassCapture.InputSha1.IsEmpty())
	{
		IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,TEXT("static-grass-producers-v1"));
		IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,GrassCapture.InputSha1);
	}
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticGrassStaticCapture world_id=%u epoch=%llu gt_frame=%llu clock=%.9f sources=%d cameras=%d read_components=%d read_instances=%lld region_components=%d region_instances=%lld plan_ms=%.3f generation_ms=%.3f selection_ms=%.3f read_ms=%.3f filter_ms=%.3f verify_ms=%.3f"),
		GetWorld()->GetUniqueID(),Runtime->Epoch,GFrameCounter,FPlatformTime::Seconds(),
		GrassCapture.SelectedSources,GrassCapture.Cameras,GrassCapture.ReadComponents,
		GrassCapture.ReadInstances,GrassCapture.Components.Num(),GrassCapture.RegionInstances,
		GrassCapture.PlanningMilliseconds,GrassCapture.GenerationMilliseconds,GrassCapture.SelectionMilliseconds,
		GrassCapture.ReadMilliseconds,GrassCapture.FilterMilliseconds,GrassCapture.VerificationMilliseconds);
	TArray<UPrimitiveComponent*> Components;
	for(TActorIterator<AActor> It(GetWorld());It;++It)
	{
		if(*It==this)continue;
		// These world-owned helpers describe editor construction / default physics,
		// not acoustic surfaces. Do not generalize this to authored BSP or volumes.
		if(*It==GetWorld()->GetDefaultBrush() || *It==GetWorld()->GetDefaultPhysicsVolume())
		{
			SceneIssues.Add(FString::Printf(TEXT("Excluded engine world helper: %s"),*It->GetPathName()));
			continue;
		}
		TArray<UPrimitiveComponent*> ActorComponents;It->GetComponents(ActorComponents);
		Components.Append(ActorComponents);
	}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	const double IMEnumerationEnded=FPlatformTime::Seconds();
#endif
	// Capture order is part of the scene fingerprint. Build each full path
	// once; recomputing it in every comparison dominated large Editor checks.
	TArray<TPair<FString,UPrimitiveComponent*>> ComponentsByPath;
	ComponentsByPath.Reserve(Components.Num());
	for(UPrimitiveComponent* Component:Components)
	{
		if(!IsValid(Component))continue;
		if(Component->GetClass()->GetPathName()==TEXT("/Script/Foliage.GrassInstancedStaticMeshComponent"))
		{
			const auto* Grass=GrassCapture.Components.Find(CastChecked<UInstancedStaticMeshComponent>(Component));
			if(Grass)ComponentsByPath.Emplace(Grass->ProducerIdentity,Component);
			continue;
		}
		ComponentsByPath.Emplace(IMStableBakeObjectPath(Component),Component);
	}
	ComponentsByPath.Sort([](const auto& A,const auto& B){return A.Key<B.Key;});
	Components.Reset(ComponentsByPath.Num());
	for(const auto& Entry:ComponentsByPath)Components.Add(Entry.Value);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	const double IMGatherEnded=FPlatformTime::Seconds();
#endif
	IMAcousticBakeVolumePrivate::FIMMeshLocalReadCache MeshLocalReadCache;
	TMap<FString,int32> ActorTris;
	TArray<TPair<int32,FString>> ActorRanges;
	for(UPrimitiveComponent* Primitive:Components)
	{
		if(!IsValid(Primitive))continue;
		const auto* Grass=GrassCapture.Components.Find(Cast<UInstancedStaticMeshComponent>(Primitive));
		// Editor LoadMap can expose registered HISM components before their
		// cached Bounds and instance ordering finish updating. A zero cached
		// box would cull acoustically relevant geometry and yield a false stale
		// fingerprint. Defer validation until the normal Editor tick completes.
		if(!Grass && Primitive->IsRegistered() && Primitive->IsA<UStaticMeshComponent>()
			&& Primitive->Bounds.BoxExtent.IsNearlyZero())
		{
			const FBox CalculatedBox=Primitive->CalcBounds(Primitive->GetComponentTransform()).GetBox();
			if(CalculatedBox.GetExtent().IsNearlyZero()==false && Region.Intersect(CalculatedBox))
			{
				SceneIssues.Add(FString::Printf(TEXT("Component bounds pending after map load: %s"),*Primitive->GetPathName()));
				Status=TEXT("Scene components are still updating bounds; retry validation after the Editor tick.");
				return false;
			}
		}
		if(!Grass&&!Region.Intersect(Primitive->Bounds.GetBox()))continue;
		const bool Required=Primitive->ComponentHasTag(TEXT("IMAcousticRequired"));
#if WITH_EDITORONLY_DATA
		if(Primitive->IsVisualizationComponent()||Primitive->IsEditorOnly())
		{
			SceneIssues.Add(FString::Printf(TEXT("Excluded editor-only component: %s"),*Primitive->GetPathName()));
			if(Required)Failed=true;
			continue;
		}
#endif
		if(Primitive->ComponentHasTag(TEXT("IMAcousticIgnore")))
		{
			SceneIssues.Add(FString::Printf(TEXT("Explicitly excluded: %s"),*Primitive->GetPathName()));
			if(Required)Failed=true;
			continue;
		}
		if(Primitive->Mobility!=EComponentMobility::Static)
		{
			SceneIssues.Add(FString::Printf(TEXT("Not baked (movable): %s"),*Primitive->GetPathName()));
			if(Required||Grass)Failed=true;
			continue;
		}
		if(ULandscapeComponent* RenderLandscape=Cast<ULandscapeComponent>(Primitive))
		{
			// The paired collision heightfield is the source of terrain triangles.
			// Require a full-resolution pair so this render surface cannot vanish.
			ULandscapeHeightfieldCollisionComponent* Collision=RenderLandscape->GetCollisionComponent();
			if(!Collision || !Region.Intersect(Collision->Bounds.GetBox()))
			{
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape collision missing from bake region: %s"),*Primitive->GetPathName()));
				Failed=true;
			}
			continue;
		}
		if(ULandscapeHeightfieldCollisionComponent* Landscape=Cast<ULandscapeHeightfieldCollisionComponent>(Primitive))
		{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			++IMObsLandscapeBlocks;
#endif
			if(MaterialTable.Physical)IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,IMStableBakeObjectPath(Primitive));
			ULandscapeComponent* Render=Landscape->GetRenderComponent();
			const int32 Quads=Landscape->CollisionSizeQuads;
			const int64 Side=int64(Quads)+1;
			const int64 Samples=Side*Side;
			const FTransform Transform=Landscape->GetComponentTransform();
			if(!Render || Render->GetCollisionComponent()!=Landscape || Quads<=0 || Samples>MAX_int32
				|| !FMath::IsNearlyEqual(Landscape->CollisionScale,1.0f)
				|| Render->ComponentSizeQuads!=Quads || Render->GetNumMaterials()!=1
				|| Landscape->CookedPhysicalMaterials.IsEmpty() || !Landscape->CookedPhysicalMaterials[0]
				|| !Transform.TransformVectorNoScale(FVector::UpVector).Equals(FVector::UpVector,1.e-4)
				|| Runtime->Geometry.Vertices.size()>size_t(MAX_int32)
				|| Runtime->Geometry.Triangles.size()>size_t(MAX_int32))
			{
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape full-resolution single-material collision unsupported: %s"),*Primitive->GetPathName()));
				Failed=true;
				continue;
			}
			const int32* LegacyMaterial=MaterialTable.RenderIndices.Find(Render->GetMaterial(0));
			if(!MaterialTable.Physical&&!LegacyMaterial)
			{
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape acoustic material missing: %s"),*Render->GetPathName()));
				Failed=true;
				continue;
			}
			TArray<float> Heights;Heights.SetNumUninitialized(int32(Samples));
			TArray<uint8> SurfaceIndices;
			FString SurfaceFailure;
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			IMObsLandscapeHeightSamples+=uint64(Samples);
			IMObsLandscapeMaterialCells+=uint64(Quads)*uint64(Quads);
			const double IMLandscapeReadStarted=FPlatformTime::Seconds();
#endif
			if(!Landscape->FillHeightTile(MakeArrayView(Heights),0,int32(Side))
				|| !IMReadLandscapeMaterialCells(*Landscape,SurfaceIndices,SurfaceFailure))
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				IMObsLandscapeReadSeconds+=FPlatformTime::Seconds()-IMLandscapeReadStarted;
#endif
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape height/physical material cells unavailable: %s %s"),*Primitive->GetPathName(),*SurfaceFailure));
				Failed=true;
				continue;
			}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			IMObsLandscapeReadSeconds+=FPlatformTime::Seconds()-IMLandscapeReadStarted;
			const double IMLandscapeCellWorkStarted=FPlatformTime::Seconds();
#endif
			bool bUnsupportedSurface=false;
			for(const float Height:Heights)if(!FMath::IsFinite(Height)){bUnsupportedSurface=true;break;}
			for(int32 Y=0;Y<Quads&&!bUnsupportedSurface;++Y)
				for(int32 X=0;X<Quads;++X)
				{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
					++IMObsLandscapeValidationCells;
#endif
					const uint8 Surface=SurfaceIndices[Y*Quads+X];
					if(!MaterialTable.Physical&&Surface!=0&&Surface!=255){bUnsupportedSurface=true;break;}
				}
			if(bUnsupportedSurface)
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				IMObsLandscapeCellWorkSeconds+=FPlatformTime::Seconds()-IMLandscapeCellWorkStarted;
#endif
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape non-finite height or multiple physical materials unsupported: %s"),*Primitive->GetPathName()));
				Failed=true;
				continue;
			}
			const int32 FirstTriangle=int32(Runtime->Geometry.Triangles.size());
			// Collision blocks are much larger than a typical bake volume. Keep
			// only cells whose full world-space height box touches the region;
			// a boundary cell remains complete so no acoustic face is lost.
			TMap<int32,int32> ExportedVertexIndices;
			auto TerrainWorldVertex=[&](int32 Index)
			{
				const int32 X=Index%int32(Side);
				const int32 Y=Index/int32(Side);
				FVector Position=Transform.TransformPosition(FVector(X*Landscape->CollisionScale,Y*Landscape->CollisionScale,0));
				Position.Z=Heights[Index];
				return Position;
			};
			auto ExportTerrainVertex=[&](int32 Index,const FVector& WorldPosition)
			{
				if(const int32* Existing=ExportedVertexIndices.Find(Index))return *Existing;
				const int32 NewIndex=int32(Runtime->Geometry.Vertices.size());
				Runtime->Geometry.Vertices.push_back(ToSDKPosition(WorldPosition,Runtime->Origin));
				ExportedVertexIndices.Add(Index,NewIndex);
				return NewIndex;
			};
			const bool Flip=Transform.GetDeterminant()<0;
			auto AddTerrainTriangle=[&](int32 A,int32 B,int32 C,int32 Material)
			{
				IPLTriangle Triangle{};Triangle.indices[0]=A;Triangle.indices[1]=B;Triangle.indices[2]=C;
				// UE clockwise terrain faces become SDK counterclockwise after
				// the handedness-changing coordinate conversion.
				if(Flip)std::swap(Triangle.indices[1],Triangle.indices[2]);
				Runtime->Geometry.Triangles.push_back(Triangle);Runtime->Geometry.MaterialIndices.push_back(Material);
			};
			bool bIndexOverflow=false;
			for(int32 Y=0;Y<Quads && !bIndexOverflow;++Y)
				for(int32 X=0;X<Quads;++X)
				{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
					++IMObsLandscapeCellsVisited;
#endif
					const int32 I=Y*int32(Side)+X;
					const uint8 CellSurface=SurfaceIndices[Y*Quads+X];
					if(CellSurface==255)
					{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
						++IMObsLandscapeVisibilityHoles;
#endif
						continue; // actual collision visibility hole
					}
					const int32 Corners[4]={I,I+int32(Side),I+1,I+int32(Side)+1};
					FVector WorldCorners[4];
					FBox CellBounds(ForceInit);
					for(int32 Corner=0;Corner<4;++Corner)
					{
						WorldCorners[Corner]=TerrainWorldVertex(Corners[Corner]);
						CellBounds+=WorldCorners[Corner];
					}
					if(!Region.Intersect(CellBounds))
					{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
						++IMObsLandscapeRegionCulled;
#endif
						continue;
					}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
					++IMObsLandscapeCellsIncluded;
#endif
					int32 Material=LegacyMaterial?*LegacyMaterial:INDEX_NONE;
					if(MaterialTable.Physical)
					{
						// The registered collision face and CookedPhysicalMaterials
						// use the same Chaos table (LandscapeCollision.cpp).
						// Resolve each collision cell, including painted layers;
						// the two exported triangles retain that cell's PM.
						const int32 Surface=CellSurface;
						UPhysicalMaterial* PM=Landscape->CookedPhysicalMaterials.IsValidIndex(Surface)
							?Landscape->CookedPhysicalMaterials[Surface].Get():nullptr;
						const int32* Resolved=MaterialTable.PhysicalIndices.Find(PM);
						if(!Resolved||!MaterialTable.Solid[*Resolved])
						{
							SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape physical profile missing/non-solid: %s cell (%d,%d)"),*Primitive->GetPathName(),X,Y));
							Failed=true;bIndexOverflow=true;break;
						}
						Material=*Resolved;
						SourceIdentity.Update(reinterpret_cast<const uint8*>(&I),sizeof(I));
						SourceIdentity.Update(reinterpret_cast<const uint8*>(&Material),sizeof(Material));
					}
					if(Runtime->Geometry.Vertices.size()+4>size_t(MAX_int32)
						|| Runtime->Geometry.Triangles.size()+2>size_t(MAX_int32))
					{
						bIndexOverflow=true;
						break;
					}
					int32 VertexIndices[4];
					for(int32 Corner=0;Corner<4;++Corner)
						VertexIndices[Corner]=ExportTerrainVertex(Corners[Corner],WorldCorners[Corner]);
					AddTerrainTriangle(VertexIndices[0],VertexIndices[1],VertexIndices[2],Material);
					AddTerrainTriangle(VertexIndices[2],VertexIndices[1],VertexIndices[3],Material);
				}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			IMObsLandscapeCellWorkSeconds+=FPlatformTime::Seconds()-IMLandscapeCellWorkStarted;
#endif
			if(bIndexOverflow)
			{
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED Landscape export aborted: invalid physical profile or clipped index limit: %s"),*Primitive->GetPathName()));
				Failed=true;
				continue;
			}
			const int32 LastTriangle=int32(Runtime->Geometry.Triangles.size());
			if(LastTriangle>FirstTriangle)
			{
				Runtime->GeometryTriangleRanges.Add(TPair<int32,int32>(FirstTriangle,LastTriangle));
				ActorRanges.Add(TPair<int32,FString>(FirstTriangle,Primitive->GetPathName()));
			}
			ActorTris.Add(Primitive->GetPathName(),LastTriangle-FirstTriangle);
			continue;
		}
		UStaticMeshComponent* MeshComponent=Cast<UStaticMeshComponent>(Primitive);
		// A static non-mesh component (PIE pawn capsule, sprite, arrow) can
		// never contribute triangles. List and skip it; only an explicit
		// IMAcousticRequired tag still fails. Meshes with missing data keep
		// the hard failure so required geometry can never pass silently.
		if(!MeshComponent)
		{ SceneIssues.Add(FString::Printf(TEXT("Excluded non-mesh component: %s"),*Primitive->GetPathName()));if(Required)Failed=true;continue; }
		UStaticMesh* Mesh=MeshComponent->GetStaticMesh();
#if WITH_EDITOR
		// GetRenderData can finish asynchronous compilation by pumping GT work.
		// Do not enter that path while borrowing earlier render-buffer addresses
		// in this capture. The next Editor check reads the rebuilt data afresh.
		if(Mesh&&(Mesh->IsCompiling()||Mesh->HasAnyDependenciesCompiling()))
		{ SceneIssues.Add(FString::Printf(TEXT("REQUIRED mesh compilation pending; retry scene check: %s"),*Mesh->GetPathName()));Failed=true;continue; }
#endif
		if(!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty())
		{ SceneIssues.Add(FString::Printf(TEXT("REQUIRED geometry unsupported: %s"),*Primitive->GetPathName()));Failed=true;continue; }
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
		++IMObsMeshComponents;
		if(!IMObsSeenMeshes.Contains(Mesh))
		{
			IMObsSeenMeshes.Add(Mesh);
			++IMObsUniqueMeshes;
		}
		const double IMMeshComponentStarted=FPlatformTime::Seconds();
		const double IMMeshLODStarted=IMMeshComponentStarted;
		double IMThisMeshLODHashSeconds=0.0,IMThisMeshInstanceExpansionSeconds=0.0;
#endif
		const auto& LOD=Mesh->GetRenderData()->LODResources[0];
		const auto& Positions=LOD.VertexBuffers.PositionVertexBuffer;
		if(Positions.GetNumVertices()==0||!Positions.GetVertexData()||LOD.IndexBuffer.GetArrayView().Num()==0)
		{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			const double IMMeshComponentElapsed=FPlatformTime::Seconds()-IMMeshComponentStarted;
			IMThisMeshLODHashSeconds=IMMeshComponentElapsed;
			IMObsMeshLODHashSeconds+=IMThisMeshLODHashSeconds;
			IMObsMeshComponentSeconds+=IMMeshComponentElapsed;
			IMObsMeshVisitCullBookkeepingSeconds+=FMath::Max(0.0,IMMeshComponentElapsed-IMThisMeshLODHashSeconds);
#endif
			SceneIssues.Add(FString::Printf(TEXT("REQUIRED mesh CPU geometry unavailable: %s"),*Mesh->GetPathName()));Failed=true;continue;
		}
		TUniquePtr<IMAcousticBakeVolumePrivate::FIMMeshLocalReadGeometry> UncachedMeshLocalGeometry;
		const auto& MeshLocalGeometry=MeshLocalReadCache.Read(Positions,LOD.IndexBuffer,UncachedMeshLocalGeometry);
		if(MaterialTable.Physical)
		{
			IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,Grass?Grass->ProducerIdentity:IMStableBakeObjectPath(Primitive));
			IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,IMStableBakeObjectPath(Mesh));
			// NonSolid topology remains in the identity. Bulk hashing feeds the
			// exact legacy float3/normalized-index stream for each component.
			IMAcousticBakeVolumePrivate::IMHashMeshLocalSource(SourceIdentity,MeshLocalGeometry);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			IMObsMeshPositionHashes+=MeshLocalGeometry.SourcePositions.size();
			IMObsMeshIndexHashes+=MeshLocalGeometry.Indices.size();
#endif
		}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
		IMThisMeshLODHashSeconds=FPlatformTime::Seconds()-IMMeshLODStarted;
		IMObsMeshLODHashSeconds+=IMThisMeshLODHashSeconds;
#endif
		UInstancedStaticMeshComponent* Instances=Cast<UInstancedStaticMeshComponent>(MeshComponent);
		const USplineMeshComponent* Spline=Cast<USplineMeshComponent>(MeshComponent);
		const int32 Count=Grass?Grass->RegionInstances.Num():(Instances?Instances->GetInstanceCount():1);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
		if(Instances)++IMObsInstancedMeshComponents;
#endif
		const int32 ActorStart=static_cast<int32>(Runtime->Geometry.Triangles.size());
		int32 OutsideRegionInstances=0;
		for(int32 Instance=0;Instance<Count;++Instance)
		{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			if(Instances)++IMObsMeshInstanceVisits;
#endif
			FTransform Transform=MeshComponent->GetComponentTransform();
			const int32 MaterialInstance=Grass?Grass->RegionInstances[Instance].RenderIndex:Instance;
			if(Grass)Transform=Grass->RegionInstances[Instance].WorldTransform;
			if(!Grass&&Instances&&!Instances->GetInstanceTransform(Instance,Transform,true))
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				++IMObsMeshTransformFailures;
#endif
				SceneIssues.Add(TEXT("REQUIRED instance transform unavailable."));Failed=true;continue;
			}
			if(Transform.ContainsNaN()||FMath::IsNearlyZero(Transform.GetDeterminant()))
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				++IMObsMeshInvalidTransforms;
#endif
				SceneIssues.Add(TEXT("REQUIRED instance has invalid/singular transform."));Failed=true;continue;
			}
			// A component-wide ISM/HISM bound may cross this region while many
			// of its instances do not. Match the independent SM inclusion rule
			// before expanding LOD0 geometry, preserving each included pose.
			if(!Grass&&Instances&&!Region.Intersect(Mesh->GetBounds().TransformBy(Transform).GetBox()))
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				++IMObsMeshInstancesOutside;
#endif
				++OutsideRegionInstances;continue;
			}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			++IMObsMeshInstancesIncluded;
			const double IMMeshInstanceExpansionStarted=FPlatformTime::Seconds();
#endif
			if(MaterialTable.Physical)
			{
				SourceIdentity.Update(reinterpret_cast<const uint8*>(&Instance),sizeof(Instance));
				const FVector Location=Transform.GetLocation(),Scale=Transform.GetScale3D();
				const FQuat Rotation=Transform.GetRotation();
				SourceIdentity.Update(reinterpret_cast<const uint8*>(&Location),sizeof(Location));
				SourceIdentity.Update(reinterpret_cast<const uint8*>(&Scale),sizeof(Scale));
				SourceIdentity.Update(reinterpret_cast<const uint8*>(&Rotation),sizeof(Rotation));
			}
			const int32 InstanceStart=static_cast<int32>(Runtime->Geometry.Triangles.size());
			const int32 Base=static_cast<int32>(Runtime->Geometry.Vertices.size());
			bool bInvalidSplineVertex=false;
			// Each included pose still contributes its own local-position byte
			// stream. Spline deformation/invalid data keep the scalar path so
			// order and failure behavior match the original capture.
			const bool BulkLocalIdentity=MaterialTable.Physical&&!Spline&&MeshLocalGeometry.AllLocalPositionsFinite;
			if(BulkLocalIdentity)IMAcousticBakeVolumePrivate::IMHashMeshLocalPositions(SourceIdentity,MeshLocalGeometry);
			for(uint32 V=0;V<Positions.GetNumVertices();++V)
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				++IMObsMeshVertices;
				if(Spline)++IMObsMeshSplineVertices;
#endif
				FVector LocalPosition=MeshLocalGeometry.LocalPositions[V];
				if(Spline)
				{
					// Match UE's PropagateSplineDeformationToMesh: the source
					// forward-axis coordinate selects a slice, then becomes
					// zero before the slice transform is applied. This keeps
					// acoustic triangles on the rendered bent surface.
					const ESplineMeshAxis::Type Axis=Spline->GetForwardAxis();
					const float Distance=float(USplineMeshComponent::GetAxisValueRef(LocalPosition,Axis));
					USplineMeshComponent::GetAxisValueRef(LocalPosition,Axis)=0.0;
					LocalPosition=Spline->CalcSliceTransform(Distance).TransformPosition(LocalPosition);
				}
				if(LocalPosition.ContainsNaN()){bInvalidSplineVertex=true;break;}
				if(MaterialTable.Physical&&!BulkLocalIdentity)SourceIdentity.Update(reinterpret_cast<const uint8*>(&LocalPosition),sizeof(LocalPosition));
				Runtime->Geometry.Vertices.push_back(ToSDKPosition(Transform.TransformPosition(LocalPosition),Runtime->Origin));
			}
			if(bInvalidSplineVertex)
			{
				Runtime->Geometry.Vertices.resize(Base);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				++IMObsMeshInvalidSplineInstances;
				const double IMExpansionElapsed=FPlatformTime::Seconds()-IMMeshInstanceExpansionStarted;
				IMObsMeshInstanceExpansionSeconds+=IMExpansionElapsed;
				IMThisMeshInstanceExpansionSeconds+=IMExpansionElapsed;
#endif
				SceneIssues.Add(FString::Printf(TEXT("REQUIRED spline deformation invalid: %s"),*Primitive->GetPathName()));
				Failed=true;
				continue;
			}
			// UE faces wind CW; IMToSDKPosition (Y,Z,-X, det -1) already converts
			// to SDK-CCW-outward, so swap only when the instance itself mirrors
			// (det<0). Swapping det>0 wound static geometry inward and conducted
			// path audio through sealed walls (inc109; wall-ray pv verdict).
			const bool Flip=Transform.GetDeterminant()<0;
			for(const auto& Section:LOD.Sections)
			{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
				++IMObsMeshSectionResolves;
#endif
				const int32 Material=MaterialTable.ResolveMesh(MeshComponent,Section.MaterialIndex,MaterialInstance);
				if(Material==INDEX_NONE)
				{ SceneIssues.Add(FString::Printf(TEXT("REQUIRED acoustic %s missing: %s section %d"),MaterialTable.Physical?TEXT("physical profile"):TEXT("legacy material"),*Primitive->GetPathName(),Section.MaterialIndex));Failed=true;continue; }
				if(MaterialTable.Physical)
				{
					SourceIdentity.Update(reinterpret_cast<const uint8*>(&Section.MaterialIndex),sizeof(Section.MaterialIndex));
					SourceIdentity.Update(reinterpret_cast<const uint8*>(&Material),sizeof(Material));
					IMAcousticBakeVolumePrivate::IMHashMaterialIdentity(SourceIdentity,IMStableBakeObjectPath(MeshComponent->GetMaterial(Section.MaterialIndex)));
				}
				IMAcousticBakeVolumePrivate::FIMGeometrySectionContribution* Contribution=nullptr;
				if(bDetailedGeometryLog)
				{
					const int32 SectionIndex=int32(&Section-LOD.Sections.GetData());
					Contribution=IMSectionContributions.FindByPredicate([&](const auto& Row)
					{return Row.Component==Primitive&&Row.SectionIndex==SectionIndex&&Row.ProfileIndex==Material;});
					if(!Contribution)
					{
						auto& Row=IMSectionContributions.AddDefaulted_GetRef();
						Row.Component=Primitive;
						Row.ComponentPath=Primitive->GetPathName();
						Row.ProducerIdentity=Grass?Grass->ProducerIdentity:IMStableBakeObjectPath(Primitive);
						Row.MeshAssetPath=IMStableBakeObjectPath(Mesh);
						Row.RenderMaterialPath=IMStableBakeObjectPath(MeshComponent->GetMaterial(Section.MaterialIndex));
						Row.SectionIndex=SectionIndex;Row.MaterialSlot=Section.MaterialIndex;Row.ProfileIndex=Material;
						Contribution=&Row;
					}
					++Contribution->IncludedInstances;
				}
				if(!MaterialTable.Solid[Material])
				{
					++NonSolidSections;NonSolidTriangles+=int32(Section.NumTriangles);
					if(Contribution)Contribution->NonSolidTriangles+=Section.NumTriangles;
					continue;
				}
				const size_t SectionTriangleStart=Contribution?Runtime->Geometry.Triangles.size():0;
				for(uint32 T=0;T<Section.NumTriangles;++T)
				{
					IPLTriangle Triangle{};
					bool ValidIndices=true;
					for(uint32 K=0;K<3;++K)
					{
						const uint64 IndexAddress=uint64(Section.FirstIndex)+uint64(T)*3+K;
						if(IndexAddress>=MeshLocalGeometry.Indices.size()){ValidIndices=false;break;}
						const uint32 Index=MeshLocalGeometry.Indices[size_t(IndexAddress)];
						if(Index>=Positions.GetNumVertices()){ValidIndices=false;break;}
						Triangle.indices[K]=Base+int32(Index);
					}
					if(!ValidIndices){Failed=true;SceneIssues.Add(TEXT("REQUIRED mesh index exceeds index/vertex buffer."));continue;}
					if(Flip)std::swap(Triangle.indices[1],Triangle.indices[2]);
					Runtime->Geometry.Triangles.push_back(Triangle);Runtime->Geometry.MaterialIndices.push_back(Material);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
					++IMObsMeshTrianglesEmitted;
#endif
				}
				if(Contribution)Contribution->SolidTriangles+=Runtime->Geometry.Triangles.size()-SectionTriangleStart;
			}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
			const double IMExpansionElapsed=FPlatformTime::Seconds()-IMMeshInstanceExpansionStarted;
			IMObsMeshInstanceExpansionSeconds+=IMExpansionElapsed;
			IMThisMeshInstanceExpansionSeconds+=IMExpansionElapsed;
#endif
			const int32 InstanceEnd=static_cast<int32>(Runtime->Geometry.Triangles.size());
			if(InstanceEnd>InstanceStart)
			{
				Runtime->GeometryTriangleRanges.Add(TPair<int32,int32>(InstanceStart,InstanceEnd));
				ActorRanges.Add(TPair<int32,FString>(InstanceStart,Primitive->GetPathName()));
			}
			// Bounds serve detailed logs only; do not walk every exported
			// triangle for them during ordinary captures/invalidation checks.
			if(bDetailedGeometryLog)
			{
				IPLVector3 Mn{1e30f,1e30f,1e30f},Mx{-1e30f,-1e30f,-1e30f};
				for(int32 TI=InstanceStart;TI<InstanceEnd;++TI)
				{
					const IPLTriangle& TrLocal=Runtime->Geometry.Triangles[TI];
					const int32 IdxLocal[3]={TrLocal.indices[0],TrLocal.indices[1],TrLocal.indices[2]};
					for(int32 KI=0;KI<3;++KI)
					{
						const IPLVector3& VLocal=Runtime->Geometry.Vertices[IdxLocal[KI]];
						Mn.x=FMath::Min(Mn.x,VLocal.x);Mn.y=FMath::Min(Mn.y,VLocal.y);Mn.z=FMath::Min(Mn.z,VLocal.z);
						Mx.x=FMath::Max(Mx.x,VLocal.x);Mx.y=FMath::Max(Mx.y,VLocal.y);Mx.z=FMath::Max(Mx.z,VLocal.z);
					}
				}
				if(bDetailedGeometryLog)
					UE_LOG(LogTemp,Verbose,TEXT("IMLogs AcousticBakeBounds sdkmin=(%.3f,%.3f,%.3f) sdkmax=(%.3f,%.3f,%.3f) %s"),Mn.x,Mn.y,Mn.z,Mx.x,Mx.y,Mx.z,*Primitive->GetPathName());
			}
		}
		if(OutsideRegionInstances>0)
			SceneIssues.Add(FString::Printf(TEXT("Excluded %d static instances outside bake bounds: %s"),OutsideRegionInstances,*Primitive->GetPathName()));
		ActorTris.Add(Primitive->GetPathName(),static_cast<int32>(Runtime->Geometry.Triangles.size())-ActorStart);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
		const double IMMeshComponentElapsed=FPlatformTime::Seconds()-IMMeshComponentStarted;
		IMObsMeshComponentSeconds+=IMMeshComponentElapsed;
		IMObsMeshVisitCullBookkeepingSeconds+=FMath::Max(0.0,
			IMMeshComponentElapsed-IMThisMeshLODHashSeconds-IMThisMeshInstanceExpansionSeconds);
#endif
	}
	ExportedTriangles=static_cast<int32>(Runtime->Geometry.Triangles.size());
	if(MaterialTable.Physical&&!Failed)
	{
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
		const double IMCompactionStarted=FPlatformTime::Seconds();
		++IMObsCompactionCalls;
		IMObsCompactionVerticesIn+=uint64(Runtime->Geometry.Vertices.size());
		IMObsCompactionTriangles+=uint64(Runtime->Geometry.Triangles.size());
#endif
		IMAcousticBakeVolumePrivate::IMCompactAcousticVertices(Runtime->Geometry);
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
		IMObsCompactionSeconds+=FPlatformTime::Seconds()-IMCompactionStarted;
		IMObsCompactionVerticesOut+=uint64(Runtime->Geometry.Vertices.size());
#endif
		SceneIssues.Add(FString::Printf(TEXT("PhysicalMaterial drive: explicit NonSolid vegetation sections=%d triangles=%d; weak leaf scattering is not simulated."),NonSolidSections,NonSolidTriangles));
	}
	if(bDetailedGeometryLog)
	{
		for(const auto& KVLocal:ActorTris)UE_LOG(LogTemp,Verbose,TEXT("IMLogs AcousticBakeActor tris=%d %s"),KVLocal.Value,*KVLocal.Key);
		for(UPrimitiveComponent* Prim:Components)if(ActorTris.Contains(Prim->GetPathName()))UE_LOG(LogTemp,Verbose,TEXT("IMLogs AcousticBakePlace loc=%s scale=%s mob=%d %s"),*Prim->GetComponentLocation().ToString(),*Prim->GetComponentScale().ToString(),int(Prim->Mobility),*Prim->GetPathName());
	}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	const double IMExportEnded=FPlatformTime::Seconds();
#endif
	// These fixed whitebox probes diagnose exported geometry; none of their
	// computed values participate in capture, identity, support or simulation.
	if(bDetailedGeometryLog)
	{
		// Segment-piercing test: the exact hole-axis segment the sweep measures.
		// Lists baked tris the ray must cross (or total=0 = baked data truly open).
		const FVector SegA(-2.0f,0.0f,1.5f),SegB(2.0f,0.0f,1.5f);
		int32 SegHits=0;
		for(int32 RI=0;RI<ActorRanges.Num()&&SegHits<20;++RI)
		{
			const int32 StartLocal=ActorRanges[RI].Key;
			const int32 EndLocal=(RI+1<ActorRanges.Num())?ActorRanges[RI+1].Key:Runtime->Geometry.Triangles.size();
			for(int32 TI=StartLocal;TI<EndLocal;++TI)
			{
				const IPLTriangle& PT=Runtime->Geometry.Triangles[TI];
				const IPLVector3& PA=Runtime->Geometry.Vertices[PT.indices[0]];
				const IPLVector3& PB=Runtime->Geometry.Vertices[PT.indices[1]];
				const IPLVector3& PC=Runtime->Geometry.Vertices[PT.indices[2]];
				const FVector VA(PA.x,PA.y,PA.z),VB(PB.x,PB.y,PB.z),VC(PC.x,PC.y,PC.z);
				const FVector DLocal=SegB-SegA,E1Local=VB-VA,E2Local=VC-VA;
				const FVector PLocal=FVector::CrossProduct(DLocal,E2Local);
				const double DetLocal=FVector::DotProduct(E1Local,PLocal);
				bool Pierce=false;FVector HitLocal(0,0,0);
				if(!FMath::IsNearlyZero(DetLocal))
				{
					const double InvLocal=1.0/DetLocal;
					const FVector TLocal=SegA-VA;
					const double ULocal=FVector::DotProduct(TLocal,PLocal)*InvLocal;
					if(ULocal>=-1e-6&&ULocal<=1.0+1e-6)
					{
						const FVector QLocal=FVector::CrossProduct(TLocal,E1Local);
						const double VLocal=FVector::DotProduct(DLocal,QLocal)*InvLocal;
						if(VLocal>=-1e-6&&ULocal+VLocal<=1.0+1e-6)
						{
							const double Tp=FVector::DotProduct(E2Local,QLocal)*InvLocal;
							if(Tp>=-1e-6&&Tp<=1.0+1e-6){Pierce=true;HitLocal=SegA+DLocal*float(Tp);}
						}
					}
				}
				if(Pierce)
				{
					++SegHits;
					UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakePierce tri=%d hit=(%.3f,%.3f,%.3f) %s"),TI,HitLocal.X,HitLocal.Y,HitLocal.Z,*ActorRanges[RI].Value);
				}
			}
		}
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakePierce totalhits=%d"),SegHits);
		int32 HoleDumped=0,HoleTotal=0;
		for(int32 RI=0;RI<ActorRanges.Num()&&HoleDumped<60;++RI)
		{
			const int32 StartLocal=ActorRanges[RI].Key;
			const int32 EndLocal=(RI+1<ActorRanges.Num())?ActorRanges[RI+1].Key:Runtime->Geometry.Triangles.size();
			for(int32 TI=StartLocal;TI<EndLocal;++TI)
			{
				const IPLTriangle& TLocal=Runtime->Geometry.Triangles[TI];
				const IPLVector3& A=Runtime->Geometry.Vertices[TLocal.indices[0]];
				const IPLVector3& B=Runtime->Geometry.Vertices[TLocal.indices[1]];
				const IPLVector3& C=Runtime->Geometry.Vertices[TLocal.indices[2]];
				const float CX=(A.x+B.x+C.x)/3, CY=(A.y+B.y+C.y)/3, CZ=(A.z+B.z+C.z)/3;
				if(CX>-0.6f&&CX<0.6f&&CY>-0.6f&&CY<0.6f&&CZ>0.5f&&CZ<2.5f)
				{
					++HoleTotal;
					if(HoleDumped<60)
					{
						++HoleDumped;
						UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeHole tri=%d c=(%.3f,%.3f,%.3f) v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f) %s"),TI,CX,CY,CZ,A.x,A.y,A.z,B.x,B.y,B.z,C.x,C.y,C.z,*ActorRanges[RI].Value);
					}
				}
			}
		}
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeHole total=%d"),HoleTotal);
	}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	const double IMDiagnosticsEnded=FPlatformTime::Seconds();
#endif
	if(Failed||ExportedTriangles==0)
	{ Status=TEXT("Scene check failed: required geometry/material incomplete. See Scene Issues.");return false; }
	const IPLVector3 Min=ToSDKPosition(Region.Min,Runtime->Origin),Max=ToSDKPosition(Region.Max,Runtime->Origin);
	auto& Params=Runtime->ProbeParams;Params={};Params.type=IPL_PROBEGENERATIONTYPE_UNIFORMFLOOR;
	Params.spacing=ProbeSpacingCm*.01f;Params.height=ProbeHeightCm*.01f;
	const float Lo[3]={FMath::Min(Min.x,Max.x),FMath::Min(Min.y,Max.y),FMath::Min(Min.z,Max.z)};
	const float Hi[3]={FMath::Max(Min.x,Max.x),FMath::Max(Min.y,Max.y),FMath::Max(Min.z,Max.z)};
	// Pinned SDK 4.8.1 probe_generator.cpp samples [-.5,.5], despite phonon.h
	// documenting [0,1]. Translation must be the center: using Min generated
	// only the negative half-room and left the listener without a path.
	for(int I=0;I<3;++I){Params.transform.elements[I][I]=Hi[I]-Lo[I];Params.transform.elements[I][3]=(Lo[I]+Hi[I])*.5f;}
	Params.transform.elements[3][3]=1;
	FSHA1 Hash;
	auto Feed=[&Hash](const auto& Values)
	{
		const uint64 Count=Values.size();Hash.Update(reinterpret_cast<const uint8*>(&Count),sizeof(Count));
		if(Count)Hash.Update(reinterpret_cast<const uint8*>(Values.data()),uint32(Count*sizeof(Values[0])));
	};
	Feed(Runtime->Geometry.Vertices);Feed(Runtime->Geometry.Triangles);Feed(Runtime->Geometry.MaterialIndices);Feed(Runtime->Geometry.Materials);
	if(MaterialTable.Physical)
	{
		SourceIdentity.Final();uint8 SourceDigest[20];SourceIdentity.GetHash(SourceDigest);
		Hash.Update(SourceDigest,sizeof(SourceDigest));
	}
	Hash.Update(reinterpret_cast<const uint8*>(&Params),sizeof(Params));
	const int RecipeVersion=IMAcousticRecipe::Version;Hash.Update(reinterpret_cast<const uint8*>(&RecipeVersion),sizeof(RecipeVersion));
	for(int I=0;I<3;++I){const double V=Runtime->Origin[I];Hash.Update(reinterpret_cast<const uint8*>(&V),sizeof(V));}
	Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);Runtime->Fingerprint=BytesToHex(Digest,20);
	if(Runtime->ProbeFingerprint==Runtime->Fingerprint)Runtime->Geometry.Probes=MoveTemp(PreviousProbes);
	else Runtime->ProbeFingerprint.Empty();
	GeneratedProbes=static_cast<int32>(Runtime->Geometry.Probes.size());
	Runtime->SceneValid=true;
	if(bDetailedGeometryLog)
		IMAcousticBakeVolumePrivate::IMLogSectionContributions(IMSectionContributions,MaterialTable,IMContributionCaptureId,
			GetWorld()->GetOutermost()->GetName(),Runtime->Fingerprint,ExportedTriangles,NonSolidTriangles);
	Status=MaterialTable.Physical
		?FString::Printf(TEXT("Scene check passed: PhysicalMaterial; NonSolid sections=%d triangles=%d; exclusions listed explicitly."),NonSolidSections,NonSolidTriangles)
		:TEXT("Scene check passed; exclusions are listed explicitly.");
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
	const double IMFingerprintEnded=FPlatformTime::Seconds();
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticMeshLocalReadCache hits=%llu misses=%llu evictions=%llu oversized_reads=%llu retained_payload_bytes=%llu peak_retained_payload_bytes=%llu payload_budget_bytes=%llu lifetime=single_capture"),
		MeshLocalReadCache.Hits,MeshLocalReadCache.Misses,MeshLocalReadCache.Evictions,MeshLocalReadCache.OversizedReads,
		MeshLocalReadCache.RetainedPayloadBytes,MeshLocalReadCache.PeakRetainedPayloadBytes,IMAcousticBakeVolumePrivate::IMMeshLocalCachePayloadBudgetBytes);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticCaptureCorePhases landscape_blocks=%llu height_samples=%llu material_cells=%llu landscape_read_ms=%.3f validation_cells=%llu cell_visits=%llu visibility_holes=%llu region_culled=%llu cells_included=%llu cell_validate_clip_expand_ms=%.3f mesh_components=%llu unique_meshes=%llu lod_positions_hashed=%llu lod_indices_hashed=%llu mesh_lod_hash_ms=%.3f instanced_components=%llu ism_hism_visits=%llu transform_unavailable=%llu invalid_transforms=%llu instances_outside=%llu mesh_expansions_included=%llu invalid_spline_expansions=%llu instance_vertices=%llu spline_vertices=%llu section_pm_resolves=%llu triangles_emitted=%llu included_instance_expand_ms=%.3f mesh_component_total_ms=%.3f visit_cull_bookkeeping_ms=%.3f compaction_calls=%llu compaction_vertices_in=%llu compaction_vertices_out=%llu compaction_triangles=%llu compaction_ms=%.3f"),
		IMObsLandscapeBlocks,IMObsLandscapeHeightSamples,IMObsLandscapeMaterialCells,1000.0*IMObsLandscapeReadSeconds,
		IMObsLandscapeValidationCells,IMObsLandscapeCellsVisited,IMObsLandscapeVisibilityHoles,IMObsLandscapeRegionCulled,
		IMObsLandscapeCellsIncluded,1000.0*IMObsLandscapeCellWorkSeconds,IMObsMeshComponents,IMObsUniqueMeshes,
		IMObsMeshPositionHashes,IMObsMeshIndexHashes,1000.0*IMObsMeshLODHashSeconds,IMObsInstancedMeshComponents,IMObsMeshInstanceVisits,
		IMObsMeshTransformFailures,IMObsMeshInvalidTransforms,IMObsMeshInstancesOutside,IMObsMeshInstancesIncluded,IMObsMeshInvalidSplineInstances,
		IMObsMeshVertices,IMObsMeshSplineVertices,IMObsMeshSectionResolves,IMObsMeshTrianglesEmitted,
		1000.0*IMObsMeshInstanceExpansionSeconds,1000.0*IMObsMeshComponentSeconds,
		1000.0*IMObsMeshVisitCullBookkeepingSeconds,IMObsCompactionCalls,IMObsCompactionVerticesIn,
		IMObsCompactionVerticesOut,IMObsCompactionTriangles,1000.0*IMObsCompactionSeconds);
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticCapturePhases components=%d triangles=%d enumerate_ms=%.3f sort_ms=%.3f gather_ms=%.3f export_ms=%.3f diagnostics_ms=%.3f fingerprint_ms=%.3f total_ms=%.3f"),
		Components.Num(),ExportedTriangles,1000.0*(IMEnumerationEnded-IMCaptureStarted),
		1000.0*(IMGatherEnded-IMEnumerationEnded),1000.0*(IMGatherEnded-IMCaptureStarted),
		1000.0*(IMExportEnded-IMGatherEnded),1000.0*(IMDiagnosticsEnded-IMExportEnded),
		1000.0*(IMFingerprintEnded-IMDiagnosticsEnded),1000.0*(IMFingerprintEnded-IMCaptureStarted));
#endif
	return true;
}

void AIMAcousticBakeVolume::InspectScene(){CaptureScene();}
#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
bool AIMAcousticBakeVolume::IM_CaptureDynamicGeometryForTest(UStaticMeshComponent* Component,
	int32& OutVertexCount, int32& OutTriangleCount, FString& Failure)
{
	check(IsInGameThread());
	OutVertexCount=0;OutTriangleCount=0;Failure.Empty();
	IMAcousticBakeVolumePrivate::FIMAcousticMaterialTable MaterialTable;
	if(!MaterialTable.Build(*this,Failure))return false;
	// Match Tick's fresh per-blocker output; expose failed partial geometry too.
	FIMAcousticDynamicMeshSnapshot Snapshot;
	const bool Captured=IMAcousticBakeVolumePrivate::BuildDynamicMeshSnapshot(Component,0,
		BakeBounds?BakeBounds->GetComponentLocation():GetActorLocation(),MaterialTable,Snapshot,Failure);
	OutVertexCount=Snapshot.Vertices.Num();OutTriangleCount=Snapshot.Triangles.Num();
	return Captured;
}

bool AIMAcousticBakeVolume::IM_CaptureSceneGeometryForTest(TArray<FVector>& OutVertices,
	TArray<FIntVector>& OutTriangles, TArray<int32>& OutMaterialIndices,
	FString& OutFingerprint)
{
	OutVertices.Reset();OutTriangles.Reset();OutMaterialIndices.Reset();OutFingerprint.Empty();
	const double CaptureStartedAt=FPlatformTime::Seconds();
	if(!CaptureScene())return false;
	const FPlatformMemoryStats CaptureMemory=FPlatformMemory::GetStats();
	UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticG2CaptureCore placements=%d triangles=%d elapsed_ms=%.3f working_set_mib=%.3f commit_mib=%.3f"),
		Runtime->GeometryTriangleRanges.Num(),static_cast<int32>(Runtime->Geometry.Triangles.size()),
		(FPlatformTime::Seconds()-CaptureStartedAt)*1000.0,
		CaptureMemory.UsedPhysical/(1024.0*1024.0),CaptureMemory.UsedVirtual/(1024.0*1024.0));
	OutVertices.Reserve(static_cast<int32>(Runtime->Geometry.Vertices.size()));
	OutTriangles.Reserve(static_cast<int32>(Runtime->Geometry.Triangles.size()));
	OutMaterialIndices.Reserve(static_cast<int32>(Runtime->Geometry.MaterialIndices.size()));
	for(const IPLVector3& Vertex:Runtime->Geometry.Vertices)
		OutVertices.Emplace(Vertex.x,Vertex.y,Vertex.z);
	for(const IPLTriangle& Triangle:Runtime->Geometry.Triangles)
		OutTriangles.Emplace(Triangle.indices[0],Triangle.indices[1],Triangle.indices[2]);
	for(const int32 MaterialIndex:Runtime->Geometry.MaterialIndices)
		OutMaterialIndices.Add(MaterialIndex);
	OutFingerprint=Runtime->Fingerprint;
	return true;
}
void AIMAcousticBakeVolume::IM_GetGeneratedProbeCentersForTest(TArray<FVector>& OutSDKCenters) const
{
	OutSDKCenters.Reset();
	for(const auto& Probe:Runtime->Geometry.Probes)
		OutSDKCenters.Emplace(Probe.center.x,Probe.center.y,Probe.center.z);
}
bool AIMAcousticBakeVolume::IM_CancelAndDrainBakeForTest()
{
	check(IsInGameThread());
	if(!Runtime)return true;
	// Evidence owner cleanup must stop the GT publication callback before it
	// restores the level binding. SDK cancellation remains cooperative; join
	// the owned future even when a latent command was abandoned before timeout.
	FTSTicker::GetCoreTicker().RemoveTicker(Runtime->BakeTicker);
	Runtime->BakeTicker.Reset();
	if(Runtime->Job)
	{
		Runtime->Job->Cancelled.store(true,std::memory_order_release);
		Runtime->Job->Future.Wait();
		Runtime->Job.Reset();
	}
	return !Runtime->Job&&!Runtime->BakeTicker.IsValid();
}

void AIMAcousticBakeVolume::IM_SetEditorValidationProfilingForTest(bool bEnable,
	FIMAcousticOperationMemorySampler* MemorySampler)
{
	Runtime->bProfileEditorValidationForTest=bEnable;
	Runtime->EditorValidationMemorySamplerForTest=bEnable?MemorySampler:nullptr;
	Runtime->ProfiledEditorValidationTicks=0;
	Runtime->ProfiledEditorCaptureMs=0.0;
	Runtime->ProfiledEditorAssetValidationMs=0.0;
	Runtime->ProfiledEditorMaxTickMs=0.0;
}
bool AIMAcousticBakeVolume::IM_GetEditorValidationProfileForTest(int32& OutTicks,
	double& OutCaptureMs, double& OutAssetValidationMs, double& OutMaxTickMs) const
{
	OutTicks=Runtime->ProfiledEditorValidationTicks;
	OutCaptureMs=Runtime->ProfiledEditorCaptureMs;
	OutAssetValidationMs=Runtime->ProfiledEditorAssetValidationMs;
	OutMaxTickMs=Runtime->ProfiledEditorMaxTickMs;
	return Runtime->bProfileEditorValidationForTest && OutTicks>0;
}
#endif
bool AIMAcousticBakeVolume::ValidateCurrentBake(FString& Failure)
{
	IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("ValidateCurrentBakeEntry"));
	if(!CaptureScene()){Failure=Status;return false;}
	if(!BakedField){Failure=TEXT("Missing acoustic bake asset.");return false;}
	return BakedField->Validate(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,Failure);
}
void AIMAcousticBakeVolume::GenerateProbes()
{
	if(Runtime->Job){Status=TEXT("Wait for the current bake or cancel it.");return;}
	if(!CaptureScene())return;
	FIMAcousticSimulation Simulation;std::string Error;
	FIMAcousticSceneInput SupportGeometry;
	const FIMAcousticSceneInput* ProbeGeometry=&Runtime->Geometry;
	if(MaterialDrive==EIMAcousticMaterialDrive::PhysicalMaterial)
	{
		SupportGeometry.Vertices=Runtime->Geometry.Vertices;
		SupportGeometry.Materials=Runtime->Geometry.Materials;
		for(size_t Index=0;Index<Runtime->Geometry.Triangles.size();++Index)
		{
			const int32 Material=Runtime->Geometry.MaterialIndices[Index];
			if(Runtime->ProbeSupportMaterials[Material])
			{SupportGeometry.Triangles.push_back(Runtime->Geometry.Triangles[Index]);SupportGeometry.MaterialIndices.push_back(Material);}
		}
		if(SupportGeometry.Triangles.empty()){Status=TEXT("No physical profile supports probe placement.");return;}
		IMAcousticBakeVolumePrivate::IMCompactAcousticVertices(SupportGeometry);
		ProbeGeometry=&SupportGeometry;
	}
	if(!Simulation.GenerateProbes(*ProbeGeometry,Runtime->ProbeParams,Runtime->Geometry.Probes,Error))
	{Status=UTF8_TO_TCHAR(Error.c_str());return;}
	GeneratedProbes=static_cast<int32>(Runtime->Geometry.Probes.size());
	Runtime->ProbeFingerprint=Runtime->Fingerprint;Status=TEXT("Probes generated; inspect coverage before baking.");
	ShowProbeCoverage();
}
void AIMAcousticBakeVolume::ShowProbeCoverage()
{
	// Enables the persistent selection overlay below. The previous one-shot draw
	// used radius-1m spheres for 10 seconds; 632 of them overlap into one blob and
	// expire before they can be inspected, which is the "cannot tell what is
	// baked" problem this replaces.
	bShowBakedFieldOverlay=true;
	if(Runtime->Geometry.Probes.empty()&&BakedField)
	{
		FString Error;
		if(!CaptureScene()||!BakedField->Validate(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,Error))
		{Status=Error.IsEmpty()?TEXT("Cannot show coverage: scene is invalid."):Error;return;}
		Status=TEXT("Baked-field overlay on; the bound bake is valid for the current scene.");return;
	}
	Status=FString::Printf(TEXT("Baked-field overlay on: %d generated probes%s."),int32(Runtime->Geometry.Probes.size()),
		BakedField?TEXT(" (a bake asset is bound; the overlay shows the bake)"):TEXT(" (not baked yet)"));
}
// Editor-only overlay: while this actor is selected (or bShowBakedFieldOverlay is
// on) the viewport shows the configured bake bounds (green), the probe extent with
// one probe radius included (orange) and every probe as a dot (cyan = read from the
// bound bake asset, yellow = generated this session but not baked). Answers
// "which areas are covered" without guessing; no runtime effect.
void AIMAcousticBakeVolume::DrawEditorFieldOverlay()
{
#if WITH_EDITOR
	if(!bShowBakedFieldOverlay&&!IsSelectedInEditor())return;
	UWorld* World=GetWorld();if(!World)return;
	const FVector BoundsCenter=BakeBounds?BakeBounds->GetComponentLocation():GetActorLocation();
	const FVector BoundsExtent=BakeBounds?BakeBounds->GetScaledBoxExtent():FVector(500,500,250);
	if(BakeBounds)
		DrawDebugBox(World,BoundsCenter,BoundsExtent,BakeBounds->GetComponentQuat(),FColor(120,255,140),false,-1.f,SDPG_Foreground,2.f);

	// Precedence mirrors ShowProbeCoverage: probes generated in this session win
	// over the bound asset's list. The asset branch reads the asset's own recorded
	// world/fingerprint, so the overlay shows what WAS baked and reports staleness
	// separately instead of hiding the data.
	TArray<FVector> Points;bool bFromAsset=false;
	if(!Runtime->Geometry.Probes.empty())
	{
		Points.Reserve(int32(Runtime->Geometry.Probes.size()));
		for(const auto& P:Runtime->Geometry.Probes)Points.Add(FromSDKPosition(P.center,Runtime->Origin));
	}
	else if(BakedField)
	{
		if(OverlayAsset.Get()!=BakedField)
		{
			OverlayAsset=BakedField;OverlayAssetProbes.Reset();OverlayAssetOrigin=FVector::ZeroVector;
			FString Error;
			if(!BakedField->GetProbePreview(BakedField->WorldPackage,BakedField->SceneFingerprint,OverlayAssetProbes,OverlayAssetOrigin,Error))
				UE_LOG(LogTemp,Warning,TEXT("IMLogs AcousticOverlay probe preview unavailable: %s"),*Error);
		}
		Points.Reserve(OverlayAssetProbes.Num());
		for(const FVector4& P:OverlayAssetProbes)Points.Add(OverlayAssetOrigin+FVector(-P.Z,P.X,P.Y)*100.0);
		bFromAsset=true;
	}
	if(!Points.IsEmpty())
	{
		FBox Covered(ForceInit);
		for(const FVector& P:Points)Covered+=P;
		Covered=Covered.ExpandBy(FVector(100)); // one probe radius, cm
		DrawDebugBox(World,Covered.GetCenter(),Covered.GetExtent(),FColor(255,170,60),false,-1.f,SDPG_Foreground,1.f);
		const FColor ProbeColor=bFromAsset?FColor(0,220,255):FColor(255,220,0);
		for(const FVector& P:Points)DrawDebugPoint(World,P,12.f,ProbeColor,false,-1.f,SDPG_Foreground);
	}
	const bool bStale=BakedField&&!Runtime->Fingerprint.IsEmpty()&&BakedField->SceneFingerprint!=Runtime->Fingerprint;
	const FString Summary=FString::Printf(TEXT("Overlay: %d probes, spacing %.0fcm, height %.0fcm, %s%s. green=bake bounds, orange=probe extent (+1m radius), %s=probes."),
		Points.Num(),ProbeSpacingCm,ProbeHeightCm,
		bFromAsset?TEXT("read from the bound bake asset"):(Points.IsEmpty()?TEXT("no probes yet: run GenerateProbes / Bake"):TEXT("generated this session, NOT baked yet")),
		bStale?TEXT("; STALE: scene changed since the bake, rebake required"):TEXT(""),
		bFromAsset?TEXT("cyan"):TEXT("yellow"));
	if(OverlaySummary!=Summary){OverlaySummary=Summary;UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticOverlay %s"),*Summary);}
#endif
}
void AIMAcousticBakeVolume::Bake()
{
#if WITH_EDITOR
	if(GetWorld()->IsGameWorld()||Runtime->Job)return;
	if(!CaptureScene())return;
	if(Runtime->Geometry.Probes.empty()||Runtime->ProbeFingerprint!=Runtime->Fingerprint)
	{Status=TEXT("Generate probes again: scene or probe configuration changed.");return;}
	auto Job=MakeShared<IMAcousticBakeVolumePrivate::FIMBakeJob,ESPMode::ThreadSafe>();Runtime->Job=Job;
	Job->Fingerprint=Runtime->Fingerprint;Job->WorldPackage=UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName());
	Job->MetadataJson=IMAcousticBakeVolumePrivate::BakeMetadata(*this,Runtime->Geometry,Runtime->Origin);
	auto Input=Runtime->Geometry;
	Job->Future=Async(EAsyncExecution::Thread,[Job,Input=MoveTemp(Input)]()
	{
		FIMAcousticSimulation Simulation;
		Job->Success=Simulation.Bake(Input,Job->Data,Job->Error,&Job->Cancelled);
		Job->Done.store(true,std::memory_order_release);
	});
	// Editor actor ticks depend on viewport realtime. Completion must also work
	// with a paused viewport, so a weak GT ticker owns only the pending bake.
	TWeakObjectPtr<AIMAcousticBakeVolume> WeakOwner(this);
	TWeakPtr<IMAcousticBakeVolumePrivate::FIMBakeJob,ESPMode::ThreadSafe> WeakJob(Job);
	Runtime->BakeTicker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakOwner,WeakJob](float)
		{
			auto* Owner=WeakOwner.Get();auto Expected=WeakJob.Pin();
			if(!Owner||!Expected||Owner->Runtime->Job!=Expected)return false;
			Owner->PollBakeCompletion();return Owner->Runtime->Job==Expected;
		}),.05f);
	Status=TEXT("Baking. Cancellation waits for the current SDK pass; previous asset is preserved.");
#endif
}
void AIMAcousticBakeVolume::CancelBake()
{
	if(Runtime->Job){Runtime->Job->Cancelled.store(true);Status=TEXT("Cancellation requested; waiting for current SDK pass.");}
}

void AIMAcousticBakeVolume::BeginPlay()
{
	Super::BeginPlay();Runtime->Epoch=IMAcousticBakeVolumePrivate::NextWorldEpoch.fetch_add(1);
	CaptureScene();
	if(!Runtime->SceneValid||!BakedField)
	{
		FString Issues;
		for(int32 I=0;I<FMath::Min(8,SceneIssues.Num());++I){if(!Issues.IsEmpty())Issues+=TEXT(" | ");Issues+=SceneIssues[I];}
		Issues.ReplaceInline(TEXT("%"),TEXT("%%"));
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeDegraded scenevalid=%d bakedfield=%d issues=%d status=%s detail=%s"),int(Runtime->SceneValid),int(BakedField!=nullptr),SceneIssues.Num(),*Status,*Issues);
	}
}
void AIMAcousticBakeVolume::ShutdownOwnedWork()
{
	if(!Runtime)return;
	const bool bUpstreamTiming=IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamTimingEnabled();
	const double ShutdownStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
	uint32 TimingDeviceId=0;
	if(Runtime->MetaSoundContext.IsValid())TimingDeviceId=Runtime->MetaSoundContext->DeviceId;
	else if(Runtime->DeviceHandle.IsValid())
	{
		if(FAudioDevice* TimingDevice=Runtime->DeviceHandle.GetAudioDevice())TimingDeviceId=TimingDevice->DeviceID;
	}
	const uint64 TimingEpoch=Runtime->Epoch;
	IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("shutdown_owned_work_begin"),0.0,0,TimingDeviceId,TimingEpoch);
	FTSTicker::GetCoreTicker().RemoveTicker(Runtime->BakeTicker);Runtime->BakeTicker.Reset();
	FTSTicker::GetCoreTicker().RemoveTicker(Runtime->ValidationTicker);Runtime->ValidationTicker.Reset();
	if(Runtime->Bridge)
	{
		// A volume that lost device enrollment must never revoke another
		// volume's reverb lease, including during repeated EndPlay/BeginDestroy.
		auto Expected=Runtime->Epoch;
		Runtime->Bridge->ReverbWorldGeneration.compare_exchange_strong(Expected,0,std::memory_order_acq_rel);
	}
	if(Runtime->Job){Runtime->Job->Cancelled.store(true);Runtime->Job->Future.Wait();Runtime->Job.Reset();}
	if(Runtime->DeviceHandle.IsValid()&&ReverbSubmix)
	{
		auto* Mixer=static_cast<Audio::FMixerDevice*>(Runtime->DeviceHandle.GetAudioDevice());
		if(ReverbPreset)Mixer->RemoveSubmixEffect(ReverbSubmix,ReverbPreset->GetUniqueID());
		Mixer->UnregisterSoundSubmix(ReverbSubmix,false);
	}
	if(ReverbPreset)ReverbPreset->StopMetaSoundHost();
	if(Runtime->MetaSoundContext) IMAcousticMetaSound::StopAcousticMetaSoundContext(Runtime->MetaSoundContext);
	if(ReverbPreset)ReverbPreset->ReleaseMetaSoundHost();
	if(Runtime->Worker)
	{
		const double WorkerStopStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
		const uint64 WorkerStopPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
		Runtime->Worker->StopAndJoin();
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("shutdown_worker_stopandjoin_return"),WorkerStopStartedAt,0,
			TimingDeviceId,TimingEpoch,1,0,WorkerStopPhysicalBefore);
		Runtime->Worker.Reset();
	}
	else IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("shutdown_worker_stopandjoin_skipped"),0.0,0,TimingDeviceId,TimingEpoch);
	Runtime->MetaSoundBus.Reset();
	Runtime->MetaSoundContext.Reset();
	Runtime->MetaSoundRegisteredSources.Reset();
	Runtime->MetaSoundAudibleReported = false;
	ReverbPreset=nullptr;ReverbSubmix=nullptr;Runtime->Reverb.Reset();Runtime->DeviceHandle.Reset();
	Runtime->Bridge.Reset();
	IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("shutdown_owned_work_end"),ShutdownStartedAt,0,TimingDeviceId,TimingEpoch);
}
void AIMAcousticBakeVolume::EndPlay(const EEndPlayReason::Type Reason){ShutdownOwnedWork();Super::EndPlay(Reason);}
void AIMAcousticBakeVolume::BeginDestroy(){ShutdownOwnedWork();Super::BeginDestroy();}

void AIMAcousticBakeVolume::PollBakeCompletion()
{
	check(IsInGameThread());
#if WITH_EDITOR
	if(Runtime->Job && Runtime->Job->Done.load(std::memory_order_acquire))
	{
		auto Job=Runtime->Job;Job->Future.Wait();Runtime->Job.Reset();
		if(Job->Cancelled.load()||!Job->Success){Status=Job->Cancelled.load()?TEXT("Bake cancelled; previous asset preserved."):UTF8_TO_TCHAR(Job->Error.c_str());return;}
		if(!CaptureScene()||Runtime->Fingerprint!=Job->Fingerprint){Status=TEXT("Bake discarded: scene changed during bake.");return;}
		if(Job->Data.Scene.size()>MAX_int32||Job->Data.ProbeBatch.size()>MAX_int32)
		{Status=TEXT("Bake exceeds the supported 2 GiB asset section limit; previous asset preserved.");return;}
		const double SaveStartedAt=FPlatformTime::Seconds();
		// Save a new complete asset first. A failed save never mutates the previous
		// complete UObject or its package; assign BakedField only after success.
		const FString Name=TEXT("IM_")+FPackageName::GetShortName(Job->WorldPackage)+TEXT("_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString PackageName=TEXT("/IceMoonAcousticField/Bakes/")+Name;
		UPackage* Package=CreatePackage(*PackageName);
		auto* Asset=NewObject<UIMAcousticBakeAsset>(Package,*Name,RF_Public|RF_Standalone);
		TArray<uint8> Scene,Probes;Scene.Append(Job->Data.Scene.data(),int32(Job->Data.Scene.size()));Probes.Append(Job->Data.ProbeBatch.data(),int32(Job->Data.ProbeBatch.size()));
		const int32 RawSceneBytes=Scene.Num(),RawProbeBytes=Probes.Num();
		FString Error;
		IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("BeforeCommitCompleteBake"));
		if(!Asset->CommitCompleteBake(Job->WorldPackage,Job->Fingerprint,Job->MetadataJson,MoveTemp(Scene),MoveTemp(Probes),Error)){Status=Error;return;}
		IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("AfterCommitCompleteBake"));
		const double EncodeEndedAt=FPlatformTime::Seconds();
		FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
		const FString Filename=FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension());
		if(!UPackage::SavePackage(Package,Asset,*Filename,Args)){Status=TEXT("Bake asset save failed; previous asset preserved.");return;}
		IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("AfterSavePackage"));
		const FPlatformMemoryStats Memory=FPlatformMemory::GetStats();
		constexpr double BytesPerMiB=1024.0*1024.0;
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticBakeAssetSave elapsed_ms=%.3f encode_ms=%.3f scene_bytes=%d probe_bytes=%d raw_scene_bytes=%d raw_probe_bytes=%d package_bytes=%lld format_version=%d package=%s working_set_mib=%.3f peak_working_set_mib=%.3f commit_mib=%.3f peak_commit_mib=%.3f"),
			(FPlatformTime::Seconds()-SaveStartedAt)*1000.0,(EncodeEndedAt-SaveStartedAt)*1000.0,
			Asset->SceneData.Num(),Asset->ProbeData.Num(),RawSceneBytes,RawProbeBytes,
			IFileManager::Get().FileSize(*Filename),Asset->FormatVersion,*PackageName,
			Memory.UsedPhysical/BytesPerMiB,Memory.PeakUsedPhysical/BytesPerMiB,
			Memory.UsedVirtual/BytesPerMiB,Memory.PeakUsedVirtual/BytesPerMiB);
		FAssetRegistryModule::AssetCreated(Asset);
		IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("AfterAssetCreated"));
		Modify();
		IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("AfterModify"));
		BakedField=Asset;MarkPackageDirty();Status=TEXT("Bake saved. Save the level to persist its asset binding.");
		IMTraceAcousticGrassSourceState(GetWorld(), Runtime->GrassGeneration, TEXT("AfterBinding"));
	}
#endif
}
bool AIMAcousticBakeVolume::SetPathingValidationForTest(bool bOn)
{
	if (!Runtime || !Runtime->Worker) { return false; }
	Runtime->Worker->RequestPathingValidationForTest(bOn);
	return true;
}
int AIMAcousticBakeVolume::GetAppliedPathingValidationForTest() const
{
	if (!Runtime || !Runtime->Worker) { return -2; }
	return Runtime->Worker->GetAppliedPathingValidationForTest();
}
bool AIMAcousticBakeVolume::SetApertureTransitForTest(bool bOn, const FVector& CenterUE,
	const FVector& HalfExtentUE)
{
	if(!Runtime)return false;
	if(!bOn)
	{
		Runtime->ApertureTransitEnabled=false;
		return true;
	}
	const FVector E=HalfExtentUE.GetAbs();
	if(CenterUE.ContainsNaN()||E.ContainsNaN()||E.GetMin()<=0.0f)return false;
	Runtime->ApertureTransitCenterUE=CenterUE;
	Runtime->ApertureTransitHalfExtentUE=E;
	Runtime->ApertureTransitEnabled=true;
	return true;
}
void AIMAcousticBakeVolume::PublishPresenceLateralNow(uint64 AudioComponentId, float LateralAzimuthRadians)
{
	if (!Runtime || !Runtime->MetaSoundContext) return;
	const int32 Slot = IMAcousticMetaSound::RegisterAcousticMetaSoundSource(Runtime->MetaSoundContext, AudioComponentId);
	if (Slot == INDEX_NONE) return;
	Runtime->MetaSoundContext->PresenceLateralAzimuth[Slot].store(LateralAzimuthRadians, std::memory_order_relaxed);
}

void AIMAcousticBakeVolume::Tick(float DeltaSeconds)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(IMAcousticBakeVolumeTick_GT);
	Super::Tick(DeltaSeconds);
	PollBakeCompletion();
	if(!GetWorld())return;
	if(!GetWorld()->IsGameWorld())
	{
#if WITH_EDITOR
		DrawEditorFieldOverlay();
#endif
		return;
	}
	const double Now=FPlatformTime::Seconds();if(Now<Runtime->NextSnapshot)return;Runtime->NextSnapshot=Now+.05;
	if(!Runtime->SceneValid||!BakedField){Status=TEXT("V2 degraded: missing/stale scene or bake asset.");return;}
	FAudioDevice* Device=GetWorld()->GetAudioDeviceRaw();if(!Device)return;
	const bool bUpstreamTiming=IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamTimingEnabled();
	const uint32 UpstreamDeviceId=Device->DeviceID;
	auto* Mixer=FAudioDeviceManager::GetAudioMixerDeviceFromWorldContext(this);
	if(!Mixer||Mixer->GetNumDeviceChannels()!=2){Status=TEXT("V2 degraded: a stereo mixer device is required.");return;}
	// Choose the pipeline from an actual source. Starting a legacy worker in
	// an empty world makes the first delayed graph source pay for shutdown,
	// another snapshot tick, and a second payload load before ordinary Play.
	bool bHasAudioConsumer = false;
	bool bGraphPipeline = false;
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		TArray<UIMAcousticSourceComponent*> Sources; It->GetComponents(Sources);
		for (auto* Source : Sources)
		{
			UAudioComponent* Audio = Source->AudioComponent;
			if (!IsValid(Audio) || !IsValid(Audio->Sound)) continue;
			bHasAudioConsumer = true;
			bGraphPipeline |= IMAcousticMetaSound::IsAcousticMetaSound(Audio->Sound);
		}
	}
	if (!bHasAudioConsumer)
	{
		if (!Runtime->Worker)
		{
			Status = TEXT("V2 waiting: no valid acoustic source.");
			return;
		}
		// An existing graph owns its convolution tail after the last source
		// is destroyed. Absence alone must not switch or stop that pipeline;
		// EndPlay still joins the worker and releases the world generation.
		bGraphPipeline = Runtime->MetaSoundContext.IsValid();
	}
	if (Runtime->Worker && (Runtime->MetaSoundContext.IsValid() != bGraphPipeline))
	{
		const bool bWasGraph = Runtime->MetaSoundContext.IsValid();
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound pipeline_rebind from=%s to=%s epoch=%llu"),
			bWasGraph ? TEXT("graph") : TEXT("legacy"),
			bGraphPipeline ? TEXT("graph") : TEXT("legacy"), Runtime->Epoch);
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("tick_pipeline_rebind_before"),0.0,0,UpstreamDeviceId,Runtime->Epoch,
			bWasGraph?1:0,bGraphPipeline?1:0);
		ShutdownOwnedWork();
		Runtime->Epoch = IMAcousticBakeVolumePrivate::NextWorldEpoch.fetch_add(1, std::memory_order_relaxed);
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("tick_pipeline_rebind_after"),0.0,0,UpstreamDeviceId,Runtime->Epoch,
			bWasGraph?1:0,bGraphPipeline?1:0);
		Runtime->ListenerSpatialVersion = 0;
		Runtime->LastListenerLocation = FVector::ZeroVector;
		Runtime->LastListenerRotation = FQuat::Identity;
		Runtime->bHasListenerPose = false;
		Status = TEXT("V2 rebind: acoustic consumer pipeline changed; rebuilding.");
		return;
	}
	if(!Runtime->Worker)
	{
		FString Error;
		TArray<uint8> RawScene, RawProbes;
		const double ReadPayloadStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
		const uint64 ReadPayloadPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
		const bool bReadPayload=BakedField->ReadPayload(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),
			Runtime->Fingerprint,RawScene,RawProbes,Error);
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("tick_read_payload"),ReadPayloadStartedAt,
			uint64(RawScene.Num())+uint64(RawProbes.Num()),UpstreamDeviceId,Runtime->Epoch,bReadPayload?1:0,0,ReadPayloadPhysicalBefore);
		if(!bReadPayload){Status=Error;return;}
		if (bGraphPipeline)
		{
			Runtime->MetaSoundContext = IMAcousticMetaSound::CreateAcousticMetaSoundContext(Device->DeviceID, Mixer->SampleRate, Runtime->Epoch);
			if (!Runtime->MetaSoundContext) { Status = TEXT("MetaSound environment already owned on this device."); return; }
			Runtime->Bridge = Runtime->MetaSoundContext->Device;
		}
		else Runtime->Bridge=IMAcousticSpatialization::FindAcousticDevice(Device);
		if(!Runtime->Bridge){Status=TEXT("V2 degraded: select IceMoon Acoustic Field as Windows spatialization plugin.");return;}
		const double RawCopyStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
		const uint64 RawCopyPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
		FIMAcousticBakeData Data;Data.Scene.assign(RawScene.GetData(),RawScene.GetData()+RawScene.Num());Data.ProbeBatch.assign(RawProbes.GetData(),RawProbes.GetData()+RawProbes.Num());
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("tick_raw_vector_copy"),RawCopyStartedAt,
			uint64(Data.Scene.size())+uint64(Data.ProbeBatch.size()),UpstreamDeviceId,Runtime->Epoch,0,0,RawCopyPhysicalBefore);
		TArray<FVector4> ProbePreview;FVector ProbeOrigin;
		const double ProbePreviewStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
		const uint64 ProbePreviewPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
		const bool bProbePreview=BakedField->GetProbePreview(UWorld::RemovePIEPrefix(GetWorld()->GetOutermost()->GetName()),Runtime->Fingerprint,ProbePreview,ProbeOrigin,Error);
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("tick_probe_preview"),ProbePreviewStartedAt,
			uint64(ProbePreview.Num())*sizeof(FVector4),UpstreamDeviceId,Runtime->Epoch,bProbePreview?1:0,0,ProbePreviewPhysicalBefore);
		if(!bProbePreview)
		{ShutdownOwnedWork();Status=Error;return;}
		for(const auto& P:ProbePreview)Data.CoverageProbes.push_back({{float(P.X),float(P.Y),float(P.Z)},float(P.W)});
		const uint64 WorkerInputBytes=uint64(Data.Scene.size())+uint64(Data.ProbeBatch.size());
		Runtime->Worker=MakeUnique<FIMAcousticSimulationWorker>();
		Runtime->Reverb=Runtime->MetaSoundContext ? Runtime->MetaSoundContext->Pool
			: MakeShared<FIMAcousticReverbPool,ESPMode::ThreadSafe>(Runtime->Epoch);
		const double WorkerStartStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
		const uint64 WorkerStartPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
		const bool bWorkerStarted=Runtime->Worker->Start(Runtime->Bridge,Runtime->Epoch,MoveTemp(Data),Runtime->Reverb);
		IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("tick_worker_start"),WorkerStartStartedAt,WorkerInputBytes,
			UpstreamDeviceId,Runtime->Epoch,bWorkerStarted?1:0,0,WorkerStartPhysicalBefore);
		if(!bWorkerStarted){ShutdownOwnedWork();Status=TEXT("V2 degraded: audio device already belongs to another acoustic world.");return;}
		Runtime->DeviceHandle=GetWorld()->GetAudioDevice();
		if (Runtime->MetaSoundContext)
		{
			const double EnvironmentBindingStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
			const uint64 EnvironmentPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
			IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("env_metasound_bind_begin"),0.0,0,UpstreamDeviceId,Runtime->Epoch);
			UMetaSoundSource* Environment = LoadObject<UMetaSoundSource>(nullptr,
				TEXT("/IceMoonAcousticField/Tests/Audio/MS_AcousticEnvironment"));
			if (!Environment)
			{
				IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("env_metasound_bind_fail"),EnvironmentBindingStartedAt,0,
					UpstreamDeviceId,Runtime->Epoch,1,0,EnvironmentPhysicalBefore);
				Status = TEXT("Missing acoustic environment MetaSound asset."); ShutdownOwnedWork(); return;
			}
			Runtime->MetaSoundBus.Reset(NewObject<UAudioBus>(this));
			Runtime->MetaSoundBus->AudioBusChannels = EAudioBusChannels::Mono;
			auto& Graph = *Runtime->MetaSoundContext;
			// Preserve the published Acoustic Bus parameter as a world-local
			// identity token. Samples travel only through graph-owned voice
			// queues; no native mixer bus or writer is started for this token.
			Graph.BusId = Runtime->MetaSoundBus->GetUniqueID();
			// R39's same-run master fit confirmed a 4441-frame send/direct
			// difference at 48 kHz; lowering the native consumer reserve alone
			// did not remove it. Source operators now hand off their existing
			// delayed Send directly to the graph reader, avoiding the native
			// AudioBus mixer round trip. Keep bounded eight-callback capacity,
			// complete FIFO blocks and explicit fault counters; no dry delay,
			// gain, kernel, quality or source-registration change compensates it.
			Graph.SourceSendCapacityFrames = FMath::DivideAndRoundUp(Mixer->GetNumOutputFrames() * 4, 512) * 512 * 2;
			Graph.SourceSendTransportEnabled = true;
			Graph.BusPrimeFrames = FIMAcousticMetaSoundContext::Frames;
			Runtime->MetaSoundContext->WetGain.store(ReverbWetGain, std::memory_order_relaxed);
			ReverbPreset=NewObject<UIMAcousticReverbPreset>(this);
			FString HostError;
			if(!ReverbPreset->BindMetaSound(Environment,Runtime->MetaSoundBus.Get(),Runtime->MetaSoundContext,
				Runtime->DeviceHandle,HostError))
			{
				Status=TEXT("Acoustic environment host failed: ")+HostError;
				UE_LOG(LogTemp,Error,TEXT("IMLogs AcousticMetaSound post_source_host_bind_fail error=%s"),*HostError);
				ShutdownOwnedWork();return;
			}
			ReverbSubmix=NewObject<USoundSubmix>(this,TEXT("IM_MetaSoundEnvironmentWet"));
			ReverbSubmix->bAutoDisable=false;ReverbSubmix->bMuteWhenBackgrounded=false;
			Mixer->RegisterSoundSubmix(ReverbSubmix);
			UAudioMixerBlueprintLibrary::AddSubmixEffect(this,ReverbSubmix,ReverbPreset);
			IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("env_metasound_host_publish_end"),EnvironmentBindingStartedAt,0,
				UpstreamDeviceId,Runtime->Epoch,1,0,EnvironmentPhysicalBefore);
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound ordinary_environment_post_source device=%u epoch=%llu graph_frames=512 mixer_frames=%d independent_environment_play=0"),
				Device->DeviceID, Runtime->Epoch, Mixer->GetNumOutputFrames());
		}
		else
		{
		ReverbSubmix=NewObject<USoundSubmix>(this,TEXT("IM_EnvironmentReverb"));
		ReverbSubmix->bAutoDisable=false;ReverbSubmix->bMuteWhenBackgrounded=false;
		Mixer->RegisterSoundSubmix(ReverbSubmix);
		ReverbPreset=NewObject<UIMAcousticReverbPreset>(this);
		ReverbPreset->Bind(Runtime->Bridge,Runtime->Reverb,ReverbWetGain);
		UAudioMixerBlueprintLibrary::AddSubmixEffect(this,ReverbSubmix,ReverbPreset);
		}
		Runtime->Bridge->ReverbWorldGeneration.store(Runtime->Epoch,std::memory_order_release);
	}
	if(Runtime->Worker->GetState()==FIMAcousticSimulationWorker::EIMWorkerState::LoadFailed){Status=TEXT("V2 degraded: SDK bake load failed.");return;}
	FIMAcousticTimingScope Timing(Runtime->Bridge->ProfilingEnabled.load(std::memory_order_relaxed)?&Runtime->Bridge->GameThreadTiming:nullptr);
	if(Runtime->PreviousSnapshot)IMAcousticTiming::AcousticRecordMaximum(Runtime->Bridge->MaxSnapshotGapUs,uint64((Now-Runtime->PreviousSnapshot)*1.e6));
	Runtime->PreviousSnapshot=Now;
	Runtime->Bridge->Enabled.store(bEnableV2,std::memory_order_release);
	Runtime->Bridge->RenderRoutes.store((bDirectRoute?1u:0u)|(bPathRoute?2u:0u)|(bReverbRoute?4u:0u),std::memory_order_relaxed);
	FTransform Listener,Extra;
	if(!Device->GetListenerTransform(0,Listener)||Device->GetListenerTransform(1,Extra))
	{Status=TEXT("V2 degraded: exactly one listener is required.");return;}
	const FVector CurrentListenerLocation=Listener.GetLocation();
	const FQuat CurrentListenerRotation=Listener.GetRotation();
	const bool bListenerMoved=!Runtime->bHasListenerPose
		|| FVector::DistSquared(CurrentListenerLocation,Runtime->LastListenerLocation)>1.0f
		|| !CurrentListenerRotation.Equals(Runtime->LastListenerRotation,0.001f);
	if(bListenerMoved)
	{
		Runtime->ListenerSpatialVersion=FMath::Max<uint64>(1,Runtime->ListenerSpatialVersion+1);
		Runtime->LastListenerLocation=CurrentListenerLocation;
		Runtime->LastListenerRotation=CurrentListenerRotation;
		Runtime->bHasListenerPose=true;
		if(Runtime->MetaSoundContext)
			Runtime->MetaSoundContext->ListenerSpatialVersion.store(Runtime->ListenerSpatialVersion,std::memory_order_release);
	}
	const FBox Region=BakeBounds->Bounds.GetBox();
	// Keep the graph source's dry and independent room-send distance curves
	// live even when the listener cannot publish a bake snapshot. The previous
	// ordering returned before this update, leaving a wall-interior/out-of-bounds
	// listener on the last valid point's gain and making the degraded window
	// report a false distance.
	if (Runtime->MetaSoundContext)
	{
		FString SourceFailure;
		bool bAnyExaggeratedSource = false;
		const float PresenceStrength = FIMAcousticPresenceParameters::ClampStrength(IMAcousticBakeVolumePrivate::CVarIMAcousticPresenceStrength.GetValueOnGameThread());
		for (TActorIterator<AActor> It(GetWorld()); It; ++It)
		{
			TArray<UIMAcousticSourceComponent*> Sources; It->GetComponents(Sources);
			for (auto* Source : Sources)
			{
				if (!Source) continue;
				if (!Source->ValidateSource(SourceFailure)) continue;
				UAudioComponent* Audio = Source->AudioComponent;
				if (!Audio || !IMAcousticMetaSound::IsAcousticMetaSound(Audio->Sound)) continue;
				const uint64 Id = Audio->GetAudioComponentID();
				const int32 Slot = IMAcousticMetaSound::RegisterAcousticMetaSoundSource(Runtime->MetaSoundContext, Id);
				if (Slot == INDEX_NONE) continue;
				Runtime->MetaSoundContext->SendGains[Slot].store(Audio->VolumeMultiplier, std::memory_order_relaxed);
				const float SourceListenerDistanceM = float(FVector::Distance(Audio->GetComponentLocation(), Listener.GetLocation()) * .01);
				const float ReverbSendDistanceGain = IMAcousticRecipe::ReverbSendDistanceGain(SourceListenerDistanceM);
				// Exaggerated presence scales only this slot's room send. Own-footstep near field also
				// cancels the room-send distance curve at the same distance as the direct path, so the
				// direct-to-reverberant ratio stays at the physical value; the extra dB adds room.
				float PresenceSendScale = 1.f;
				if (Source->PresenceFlags & IMAcousticPresenceFlags::Exaggerated)
				{
					PresenceSendScale = FMath::Pow(10.f, FIMAcousticPresenceParameters::WetSendExtraDbAt(PresenceStrength) / 20.f);
					if (Source->PresenceFlags & IMAcousticPresenceFlags::SelfNearField)
						PresenceSendScale *= FMath::Max(1.f, SourceListenerDistanceM);
					bAnyExaggeratedSource = true;
				}
				Runtime->MetaSoundContext->PresenceFlags[Slot].store(Source->PresenceFlags, std::memory_order_relaxed);
				Runtime->MetaSoundContext->PresenceLateralAzimuth[Slot].store(Source->PresenceLateralAzimuthRadians, std::memory_order_relaxed);
				Runtime->MetaSoundContext->ReverbSendGains[Slot].store(
					Audio->VolumeMultiplier * ReverbSendDistanceGain * PresenceSendScale, std::memory_order_relaxed);
				Runtime->MetaSoundContext->DistanceGains[Slot].store(
					1.f / FMath::Max(1.f, SourceListenerDistanceM), std::memory_order_relaxed);
				Runtime->MetaSoundContext->DistanceCm[Slot].store(
					SourceListenerDistanceM * 100.f, std::memory_order_relaxed);
				Runtime->MetaSoundContext->WetGain.store(FMath::Clamp(ReverbWetGain, 0.f, 1.f), std::memory_order_relaxed);
				if (!Runtime->MetaSoundRegisteredSources.Contains(Id))
				{
					// Registration precedes the first generator construction;
					// this must also work when the first listener snapshot is
					// intentionally rejected by bounds/geometry fail-closed.
					const double SourceBindingStartedAt=bUpstreamTiming?FPlatformTime::Seconds():0.0;
					const uint64 SourceBindingPhysicalBefore=bUpstreamTiming?IMAcousticBakeVolumePrivate::IMBakeVolumeUpstreamPhysicalBytes():0;
					IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("source_metasound_bind_begin"),0.0,0,UpstreamDeviceId,Runtime->Epoch,Id,Slot);
					Audio->Stop();
					Audio->SetIntParameter(TEXT("Acoustic Voice"), Slot);
					Audio->SetObjectParameter(TEXT("Acoustic Bus"), Runtime->MetaSoundBus.Get());
					Runtime->MetaSoundRegisteredSources.Add(Id);
					Audio->Play();
					IMAcousticBakeVolumePrivate::IMLogBakeVolumeUpstreamTiming(TEXT("source_metasound_bind_end"),SourceBindingStartedAt,0,
						UpstreamDeviceId,Runtime->Epoch,Id,Slot,SourceBindingPhysicalBefore);
					UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound ordinary_source audio=%llu slot=%d epoch=%llu"), Id, Slot, Runtime->Epoch);
				}
			}
		}
		// The environment is one shared convolution output; its wet width follows whether any
		// registered source asked for exaggeration, and is neutral (1) otherwise.
		Runtime->MetaSoundContext->PresenceWetSideGain.store(
			bAnyExaggeratedSource ? FIMAcousticPresenceParameters::WetSideGainAt(PresenceStrength) : 1.f, std::memory_order_relaxed);
		Runtime->MetaSoundContext->PresenceStrength.store(PresenceStrength, std::memory_order_relaxed);
	}
	if(!Region.IsInsideOrOn(Listener.GetLocation()))
	{
		Runtime->Bridge->RenderRoutes.store((bDirectRoute ? 1u : 0u) | (bPathRoute ? 2u : 0u), std::memory_order_relaxed);
		Status=TEXT("V2 degraded: listener outside bake bounds.");return;
	}
	const IPLCoordinateSpace3 ListenerSpace = ToSDKSpace(Listener, Runtime->Origin);
	if (IMAcousticBakeVolumePrivate::PointInsideAcousticGeometry(Runtime->Geometry, Runtime->GeometryTriangleRanges, ListenerSpace.origin))
	{
		// Do not let an invalid wall-interior position keep selecting a
		// nearest baked probe. Stop publishing the snapshot and immediately
		// remove only the reverb route; direct/path dry audio remains enabled.
		// The environment operator also treats this route bit as part of its
		// freshness contract, so an already queued IR cannot produce wet
		// output during the 250 ms lease transition.
		Runtime->Bridge->RenderRoutes.store((bDirectRoute ? 1u : 0u) | (bPathRoute ? 2u : 0u), std::memory_order_relaxed);
		Status = TEXT("V2 degraded: listener inside baked acoustic geometry.");
		return;
	}
	auto Snapshot=MakeShared<FIMAcousticWorldSnapshot,ESPMode::ThreadSafe>();
	Snapshot->WorldGeneration=Runtime->Epoch;Snapshot->CapturedSeconds=Now;
	Snapshot->SpatialVersion=Runtime->ListenerSpatialVersion;Snapshot->Listener=ListenerSpace;
	Snapshot->ApertureTransitEnabled=Runtime->ApertureTransitEnabled;
	if(Runtime->ApertureTransitEnabled)
	{
		Snapshot->ApertureTransitCenter=ToSDKPosition(Runtime->ApertureTransitCenterUE,Runtime->Origin);
		const FVector E=Runtime->ApertureTransitHalfExtentUE.GetAbs()*0.01f;
		Snapshot->ApertureTransitHalfExtent={float(E.Y),float(E.Z),float(E.X)};
	}
	FString Failure;
	IMAcousticBakeVolumePrivate::FIMAcousticMaterialTable DynamicMaterialTable;
	FString DynamicMaterialFailure;
	// Only registered rigid blockers consume this table. An empty list still
	// submits an empty dynamic snapshot, which removes former worker instances.
	const bool DynamicMaterialsValid=DynamicBlockers.IsEmpty()||DynamicMaterialTable.Build(*this,DynamicMaterialFailure);
	FString FirstDynamicFailure = DynamicMaterialsValid ? FString() : DynamicMaterialFailure;
	// Sub-phase diagnostic: world traversal isolated from worker submit.
	const uint64 TravStart=FPlatformTime::Cycles64();
	for(TActorIterator<AActor> It(GetWorld());It;++It)
	{
		TArray<UIMAcousticSourceComponent*> Sources;It->GetComponents(Sources);
		for(auto* Source:Sources)
		{
			if(!Source->ValidateSource(Failure))continue;
			UAudioComponent* Audio=Source->AudioComponent;
			if(!Region.IsInsideOrOn(Audio->GetComponentLocation())){Failure=TEXT("Source outside bake bounds.");continue;}
			Snapshot->Sources.Add({Audio->GetAudioComponentID(),ToSDKSpace(Audio->GetComponentTransform(),Runtime->Origin)});
		}
	}
	if (Snapshot->Sources.Num() > 1)
	{
		static double SrcCountLast = 0;
		if (Now - SrcCountLast > 5.0)
		{
			SrcCountLast = Now;
			const uint64 FirstId = Snapshot->Sources.Num() ? Snapshot->Sources[0].AudioComponentId : 0;
			UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticSourceCount n=%d first_id=%llu"), Snapshot->Sources.Num(), FirstId);
		}
	}
	// H1 W2: collect registered dynamic blockers into this snapshot. Validity
	// is checked here on the GT (no UObject crosses to the worker); missing or
	// failed entries are omitted so the worker removes them, degrading to
	// unoccluded audio rather than stale geometry. Skips are observable via
	// the snapshot probe counters, volume status and change-only warning.
	int32 DynamicSkipped = 0;
	for (const TObjectPtr<UStaticMeshComponent>& Blocker : DynamicBlockers)
	{
		if (!IsValid(Blocker)||!DynamicMaterialsValid)
		{
			++DynamicSkipped;
			if (FirstDynamicFailure.IsEmpty()) FirstDynamicFailure = TEXT("Registered dynamic blocker component is unavailable.");
			continue;
		}
		FIMAcousticDynamicMeshSnapshot DynamicEntry;
		FString DynamicFailure;
		if (!IMAcousticBakeVolumePrivate::BuildDynamicMeshSnapshot(Blocker.Get(), 0, Runtime->Origin, DynamicMaterialTable, DynamicEntry, DynamicFailure))
		{
			++DynamicSkipped;
			if (FirstDynamicFailure.IsEmpty()) FirstDynamicFailure = Blocker->GetPathName() + TEXT(": ") + DynamicFailure;
			continue;
		}
		if(!DynamicEntry.Triangles.IsEmpty())Snapshot->DynamicMeshes.Add(MoveTemp(DynamicEntry));
	}
	if (FirstDynamicFailure != Runtime->LastDynamicGeometryFailure)
	{
		Runtime->LastDynamicGeometryFailure = FirstDynamicFailure;
		if (!FirstDynamicFailure.IsEmpty())
			UE_LOG(LogTemp, Warning, TEXT("[IM] Dynamic acoustic geometry degraded: %s"), *FirstDynamicFailure);
	}
	if(Runtime->Bridge->ProfilingEnabled.load(std::memory_order_relaxed))Runtime->Bridge->GTTraverseTiming.Record(TravStart);
	// Sub-phase diagnostic: worker submit isolated from traversal.
	const uint64 SubStart=FPlatformTime::Cycles64();
	const bool ProbeHasWorker = (Runtime->Worker != nullptr);
	const bool ProbeSubmitted = ProbeHasWorker && Runtime->Worker->Submit(Snapshot);
	if(Runtime->Bridge->ProfilingEnabled.load(std::memory_order_relaxed))Runtime->Bridge->GTSubmitTiming.Record(SubStart);
	const double ProbeSubmitDone = FPlatformTime::Seconds();
	// W1 causal probe (GT side): actual AudioDevice listener snapshot that the
	// worker will consume. Joins to worker results and audio blocks by
	// (WorldGeneration, CapturedSeconds). Preallocated write-once log; zero behavior.
	// Times are FPlatformTime::Seconds() wall clock, seconds.
	FIMAcousticSnapshotProbe ProbeSnap{};
	ProbeSnap.WorldGeneration = Snapshot->WorldGeneration;
	ProbeSnap.SpatialVersion = Snapshot->SpatialVersion;
	ProbeSnap.CapturedSeconds = Snapshot->CapturedSeconds;
	ProbeSnap.SubmitSeconds = ProbeHasWorker ? ProbeSubmitDone : 0.0;
	ProbeSnap.ListenerUEX = float(Listener.GetLocation().X);
	ProbeSnap.ListenerUEY = float(Listener.GetLocation().Y);
	ProbeSnap.ListenerUEZ = float(Listener.GetLocation().Z);
	ProbeSnap.ListenerSDKX = Snapshot->Listener.origin.x;
	ProbeSnap.ListenerSDKY = Snapshot->Listener.origin.y;
	ProbeSnap.ListenerSDKZ = Snapshot->Listener.origin.z;
	if (Snapshot->Sources.Num() > 0)
	{
		ProbeSnap.Source0X = Snapshot->Sources[0].Source.origin.x;
		ProbeSnap.Source0Y = Snapshot->Sources[0].Source.origin.y;
		ProbeSnap.Source0Z = Snapshot->Sources[0].Source.origin.z;
		ProbeSnap.Source0AudioId = Snapshot->Sources[0].AudioComponentId;
	}
	ProbeSnap.NumSources = uint32(Snapshot->Sources.Num());
	ProbeSnap.NumDynamicMeshes = uint32(Snapshot->DynamicMeshes.Num());
	ProbeSnap.NumDynamicSkipped = uint32(DynamicSkipped);
	if (Snapshot->DynamicMeshes.Num() > 0)
	{
		ProbeSnap.Door0TX = Snapshot->DynamicMeshes[0].Transform.elements[0][3];
		ProbeSnap.Door0TY = Snapshot->DynamicMeshes[0].Transform.elements[1][3];
		ProbeSnap.Door0TZ = Snapshot->DynamicMeshes[0].Transform.elements[2][3];
	}
	ProbeSnap.Submitted = ProbeSubmitted ? 1 : 0;
	ProbeSnap.FailCode = ProbeSubmitted ? 0 : (ProbeHasWorker ? 1 : 2);
	Runtime->Bridge->TraceSnapshot(ProbeSnap);
	Status=Failure.IsEmpty()?FString::Printf(TEXT("V2 routing %d sources; rendered %llu, degraded %llu, reverb audible %llu blocks."),Snapshot->Sources.Num(),Runtime->Bridge->RenderedBlocks.load(),Runtime->Bridge->RejectedBlocks.load(),Runtime->Bridge->ReverbNonzeroBlocks.load()):TEXT("V2 source rejected: ")+Failure;
	if (DynamicSkipped > 0) Status += FString::Printf(TEXT(" Dynamic blockers skipped=%d: %s"), DynamicSkipped, *FirstDynamicFailure);
	if (Runtime->MetaSoundContext && !Runtime->MetaSoundAudibleReported && Runtime->Bridge->ReverbNonzeroBlocks.load() >= 50)
	{
		Runtime->MetaSoundAudibleReported = true;
		UE_LOG(LogTemp, Display, TEXT("IMLogs AcousticMetaSound ordinary_wet_active epoch=%llu source_blocks=%llu wet_blocks=%llu ir=%llu post_source_host=%d"),
			Runtime->Epoch, Runtime->MetaSoundContext->SourceBlocks.load(), Runtime->Bridge->ReverbNonzeroBlocks.load(),
			Runtime->MetaSoundContext->LastIRSequence.load(), ReverbSubmix != nullptr);
	}
}
