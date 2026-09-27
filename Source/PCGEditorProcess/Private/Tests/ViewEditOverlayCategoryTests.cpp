#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "DetailsCategoryAliasCustomization.h"
#include "ViewEditCategoryViewportOverlay.h"

#include "EdGraphSchema_K2.h"                  // PC_Boolean / K2 schema
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "K2Node_EditablePinBase.h"            // FKismetUserDeclaredFunctionMetadata
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Modules/ModuleManager.h"
#include "ObjectEditorUtils.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

// -----------------------------------------------------------------------------
// ViewEdit 浮窗的分类别名（2026-09-22，用户："大写自动空一格的情况要一视同仁，兜底我的输入名字错误"）
//
// 现场：BP_CSSW_Capture 的成员一部分存成 "ViewEdit"、一部分存成 "View Edit"，浮窗里只剩一个 Sim 按钮。
// 两种写法在蓝图 My Blueprint 面板里按显示名分组，是同一个 "View Edit" 分类节点，看不出区别；往那个分类上
// 拖成员、粘贴、改分类名，写回去的都是带空格的显示名。浮窗以前按原样比较分类名，带空格的成员于是全被滤掉，
// **不报任何错**；成员全是带空格写法的 actor 则整个浮窗都不出现。
//
// 这里钉三层：
//   ① `CategoryMatch`：比较规则本身 —— 空白、下划线、大小写都不算区别；只看根段；不误伤 "ViewEditor" 这类别的分类。
//   ② `MergedLayout`：真造一个两种写法混用的蓝图，走引擎真的 details 布局（IPropertyRowGenerator，与浮窗的
//      IDetailsView 同一套 DetailLayoutHelpers 与定制查询），断言属性与按钮都进了同一个 ViewEdit 分类、按钮只拼成一行。
//   ③ `AliasOnlyActor`：只有别名写法成员的 actor 也要浮出窗来（属性、函数两条路各钉一次）。
// 做不到：浮窗 Slate 控件真画出来的样子（要真视口）。那一层靠浮窗的过滤委托调 ① 的规则，编辑器里人工看。
// -----------------------------------------------------------------------------

namespace
{
const FName ViewEditCategory(TEXT("ViewEdit"));

struct FMemberSpec
{
	const TCHAR* Name;
	const TCHAR* Category;
};

/** 造一个 AActor 蓝图：Variables 是实例可编辑的 bool，Functions 是勾了 Call In Editor 的空函数。 */
UBlueprint* MakeActorBlueprint(const TCHAR* BaseName, const TArray<FMemberSpec>& Variables, const TArray<FMemberSpec>& Functions)
{
	UPackage* Package = GetTransientPackage();
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		MakeUniqueObjectName(Package, UBlueprint::StaticClass(), BaseName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!Blueprint) return nullptr;

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	for (const FMemberSpec& Variable : Variables)
	{
		const FName VariableName(Variable.Name);
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, BoolType);
		// 新变量默认没勾 "Instance Editable"（CPF_DisableEditOnInstance），关卡里的 actor 的 Details 看不到它。
		FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VariableName, false);
		FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VariableName, nullptr, FText::FromString(Variable.Category), true);
	}

	for (const FMemberSpec& Function : Functions)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(Function.Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, Graph, /*bIsUserCreated=*/true, nullptr);
		FKismetUserDeclaredFunctionMetadata* MetaData = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
		if (!MetaData) return nullptr;

		MetaData->bCallInEditor = true;
		FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(Function.Category), true);
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return Blueprint;
}

/** 蓝图留在临时包里，去掉 RF_Standalone，测试地图换掉之后就能被 GC 收走。 */
void ReleaseBlueprint(UBlueprint* Blueprint)
{
	if (Blueprint) Blueprint->ClearFlags(RF_Standalone);
}

