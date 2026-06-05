#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "HairMeshAssetFactory.generated.h"

/**
 * Factory for creating new UHairMeshAsset in the Content Browser.
 * Supports:
 *   - Creating empty assets (right-click → Hair Mesh Rendering → Hair Mesh Asset)
 *   - Importing from .obj files (drag & drop or File → Import)
 */
UCLASS()
class HAIRMESHRENDERINGEDITOR_API UHairMeshAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UHairMeshAssetFactory();

	// UFactory interface
	virtual UObject* FactoryCreateNew(
		UClass* InClass, UObject* InParent, FName InName,
		EObjectFlags Flags, UObject* Context,
		FFeedbackContext* Warn) override;

	virtual bool CanCreateNew() const override { return true; }
	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
};

/**
 * Factory for importing .obj files as UHairMeshAsset.
 */
UCLASS()
class HAIRMESHRENDERINGEDITOR_API UHairMeshAssetImportFactory : public UFactory
{
	GENERATED_BODY()

public:
	UHairMeshAssetImportFactory();

	virtual UObject* FactoryCreateFile(
		UClass* InClass, UObject* InParent, FName InName,
		EObjectFlags Flags, const FString& Filename,
		const TCHAR* Parms, FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual FText GetDisplayName() const override;
};
