#include "GeneralMath.h"

// ==================== BlueprintCallable 具体化实现 ====================

int32 UGeneralMath::Reduce_Int32Sum(const TArray<int32>& Values)
{
	return Reduce<int32, int32>(Values, [](int32 Acc, int32 V) { return Acc + V; }, 0);
}

float UGeneralMath::Reduce_FloatSum(const TArray<float>& Values)
{
	return Reduce<float, float>(Values, [](float Acc, float V) { return Acc + V; }, 0.0f);
}

double UGeneralMath::Reduce_DoubleSum(const TArray<double>& Values)
{
	return Reduce<double, double>(Values, [](double Acc, double V) { return Acc + V; }, 0.0);
}

int32 UGeneralMath::Reduce_Int32Max(const TArray<int32>& Values)
{
	return Reduce<int32, int32>(Values, [](int32 Acc, int32 V) { return (Acc > V) ? Acc : V; }, TNumericLimits<int32>::Lowest());
}

float UGeneralMath::Reduce_FloatMax(const TArray<float>& Values)
{
	return Reduce<float, float>(Values, [](float Acc, float V) { return (Acc > V) ? Acc : V; }, -TNumericLimits<float>::Max());
}

int32 UGeneralMath::Reduce_Int32Min(const TArray<int32>& Values)
{
	return Reduce<int32, int32>(Values, [](int32 Acc, int32 V) { return (Acc < V) ? Acc : V; }, TNumericLimits<int32>::Max());
}

float UGeneralMath::Reduce_FloatMin(const TArray<float>& Values)
{
	return Reduce<float, float>(Values, [](float Acc, float V) { return (Acc < V) ? Acc : V; }, TNumericLimits<float>::Max());
}

int32 UGeneralMath::Reduce_Int32Custom(const TArray<int32>& Values, EReductionCustomOp Operation, int32 InitValue)
{
	auto Op = [Operation](int32 Acc, int32 V) -> int32 {
		switch (Operation)
		{
		case EReductionCustomOp::Add:         return Acc + V;
		case EReductionCustomOp::Subtract:    return Acc - V;
		case EReductionCustomOp::Multiply:   return Acc * V;
		case EReductionCustomOp::Divide:      return (V != 0) ? Acc / V : Acc;
		case EReductionCustomOp::Modulo:      return (V != 0) ? Acc % V : Acc;
		case EReductionCustomOp::BitAnd:      return Acc & V;
		case EReductionCustomOp::BitOr:       return Acc | V;
		case EReductionCustomOp::Xor:         return Acc ^ V;
		case EReductionCustomOp::LeftShift:   return Acc << V;
		case EReductionCustomOp::RightShift:   return Acc >> V;
		case EReductionCustomOp::Max:         return (Acc > V) ? Acc : V;
		case EReductionCustomOp::Min:         return (Acc < V) ? Acc : V;
		case EReductionCustomOp::Conditional: return (V != 0) ? V : Acc;
		default:                              return Acc;
		}
	};
	return Reduce<int32, int32>(Values, Op, InitValue);
}

// ==================== Blueprint 占位函数 ====================

void UGeneralMath::Reduction()
{
	// 保留原始声明；核心实现在模板 Reduce<> 和具体化函数中
}
