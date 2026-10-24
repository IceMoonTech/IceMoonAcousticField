#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "IMAcousticBakeVolume.h"
#include "IMAcousticBakeAsset.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonWriter.h"

namespace IMAcousticG1InstanceTestPrivate
{
constexpr const TCHAR* IMG1Map=TEXT("/IceMoonAcousticField/L_IceMoonAcousticField");
constexpr double IMG1CoordinateToleranceMeters=0.0005; // 0.05 cm, the float round-trip bound used by IMAcousticCoordinateTest.cpp at km scale.

struct FIMG1Source
{
	UWorld* World=nullptr;
	UStaticMesh* Mesh=nullptr;
	UMaterialInterface* BaseMaterial=nullptr;
	UMaterialInterface* OverrideMaterial=nullptr;
	int32 WhiteboxComponents=0;
	FString MapIdentity;
	FString BakeAssetIdentity;
};

struct FIMG1Capture
{
	TArray<FVector> Vertices;
	TArray<FIntVector> Triangles;
	TArray<int32> MaterialIndices;
	FString Fingerprint;
};

struct FIMG1Case
{
	FString Name;
	bool bPassed=false;
	int32 InputInstances=0;
	int32 OutsideInstancesExpected=0;
	int32 OutsideInstancesReported=0;
	int32 VertexCount=0;
	int32 TriangleCount=0;
	int32 Material0Triangles=0;
	int32 Material1Triangles=0;
	FString Fingerprint;
	bool bGeometryMatchesReference=false;
	bool bWorldTransformExpected=false;
	bool bPositionNegativeControl=false;
	FString Details;
};

static FString IMG1HashStrings(TArray<FString> Items)
{
	Items.Sort();
	FSHA1 Hash;
	for(const FString& Item:Items)
	{
		FTCHARToUTF8 Bytes(*Item);
		Hash.Update(reinterpret_cast<const uint8*>(Bytes.Get()),Bytes.Length());
		const uint8 Separator=0;
		Hash.Update(&Separator,1);
	}
	Hash.Final();uint8 Digest[20];Hash.GetHash(Digest);return BytesToHex(Digest,20);
}

static FString IMG1BakeAssetIdentity(UWorld* World)
{
	TArray<FString> Items;
	for(TActorIterator<AIMAcousticBakeVolume> It(World);It;++It)
	{
		UIMAcousticBakeAsset* Asset=It->BakedField;
		FString Item=It->GetPathName()+TEXT("|")+GetPathNameSafe(Asset);
		if(Asset)
		{
			Item+=TEXT("|")+Asset->WorldPackage+TEXT("|")+Asset->SceneFingerprint+TEXT("|stored_payload_digest=")+Asset->PayloadDigest;
			TArray<FString> PayloadParts;
			PayloadParts.Add(Asset->MetadataJson);
			Item+=TEXT("|")+IMG1HashStrings(MoveTemp(PayloadParts));
			FSHA1 StoredSectionsHash;
			if(Asset->SceneData.Num())StoredSectionsHash.Update(Asset->SceneData.GetData(),Asset->SceneData.Num());
			if(Asset->ProbeData.Num())StoredSectionsHash.Update(Asset->ProbeData.GetData(),Asset->ProbeData.Num());
			StoredSectionsHash.Final();uint8 StoredSectionsDigest[20];StoredSectionsHash.GetHash(StoredSectionsDigest);
			Item+=TEXT("|stored_sections_sha1=")+BytesToHex(StoredSectionsDigest,20);
		}
		Items.Add(MoveTemp(Item));
	}
	return IMG1HashStrings(MoveTemp(Items));
}

static FString IMG1MapIdentity(UWorld* World, UStaticMesh*& OutMesh, UMaterialInterface*& OutMaterial,
	int32& OutWhiteboxComponents, int32& OutUniqueMeshes)
{
	OutMesh=nullptr;OutMaterial=nullptr;OutWhiteboxComponents=0;OutUniqueMeshes=0;
	TSet<UStaticMesh*> Meshes;bool bUniformBaseMaterial=true;
	TArray<FString> Items;
	for(TActorIterator<AActor> It(World);It;++It)
	{
		if(!It->ActorHasTag(TEXT("IMAcousticWhitebox")))continue;
		TArray<UStaticMeshComponent*> Components;It->GetComponents(Components);
		for(UStaticMeshComponent* Component:Components)
		{
			UStaticMesh* Mesh=Component->GetStaticMesh();
			if(!Mesh)continue;
			++OutWhiteboxComponents;Meshes.Add(Mesh);
			if(!OutMesh)OutMesh=Mesh;
			if(OutWhiteboxComponents==1)OutMaterial=Component->GetMaterial(0);
			else if(OutMaterial!=Component->GetMaterial(0))bUniformBaseMaterial=false;
			Items.Add(Component->GetPathName()+TEXT("|")+GetPathNameSafe(Mesh)+TEXT("|")+
				Component->GetComponentTransform().ToString()+TEXT("|")+GetPathNameSafe(Component->GetMaterial(0)));
		}
	}
	OutUniqueMeshes=Meshes.Num();if(!bUniformBaseMaterial)OutMaterial=nullptr;
	return World->GetOutermost()->GetName()+TEXT("|")+IMG1HashStrings(MoveTemp(Items));
}

static FIntVector IMG1QuantizeSDKPoint(const FVector& Point)
{
	return FIntVector(FMath::RoundToInt(Point.X/IMG1CoordinateToleranceMeters),
		FMath::RoundToInt(Point.Y/IMG1CoordinateToleranceMeters),
		FMath::RoundToInt(Point.Z/IMG1CoordinateToleranceMeters));
}

static bool IMG1PointLess(const FIntVector& A,const FIntVector& B)
{
	if(A.X!=B.X)return A.X<B.X;if(A.Y!=B.Y)return A.Y<B.Y;return A.Z<B.Z;
}

static bool IMG1RotationLess(const FIntVector P[3],int32 A,int32 B)
{
	for(int32 K=0;K<3;++K)
	{
		const FIntVector& PA=P[(A+K)%3];const FIntVector& PB=P[(B+K)%3];
		if(PA==PB)continue;return IMG1PointLess(PA,PB);
	}
	return false;
}

static FString IMG1TriangleKey(const FVector& A,const FVector& B,const FVector& C,int32 Material)
{
	const FIntVector P[3]={IMG1QuantizeSDKPoint(A),IMG1QuantizeSDKPoint(B),IMG1QuantizeSDKPoint(C)};
	int32 First=0;for(int32 Candidate=1;Candidate<3;++Candidate)if(IMG1RotationLess(P,Candidate,First))First=Candidate;
	const FIntVector& P0=P[First];const FIntVector& P1=P[(First+1)%3];const FIntVector& P2=P[(First+2)%3];
	return FString::Printf(TEXT("%d|%d,%d,%d>%d,%d,%d>%d,%d,%d"),Material,
		P0.X,P0.Y,P0.Z,P1.X,P1.Y,P1.Z,P2.X,P2.Y,P2.Z);
}

static bool IMG1Canonicalize(const FIMG1Capture& Capture,TArray<FString>& Out,FString& Error)
{
	Out.Reset();
	if(Capture.Triangles.Num()!=Capture.MaterialIndices.Num())
	{Error=TEXT("triangle/material-index cardinality mismatch");return false;}
	Out.Reserve(Capture.Triangles.Num());
	for(int32 I=0;I<Capture.Triangles.Num();++I)
	{
		const FIntVector T=Capture.Triangles[I];
		if(!Capture.Vertices.IsValidIndex(T.X)||!Capture.Vertices.IsValidIndex(T.Y)||!Capture.Vertices.IsValidIndex(T.Z))
		{Error=FString::Printf(TEXT("triangle %d has an invalid vertex index"),I);return false;}
		// Cyclic vertex rotations are equivalent. Reversed winding remains a different key.
		Out.Add(IMG1TriangleKey(Capture.Vertices[T.X],Capture.Vertices[T.Y],Capture.Vertices[T.Z],Capture.MaterialIndices[I]));
	}
	Out.Sort();return true;
}

struct FIMG1ExpectedMesh { FTransform Transform; UMaterialInterface* Material=nullptr; };
static bool IMG1BuildExpected(UStaticMesh* Mesh,const FVector& Origin,const TMap<UMaterialInterface*,int32>& MaterialIndices,
	const TArray<FIMG1ExpectedMesh>& ExpectedMeshes,TArray<FString>& Out,FString& Error)
{
	Out.Reset();
	if(!Mesh||!Mesh->GetRenderData()||Mesh->GetRenderData()->LODResources.IsEmpty())
	{Error=TEXT("fixture Cube has no LOD0 CPU mesh data");return false;}
	const FStaticMeshLODResources& LOD=Mesh->GetRenderData()->LODResources[0];
	for(const FIMG1ExpectedMesh& Expected:ExpectedMeshes)
	{
		const int32* MaterialIndex=MaterialIndices.Find(Expected.Material);
		if(!MaterialIndex){Error=TEXT("expected material missing from bake mapping");return false;}
		for(const FStaticMeshSection& Section:LOD.Sections)
		{
			for(uint32 T=0;T<Section.NumTriangles;++T)
			{
				uint32 Indices[3];FVector SDKPoints[3];
				for(uint32 K=0;K<3;++K)
				{
					Indices[K]=LOD.IndexBuffer.GetIndex(Section.FirstIndex+T*3+K);
					if(Indices[K]>=LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices())
					{Error=TEXT("fixture Cube index exceeds vertex buffer");return false;}
					const FVector World=Expected.Transform.TransformPosition(FVector(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(Indices[K])));
					const FVector Delta=(World-Origin)*0.01;
					SDKPoints[K]=FVector(Delta.Y,Delta.Z,-Delta.X); // UE cm to SDK metres, then Y/Z/-X axis mapping.
				}
				if(Expected.Transform.GetDeterminant()<0)Swap(SDKPoints[1],SDKPoints[2]);
				Out.Add(IMG1TriangleKey(SDKPoints[0],SDKPoints[1],SDKPoints[2],*MaterialIndex));
			}
		}
	}
	Out.Sort();return true;
}

class FIMG1Fixture
{
public:
	~FIMG1Fixture(){Cleanup();}

