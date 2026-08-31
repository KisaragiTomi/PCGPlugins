#include "EditorShortcutTagLibrary.h"

#include "GameFramework/Actor.h"

namespace
{
/** Modify() is what makes the change undoable; MarkPackageDirty() is what makes it savable. */
int32 ApplyTag(const TArray<AActor*>& Actors, FName Tag, bool bAddWhereMissing, bool bRemoveWherePresent)
{
	if (Tag.IsNone()) return 0;

	int32 Changed = 0;
	for (AActor* Actor : Actors)
	{
		if (!Actor) continue;

		const bool bHasTag = Actor->Tags.Contains(Tag);
		const bool bShouldAdd = bAddWhereMissing && !bHasTag;
		const bool bShouldRemove = bRemoveWherePresent && bHasTag;
		if (!bShouldAdd && !bShouldRemove) continue;

		Actor->Modify();
		if (bShouldAdd) Actor->Tags.Add(Tag);
		else Actor->Tags.Remove(Tag);
		Actor->MarkPackageDirty();
		++Changed;
	}

	return Changed;
}
} // namespace

int32 UEditorShortcutTagLibrary::AddTagToActors(const TArray<AActor*>& Actors, FName Tag)
{
	return ApplyTag(Actors, Tag, /*bAddWhereMissing=*/true, /*bRemoveWherePresent=*/false);
}

int32 UEditorShortcutTagLibrary::RemoveTagFromActors(const TArray<AActor*>& Actors, FName Tag)
{
	return ApplyTag(Actors, Tag, /*bAddWhereMissing=*/false, /*bRemoveWherePresent=*/true);
}

int32 UEditorShortcutTagLibrary::ToggleTagOnActors(const TArray<AActor*>& Actors, FName Tag)
{
	return ApplyTag(Actors, Tag, /*bAddWhereMissing=*/true, /*bRemoveWherePresent=*/true);
}
