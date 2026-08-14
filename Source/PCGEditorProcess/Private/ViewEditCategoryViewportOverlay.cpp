#include "ViewEditCategoryViewportOverlay.h"

bool FViewEditCategoryViewportOverlay::SupportsActor(const AActor* Actor) const
{
	// 不按类型筛。真正的门槛是基类按 GetRequiredDetailsCategoryName() 检查 actor 有没有
	// ViewEdit 分类 —— 有就浮出来，没有就不显示。
	return Actor != nullptr;
}

FText FViewEditCategoryViewportOverlay::GetDetailsCategoryTitle(TWeakObjectPtr<AActor> Actor) const
{
	return FText::FromString(TEXT("ViewEdit"));
}
