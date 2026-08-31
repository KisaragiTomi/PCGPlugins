#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSSplineBlockActor.h"
#include "Math/RandomStream.h"

// -----------------------------------------------------------------------------
// SolveBlockLayout 纯 CPU 用例：不碰 RHI / world，只钉排布数学 ——
// 恰好整除、任意长度贴合、两候选的 |log| 取舍、固定种子可复现。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSSplineBlock_ prefix
// (与实现 TU 可能进同一个 unity blob，名字还须与 CSSplineBlockActor.cpp 内的不同)。

/** 序列按 scale 缩放后的总长：(Σ块长 + (n-1)×gap) × scale —— 与实现的 pitch 语义一致。 */
float CSSplineBlock_TestScaledTotal(const TArray<int32>& Sequence, const TArray<float>& PaletteLengths, float Gap, float Scale)
{
	float Sum = 0.0f;
	for (const int32 Index : Sequence) Sum += PaletteLengths[Index];
	if (Sequence.Num() > 1) Sum += Gap * float(Sequence.Num() - 1);
	return Sum * Scale;
}
}

// -----------------------------------------------------------------------------
// 恰好整除 → scale == 1；无效输入 → scale == 0 且序列为空
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSSplineBlockLayoutExactFitTest,
	"PCGPlugins.ComputeShaderGenerator.SplineBlock.LayoutExactFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSSplineBlockLayoutExactFitTest::RunTest(const FString& Parameters)
{
	// 4 × 100 恰好铺满 400：任何压缩/拉伸都是多余的，scale 必须为 1（容差）。
	{
		const TArray<float> Lengths = { 100.0f };
		FRandomStream Rand(3);
		TArray<int32> Sequence;
		const float Scale = ACSSplineBlockActor::SolveBlockLayout(400.0f, 0.0f, Lengths, Rand, Sequence);
		TestEqual(TEXT("Exact fit keeps four blocks"), Sequence.Num(), 4);
		TestTrue(TEXT("Exact fit scale is 1"), FMath::IsNearlyEqual(Scale, 1.0f, 1.0e-4f));
	}

	// 带 gap 的恰好整除：3 块 100 + 2 个 gap 50 = 400（gap 只算块与块之间）。
	{
		const TArray<float> Lengths = { 100.0f };
		FRandomStream Rand(3);
		TArray<int32> Sequence;
		const float Scale = ACSSplineBlockActor::SolveBlockLayout(400.0f, 50.0f, Lengths, Rand, Sequence);
		TestEqual(TEXT("Gapped exact fit keeps three blocks"), Sequence.Num(), 3);
		TestTrue(TEXT("Gapped exact fit scale is 1"), FMath::IsNearlyEqual(Scale, 1.0f, 1.0e-4f));
	}

	// 无效输入：零长样条 / 空 palette / 全非正长度 → 0 缩放 + 空序列（调用方以此清空显示）。
	{
		FRandomStream Rand(3);
		TArray<int32> Sequence;
		TestEqual(TEXT("Zero spline length yields no layout"),
			ACSSplineBlockActor::SolveBlockLayout(0.0f, 0.0f, { 100.0f }, Rand, Sequence), 0.0f);
		TestEqual(TEXT("Zero spline length yields empty sequence"), Sequence.Num(), 0);
		TestEqual(TEXT("Empty palette yields no layout"),
			ACSSplineBlockActor::SolveBlockLayout(500.0f, 0.0f, TArray<float>(), Rand, Sequence), 0.0f);
		TestEqual(TEXT("All-invalid palette yields no layout"),
			ACSSplineBlockActor::SolveBlockLayout(500.0f, 0.0f, { 0.0f, -10.0f }, Rand, Sequence), 0.0f);
		TestEqual(TEXT("All-invalid palette yields empty sequence"), Sequence.Num(), 0);
	}
	return true;
}

