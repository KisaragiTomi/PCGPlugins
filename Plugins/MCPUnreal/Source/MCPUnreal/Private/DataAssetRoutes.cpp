// Copyright (c) mcp-unreal project contributors. Apache-2.0 license.
//
// DataAssetRoutes.cpp — HTTP routes for DataTable management: list, read rows,
// add/update/delete rows, create tables, and CSV import.
// See issue #46.

#include "MCPUnrealUtils.h"

#include "Engine/DataTable.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/StructOnScope.h"
#include "Misc/FileHelper.h"

namespace MCPUnreal {

  // Serialize a DataTable row to JSON using UE property reflection.
  static TSharedPtr<FJsonObject> RowToJson(const UScriptStruct* RowStruct, const uint8* RowData) {
    TSharedPtr<FJsonObject> RowJson = MakeShareable(new FJsonObject());

    for (TFieldIterator<FProperty> PropIt(RowStruct); PropIt; ++PropIt) {
      FProperty* Prop = *PropIt;
      const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);

      if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop)) {
        RowJson->SetNumberField(Prop->GetName(), IntProp->GetPropertyValue(ValuePtr));
      } else if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop)) {
        RowJson->SetNumberField(Prop->GetName(), FloatProp->GetPropertyValue(ValuePtr));
      } else if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop)) {
        RowJson->SetNumberField(Prop->GetName(), DoubleProp->GetPropertyValue(ValuePtr));
      } else if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop)) {
        RowJson->SetBoolField(Prop->GetName(), BoolProp->GetPropertyValue(ValuePtr));
      } else if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop)) {
        RowJson->SetStringField(Prop->GetName(), StrProp->GetPropertyValue(ValuePtr));
      } else if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop)) {
        RowJson->SetStringField(Prop->GetName(), NameProp->GetPropertyValue(ValuePtr).ToString());
      } else if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop)) {
        RowJson->SetStringField(Prop->GetName(), TextProp->GetPropertyValue(ValuePtr).ToString());
      } else {
        // Fallback: export as string.
        FString ValueStr;
        Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
        RowJson->SetStringField(Prop->GetName(), ValueStr);
      }
    }

    return RowJson;
  }

  // Set a property value from a JSON value.
  static void SetPropertyFromJson(FProperty* Prop, uint8* RowData,
                                  const TSharedPtr<FJsonValue>& JsonValue) {
    void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);

    if (FIntProperty* IntProp = CastField<FIntProperty>(Prop)) {
      IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(JsonValue->AsNumber()));
    } else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop)) {
      FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(JsonValue->AsNumber()));
    } else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop)) {
      DoubleProp->SetPropertyValue(ValuePtr, JsonValue->AsNumber());
    } else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop)) {
      BoolProp->SetPropertyValue(ValuePtr, JsonValue->AsBool());
    } else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop)) {
      StrProp->SetPropertyValue(ValuePtr, JsonValue->AsString());
    } else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop)) {
      NameProp->SetPropertyValue(ValuePtr, FName(*JsonValue->AsString()));
    } else if (FTextProperty* TextProp = CastField<FTextProperty>(Prop)) {
      TextProp->SetPropertyValue(ValuePtr, FText::FromString(JsonValue->AsString()));
    }
  }

  // ---------------------------------------------------------------------------
  // POST /api/data/ops
  // ---------------------------------------------------------------------------

  static bool HandleDataOps(const FHttpServerRequest& Request,
                            const FHttpResultCallback& OnComplete) {
    TSharedPtr<FJsonObject> Body;
    if (!ParseJsonBody(Request, Body)) {
      SendError(OnComplete, TEXT("Invalid JSON in request body"));
      return true;
    }

    const FString Operation = Body->GetStringField(TEXT("operation"));
    if (Operation.IsEmpty()) {
      SendError(OnComplete, TEXT("operation is required"));
      return true;
    }

    // --- list_tables ---
    if (Operation == TEXT("list_tables")) {
      const FString Path = Body->GetStringField(TEXT("path"));
      if (Path.IsEmpty()) {
        SendError(OnComplete, TEXT("path is required for list_tables"));
        return true;
      }

      FAssetRegistryModule& ARM =
          FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
      TArray<FAssetData> Assets;
      ARM.Get().GetAssetsByPath(FName(*Path), Assets, /*bRecursive=*/true);

      TArray<TSharedPtr<FJsonValue>> TablesArray;
      for (const FAssetData& AssetData : Assets) {
        if (AssetData.AssetClassPath.GetAssetName() != TEXT("DataTable")) {
          continue;
        }
        UDataTable* DT = Cast<UDataTable>(AssetData.GetAsset());
        if (!DT) continue;

        TSharedPtr<FJsonObject> TableInfo = MakeShareable(new FJsonObject());
        TableInfo->SetStringField(TEXT("asset"), DT->GetPathName());
        TableInfo->SetStringField(TEXT("name"), DT->GetName());
        TableInfo->SetStringField(TEXT("row_struct"), DT->GetRowStructPathName().ToString());
        TableInfo->SetNumberField(TEXT("row_count"), DT->GetRowMap().Num());
        TablesArray.Add(MakeShareable(new FJsonValueObject(TableInfo)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetArrayField(TEXT("tables"), TablesArray);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- get_table ---
    if (Operation == TEXT("get_table")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset"));
      if (AssetPath.IsEmpty()) {
        SendError(OnComplete, TEXT("asset is required for get_table"));
        return true;
      }

      UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
      if (!DT) {
        SendError(OnComplete, FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
        return true;
      }

      const UScriptStruct* RowStruct = DT->GetRowStruct();
      TArray<TSharedPtr<FJsonValue>> RowsArray;

      for (const auto& Pair : DT->GetRowMap()) {
        TSharedPtr<FJsonObject> RowObj = MakeShareable(new FJsonObject());
        RowObj->SetStringField(TEXT("row_name"), Pair.Key.ToString());
        RowObj->SetObjectField(TEXT("data"), RowToJson(RowStruct, Pair.Value));
        RowsArray.Add(MakeShareable(new FJsonValueObject(RowObj)));
      }

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("asset"), AssetPath);
      ResponseJson->SetNumberField(TEXT("row_count"), RowsArray.Num());
      ResponseJson->SetArrayField(TEXT("rows"), RowsArray);

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- add_row ---
    if (Operation == TEXT("add_row")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset"));
      const FString RowName = Body->GetStringField(TEXT("row_name"));
      const TSharedPtr<FJsonObject>* DataObj;

      if (AssetPath.IsEmpty() || RowName.IsEmpty()) {
        SendError(OnComplete, TEXT("asset and row_name are required for add_row"));
        return true;
      }
      if (!Body->TryGetObjectField(TEXT("data"), DataObj)) {
        SendError(OnComplete, TEXT("data object is required for add_row"));
        return true;
      }

      UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
      if (!DT) {
        SendError(OnComplete, FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
        return true;
      }

      const UScriptStruct* RowStruct = DT->GetRowStruct();

      // Allocate a new row.
      uint8* RowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
      RowStruct->InitializeStruct(RowData);

      // Set property values from JSON.
      for (const auto& KV : (*DataObj)->Values) {
        FProperty* Prop = RowStruct->FindPropertyByName(FName(*KV.Key));
        if (Prop) {
          SetPropertyFromJson(Prop, RowData, KV.Value);
        }
      }

      DT->AddRow(FName(*RowName), *reinterpret_cast<FTableRowBase*>(RowData));

      RowStruct->DestroyStruct(RowData);
      FMemory::Free(RowData);

      DT->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("asset"), AssetPath);
      ResponseJson->SetNumberField(TEXT("row_count"), DT->GetRowMap().Num());
      ResponseJson->SetStringField(TEXT("message"), TEXT("Row added"));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- update_row ---
    if (Operation == TEXT("update_row")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset"));
      const FString RowName = Body->GetStringField(TEXT("row_name"));
      const TSharedPtr<FJsonObject>* DataObj;

      if (AssetPath.IsEmpty() || RowName.IsEmpty()) {
        SendError(OnComplete, TEXT("asset and row_name are required for update_row"));
        return true;
      }
      if (!Body->TryGetObjectField(TEXT("data"), DataObj)) {
        SendError(OnComplete, TEXT("data object is required for update_row"));
        return true;
      }

      UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
      if (!DT) {
        SendError(OnComplete, FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
        return true;
      }

      const UScriptStruct* RowStruct = DT->GetRowStruct();
      uint8* RowData = DT->FindRowUnchecked(FName(*RowName));
      if (!RowData) {
        SendError(OnComplete,
                  FString::Printf(TEXT("Row '%s' not found in %s"), *RowName, *AssetPath));
        return true;
      }

      // Update only specified fields.
      for (const auto& KV : (*DataObj)->Values) {
        FProperty* Prop = RowStruct->FindPropertyByName(FName(*KV.Key));
        if (Prop) {
          SetPropertyFromJson(Prop, RowData, KV.Value);
        }
      }

      DT->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("asset"), AssetPath);
      ResponseJson->SetNumberField(TEXT("row_count"), DT->GetRowMap().Num());
      ResponseJson->SetStringField(TEXT("message"), TEXT("Row updated"));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- delete_row ---
    if (Operation == TEXT("delete_row")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset"));
      const FString RowName = Body->GetStringField(TEXT("row_name"));

      if (AssetPath.IsEmpty() || RowName.IsEmpty()) {
        SendError(OnComplete, TEXT("asset and row_name are required for delete_row"));
        return true;
      }

      UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
      if (!DT) {
        SendError(OnComplete, FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
        return true;
      }

      DT->RemoveRow(FName(*RowName));
      DT->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("asset"), AssetPath);
      ResponseJson->SetNumberField(TEXT("row_count"), DT->GetRowMap().Num());
      ResponseJson->SetStringField(TEXT("message"), TEXT("Row deleted"));

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    // --- import_csv ---
    if (Operation == TEXT("import_csv")) {
      const FString AssetPath = Body->GetStringField(TEXT("asset"));
      const FString SourcePath = Body->GetStringField(TEXT("source_path"));

      if (AssetPath.IsEmpty() || SourcePath.IsEmpty()) {
        SendError(OnComplete, TEXT("asset and source_path are required for import_csv"));
        return true;
      }

      UDataTable* DT = LoadObject<UDataTable>(nullptr, *AssetPath);
      if (!DT) {
        SendError(OnComplete, FString::Printf(TEXT("DataTable not found: %s"), *AssetPath));
        return true;
      }

      FString CSVContent;
      if (!FFileHelper::LoadFileToString(CSVContent, *SourcePath)) {
        SendError(OnComplete, FString::Printf(TEXT("Failed to read CSV file: %s"), *SourcePath));
        return true;
      }

      TArray<FString> Errors = DT->CreateTableFromCSVString(CSVContent);
      if (Errors.Num() > 0) {
        FString ErrorStr = FString::Join(Errors, TEXT("; "));
        SendError(OnComplete, FString::Printf(TEXT("CSV import errors: %s"), *ErrorStr));
        return true;
      }

      DT->MarkPackageDirty();

      TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject());
      ResponseJson->SetBoolField(TEXT("success"), true);
      ResponseJson->SetStringField(TEXT("asset"), AssetPath);
      ResponseJson->SetNumberField(TEXT("row_count"), DT->GetRowMap().Num());
      ResponseJson->SetStringField(
          TEXT("message"),
          FString::Printf(TEXT("Imported %d rows from CSV"), DT->GetRowMap().Num()));

      UE_LOG(LogMCPUnreal, Log, TEXT("Imported CSV '%s' into DataTable '%s' (%d rows)"),
             *SourcePath, *AssetPath, DT->GetRowMap().Num());

      SendJson(OnComplete, ResponseJson);
      return true;
    }

    SendError(OnComplete, FString::Printf(TEXT("Unknown data asset operation: %s"), *Operation));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Registration
  // ---------------------------------------------------------------------------

  void RegisterDataAssetRoutes(TSharedPtr<IHttpRouter> Router, TArray<FHttpRouteHandle>& Handles) {
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/data/ops")),
                                  EHttpServerRequestVerbs::VERB_POST,
                                  FHttpRequestHandler::CreateStatic(&HandleDataOps)));

    UE_LOG(LogMCPUnreal, Verbose, TEXT("Registered data asset routes (1 endpoint)"));
  }

}  // namespace MCPUnreal
