#include "EditorShortcutSetupLibrary.h"

#include "EditorShortcutTagLibrary.h"
#include "EditorShortcutsLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "EditorUtilityBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
/**
 * The one class a shortcut Blueprint is parented on.
 *
 * UEditorFunctionLibrary is the engine's sentinel base for Blueprints holding editor
 * logic. Two things follow from it and nothing else provides both: it lives in an
 * editor module, which is what lets the graphs call editor-only functions, and the
 * Editor Utility Blueprint factory recognises it and switches the asset to
 * BPTYPE_FunctionLibrary, which is what lets the same asset be created by hand.
 *
 * Its header is private to Blutility, so it is resolved by path rather than included.
 */
UClass* FindShortcutBlueprintParentClass()
{
	return FindObject<UClass>(nullptr, TEXT("/Script/Blutility.EditorFunctionLibrary"));
}

/** The pin type of a "Array of Actor" input, which is what the selection is fed into. */
FEdGraphPinType MakeActorArrayPinType()
{
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
	PinType.PinSubCategoryObject = AActor::StaticClass();
	PinType.ContainerType = EPinContainerType::Array;
	return PinType;
}

/**
 * Builds one function: an Actors input wired straight into ToggleTagOnActors,
 * with the tag written into the call's Tag pin.
 */
bool BuildToggleFunction(UBlueprint* Blueprint, FName FunctionName, FName Tag, FString& OutError)
{
	// Rebuild rather than duplicate when the function is already there.
	if (UEdGraph* Existing = FindObject<UEdGraph>(Blueprint, *FunctionName.ToString())) FBlueprintEditorUtils::RemoveGraph(Blueprint, Existing);

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("could not create a graph for %s"), *FunctionName.ToString());
		return false;
	}

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated=*/true, nullptr);

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	Graph->GetNodesOfClass(EntryNodes);
	if (EntryNodes.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s has no entry node"), *FunctionName.ToString());
		return false;
	}

	UK2Node_FunctionEntry* EntryNode = EntryNodes[0];
	UEdGraphPin* ActorsOut = EntryNode->CreateUserDefinedPin(TEXT("Actors"), MakeActorArrayPinType(), EGPD_Output);
	if (!ActorsOut)
	{
		OutError = FString::Printf(TEXT("could not add the Actors input to %s"), *FunctionName.ToString());
		return false;
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
	CallNode->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(UEditorShortcutTagLibrary, ToggleTagOnActors),
		UEditorShortcutTagLibrary::StaticClass());
	CallNode->CreateNewGuid();
	CallNode->PostPlacedNewNode();
	CallNode->SetFlags(RF_Transactional);
	CallNode->AllocateDefaultPins();
	Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	CallNode->NodePosX = EntryNode->NodePosX + 320;
	CallNode->NodePosY = EntryNode->NodePosY;

	UEdGraphPin* EntryThen = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* CallExec = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UEdGraphPin* CallActors = CallNode->FindPin(TEXT("Actors"), EGPD_Input);
	UEdGraphPin* CallTag = CallNode->FindPin(TEXT("Tag"), EGPD_Input);

	if (!EntryThen || !CallExec || !CallActors || !CallTag)
	{
		OutError = FString::Printf(TEXT("%s: ToggleTagOnActors did not expose the expected pins"), *FunctionName.ToString());
		return false;
	}

	EntryThen->MakeLinkTo(CallExec);
	ActorsOut->MakeLinkTo(CallActors);
	CallTag->DefaultValue = Tag.ToString();

	return true;
}
} // namespace

FString UEditorShortcutSetupLibrary::CreateTagToggleLibrary(const FString& PackageName, const TArray<FName>& Tags, bool bSave)
{
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("'%s' is not a valid package name"), *PackageName);
		return FString();
	}

	if (Tags.IsEmpty())
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("no tags given, nothing to generate"));
		return FString();
	}

	const FString AssetName = FPackageName::GetShortName(PackageName);

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("could not create package %s"), *PackageName);
		return FString();
	}
	Package->FullyLoad();

	UClass* ParentClass = FindShortcutBlueprintParentClass();
	if (!ParentClass)
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("could not resolve /Script/Blutility.EditorFunctionLibrary; is the Blutility module loaded?"));
		return FString();
	}

	UBlueprint* Blueprint = FindObject<UBlueprint>(Package, *AssetName);

	// The parent class is what decides whether editor functions may be called, so an
	// asset carrying the wrong one can never compile and has to be replaced outright.
	if (Blueprint && (Blueprint->ParentClass != ParentClass || !Blueprint->IsA<UEditorUtilityBlueprint>()))
	{
		UE_LOG(LogEditorShortcuts, Warning, TEXT("%s cannot call editor functions (parent %s); replacing it"),
			*PackageName, Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("none"));
		Blueprint->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Blueprint->MarkAsGarbage();
		Blueprint = nullptr;
	}

	const bool bIsNew = Blueprint == nullptr;
	if (bIsNew)
	{
		// Deliberately the same shape UEditorUtilityBlueprintFactory produces for this
		// parent, so a generated asset and a hand-made one are indistinguishable.
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			*AssetName,
			BPTYPE_FunctionLibrary,
			UEditorUtilityBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
	}

	if (!Blueprint)
	{
		UE_LOG(LogEditorShortcuts, Error, TEXT("could not create blueprint %s"), *PackageName);
		return FString();
	}

	int32 Built = 0;
	for (FName Tag : Tags)
	{
		if (Tag.IsNone()) continue;

		const FName FunctionName(*FString::Printf(TEXT("Toggle%s"), *Tag.ToString()));

		FString Error;
		if (!BuildToggleFunction(Blueprint, FunctionName, Tag, Error))
		{
			UE_LOG(LogEditorShortcuts, Error, TEXT("%s"), *Error);
			continue;
		}

		UE_LOG(LogEditorShortcuts, Log, TEXT("Generated %s -> ToggleTagOnActors(Actors, %s)"), *FunctionName.ToString(), *Tag.ToString());
		++Built;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	if (bIsNew) FAssetRegistryModule::AssetCreated(Blueprint);
	Blueprint->MarkPackageDirty();

	if (bSave)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;

		if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs))
		{
			UE_LOG(LogEditorShortcuts, Error, TEXT("could not save %s"), *FileName);
			return FString();
		}
	}

	UE_LOG(LogEditorShortcuts, Log, TEXT("%s %s with %d function(s)"), bIsNew ? TEXT("Created") : TEXT("Updated"), *PackageName, Built);

	return FString::Printf(TEXT("%s.%s_C"), *PackageName, *AssetName);
}
