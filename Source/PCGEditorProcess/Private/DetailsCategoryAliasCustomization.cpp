#include "DetailsCategoryAliasCustomization.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Internationalization/Text.h"      // FTextBuilder
#include "ObjectEditorUtils.h"
#include "ObjectTools.h"                    // GetUserFacingFunctionName
#include "PropertyCustomizationHelpers.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Script.h"                 // FEditorScriptExecutionGuard
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Layout/SWrapBox.h"

namespace
{
/**
 * 分类名的一段去掉空白与下划线（大小写由调用方忽略）。
 *
 * 为什么要这样放宽：蓝图 My Blueprint 面板按**显示名**给成员分组 —— FEditorCategoryUtils::GetCategoryDisplayString
 * 把 "ViewEdit" 显示成 "View Edit"，两种写法在那里是同一个分类节点，肉眼分不出来；而把成员拖到分类上、
 * 在分类里粘贴、改分类名，写回去的都是这个显示名。Details 面板却按原样 FName 分组，两种写法各成一个分类。
 * FName::NameToDisplayString 把下划线也换成空格，所以 "View_Edit" 显示出来同样是 "View Edit"，一并算同一个。
 */
FString NormalizeCategorySegment(FStringView Segment)
{
	FString Result;
	Result.Reserve(Segment.Len());
	for (const TCHAR Char : Segment)
	{
		if (FChar::IsWhitespace(Char) || Char == TEXT('_')) continue;
		Result.AppendChar(Char);
	}
	return Result;
}

FStringView GetRootCategorySegment(FStringView CategoryPath)
{
	int32 DelimiterIndex = INDEX_NONE;
	return CategoryPath.FindChar(TEXT('|'), DelimiterIndex) ? CategoryPath.Left(DelimiterIndex) : CategoryPath;
}

bool IsRootLevelCategory(FName CategoryName)
{
	int32 DelimiterIndex = INDEX_NONE;
	return !CategoryName.ToString().FindChar(TEXT('|'), DelimiterIndex);
}

/** 与引擎给 CallInEditor 按钮排序时同一种解析：缺省或写成非数字都排在最后。 */
int32 GetDisplayPriority(const UFunction& Function)
{
	static const FName NAME_DisplayPriority(TEXT("DisplayPriority"));
	const FString PriorityString = Function.GetMetaData(NAME_DisplayPriority);
	if (PriorityString.IsEmpty()) return MAX_int32;

	const int32 Priority = FCString::Atoi(*PriorityString);
	return (Priority == 0 && !FCString::IsNumeric(*PriorityString)) ? MAX_int32 : Priority;
}

/** 与引擎 CallInEditor 按钮的执行同一套：一个事务、放开编辑器里的脚本执行、逐个对象调。 */
FReply ExecuteCallInEditorFunction(UFunction* Function, const TArray<TWeakObjectPtr<UObject>>& Objects)
{
	if (!Function) return FReply::Handled();

	FScopedTransaction Transaction(ObjectTools::GetUserFacingFunctionName(Function));
	TStrongObjectPtr<UFunction> CallingFunction(Function);
	FEditorScriptExecutionGuard ScriptGuard;
	for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
	{
		UObject* Object = WeakObject.Get();
		if (!Object) continue;

		TStrongObjectPtr<UObject> StrongObject(Object); // 调用期间防 GC
		Object->ProcessEvent(Function, nullptr);
	}
	return FReply::Handled();
}
}

bool FDetailsCategoryAliasCustomization::DoesCategoryMatch(FName CandidateCategoryName, FName RequiredCategoryName)
{
	if (CandidateCategoryName.IsNone() || RequiredCategoryName.IsNone()) return false;

	const FString Candidate = CandidateCategoryName.ToString();
	const FString Required = RequiredCategoryName.ToString();
	const FString NormalizedRequired = NormalizeCategorySegment(GetRootCategorySegment(Required));
	return !NormalizedRequired.IsEmpty()
		&& NormalizeCategorySegment(GetRootCategorySegment(Candidate)).Equals(NormalizedRequired, ESearchCase::IgnoreCase);
}

void FDetailsCategoryAliasCustomization::GetCallInEditorFunctions(const UClass* Class, FName CategoryName, TArray<UFunction*>& OutFunctions)
{
	if (!Class) return;

	PropertyCustomizationHelpers::GetCallInEditorFunctionsForClass(
		Class,
		[Class, CategoryName](const UFunction* Function)
		{
			return !FObjectEditorUtils::IsFunctionHiddenFromClass(Function, Class)
				&& DoesCategoryMatch(FObjectEditorUtils::GetCategoryFName(Function), CategoryName);
		},
		OutFunctions);
}

void FDetailsCategoryAliasCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// 先出按钮：分类里的 custom 行按添加顺序排在默认属性之前，按钮行因此留在最上面（与引擎一致）。
	AddCallInEditorButtons(DetailBuilder);
	MergeAliasedPropertyCategories(DetailBuilder);
}

