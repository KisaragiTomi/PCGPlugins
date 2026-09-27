#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class UClass;
class UFunction;

/**
 * 把「写法不同的同一个分类」当成一个分类：只在 ViewEdit 浮窗自己的 details view 里注册在 UObject 上，
 * 顶替引擎的 FObjectDetails（Level Editor 的 Details 面板照旧）。
 *
 * 引擎按分类的原样 FName 分组（FName 只忽略大小写），所以 "ViewEdit" 与 "View Edit" 在 Details 里是两个
 * 分类、标题却都显示成 "View Edit"；CallInEditor 按钮也按原样分类各拼一行。这里做两件事：
 *   · 属性：别名写法的根级分类里的默认属性挪进规范分类，接在规范分类自己的属性后面；
 *   · 按钮：按规范分类收集 CallInEditor 函数、拼成一行 —— FObjectDetails 在这个 view 里不再跑，不会重复出按钮。
 *
 * 子分类（"View Edit|Sub"）不合并：子分类节点的内容挂在引擎私有的 SubCategoryMap 上，跟着挪要改引擎数据。
 * 它们仍按引擎原样落在别名分类下，靠浮窗过滤用的 DoesCategoryMatch 兜底显示出来。
 */
class FDetailsCategoryAliasCustomization : public IDetailCustomization
{
public:
	explicit FDetailsCategoryAliasCustomization(FName InCategoryName) : CategoryName(InCategoryName) {}

	/**
	 * Candidate 是否是 Required 分类本身或其子分类（"A|B" 只看根段 "A"）。
	 * 比较前去掉空白与下划线、忽略大小写 —— 为什么这样放宽见 .cpp 的 NormalizeCategorySegment。
	 */
	static bool DoesCategoryMatch(FName CandidateCategoryName, FName RequiredCategoryName);

	/** 与 FObjectDetails 同一套筛选（CallInEditor、无参数、权限表、未被类隐藏）里，分类匹配 CategoryName 的函数。 */
	static void GetCallInEditorFunctions(const UClass* Class, FName CategoryName, TArray<UFunction*>& OutFunctions);

	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	void AddCallInEditorButtons(IDetailLayoutBuilder& DetailBuilder) const;
	void MergeAliasedPropertyCategories(IDetailLayoutBuilder& DetailBuilder) const;

	/** 函数的按钮落在哪个分类：根级别名一律并到 CategoryName，子分类保持引擎原样。 */
	FName GetButtonCategory(const UFunction* Function) const;

	FName CategoryName;
};
