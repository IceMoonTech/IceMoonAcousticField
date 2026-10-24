#pragma once

#include "CoreMinimal.h"

// 夸张临场感的全部全局参数集中在这里。参数与素材、材质、声源无关：
// 木、石和以后新增的任何声源都套同一组，不允许按材质另调混响。
// 夸张只改变“直达方向”“湿声发送”“湿声宽度”，素材本身的音色来自素材，不再被处理改写。
//
// 夸张强度 S（IM.AcousticField.PresenceStrength，默认 1；AIMAcousticFootstepCharacter::PresenceStrength 驱动同一个变量）：
// 下面四个常量是 S = 1 的取值，2026-10-22 用户验收第三轮夸张版（"有点夸张但没问题，夸张强弱可调就行"），来源见各常量注释。
// 其余强度由 *At(S) 函数映射，S = 0 即物理路径（不整形方位、湿声 +0 dB、侧通道增益 1），S = 1 逐值等于验收版：
//   湿声发送额外增益  dB = WetSendExtraDb * S                       （dB 域线性）
//   湿声侧通道增益        = 1 + (WetSideGain - 1) * S                （线性）
//   直达侧向方位倍数      = 1 + (DirectAzimuthScale - 1) * S         （只放大脚相对身体中轴的侧向方位，随 S 线性、连续、单调）
//   方位上限（度）        = S <= 1 ? DirectAzimuthLimitDegrees : 60 + (90 - 60) * (S - 1)
// S > 1 时方位上限随 S 从 60 度线性放宽到 90 度，否则侧向方位在 S 约 1.3 就被 60 度截断，S 再增大只剩湿声变化；
// 90 度是前半球边界（ScaleAzimuth 不处理身后声源），不是听感阈值。S = 0 时方位倍数为 1，ScaleAzimuth 直接返回原方向（不重算三角函数）。
// S 的 0..2 范围只是控制台/Details 的使用范围，不是安全护栏；越界输入被夹到该范围，非有限值回到 1。
// 物理声源（没有 Exaggerated 标志）在任何 S 下都不受影响。SelfNearField（自身脚步去掉距离衰减并对湿声发送做同距离补偿）
// 不属于夸张强度，S = 0 时仍保留，因此 S = 0 与物理版的绝对声级不同，直达/混响比和空间特征相同。
struct FIMAcousticPresenceParameters
{
	// 直达声里“脚相对身体中轴的侧向方位”的放大倍数。听者在角色身后约 3 m，左右脚各偏 13 cm，
	// 该侧向方位仅约 +-2.3 度，HRTF 几乎没有左右差。上一版整体方位角乘 6（到 +-15 度），实测
	// 全频段声级差只有约 1 dB，高频约 4 dB，仍然太弱；这里取 20（到约 +-46 度）。
	// 20 没有测量来源，是“夸张到左右明显可辨”的试听档，最终由用户听感决定；只放大侧向部分，
	// 听者转向或快照滞后造成的整体方位偏移不随之放大。
	// S = 1 取值，用户验收 2026-10-22（第三轮）。
	static constexpr float DirectAzimuthScale = 20.f;
	// 放大后方位角的上限（度），S = 1 取值，用户验收 2026-10-22（第三轮）。60 度仍在前半球内，不把脚步推到身体正侧面或后方；同样是试听档。
	static constexpr float DirectAzimuthLimitDegrees = 60.f;
	// 湿声发送相对物理值的额外增益（dB）。自身近场去掉距离衰减后，湿声发送同步
	// 去掉同一距离衰减以保持物理版的直达/混响比；此处只在其上再加一点房间感。
	// S = 1 取值 6：2026-10-21 第三轮在同一条录制路线上扫 3/6/9/12 dB（当时用的临时 CVar IM.AcousticField.PresenceWetSendExtraDb 已并入 PresenceStrength，Saved/ClaudeRuntime/AcousticV2Footstep_20261021/
	// r3/IMSweepAnalysis.json），60 Hz 以上混响峰值相对脚步直达峰值：3 dB 为 -5.8..-7.1，6 dB 为 -3.3..-4.1，9 dB 为 +0.1..-1.2，12 dB 为 +1.6..+3.4
	// （室内三个空间）。6 是混响峰值仍低于脚步直达峰值的最大扫描值；9 以上混响与脚步同级甚至更响。不是听感阈值，最终由用户听感决定。
	static constexpr float WetSendExtraDb = 6.f;
	// 湿声（卷积混响输出）的侧通道增益。只作用于湿声的 mid/side，不碰直达声。
	// 1 为不变；1.8 约把湿声侧能量提高 5 dB。
	static constexpr float WetSideGain = 1.8f;
	// S > 1 时方位上限的终点（度），见文件头映射。
	static constexpr float DirectAzimuthLimitMaxDegrees = 90.f;
	// 控制台/Details 的强度范围上限（使用范围，不是安全护栏）。
	static constexpr float StrengthMax = 2.f;

	static float ClampStrength(float Strength)
	{
		return FMath::IsFinite(Strength) ? FMath::Clamp(Strength, 0.f, StrengthMax) : 1.f;
	}
	static float WetSendExtraDbAt(float Strength) { return WetSendExtraDb * Strength; }
	static float WetSideGainAt(float Strength) { return 1.f + (WetSideGain - 1.f) * Strength; }
	static float DirectAzimuthScaleAt(float Strength) { return 1.f + (DirectAzimuthScale - 1.f) * Strength; }
	static float DirectAzimuthLimitDegreesAt(float Strength)
	{
		return Strength <= 1.f ? DirectAzimuthLimitDegrees
			: DirectAzimuthLimitDegrees + (DirectAzimuthLimitMaxDegrees - DirectAzimuthLimitDegrees) * (Strength - 1.f);
	}
};

// 位标志：每个声源槽位由 GT 发布，音频线程只读。
namespace IMAcousticPresenceFlags
{
	// 方位放大、湿声发送加成、湿声加宽。
	constexpr uint8 Exaggerated = 1;
	// 玩家自身脚步：直达声不再按听者距离衰减；方向仍来自 HRTF，不改成中置声像。
	constexpr uint8 SelfNearField = 2;
}