void FDetailsCategoryAliasCustomization::AddCallInEditorButtons(IDetailLayoutBuilder& DetailBuilder) const
{
	TArray<UFunction*> Functions;
	GetCallInEditorFunctions(DetailBuilder.GetBaseClass(), CategoryName, Functions);
	if (Functions.IsEmpty()) return;

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	Objects.RemoveAllSwap([](const TWeakObjectPtr<UObject>& Object) { return !Object.IsValid() || Object->HasAnyFlags(RF_ArchetypeObject); });
	if (Objects.IsEmpty()) return;

	// 引擎按原样分类排序；这里换成合并后的分类再排，别名写法的函数才会按 DisplayPriority / 名字真正混排在一起。
	Functions.Sort([this](const UFunction& A, const UFunction& B)
	{
		const int32 CategoryOrder = GetButtonCategory(&A).Compare(GetButtonCategory(&B));
		if (CategoryOrder != 0) return CategoryOrder < 0;

		const int32 PriorityA = GetDisplayPriority(A);
		const int32 PriorityB = GetDisplayPriority(B);
		return PriorityA != PriorityB ? PriorityA < PriorityB : A.GetName() < B.GetName();
	});

	const FPropertyFunctionCallArgs::FOnExecute OnExecute = FPropertyFunctionCallArgs::FOnExecute::CreateLambda(
		[Objects](TWeakObjectPtr<UFunction> WeakFunction) { return ExecuteCallInEditorFunction(WeakFunction.Get(), Objects); });

	for (int32 RowStart = 0; RowStart < Functions.Num();)
	{
		const FName RowCategory = GetButtonCategory(Functions[RowStart]);
		int32 RowEnd = RowStart + 1;
		while (RowEnd < Functions.Num() && GetButtonCategory(Functions[RowEnd]) == RowCategory) ++RowEnd;

		// PreferredSize 与引擎同一个权宜：放在滚动框里时，首帧 tick 前 SWrapBox 按 preferred 尺寸排版，
		// 设小了会换行过多、向滚动框多要高度，直到被滚动一下才纠正。
		const TSharedRef<SWrapBox> WrapBox = SNew(SWrapBox).PreferredSize(2000.0f).UseAllottedSize(true);
		FTextBuilder SearchText;
		for (int32 Index = RowStart; Index < RowEnd; ++Index)
		{
			UFunction* Function = Functions[Index];
			const FText Label = ObjectTools::GetUserFacingFunctionName(Function);
			const FText ToolTip = Function->GetToolTipText();
			WrapBox->AddSlot()
			.Padding(0.0f, 0.0f, 5.0f, 3.0f)
			[
				PropertyCustomizationHelpers::MakeFunctionCallButton(
					FPropertyFunctionCallArgs(Function, OnExecute, {}, Label, ToolTip.IsEmpty() ? Label : ToolTip, &SearchText))
			];
		}

		DetailBuilder.EditCategory(RowCategory).AddCustomRow(SearchText.ToText())
		.RowTag(Functions[RowEnd - 1]->GetFName())
		[
			WrapBox
		];

		RowStart = RowEnd;
	}
}

void FDetailsCategoryAliasCustomization::MergeAliasedPropertyCategories(IDetailLayoutBuilder& DetailBuilder) const
{
	TArray<FName> CategoryNames;
	DetailBuilder.GetCategoryNames(CategoryNames);

	const TArray<FName> AliasNames = CategoryNames.FilterByPredicate([this](FName Name)
	{
		return Name != CategoryName && IsRootLevelCategory(Name) && DoesCategoryMatch(Name, CategoryName);
	});
	if (AliasNames.IsEmpty()) return;

	// AddProperty 出来的是 custom 行，排在默认行之前。规范分类自己的属性也按原顺序重挂一遍，
	// 结果仍是「规范写法的在前、别名的接在后」，而不是别名的反插到前面。
	IDetailCategoryBuilder& TargetCategory = DetailBuilder.EditCategory(CategoryName);
	TArray<TSharedRef<IPropertyHandle>> Handles;
	TargetCategory.GetDefaultProperties(Handles);
	for (const FName AliasName : AliasNames) DetailBuilder.EditCategory(AliasName).GetDefaultProperties(Handles);

	// 子分类节点没有 FProperty：留在原处（见头文件说明）。
	Handles.RemoveAll([](const TSharedRef<IPropertyHandle>& Handle) { return Handle->GetProperty() == nullptr; });
	for (const TSharedRef<IPropertyHandle>& Handle : Handles) TargetCategory.AddProperty(Handle);
}

FName FDetailsCategoryAliasCustomization::GetButtonCategory(const UFunction* Function) const
{
	const FName FunctionCategory = FObjectEditorUtils::GetCategoryFName(Function);
	return IsRootLevelCategory(FunctionCategory) ? CategoryName : FunctionCategory;
}