/** 与浮窗同一份定制注册法：UObject 上顶替 FObjectDetails。 */
TSharedRef<IPropertyRowGenerator> MakeOverlayRowGenerator(AActor* Actor)
{
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	TSharedRef<IPropertyRowGenerator> Generator = PropertyEditorModule.CreatePropertyRowGenerator(FPropertyRowGeneratorArgs());
	Generator->RegisterInstancedCustomPropertyLayout(
		UObject::StaticClass(),
		FOnGetDetailCustomizationInstance::CreateLambda([]() -> TSharedRef<IDetailCustomization>
		{
			return MakeShared<FDetailsCategoryAliasCustomization>(ViewEditCategory);
		}));
	Generator->SetObjects({ Actor });
	return Generator;
}

struct FCategoryRows
{
	/** 属性行的 FProperty 名，按显示顺序。 */
	TArray<FName> Properties;
	/** 其余行各自的过滤串；按钮行把整行按钮的标签与函数名都拼在里面。 */
	TArray<FString> OtherRows;
};

FCategoryRows CollectCategoryRows(const TSharedRef<IDetailTreeNode>& CategoryNode)
{
	FCategoryRows Rows;
	TArray<TSharedRef<IDetailTreeNode>> Children;
	CategoryNode->GetChildren(Children);
	for (const TSharedRef<IDetailTreeNode>& Child : Children)
	{
		const TSharedPtr<IPropertyHandle> Handle = Child->CreatePropertyHandle();
		if (Handle.IsValid() && Handle->GetProperty())
		{
			Rows.Properties.Add(Handle->GetProperty()->GetFName());
			continue;
		}

		TArray<FString> FilterStrings;
		Child->GetFilterStrings(FilterStrings);
		const FString Joined = FString::Join(FilterStrings, TEXT("\n"));
		if (!Joined.IsEmpty()) Rows.OtherRows.Add(Joined);
	}
	return Rows;
}