// -----------------------------------------------------------------------------
// 任意长度：缩放后总长贴合 TotalLength（容差 1e-3）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSSplineBlockLayoutTotalMatchesTest,
	"PCGPlugins.ComputeShaderGenerator.SplineBlock.LayoutTotalMatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSSplineBlockLayoutTotalMatchesTest::RunTest(const FString& Parameters)
{
	// 非整除目标 × 多组 gap/seed：无论候选 A/B 谁胜出，缩放后的总长都必须钉在目标上 ——
	// 这是"整体缩放到恰好达到 spline 长度"的定义本身。
	const TArray<float> Lengths = { 70.0f, 110.0f, 90.0f };
	const float GapValues[] = { 0.0f, 15.0f };
	const float Targets[] = { 777.77f, 1234.5f, 95.0f, 61.3f };
	for (const float GapValue : GapValues)
	{
		for (const float Target : Targets)
		{
			for (int32 SeedValue = 0; SeedValue < 8; ++SeedValue)
			{
				FRandomStream Rand(SeedValue);
				TArray<int32> Sequence;
				const float Scale = ACSSplineBlockActor::SolveBlockLayout(Target, GapValue, Lengths, Rand, Sequence);
				if (!TestTrue(TEXT("Layout yields a sequence"), Sequence.Num() > 0)) return false;
				for (const int32 Index : Sequence) TestTrue(TEXT("Sequence indices stay inside the palette"), Lengths.IsValidIndex(Index));

				const float ScaledTotal = CSSplineBlock_TestScaledTotal(Sequence, Lengths, GapValue, Scale);
				TestTrue(FString::Printf(TEXT("Scaled total %.4f matches target %.4f (gap %.1f seed %d)"), ScaledTotal, Target, GapValue, SeedValue),
					FMath::IsNearlyEqual(ScaledTotal, Target, 1.0e-3f));
			}
		}
	}
	return true;
}

// -----------------------------------------------------------------------------
// 两候选取舍：|log(scale)| 较小者胜；单块序列强制候选 A
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSSplineBlockLayoutCandidateChoiceTest,
	"PCGPlugins.ComputeShaderGenerator.SplineBlock.LayoutCandidateChoice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSSplineBlockLayoutCandidateChoiceTest::RunTest(const FString& Parameters)
{
	// 单条目 palette 让贪心序列与两候选完全可预测：越界总发生在 3 块（300）对 2 块（200）之间。
	const TArray<float> Lengths = { 100.0f };

	// 目标 250：压缩 3 块（250/300，|log|≈0.182）优于拉伸 2 块（250/200，|log|≈0.223）→ 选 A。
	{
		FRandomStream Rand(0);
		TArray<int32> Sequence;
		const float Scale = ACSSplineBlockActor::SolveBlockLayout(250.0f, 0.0f, Lengths, Rand, Sequence);
		TestEqual(TEXT("250 keeps the overshooting block"), Sequence.Num(), 3);
		TestTrue(TEXT("250 compresses to 250/300"), FMath::IsNearlyEqual(Scale, 250.0f / 300.0f, 1.0e-4f));
	}

	// 目标 230：拉伸 2 块（230/200，|log|≈0.140）优于压缩 3 块（230/300，|log|≈0.266）→ 选 B。
	{
		FRandomStream Rand(0);
		TArray<int32> Sequence;
		const float Scale = ACSSplineBlockActor::SolveBlockLayout(230.0f, 0.0f, Lengths, Rand, Sequence);
		TestEqual(TEXT("230 drops the overshooting block"), Sequence.Num(), 2);
		TestTrue(TEXT("230 stretches to 230/200"), FMath::IsNearlyEqual(Scale, 230.0f / 200.0f, 1.0e-4f));
	}

	// 目标短于最小块长：序列只有一块，没有候选 B，强制单块压缩。
	{
		FRandomStream Rand(0);
		TArray<int32> Sequence;
		const float Scale = ACSSplineBlockActor::SolveBlockLayout(30.0f, 0.0f, Lengths, Rand, Sequence);
		TestEqual(TEXT("Short target forces a single block"), Sequence.Num(), 1);
		TestTrue(TEXT("Single block compresses to 30/100"), FMath::IsNearlyEqual(Scale, 0.3f, 1.0e-4f));
	}
	return true;
}

// -----------------------------------------------------------------------------
// 固定种子可复现：OnConstruction 高频重建依赖这一点不闪变
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSSplineBlockLayoutDeterministicTest,
	"PCGPlugins.ComputeShaderGenerator.SplineBlock.LayoutDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSSplineBlockLayoutDeterministicTest::RunTest(const FString& Parameters)
{
	const TArray<float> Lengths = { 50.0f, 80.0f, 120.0f };

	FRandomStream RandA(777);
	TArray<int32> SequenceA;
	const float ScaleA = ACSSplineBlockActor::SolveBlockLayout(1000.0f, 7.0f, Lengths, RandA, SequenceA);

	FRandomStream RandB(777);
	TArray<int32> SequenceB;
	const float ScaleB = ACSSplineBlockActor::SolveBlockLayout(1000.0f, 7.0f, Lengths, RandB, SequenceB);

	TestTrue(TEXT("Same seed yields a sequence"), SequenceA.Num() > 0);
	TestEqual(TEXT("Same seed reproduces the scale"), ScaleA, ScaleB);
	TestTrue(TEXT("Same seed reproduces the sequence"), SequenceA == SequenceB);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
