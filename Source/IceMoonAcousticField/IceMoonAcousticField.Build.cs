// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class IceMoonAcousticField : ModuleRules
{
	public IceMoonAcousticField(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		Type = ModuleType.CPlusPlus;
		CppStandard = CppStandardVersion.Cpp20;
		PrivateDefinitions.Add("METASOUND_PLUGIN=IceMoonAcousticField");
		PrivateDefinitions.Add("METASOUND_MODULE=IceMoonAcousticField");
		PrivateDependencyModuleNames.Add("SignalProcessing");
		// Pin the upstream SDK; no dependency on the deprecated engine integration.
		string SteamAudio = Path.GetFullPath(Path.Combine(ModuleDirectory, "../ThirdParty/SteamAudio"));
		PublicSystemIncludePaths.Add(Path.Combine(SteamAudio, "include"));
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicAdditionalLibraries.Add(Path.Combine(SteamAudio, "lib/windows-x64/phonon.lib"));
			PublicDelayLoadDLLs.Add("phonon.dll");
			foreach (string Dll in new[] { "phonon.dll", "GPUUtilities.dll", "TrueAudioNext.dll" })
			{
				RuntimeDependencies.Add("$(PluginDir)/Binaries/ThirdParty/SteamAudio/Win64/" + Dll,
					Path.Combine(SteamAudio, "lib/windows-x64", Dll));
			}
		}
		PublicDependencyModuleNames.Add("AudioExtensions");
		PrivateDependencyModuleNames.AddRange(new[] { "AudioMixer", "Projects", "RenderCore", "RHI", "AssetRegistry", "MetasoundEngine", "MetasoundFrontend", "MetasoundGraphCore" });
		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);
				
		
		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Engine",
				"PhysicsCore",
				"IM_Common",
				
				// ... add other public dependencies that you statically link with here ...
			}
			);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
					"PhysicsCore",
					"Json",
					"JsonUtilities",
				"IM_Common",
				"IceMoonBlueprintGPUMathUtilities"
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
		// 添加编辑器模块依赖
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",           // 核心编辑器功能
				"LevelEditor",        // 可选,如果需要更多编辑器功能
				"AudioEditor"         // USoundFactory 导入固定参考声
			});
		}
	}
}
