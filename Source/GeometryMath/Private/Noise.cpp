#include "Noise.h"


FVector UNoise::CurlNoise(FVector Pos, FVector& Out_AddedPos, FVector Offset, float Strength, float Frequency)
{
	FVector curl = FVector(0, 0, 0);
	float h = 0.001;
	float n, n1, a, b;
	Frequency /= 100;
	FVector NoisePos = (Pos + Offset) * Frequency;
	n = FMath::PerlinNoise3D(NoisePos);

	n1 = FMath::PerlinNoise3D((NoisePos - FVector(0, h, 0)));
	a = (n - n1) / h;

	n1 = FMath::PerlinNoise3D((NoisePos - FVector(0, 0, h)));
	b = (n - n1) / h;
	curl.X = a - b;

	a = (n - n1) / h;
	
	n1 = FMath::PerlinNoise3D((NoisePos - FVector(h, 0, 0)));
	b = (n - n1) / h;
	curl.Y = a - b;

	a = (n - n1) / h;
	
	n1 = FMath::PerlinNoise3D((NoisePos - FVector(0, h, 0)));
	b = (n - n1) / h;
	curl.Z = a - b;

	Out_AddedPos = Pos + curl * Strength;
	return curl;
}

FVector UNoise::PerlinNoise3D(FVector Pos, FVector& Out_AddedPos, FVector Offset, float Strength, float Frequency, int32 RandomSeed)
{
	FRandomStream Random(RandomSeed);
	
	FVector Displacement;
	FVector Offsets[3];
	for (int32 k = 0; k < 3; ++k)
	{
		const float RandomOffset = 10000.0f * Random.GetFraction();
		Offsets[k] = FVector(RandomOffset, RandomOffset, RandomOffset);
		Offsets[k] += Offset;
		FVector NoisePos = (FVector)(Frequency * (Pos + Offsets[k]));
		Displacement[k] = Strength * FMath::PerlinNoise3D(Frequency * NoisePos);
	}

	Out_AddedPos = Pos + Displacement;
	return Displacement;
}
