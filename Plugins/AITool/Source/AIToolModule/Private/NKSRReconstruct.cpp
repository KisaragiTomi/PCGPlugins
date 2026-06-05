#include "NKSRReconstruct.h"

#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformProcess.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Interfaces/IPluginManager.h"

namespace
{
bool IsValidPythonExe(const FString& CandidatePath)
{
	return !CandidatePath.IsEmpty() && FPaths::FileExists(CandidatePath);
}

bool IsValidNKSRPackageDir(const FString& CandidatePath)
{
	if (CandidatePath.IsEmpty())
	{
		return false;
	}

	const FString InitPyPath = FPaths::Combine(CandidatePath, TEXT("nksr"), TEXT("__init__.py"));
	const FString NativeModulePath = FPaths::Combine(CandidatePath, TEXT("nksr"), TEXT("_C.pyd"));
	return FPaths::FileExists(InitPyPath) && FPaths::FileExists(NativeModulePath);
}

FString GetKnownExternalNKSRRoot()
{
	return TEXT("D:/MyProject/AITest/ConvertToSurface/ConvertToSurface/NKSR");
}

FString GetKnownExternalNKSRPackagePath()
{
	return FPaths::Combine(GetKnownExternalNKSRRoot(), TEXT("package"));
}

FString GetKnownSystemPythonPath()
{
	return TEXT("C:/Users/KLW/AppData/Local/Programs/Python/Python311/python.exe");
}

FString GetBundledNKSRPackagePath(const FString& PluginDir)
{
	return FPaths::Combine(PluginDir, TEXT("ThirdParty"), TEXT("Python"), TEXT("Lib"), TEXT("site-packages"));
}
}

FString UNKSRReconstructLibrary::GetAIToolPluginDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AITool"));
	if (Plugin.IsValid())
	{
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	}
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("AITool")));
}

FString UNKSRReconstructLibrary::GetBundledPythonPath()
{
	return FPaths::Combine(GetAIToolPluginDir(), TEXT("ThirdParty"), TEXT("Python"), TEXT("python.exe"));
}

FString UNKSRReconstructLibrary::GetDefaultPythonPath()
{
	FString CmdLinePythonPath;
	FParse::Value(FCommandLine::Get(), TEXT("AIToolPython="), CmdLinePythonPath);
	if (IsValidPythonExe(CmdLinePythonPath))
	{
		return CmdLinePythonPath;
	}

	const FString EnvPythonPath = FPlatformMisc::GetEnvironmentVariable(TEXT("AITOOL_PYTHON"));
	if (IsValidPythonExe(EnvPythonPath))
	{
		return EnvPythonPath;
	}

	const FString KnownSystemPythonPath = GetKnownSystemPythonPath();
	if (IsValidPythonExe(KnownSystemPythonPath))
	{
		return KnownSystemPythonPath;
	}

	const FString PythonHome = FPlatformMisc::GetEnvironmentVariable(TEXT("PYTHONHOME"));
	if (IsValidPythonExe(FPaths::Combine(PythonHome, TEXT("python.exe"))))
	{
		return FPaths::Combine(PythonHome, TEXT("python.exe"));
	}

	const FString BundledPythonPath = GetBundledPythonPath();
	if (IsValidPythonExe(BundledPythonPath))
	{
		return BundledPythonPath;
	}

	return FString();
}

FString UNKSRReconstructLibrary::GetDefaultNKSRPackagePath()
{
	FString CmdLinePackagePath;
	FParse::Value(FCommandLine::Get(), TEXT("AIToolNKSRPackage="), CmdLinePackagePath);
	if (IsValidNKSRPackageDir(CmdLinePackagePath))
	{
		return CmdLinePackagePath;
	}

	const FString EnvPackagePath = FPlatformMisc::GetEnvironmentVariable(TEXT("AITOOL_NKSR_PACKAGE"));
	if (IsValidNKSRPackageDir(EnvPackagePath))
	{
		return EnvPackagePath;
	}

	const FString KnownExternalPackagePath = GetKnownExternalNKSRPackagePath();
	if (IsValidNKSRPackageDir(KnownExternalPackagePath))
	{
		return KnownExternalPackagePath;
	}

	const FString BundledPackagePath = GetBundledNKSRPackagePath(GetAIToolPluginDir());
	if (IsValidNKSRPackageDir(BundledPackagePath))
	{
		return BundledPackagePath;
	}

	return FString();
}

FString UNKSRReconstructLibrary::GetNKSRScriptPath()
{
	return FPaths::Combine(GetAIToolPluginDir(), TEXT("Scripts"), TEXT("NKSR"), TEXT("reconstruct.py"));
}