	void Cleanup()
	{
		for(const TWeakObjectPtr<AActor>& Actor:TransientActors)if(Actor.IsValid())Actor->Destroy();
		TransientActors.Reset();
		if(Volume.IsValid())Volume->Destroy();Volume.Reset();
		for(const TWeakObjectPtr<UPrimitiveComponent>& Component:TaggedComponents)
			if(Component.IsValid())Component->ComponentTags.Remove(TEXT("IMAcousticIgnore"));
		TaggedComponents.Reset();World=nullptr;CurrentSource={};
	}

	bool BeginCase(FIMG1Source& Out,FString& Error)
	{
		Cleanup();
		if(!FEditorFileUtils::LoadMap(IMG1Map,false,true))
		{Error=TEXT("cannot load the required whitebox map");return false;}
		World=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
		if(!World){Error=TEXT("Editor world is unavailable after map load");return false;}
		int32 UniqueMeshes=0;
		const FString MapIdentity=IMG1MapIdentity(World,Out.Mesh,Out.BaseMaterial,Out.WhiteboxComponents,UniqueMeshes);
		if(Out.WhiteboxComponents!=60||UniqueMeshes!=1||!Out.Mesh||!Out.BaseMaterial||Out.Mesh->GetStaticMaterials().Num()!=1)
		{Error=FString::Printf(TEXT("whitebox source drift: components=%d unique_meshes=%d mesh=%s material=%s"),
			Out.WhiteboxComponents,UniqueMeshes,*GetPathNameSafe(Out.Mesh),*GetPathNameSafe(Out.BaseMaterial));return false;}
		Out.World=World;Out.MapIdentity=MapIdentity;Out.BakeAssetIdentity=IMG1BakeAssetIdentity(World);
		if(!BaselineMapIdentity.IsEmpty()&&Out.MapIdentity!=BaselineMapIdentity)
		{Error=TEXT("whitebox map source identity changed between isolated cases");return false;}
		if(!BaselineBakeAssetIdentity.IsEmpty()&&Out.BakeAssetIdentity!=BaselineBakeAssetIdentity)
		{Error=TEXT("original baked asset identity changed between isolated cases");return false;}
		if(BaselineMapIdentity.IsEmpty())BaselineMapIdentity=Out.MapIdentity;
		if(BaselineBakeAssetIdentity.IsEmpty())BaselineBakeAssetIdentity=Out.BakeAssetIdentity;

		Out.OverrideMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
		if(!Out.OverrideMaterial||Out.OverrideMaterial==Out.BaseMaterial)
		{Error=TEXT("a distinct loadable component override material is unavailable");return false;}
		// Ignore pre-existing map geometry only in memory; every case reloads the map and the final reload verifies it.
		for(TActorIterator<AActor> It(World);It;++It)
		{
			TArray<UPrimitiveComponent*> Components;It->GetComponents(Components);
			for(UPrimitiveComponent* Component:Components)
				if(!Component->ComponentHasTag(TEXT("IMAcousticIgnore")))
				{Component->ComponentTags.Add(TEXT("IMAcousticIgnore"));TaggedComponents.Add(Component);}
		}
		FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transient;Params.Name=NAME_None;
		Volume=World->SpawnActor<AIMAcousticBakeVolume>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
		if(!Volume.IsValid()){Error=TEXT("could not spawn transient bake fixture");return false;}
		Volume->MaterialDrive=EIMAcousticMaterialDrive::LegacyRenderMaterial;
		Volume->BakeBounds->SetBoxExtent(FVector(1000,1000,1000));
		FIMAcousticMaterialMapping Base;Base.Material=Out.BaseMaterial;Base.Absorption=FVector(.2,.3,.4);Base.Scattering=.35f;
		FIMAcousticMaterialMapping Override;Override.Material=Out.OverrideMaterial;Override.Absorption=FVector(.6,.4,.2);Override.Scattering=.65f;
		Volume->Materials={Base,Override};CurrentSource=Out;
		return true;
	}

