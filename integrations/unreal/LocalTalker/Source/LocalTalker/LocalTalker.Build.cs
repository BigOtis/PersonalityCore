using UnrealBuildTool;
public class LocalTalker : ModuleRules {
 public LocalTalker(ReadOnlyTargetRules Target) : base(Target) {
  PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
  PublicDependencyModuleNames.AddRange(new string[]{"Core","CoreUObject","Engine","DeveloperSettings"});
  PrivateDependencyModuleNames.AddRange(new string[]{"HTTP","Json","JsonUtilities","WebSockets","AudioCaptureCore","AudioMixer","SignalProcessing"});
 }
}