/** 把门槛（protected）提出来直接问。 */
class FViewEditOverlayProbe : public FViewEditCategoryViewportOverlay
{
public:
	using FSelectedActorViewportOverlayBase::ActorHasRequiredDetailsCategory;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewEditOverlayCategoryMatchTest,
	"PCGPlugins.PCGEditorProcess.ViewEditOverlay.CategoryMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FViewEditOverlayCategoryMatchTest::RunTest(const FString& Parameters)
{
	auto Matches = [](const TCHAR* Candidate) { return FDetailsCategoryAliasCustomization::DoesCategoryMatch(FName(Candidate), ViewEditCategory); };

	// 同一个分类的各种写法：My Blueprint 拖放/粘贴写回的显示名、手敲时的大小写与多余空白、下划线（显示名里也是空格）。
	const TCHAR* const SameCategory[] = { TEXT("ViewEdit"), TEXT("View Edit"), TEXT("view edit"), TEXT("VIEWEDIT"), TEXT(" View  Edit "), TEXT("View_Edit") };
	for (const TCHAR* Spelling : SameCategory) TestTrue(FString::Printf(TEXT("'%s' is ViewEdit"), Spelling), Matches(Spelling));

	// 子分类跟着根段走，两种写法的根都算。
	TestTrue(TEXT("'ViewEdit|Debug' belongs to ViewEdit"), Matches(TEXT("ViewEdit|Debug")));
	TestTrue(TEXT("'View Edit|Debug' belongs to ViewEdit"), Matches(TEXT("View Edit|Debug")));

	// 不误伤：只是前缀相同、多了词、或 ViewEdit 只出现在子分类里的，都是别的分类。
	const TCHAR* const OtherCategories[] = { TEXT("ViewEditor"), TEXT("View Edit Tools"), TEXT("View"), TEXT("Edit"), TEXT("Debug|ViewEdit"), TEXT("Default") };
	for (const TCHAR* Spelling : OtherCategories) TestFalse(FString::Printf(TEXT("'%s' is not ViewEdit"), Spelling), Matches(Spelling));

	TestFalse(TEXT("NAME_None is never ViewEdit"), FDetailsCategoryAliasCustomization::DoesCategoryMatch(NAME_None, ViewEditCategory));
	TestTrue(TEXT("The rule does not care which side carries the space"),
		FDetailsCategoryAliasCustomization::DoesCategoryMatch(ViewEditCategory, FName(TEXT("View Edit"))));
	TestFalse(TEXT("A required category that is all blanks matches nothing"),
		FDetailsCategoryAliasCustomization::DoesCategoryMatch(FName(TEXT("_")), FName(TEXT(" _ "))));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewEditOverlayMergedLayoutTest,
	"PCGPlugins.PCGEditorProcess.ViewEditOverlay.MergedLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FViewEditOverlayMergedLayoutTest::RunTest(const FString& Parameters)
{
	UBlueprint* Blueprint = MakeActorBlueprint(
		TEXT("BP_ViewEditAliasMixed"),
		{ { TEXT("CanonVar"), TEXT("ViewEdit") }, { TEXT("SpacedVar"), TEXT("View Edit") }, { TEXT("UnderscoreVar"), TEXT("View_Edit") }, { TEXT("OtherVar"), TEXT("Other") } },
		{ { TEXT("CanonFunc"), TEXT("ViewEdit") }, { TEXT("SpacedFunc"), TEXT("View Edit") }, { TEXT("OtherFunc"), TEXT("Other") } });
	if (!TestNotNull(TEXT("Mixed-spelling blueprint"), Blueprint)) return false;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	AActor* Actor = World->SpawnActor<AActor>(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("Spawned actor"), Actor)) return false;

	// 前提自检：编译出来的元数据真的是两种写法、函数真的是 CallInEditor —— 否则下面的合并断言会靠错误的理由通过。
	UClass* Class = Actor->GetClass();
	TestEqual(TEXT("SpacedVar keeps its spaced spelling"),
		FObjectEditorUtils::GetCategoryFName(Class->FindPropertyByName(TEXT("SpacedVar"))), FName(TEXT("View Edit")));
	const UFunction* SpacedFunc = Class->FindFunctionByName(TEXT("SpacedFunc"));
	if (!TestNotNull(TEXT("SpacedFunc was compiled"), SpacedFunc)) return false;
	TestEqual(TEXT("SpacedFunc keeps its spaced spelling"), FObjectEditorUtils::GetCategoryFName(SpacedFunc), FName(TEXT("View Edit")));
	TestTrue(TEXT("SpacedFunc is CallInEditor"), SpacedFunc->GetBoolMetaData(TEXT("CallInEditor")));

	const TSharedRef<IPropertyRowGenerator> Generator = MakeOverlayRowGenerator(Actor);
	TSharedPtr<IDetailTreeNode> ViewEditNode;
	for (const TSharedRef<IDetailTreeNode>& Root : Generator->GetRootTreeNodes())
	{
		if (Root->GetNodeType() != EDetailNodeType::Category) continue;

		const FName RootName = Root->GetNodeName();
		if (RootName == ViewEditCategory)
		{
			ViewEditNode = Root;
			continue;
		}

		// 别名写法的分类可以还在（空的），但不许再带着内容 —— 那就是又分成了两个 "View Edit" 标题。
		if (!FDetailsCategoryAliasCustomization::DoesCategoryMatch(RootName, ViewEditCategory)) continue;
		const FCategoryRows AliasRows = CollectCategoryRows(Root);
		TestTrue(FString::Printf(TEXT("Alias category '%s' was emptied into ViewEdit"), *RootName.ToString()),
			AliasRows.Properties.IsEmpty() && AliasRows.OtherRows.IsEmpty());
	}

	if (!TestTrue(TEXT("There is a ViewEdit category"), ViewEditNode.IsValid())) return false;
	const FCategoryRows Rows = CollectCategoryRows(ViewEditNode.ToSharedRef());

	// 属性：三种写法都进来了，规范写法的仍在最前（别名的接在后面）。
	TestTrue(TEXT("CanonVar is in ViewEdit"), Rows.Properties.Contains(FName(TEXT("CanonVar"))));
	TestTrue(TEXT("SpacedVar ('View Edit') is merged into ViewEdit"), Rows.Properties.Contains(FName(TEXT("SpacedVar"))));
	TestTrue(TEXT("UnderscoreVar ('View_Edit') is merged into ViewEdit"), Rows.Properties.Contains(FName(TEXT("UnderscoreVar"))));
	TestFalse(TEXT("OtherVar stays out of ViewEdit"), Rows.Properties.Contains(FName(TEXT("OtherVar"))));
	if (Rows.Properties.Num() > 0) TestEqual(TEXT("The canonical spelling keeps the first slot"), Rows.Properties[0], FName(TEXT("CanonVar")));

	// 按钮：两种写法的函数拼在同一行（引擎按原样分类会拼成两行、分在两个分类里）。
	if (TestEqual(TEXT("All ViewEdit buttons share one row"), Rows.OtherRows.Num(), 1))
	{
		TestTrue(TEXT("CanonFunc has a button"), Rows.OtherRows[0].Contains(TEXT("CanonFunc")));
		TestTrue(TEXT("SpacedFunc ('View Edit') has a button in the same row"), Rows.OtherRows[0].Contains(TEXT("SpacedFunc")));
		TestFalse(TEXT("OtherFunc is not a ViewEdit button"), Rows.OtherRows[0].Contains(TEXT("OtherFunc")));
	}

	ReleaseBlueprint(Blueprint);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewEditOverlayAliasOnlyActorTest,
	"PCGPlugins.PCGEditorProcess.ViewEditOverlay.AliasOnlyActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FViewEditOverlayAliasOnlyActorTest::RunTest(const FString& Parameters)
{
	// 属性、函数两条路分开造：只有一种成员时门槛也得认出来。
	UBlueprint* SpacedVarOnly = MakeActorBlueprint(TEXT("BP_ViewEditAliasVarOnly"), { { TEXT("SpacedVar"), TEXT("View Edit") } }, {});
	UBlueprint* SpacedFuncOnly = MakeActorBlueprint(TEXT("BP_ViewEditAliasFuncOnly"), {}, { { TEXT("SpacedFunc"), TEXT("View Edit") } });
	if (!TestNotNull(TEXT("Variable-only blueprint"), SpacedVarOnly)) return false;
	if (!TestNotNull(TEXT("Function-only blueprint"), SpacedFuncOnly)) return false;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	AActor* VarActor = World->SpawnActor<AActor>(SpacedVarOnly->GeneratedClass);
	AActor* FuncActor = World->SpawnActor<AActor>(SpacedFuncOnly->GeneratedClass);
	AActor* PlainActor = World->SpawnActor<AActor>(AActor::StaticClass());
	if (!TestNotNull(TEXT("Variable-only actor"), VarActor)) return false;
	if (!TestNotNull(TEXT("Function-only actor"), FuncActor)) return false;
	if (!TestNotNull(TEXT("Plain actor"), PlainActor)) return false;

	FViewEditOverlayProbe Overlay;
	TestTrue(TEXT("An actor whose only ViewEdit member is a 'View Edit' property gets the overlay"), Overlay.ActorHasRequiredDetailsCategory(VarActor));
	TestTrue(TEXT("An actor whose only ViewEdit member is a 'View Edit' button gets the overlay"), Overlay.ActorHasRequiredDetailsCategory(FuncActor));
	TestFalse(TEXT("A plain actor gets no overlay"), Overlay.ActorHasRequiredDetailsCategory(PlainActor));

	ReleaseBlueprint(SpacedVarOnly);
	ReleaseBlueprint(SpacedFuncOnly);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
