#include "RoadMeshComponent.h"
#include "RoadBuilderShaders.h"

#include "CSMesh.h"

void URoadMeshComponent::SetBuildInput(const FRoadBuildInput& Input, const FTransform& InputToWorld)
{
	// 几何挂在基类那一个 GpuMesh 上，组件不再另存一份指针：两个指向同一块几何的成员只会让
	// 人记不住哪个才是当前的。失败路径先把它取到局部再解绑，见下。
	UCSMesh* Mesh = GetGpuMesh();
	if (!Mesh)
	{
		Mesh = NewObject<UCSMesh>(this);
		SetGpuMesh(Mesh);
	}

	// One entry, not a section table: the whole road surface is one material, and
	// BuildMaterialSections would sort the index buffer and grow the indirect args to split it
	// into exactly one run. The table and MeshMaterial have different jobs — this is where the
	// saved asset's material slot comes from, MeshMaterial is what the single draw batch uses.
	Mesh->SetMaterial(0, MeshMaterial);

	GeometryToWorld = InputToWorld;
	if (!BuildRoadGeometryIntoMesh(Mesh, Input, InputToWorld))
	{
		// Nothing was built, so what the mesh still holds is the *previous* road. Leaving it drawn
		// next to splines it no longer matches is the worst outcome: it looks like a successful
		// build. Release rather than merely unbind — the allocation was sized for the road that is
		// now gone, and a refused build is exactly when the VRAM is worth handing back.
		//
		// Unbind first, in that order: the proxy borrows the resident buffers, so freeing them
		// while it is still bound leaves it drawing from an index buffer that no longer exists.
		// Mesh 是解绑前取的局部：SetGpuMesh(nullptr) 之后基类成员已经为空，但同一个调用栈里
		// 不会发生 GC，指针仍然有效，足够把这块分配还回去。
		SetGpuMesh(nullptr);
		const FCSMeshResident* Resident = Mesh->GetResidentPtr();
		if (Resident && Resident->IsAllocated()) Mesh->ReleaseSync();
		return;
	}
	// 已经在上面绑过了；重绑一次是为了让 SetGpuMesh 的"同对象但重新分配过"分支去刷新代理。
	SetGpuMesh(Mesh);
}