	UStaticMeshComponent* AddStatic(const FTransform& Transform,UMaterialInterface* Material)
	{
		FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transient;
		AStaticMeshActor* Actor=World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
		if(!Actor)return nullptr;
		TransientActors.Add(Actor);Actor->SetActorTransform(Transform);
		UStaticMeshComponent* Component=Actor->GetStaticMeshComponent();
		Component->SetStaticMesh(CurrentSource.Mesh);Component->SetMaterial(0,Material);return Component;
	}

	UInstancedStaticMeshComponent* AddInstanced(bool bHierarchical,UMaterialInterface* Material,
		const TArray<FTransform>& Transforms)
	{
		FActorSpawnParameters Params;Params.ObjectFlags|=RF_Transient;
		AActor* Actor=World->SpawnActor<AActor>(AActor::StaticClass(),FTransform::Identity,Params);
		if(!Actor)return nullptr;TransientActors.Add(Actor);
		USceneComponent* Root=NewObject<USceneComponent>(Actor,NAME_None,RF_Transient);
		Root->SetMobility(EComponentMobility::Static);
		Actor->SetRootComponent(Root);Actor->AddInstanceComponent(Root);Root->RegisterComponent();
		UInstancedStaticMeshComponent* Component=bHierarchical
			?static_cast<UInstancedStaticMeshComponent*>(NewObject<UHierarchicalInstancedStaticMeshComponent>(Actor,NAME_None,RF_Transient))
			:NewObject<UInstancedStaticMeshComponent>(Actor,NAME_None,RF_Transient);
		Actor->AddInstanceComponent(Component);Component->SetupAttachment(Root);Component->SetStaticMesh(CurrentSource.Mesh);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetMaterial(0,Material);
		for(const FTransform& Transform:Transforms)if(Component->AddInstance(Transform,true)<0)return nullptr;
		Component->RegisterComponent();return Component;
	}

