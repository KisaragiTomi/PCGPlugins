// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ScreenGenerate.h"

/**
 * 
 */

DECLARE_MULTICAST_DELEGATE(FAsyncCalDelegate);

template<typename OpType>
class TATOOLSPLUGIN_API FAsyncTasksTemplate : public FNonAbandonableTask
{
	friend class FAutoDeleteAsyncTask<FAsyncTasksTemplate<OpType>>;
public:
	FAsyncTasksTemplate(TSharedPtr<OpType, ESPMode::ThreadSafe>& Inclass) :
		ThreadClass(Inclass)
	{
	}
	TSharedPtr<OpType, ESPMode::ThreadSafe > ThreadClass;
protected:

	void DoWork()
	{
		if (ThreadClass)
		{
			ThreadClass->DoCalculate();
		}
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FAsyncTasksTemplate, STATGROUP_ThreadPoolAsyncTasks);
	}
};

class TATOOLSPLUGIN_API AsyncAble
{
public:
	AsyncAble() {}

	virtual ~AsyncAble() {}

	FAsyncCalDelegate FAsyncCalDelegate;
	virtual void CalculateResult() = 0;
	virtual void DoCalculate() = 0;

};