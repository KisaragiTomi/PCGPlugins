#pragma once

#include "SelectedActorViewportOverlayBase.h"

class AActor;

/**
 * 通用的「ViewEdit 分类浮窗」：任何在 Details 里有 ViewEdit 分类的 actor，选中时就在视口左上角
 * 把该分类原样浮出来，不认具体的 actor 类型。
 *
 * 基类的 SupportsActor 是纯虚，所以这层必须存在；它提供的只有「按分类名筛选」这一条策略。
 * 这里以前叫 FVineContainerViewportOverlay，还会给 AVineContainer 额外包一排操作按钮——按钮
 * 已移除（改在蓝图侧做），剩下的部分与藤蔓无关，故改成按职责命名。
 */
class FViewEditCategoryViewportOverlay : public FSelectedActorViewportOverlayBase
{
protected:
	virtual bool SupportsActor(const AActor* Actor) const override;
	virtual int32 GetOverlayZOrder() const override { return 250; }
	virtual FName GetRequiredDetailsCategoryName() const override { return TEXT("ViewEdit"); }
	virtual FText GetDetailsCategoryTitle(TWeakObjectPtr<AActor> Actor) const override;
};