	bool Capture(FIMG1Capture& Out,FString& Error)
	{
		if(!Volume.IsValid()||!Volume->IM_CaptureSceneGeometryForTest(Out.Vertices,Out.Triangles,Out.MaterialIndices,Out.Fingerprint))
		{
			Error=Volume.IsValid()?Volume->Status:TEXT("transient bake fixture was destroyed");
			if(Volume.IsValid())
			{
				Error+=FString::Printf(TEXT("; exported_triangles=%d"),Volume->ExportedTriangles);
				for(const FString& Issue:Volume->SceneIssues)Error+=TEXT("; ")+Issue;
			}
			return false;
		}
		if(Out.Vertices.IsEmpty()||Out.Triangles.IsEmpty()||Out.MaterialIndices.Num()!=Out.Triangles.Num()||Out.Fingerprint.IsEmpty())
		{Error=TEXT("test accessor returned empty or inconsistent scene geometry");return false;}
		return true;
	}

	int32 ReportedOutsideInstances() const
	{
		int32 Count=0;
		if(Volume.IsValid())for(const FString& Issue:Volume->SceneIssues)
			if(Issue.Contains(TEXT("Excluded 1 static instances outside bake bounds")))++Count;
		return Count;
	}

	UWorld* World=nullptr;
	TWeakObjectPtr<AIMAcousticBakeVolume> Volume;
	FIMG1Source CurrentSource;
	TArray<TWeakObjectPtr<AActor>> TransientActors;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> TaggedComponents;
	FString BaselineMapIdentity;
	FString BaselineBakeAssetIdentity;
};

static void IMG1FillCase(FIMG1Case& Result,const FIMG1Capture& Capture)
{
	Result.VertexCount=Capture.Vertices.Num();Result.TriangleCount=Capture.Triangles.Num();Result.Fingerprint=Capture.Fingerprint;
	for(int32 Index:Capture.MaterialIndices){if(Index==0)++Result.Material0Triangles;else if(Index==1)++Result.Material1Triangles;}
}

static bool IMG1WriteReport(const FString& Directory,const FString& RunId,const FIMG1Source& Source,
	bool bSuccess,const FString& Summary,const TArray<FIMG1Case>& Cases,bool bMapRestored,bool bAssetRestored)
{
	FString Json;auto W=TJsonWriterFactory<>::Create(&Json);
	W->WriteObjectStart();W->WriteValue(TEXT("run_id"),RunId);W->WriteValue(TEXT("map"),IMG1Map);
	W->WriteValue(TEXT("source_identity"),Source.MapIdentity);W->WriteValue(TEXT("baked_asset_identity"),Source.BakeAssetIdentity);
	W->WriteValue(TEXT("bake_asset_identity_storage_basis"),TEXT("Serialized SceneData and ProbeData plus metadata; Format 2 stores raw sections and Format 3 stores packed sections."));
	W->WriteValue(TEXT("mesh"),GetPathNameSafe(Source.Mesh));W->WriteValue(TEXT("base_material"),GetPathNameSafe(Source.BaseMaterial));
	W->WriteValue(TEXT("override_material"),GetPathNameSafe(Source.OverrideMaterial));
	W->WriteValue(TEXT("coordinate_tolerance_meters"),IMG1CoordinateToleranceMeters);
	W->WriteValue(TEXT("final_result"),bSuccess?TEXT("PASS"):TEXT("FAIL"));W->WriteValue(TEXT("summary"),Summary);
	W->WriteValue(TEXT("original_map_identity_restored"),bMapRestored);W->WriteValue(TEXT("original_baked_asset_unchanged"),bAssetRestored);
	W->WriteArrayStart(TEXT("cases"));
	for(const FIMG1Case& C:Cases)
	{
		W->WriteObjectStart();W->WriteValue(TEXT("name"),C.Name);W->WriteValue(TEXT("passed"),C.bPassed);
		W->WriteValue(TEXT("input_instances"),C.InputInstances);W->WriteValue(TEXT("outside_expected"),C.OutsideInstancesExpected);
		W->WriteValue(TEXT("outside_reported"),C.OutsideInstancesReported);W->WriteValue(TEXT("vertices"),C.VertexCount);
		W->WriteValue(TEXT("triangles"),C.TriangleCount);W->WriteValue(TEXT("material_0_triangles"),C.Material0Triangles);
		W->WriteValue(TEXT("material_1_triangles"),C.Material1Triangles);W->WriteValue(TEXT("fingerprint"),C.Fingerprint);
		W->WriteValue(TEXT("geometry_matches_reference"),C.bGeometryMatchesReference);
		W->WriteValue(TEXT("world_transform_expected"),C.bWorldTransformExpected);
		W->WriteValue(TEXT("position_negative_control"),C.bPositionNegativeControl);W->WriteValue(TEXT("details"),C.Details);
		W->WriteObjectEnd();
	}
	W->WriteArrayEnd();W->WriteArrayStart(TEXT("not_covered"));
	W->WriteValue(TEXT("Steam Audio SDK scene commit, bake payload persistence and cold runtime load."));
	W->WriteValue(TEXT("acoustic listening, runtime spatialization and GPU performance."));
	W->WriteArrayEnd();W->WriteObjectEnd();W->Close();
	return IFileManager::Get().MakeDirectory(*Directory,true)
		&&FFileHelper::SaveStringToFile(Json,*FPaths::Combine(Directory,TEXT("report.json")));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(IM_AcousticG1Instances,"IceMoon.AcousticField.G1.InstanceGeometry",
	EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool IM_AcousticG1Instances::RunTest(const FString&)
{
	IMAcousticG1InstanceTestPrivate::FIMG1Fixture Fixture;IMAcousticG1InstanceTestPrivate::FIMG1Source Source;TArray<IMAcousticG1InstanceTestPrivate::FIMG1Case> Cases;
	const FString RunId=FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("AcousticV2/G1Instances"),RunId);
	FString Failure;bool bMapRestored=false,bAssetRestored=false;
	auto Finish=[&](bool bPass,const FString& Summary)
	{
		Fixture.Cleanup();
		if(FEditorFileUtils::LoadMap(IMAcousticG1InstanceTestPrivate::IMG1Map,false,true))
		{
			UWorld* Restored=GEditor?GEditor->GetEditorWorldContext().World():nullptr;
			if(Restored)
			{
				UStaticMesh* Mesh=nullptr;UMaterialInterface* Material=nullptr;int32 Components=0,UniqueMeshes=0;
				bMapRestored=IMAcousticG1InstanceTestPrivate::IMG1MapIdentity(Restored,Mesh,Material,Components,UniqueMeshes)==Fixture.BaselineMapIdentity;
				bAssetRestored=IMAcousticG1InstanceTestPrivate::IMG1BakeAssetIdentity(Restored)==Fixture.BaselineBakeAssetIdentity;
			}
		}
		const bool bCandidate=bPass&&bMapRestored&&bAssetRestored;
		FString FinalSummary=Summary;
		if(!bMapRestored||!bAssetRestored)FinalSummary+=TEXT("; map or baked asset restore verification failed");
		const bool bReportSaved=IMAcousticG1InstanceTestPrivate::IMG1WriteReport(Directory,RunId,Source,bCandidate,FinalSummary,Cases,bMapRestored,bAssetRestored);
		const bool bFinal=bCandidate&&bReportSaved;
		if(!bReportSaved)AddError(TEXT("required G1 report could not be written: ")+FPaths::Combine(Directory,TEXT("report.json")));
		if(!bCandidate)AddError(FinalSummary);
		UE_LOG(LogTemp,Display,TEXT("IMLogs AcousticG1Instances run=%s cases=%d map_restored=%d asset_unchanged=%d %s"),
			*RunId,Cases.Num(),bMapRestored,bAssetRestored,bFinal?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("[IM][PIE_TEST] AcousticG1Instances %s"),bFinal?TEXT("PASS"):TEXT("FAIL"));
		UE_LOG(LogTemp,Display,TEXT("IMExitEditor %s"),bFinal?TEXT("PASS"):TEXT("FAIL"));return bFinal;
	};
	auto Prepare=[&](const TCHAR* Name)->bool
	{
		(void)Name;
		if(!Fixture.BeginCase(Source,Failure)){AddError(Failure);return false;}return true;
	};
	const FTransform Basic(FRotator(0,25,0),FVector(-140,45,35),FVector(1.25,.75,1.5));
	const FTransform Mirrored(FRotator(15,-35,20),FVector(170,-85,50),FVector(-.8,1.2,1.05));
	const FTransform Outside(FRotator::ZeroRotator,FVector(4000,0,0),FVector::OneVector);
	const FTransform WrongLocation(FRotator::ZeroRotator,FVector(0,0,0),FVector::OneVector);
	TArray<FString> ReferenceKeys;IMAcousticG1InstanceTestPrivate::FIMG1Capture ReferenceCapture;
	TArray<IMAcousticG1InstanceTestPrivate::FIMG1ExpectedMesh> ExpectedMeshes;

	if(!Prepare(TEXT("static_reference")))return Finish(false,Failure);
	Fixture.AddStatic(Basic,Source.BaseMaterial);Fixture.AddStatic(Mirrored,Source.OverrideMaterial);
	TMap<UMaterialInterface*,int32> MaterialMap;MaterialMap.Add(Source.BaseMaterial,0);MaterialMap.Add(Source.OverrideMaterial,1);
	ExpectedMeshes={{Basic,Source.BaseMaterial},{Mirrored,Source.OverrideMaterial}};
	if(!Fixture.Capture(ReferenceCapture,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(ReferenceCapture,ReferenceKeys,Failure))return Finish(false,Failure);
	TArray<FString> ExpectedKeys;
	if(!IMAcousticG1InstanceTestPrivate::IMG1BuildExpected(Source.Mesh,FVector::ZeroVector,MaterialMap,ExpectedMeshes,ExpectedKeys,Failure))return Finish(false,Failure);
	IMAcousticG1InstanceTestPrivate::FIMG1Case StaticCase;StaticCase.Name=TEXT("static_reference");IMAcousticG1InstanceTestPrivate::IMG1FillCase(StaticCase,ReferenceCapture);
	StaticCase.bWorldTransformExpected=ReferenceKeys==ExpectedKeys;StaticCase.bGeometryMatchesReference=StaticCase.bWorldTransformExpected;
	TArray<FString> WrongKeys;TArray<IMAcousticG1InstanceTestPrivate::FIMG1ExpectedMesh> WrongExpected={{WrongLocation,Source.BaseMaterial},{WrongLocation,Source.OverrideMaterial}};
	if(!IMAcousticG1InstanceTestPrivate::IMG1BuildExpected(Source.Mesh,FVector::ZeroVector,MaterialMap,WrongExpected,WrongKeys,Failure))return Finish(false,Failure);
	StaticCase.bPositionNegativeControl=ReferenceKeys!=WrongKeys;
	StaticCase.bPassed=StaticCase.bWorldTransformExpected&&StaticCase.bPositionNegativeControl;
	StaticCase.Details=TEXT("two independent SMs; translated/rotated/non-uniform and rotated negative-scale transforms; material override");
	Cases.Add(StaticCase);if(!StaticCase.bPassed)return Finish(false,TEXT("static reference failed expected-world-geometry or position negative control"));

	for(const bool bHierarchical:{false,true})
	{
		if(!Prepare(bHierarchical?TEXT("hism_equivalence"):TEXT("ism_equivalence")))return Finish(false,Failure);
		UInstancedStaticMeshComponent* Base=Fixture.AddInstanced(bHierarchical,Source.BaseMaterial,{Basic,Outside});
		UInstancedStaticMeshComponent* Override=Fixture.AddInstanced(bHierarchical,Source.OverrideMaterial,{Mirrored});
		if(!Base||!Override)return Finish(false,TEXT("could not create transient instanced mesh fixture"));
		IMAcousticG1InstanceTestPrivate::FIMG1Capture Captured;TArray<FString> Keys;
		if(!Fixture.Capture(Captured,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Captured,Keys,Failure))return Finish(false,Failure);
		IMAcousticG1InstanceTestPrivate::FIMG1Case C;C.Name=bHierarchical?TEXT("hism_equivalence"):TEXT("ism_equivalence");C.InputInstances=3;
		C.OutsideInstancesExpected=1;C.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(C,Captured);
		C.bGeometryMatchesReference=Keys==ReferenceKeys;
		C.bWorldTransformExpected=Keys==ExpectedKeys;
		TArray<IMAcousticG1InstanceTestPrivate::FIMG1ExpectedMesh> WrongInstanceExpected={{WrongLocation,Source.BaseMaterial},{WrongLocation,Source.OverrideMaterial}};
		TArray<FString> WrongInstanceKeys;
		if(!IMAcousticG1InstanceTestPrivate::IMG1BuildExpected(Source.Mesh,FVector::ZeroVector,MaterialMap,WrongInstanceExpected,WrongInstanceKeys,Failure))return Finish(false,Failure);
		C.bPositionNegativeControl=Keys!=WrongInstanceKeys;
		C.bPassed=C.bGeometryMatchesReference&&C.bWorldTransformExpected&&C.bPositionNegativeControl
			&&C.OutsideInstancesReported==1&&C.Material0Triangles>0&&C.Material1Triangles>0;
		C.Details=TEXT("one in-region and one out-of-region base-material instance; one in-region component-override instance");
		Cases.Add(C);if(!C.bPassed)return Finish(false,C.Name+TEXT(" did not match the static reference or a required control failed"));

		if(bHierarchical)
		{
			const int32 NewIndex=Base->AddInstance(FTransform(FRotator::ZeroRotator,FVector(350,120,10),FVector::OneVector),true);
			if(NewIndex<0)return Finish(false,TEXT("HISM add-instance mutation failed"));
			IMAcousticG1InstanceTestPrivate::FIMG1Capture Added;TArray<FString> AddedKeys;
			if(!Fixture.Capture(Added,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Added,AddedKeys,Failure))return Finish(false,Failure);
			IMAcousticG1InstanceTestPrivate::FIMG1Case AddedCase;AddedCase.Name=TEXT("hism_add_instance");AddedCase.InputInstances=4;
			AddedCase.OutsideInstancesExpected=1;AddedCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(AddedCase,Added);
			AddedCase.bPassed=Added.Fingerprint!=Captured.Fingerprint&&AddedKeys!=Keys&&AddedCase.TriangleCount==144
				&&AddedCase.OutsideInstancesReported==1;
			AddedCase.Details=TEXT("one in-region HISM instance added; geometry and fingerprint changed");Cases.Add(AddedCase);
			if(!AddedCase.bPassed)return Finish(false,TEXT("adding a HISM instance did not change exported geometry and fingerprint"));
			if(!Base->RemoveInstance(NewIndex))return Finish(false,TEXT("HISM remove-instance mutation failed"));
			IMAcousticG1InstanceTestPrivate::FIMG1Capture Restored;TArray<FString> RestoredKeys;
			if(!Fixture.Capture(Restored,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Restored,RestoredKeys,Failure))return Finish(false,Failure);
			IMAcousticG1InstanceTestPrivate::FIMG1Case RestoredCase;RestoredCase.Name=TEXT("hism_restore_instances");RestoredCase.InputInstances=3;
			RestoredCase.OutsideInstancesExpected=1;RestoredCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(RestoredCase,Restored);
			RestoredCase.bGeometryMatchesReference=RestoredKeys==ReferenceKeys;
			RestoredCase.bPassed=Restored.Fingerprint==Captured.Fingerprint&&RestoredKeys==Keys
				&&RestoredCase.bGeometryMatchesReference&&RestoredCase.OutsideInstancesReported==1;
			RestoredCase.Details=TEXT("removing added HISM instance restored exact geometry and fingerprint");Cases.Add(RestoredCase);
			if(!RestoredCase.bPassed)return Finish(false,TEXT("HISM remove did not restore the original scene fingerprint"));
		}
		if(!bHierarchical)
		{
			Override->SetMaterial(0,Source.BaseMaterial);
			IMAcousticG1InstanceTestPrivate::FIMG1Capture EditedMaterial;TArray<FString> EditedMaterialKeys;
			if(!Fixture.Capture(EditedMaterial,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(EditedMaterial,EditedMaterialKeys,Failure))return Finish(false,Failure);
			IMAcousticG1InstanceTestPrivate::FIMG1Case MaterialCase;MaterialCase.Name=TEXT("ism_component_material_edit");MaterialCase.InputInstances=3;
			MaterialCase.OutsideInstancesExpected=1;MaterialCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(MaterialCase,EditedMaterial);
			MaterialCase.bPassed=EditedMaterial.Fingerprint!=Captured.Fingerprint&&EditedMaterialKeys!=Keys
				&&MaterialCase.TriangleCount==96&&MaterialCase.Material0Triangles==96&&MaterialCase.Material1Triangles==0
				&&MaterialCase.OutsideInstancesReported==1;
			MaterialCase.Details=TEXT("one ISM component material changed after capture; exported material and fingerprint changed");Cases.Add(MaterialCase);
			if(!MaterialCase.bPassed)return Finish(false,TEXT("ISM component material edit did not invalidate the exported scene"));
			Override->SetMaterial(0,Source.OverrideMaterial);
			IMAcousticG1InstanceTestPrivate::FIMG1Capture RestoredMaterial;TArray<FString> RestoredMaterialKeys;
			if(!Fixture.Capture(RestoredMaterial,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(RestoredMaterial,RestoredMaterialKeys,Failure))return Finish(false,Failure);
			IMAcousticG1InstanceTestPrivate::FIMG1Case MaterialRestoredCase;MaterialRestoredCase.Name=TEXT("ism_component_material_restore");MaterialRestoredCase.InputInstances=3;
			MaterialRestoredCase.OutsideInstancesExpected=1;MaterialRestoredCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(MaterialRestoredCase,RestoredMaterial);
			MaterialRestoredCase.bGeometryMatchesReference=RestoredMaterialKeys==ReferenceKeys;
			MaterialRestoredCase.bPassed=RestoredMaterial.Fingerprint==Captured.Fingerprint&&RestoredMaterialKeys==Keys
				&&MaterialRestoredCase.bGeometryMatchesReference&&MaterialRestoredCase.OutsideInstancesReported==1;
			MaterialRestoredCase.Details=TEXT("restoring the ISM component material recovered exact geometry and fingerprint");Cases.Add(MaterialRestoredCase);
			if(!MaterialRestoredCase.bPassed)return Finish(false,TEXT("ISM component material restore did not recover the original scene"));
			IMAcousticG1InstanceTestPrivate::FIMG1Capture Before;TArray<FString> BeforeKeys;
			if(!Fixture.Capture(Before,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Before,BeforeKeys,Failure))return Finish(false,Failure);
			const FString OriginalFingerprint=Before.Fingerprint;
			if(Base->AddInstance(FTransform(FRotator(0,0,0),FVector(350,120,10),FVector::OneVector),true)<0)
				return Finish(false,TEXT("ISM add-instance mutation failed"));
			IMAcousticG1InstanceTestPrivate::FIMG1Capture Added;TArray<FString> AddedKeys;
			if(!Fixture.Capture(Added,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Added,AddedKeys,Failure))return Finish(false,Failure);
			const FString AddedFingerprint=Added.Fingerprint;
			const bool bAddInvalidated=AddedFingerprint!=OriginalFingerprint&&AddedKeys!=BeforeKeys;
			IMAcousticG1InstanceTestPrivate::FIMG1Case AddedCase;AddedCase.Name=TEXT("ism_add_instance");AddedCase.InputInstances=4;AddedCase.OutsideInstancesExpected=1;
			AddedCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(AddedCase,Added);
			AddedCase.bPassed=bAddInvalidated;AddedCase.Details=TEXT("one in-region instance added; geometry and fingerprint both changed");Cases.Add(AddedCase);
			if(!bAddInvalidated)return Finish(false,TEXT("adding an ISM instance did not change exported geometry and fingerprint"));
			const FTransform Moved(Basic.GetRotation(),Basic.GetLocation()+FVector(100,70,30),Basic.GetScale3D());
			const bool bMoveApplied=Base->UpdateInstanceTransform(0,Moved,true,true,true);
			IMAcousticG1InstanceTestPrivate::FIMG1Capture MovedCapture;TArray<FString> MovedKeys;
			if(!bMoveApplied||!Fixture.Capture(MovedCapture,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(MovedCapture,MovedKeys,Failure))
				return Finish(false,bMoveApplied?Failure:TEXT("ISM move-instance mutation failed"));
			const FString MovedFingerprint=MovedCapture.Fingerprint;
			const bool bMoveInvalidated=MovedFingerprint!=AddedFingerprint&&MovedKeys!=AddedKeys;
			IMAcousticG1InstanceTestPrivate::FIMG1Case MovedCase;MovedCase.Name=TEXT("ism_move_instance");MovedCase.InputInstances=4;MovedCase.OutsideInstancesExpected=1;
			MovedCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(MovedCase,MovedCapture);
			MovedCase.bPassed=bMoveInvalidated;MovedCase.Details=TEXT("one existing in-region instance moved; geometry and fingerprint both changed");Cases.Add(MovedCase);
			if(!bMoveInvalidated)return Finish(false,TEXT("moving an ISM instance did not change exported geometry and fingerprint"));
			const bool bRemoved=Base->RemoveInstance(2);
			IMAcousticG1InstanceTestPrivate::FIMG1Capture Removed;TArray<FString> RemovedKeys;
			if(!bRemoved||!Fixture.Capture(Removed,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Removed,RemovedKeys,Failure))
				return Finish(false,bRemoved?Failure:TEXT("ISM remove-instance mutation failed"));
			const FString RemovedFingerprint=Removed.Fingerprint;
			const bool bRemoveInvalidated=RemovedFingerprint!=MovedFingerprint&&RemovedKeys!=MovedKeys;
			IMAcousticG1InstanceTestPrivate::FIMG1Case RemovedCase;RemovedCase.Name=TEXT("ism_remove_instance");RemovedCase.InputInstances=3;RemovedCase.OutsideInstancesExpected=1;
			RemovedCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(RemovedCase,Removed);
			RemovedCase.bPassed=bRemoveInvalidated;RemovedCase.Details=TEXT("new instance removed; geometry and fingerprint both changed");Cases.Add(RemovedCase);
			if(!bRemoveInvalidated)return Finish(false,TEXT("removing an ISM instance did not change exported geometry and fingerprint"));
			if(!Base->UpdateInstanceTransform(0,Basic,true,true,true))return Finish(false,TEXT("ISM transform restore failed"));
			IMAcousticG1InstanceTestPrivate::FIMG1Capture Restored;TArray<FString> RestoredKeys;
			if(!Fixture.Capture(Restored,Failure)||!IMAcousticG1InstanceTestPrivate::IMG1Canonicalize(Restored,RestoredKeys,Failure))return Finish(false,Failure);
			const bool bExactRestore=Restored.Fingerprint==OriginalFingerprint&&RestoredKeys==BeforeKeys;
			IMAcousticG1InstanceTestPrivate::FIMG1Case RestoredCase;RestoredCase.Name=TEXT("ism_restore_instances");RestoredCase.InputInstances=3;
			RestoredCase.OutsideInstancesExpected=1;RestoredCase.OutsideInstancesReported=Fixture.ReportedOutsideInstances();IMAcousticG1InstanceTestPrivate::IMG1FillCase(RestoredCase,Restored);
			RestoredCase.bGeometryMatchesReference=RestoredKeys==ReferenceKeys;RestoredCase.bWorldTransformExpected=bExactRestore;
			RestoredCase.bPassed=bExactRestore&&RestoredCase.bGeometryMatchesReference;
			RestoredCase.Details=FString::Printf(TEXT("restored fingerprint=%s; add=%d move=%d remove=%d exact_restore=%d"),
				*Restored.Fingerprint,bAddInvalidated,bMoveInvalidated,bRemoveInvalidated,bExactRestore);Cases.Add(RestoredCase);
			if(!RestoredCase.bPassed)return Finish(false,TEXT("ISM edits invalidated fingerprints but exact restore did not recover reference geometry"));
		}
	}
	return Finish(true,TEXT("SM/ISM/HISM geometry, transforms, outside filtering, material override and ISM/HISM edit fingerprints passed"));
}
#endif
