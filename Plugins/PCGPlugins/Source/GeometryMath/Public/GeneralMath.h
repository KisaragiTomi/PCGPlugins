// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeneralMath.generated.h"

/**
 * 归约操作枚举，供 Blueprint 层的自定义归约使用
 */
UENUM(BlueprintType)
enum class EReductionCustomOp : uint8
{
	Add            UMETA(DisplayName = "Add (Acc + V)"),
	Subtract       UMETA(DisplayName = "Subtract (Acc - V)"),
	Multiply       UMETA(DisplayName = "Multiply (Acc * V)"),
	Divide         UMETA(DisplayName = "Divide (Acc / V)"),
	Modulo         UMETA(DisplayName = "Modulo (Acc % V)"),
	BitAnd         UMETA(DisplayName = "BitAnd (Acc & V)"),
	BitOr          UMETA(DisplayName = "BitOr (Acc | V)"),
	Xor            UMETA(DisplayName = "Xor (Acc ^ V)"),
	LeftShift      UMETA(DisplayName = "LeftShift (Acc << V)"),
	RightShift     UMETA(DisplayName = "RightShift (Acc >> V)"),
	Max            UMETA(DisplayName = "Max (Acc > V ? Acc : V)"),
	Min            UMETA(DisplayName = "Min (Acc < V ? Acc : V)"),
	Conditional    UMETA(DisplayName = "Conditional (V != 0 ? V : Acc)"),
};

/**
 * 通用数学工具库，提供模板归约与 Blueprint 归约函数
 */
UCLASS()
class GEOMETRYMATH_API UGeneralMath : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	
	UFUNCTION(BlueprintCallable, Category = Noise)
	static void Reduction();

	// ==================== 通用归约 API（C++ 层） ====================
	//
	// 用法示例（详细说明见实现）：
	//
	//   TArray<int32> Values = { 1, 2, 3, 4, 5 };
	//
	//   // 1. Sum: 将所有元素累加
	//   int32 Sum = UGeneralMath::Reduce<int32, int32>(
	//       Values,
	//       [](int32 Acc, int32 V) { return Acc + V; },
	//       0
	//   );
	//
	//   // 2. CountIf: 统计满足条件的元素数量
	//   int32 Count = UGeneralMath::Reduce<int32, int32>(
	//       Values,
	//       [](int32 Acc, int32 V) { return Acc + (V > 2 ? 1 : 0); },
	//       0
	//   );
	//
	//   // 3. Max: 求最大值
	//   int32 Max = UGeneralMath::Reduce<int32, int32>(
	//       Values,
	//       [](int32 Acc, int32 V) { return Acc > V ? Acc : V; },
	//       TNumericLimits<int32>::Lowest()
	//   );
	//
	//   // 4. FindFirst: 返回第一个满足条件的元素（不存在则返回默认值）
	//   int32 First = UGeneralMath::Reduce<int32, int32>(
	//       Values,
	//       [](int32 Acc, int32 V) {
	//           return (Acc != TNumericLimits<int32>::Max()) ? Acc : (V > 3 ? V : TNumericLimits<int32>::Max());
	//       },
	//       TNumericLimits<int32>::Max()
	//   );
	//
	//   // 5. ToSet: 归约到 TSet（Acc 本身是输出容器，每次 lambda 负责 insert）
	//   TSet<int32> UniqueSet = UGeneralMath::Reduce<int32, TSet<int32>>(
	//       Values,
	//       [](TSet<int32> Acc, int32 V) { Acc.Add(V); return Acc; },
	//       TSet<int32>()
	//   );
	//
	//   // 6. Transform+Reduce: 计算平方和
	//   float SumSq = UGeneralMath::Reduce<int32, float>(
	//       Values,
	//       [](float Acc, int32 V) { return Acc + float(V * V); },
	//       0.0f
	//   );
	//
	// 参数说明：
	//   - T      : 输入数组元素类型
	//   - R      : 归约结果/累加器类型（可与 T 相同或不同）
	//   - Input  : 输入数据（TArrayView，支持任意 TArray 兼容类型）
	//   - Lambda : R(Acc, T Element) -> R，自定义归约二元运算
	//   - Init   : 累加器初始值
	//
	// 注意：
	//   - Lambda 应满足结合律以保证结果正确性（尤其在并行场景下）
	//   - 当 Input 为空时，直接返回 Init

	template<typename T, typename R>
	static R Reduce(TArrayView<const T> Input, TFunctionRef<R(R, T)> Lambda, R Init)
	{
		R Acc = MoveTemp(Init);
		for (const T& Elem : Input)
			Acc = Lambda(Acc, Elem);
		return Acc;
	}

	// ==================== BlueprintCallable 具体化 ====================

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Sum"))
	static int32 Reduce_Int32Sum(const TArray<int32>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Float Sum"))
	static float Reduce_FloatSum(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Double Sum"))
	static double Reduce_DoubleSum(const TArray<double>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Max"))
	static int32 Reduce_Int32Max(const TArray<int32>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Float Max"))
	static float Reduce_FloatMax(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Min"))
	static int32 Reduce_Int32Min(const TArray<int32>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Float Min"))
	static float Reduce_FloatMin(const TArray<float>& Values);

	UFUNCTION(BlueprintCallable, Category = "Math|Reduction", meta = (DisplayName = "Reduce Int32 Custom"))
	static int32 Reduce_Int32Custom(
		const TArray<int32>& Values,
		EReductionCustomOp Operation,
		int32 InitValue = 0
	);

	// ==================== 仅限 C++ 的通用 API（含 lambda）====================

	// 统计满足条件的元素数量（仅 C++，支持任意 lambda）
	template<typename T>
	static int32 CountIf(TArrayView<const T> Input, TFunctionRef<bool(T)> Predicate)
	{
		return Reduce<T, int32>(Input, [&Predicate](int32 Acc, T V) {
			return Acc + (Predicate(V) ? 1 : 0);
		}, 0);
	}

	// 自定义 lambda 归约（仅 C++）
	template<typename T, typename R>
	static R Reduce_Custom(TArrayView<const T> Input, TFunctionRef<R(R, T)> Lambda, R Init)
	{
		return Reduce<T, R>(Input, Lambda, MoveTemp(Init));
	}

	template<typename T, typename R>
	static R Reduce_Custom(const TArray<T>& Input, TFunctionRef<R(R, T)> Lambda, R Init)
	{
		return Reduce_Custom<T, R>(TArrayView<const T>(Input), Lambda, MoveTemp(Init));
	}
	


};
