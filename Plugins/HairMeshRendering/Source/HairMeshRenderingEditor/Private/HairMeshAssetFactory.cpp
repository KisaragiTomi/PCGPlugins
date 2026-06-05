#include "HairMeshAssetFactory.h"
#include "HairMeshAsset.h"
#include "HairMeshImporter.h"
#include "AssetToolsModule.h"

// ============================================================================
// New Asset Factory
// ============================================================================

UHairMeshAssetFactory::UHairMeshAssetFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UHairMeshAsset::StaticClass();
}

UObject* UHairMeshAssetFactory::FactoryCreateNew(
	UClass* InClass, UObject* InParent, FName InName,
	EObjectFlags Flags, UObject* Context,
	FFeedbackContext* Warn)
{
	UHairMeshAsset* Asset = NewObject<UHairMeshAsset>(InParent, InClass, InName, Flags);
	return Asset;
}

FText UHairMeshAssetFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("Hair Mesh Asset"));
}

uint32 UHairMeshAssetFactory::GetMenuCategories() const
{
	return EAssetTypeCategories::Misc;
}

// ============================================================================
// Import Factory
// ============================================================================

UHairMeshAssetImportFactory::UHairMeshAssetImportFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	SupportedClass = UHairMeshAsset::StaticClass();
	Formats.Add(TEXT("obj;Hair Mesh OBJ File"));
}

UObject* UHairMeshAssetImportFactory::FactoryCreateFile(
	UClass* InClass, UObject* InParent, FName InName,
	EObjectFlags Flags, const FString& Filename,
	const TCHAR* Parms, FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	UHairMeshAsset* Asset = UHairMeshImporter::ImportFromOBJ(
		Filename, InParent, InName.ToString());

	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("HairMeshAssetImportFactory: Failed to import %s"), *Filename);
		return nullptr;
	}

	Asset->Rename(*InName.ToString(), InParent);
	Asset->SetFlags(Flags);

	return Asset;
}

bool UHairMeshAssetImportFactory::FactoryCanImport(const FString& Filename)
{
	return Filename.EndsWith(TEXT(".obj"));
}

FText UHairMeshAssetImportFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("Hair Mesh OBJ"));
}