FNKSRResult UNKSRReconstructLibrary::RunNKSRReconstruction(
	const FString& InputFilePath,
	const FString& OutputFilePath,
	const FNKSRSettings& Settings)
{
	FNKSRResult Result;

	if (!FPaths::FileExists(InputFilePath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Input file not found: %s"), *InputFilePath);
		UE_LOG(LogTemp, Error, TEXT("NKSR: %s"), *Result.ErrorMessage);
		return Result;
	}

	// Resolve Python path: prefer Settings, fallback to bundled
	FString PythonPath = Settings.PythonExePath;
	if (PythonPath.IsEmpty() || !FPaths::FileExists(PythonPath))
	{
		PythonPath = GetDefaultPythonPath();
	}
	if (!FPaths::FileExists(PythonPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("Python not found: %s"), *PythonPath);
		UE_LOG(LogTemp, Error, TEXT("NKSR: %s"), *Result.ErrorMessage);
		return Result;
	}

	const FString ScriptPath = GetNKSRScriptPath();
	if (!FPaths::FileExists(ScriptPath))
	{
		Result.ErrorMessage = FString::Printf(TEXT("reconstruct.py not found: %s"), *ScriptPath);
		UE_LOG(LogTemp, Error, TEXT("NKSR: %s"), *Result.ErrorMessage);
		return Result;
	}

	FString ActualOutput = OutputFilePath;
	if (ActualOutput.IsEmpty())
	{
		ActualOutput = FPaths::ChangeExtension(InputFilePath, TEXT("")) + TEXT("_nksr.obj");
	}

	// Build command-line args; --nksr-package only if external path is specified
	FString Args = FString::Printf(
		TEXT("\"%s\" --input \"%s\" --output \"%s\" --detail-level %f --device %s"),
		*ScriptPath, *InputFilePath, *ActualOutput,
		Settings.DetailLevel, *Settings.Device);

	FString NKSRPackagePath = Settings.NKSRPackagePath;
	if (!IsValidNKSRPackageDir(NKSRPackagePath))
	{
		NKSRPackagePath = GetDefaultNKSRPackagePath();
	}
	if (IsValidNKSRPackageDir(NKSRPackagePath))
	{
		Args += FString::Printf(TEXT(" --nksr-package \"%s\""), *NKSRPackagePath);
	}
	else
	{
		Result.ErrorMessage = TEXT("NKSR package path not found or incomplete. Expected nksr/__init__.py and nksr/_C.pyd.");
		UE_LOG(LogTemp, Error, TEXT("NKSR: %s"), *Result.ErrorMessage);
		return Result;
	}

	UE_LOG(LogTemp, Log, TEXT("NKSR: Launching: %s %s"), *PythonPath, *Args);

	int32 ReturnCode = -1;
	FString StdOut;
	FString StdErr;

	const bool bLaunchSuccess = FPlatformProcess::ExecProcess(
		*PythonPath, *Args,
		&ReturnCode, &StdOut, &StdErr);

	if (!bLaunchSuccess)
	{
		Result.ErrorMessage = TEXT("Failed to launch Python process");
		UE_LOG(LogTemp, Error, TEXT("NKSR: %s"), *Result.ErrorMessage);
		return Result;
	}

	if (!StdOut.IsEmpty())
	{
		UE_LOG(LogTemp, Log, TEXT("NKSR stdout:\n%s"), *StdOut);
	}
	if (!StdErr.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("NKSR stderr:\n%s"), *StdErr);
	}

	// Parse the last non-empty stdout line as JSON
	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines);
	FString LastLine;
	for (int32 i = Lines.Num() - 1; i >= 0; --i)
	{
		if (!Lines[i].TrimStartAndEnd().IsEmpty())
		{
			LastLine = Lines[i].TrimStartAndEnd();
			break;
		}
	}

	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LastLine);
	if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
	{
		const FString Status = Json->HasField(TEXT("status"))
			? Json->GetStringField(TEXT("status")) : FString();

		if (Status == TEXT("ok"))
		{
			Result.bSuccess = true;
			if (Json->HasField(TEXT("output")))
			{
				Result.OutputFilePath = Json->GetStringField(TEXT("output"));
			}
			if (Json->HasField(TEXT("vertices")))
			{
				Result.VertexCount = static_cast<int32>(Json->GetNumberField(TEXT("vertices")));
			}
			if (Json->HasField(TEXT("faces")))
			{
				Result.FaceCount = static_cast<int32>(Json->GetNumberField(TEXT("faces")));
			}
			UE_LOG(LogTemp, Log, TEXT("NKSR: Success - %d verts, %d faces -> %s"),
				Result.VertexCount, Result.FaceCount, *Result.OutputFilePath);
		}
		else
		{
			Result.ErrorMessage = Json->HasField(TEXT("message"))
				? Json->GetStringField(TEXT("message")) : TEXT("Unknown error");
			UE_LOG(LogTemp, Error, TEXT("NKSR: Python error: %s"), *Result.ErrorMessage);
		}
	}
	else
	{
		if (ReturnCode != 0)
		{
			Result.ErrorMessage = FString::Printf(
				TEXT("Python exited with code %d. stderr: %s"), ReturnCode, *StdErr);
		}
		else
		{
			Result.bSuccess = FPaths::FileExists(ActualOutput);
			Result.OutputFilePath = ActualOutput;
			if (!Result.bSuccess)
			{
				Result.ErrorMessage = TEXT("Python finished but output file not found");
			}
		}
	}

	return Result;
}
