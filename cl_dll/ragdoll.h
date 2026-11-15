#pragma once

// Client-side snapshot receiver and studio-bone override for server ragdolls.
void Ragdoll_Init( void );
void Ragdoll_ClearAll( void );
int Ragdoll_MessageHandler( const char *name, int size, void *buffer );
int Ragdoll_ImpactMessageHandler( const char *name, int size, void *buffer );
void Ragdoll_DebugRenderGate( int entityIndex, int ragdollMarker, int hardwareRenderer );
int Ragdoll_GetBoneTransform( int entityIndex, int boneIndex, float transform[3][4] );
int Ragdoll_TraceLine(
	const float start[3],
	const float end[3],
	float maximumFraction,
	float *hitFraction,
	float hitPosition[3],
	int *hitEntityIndex
);
int Ragdoll_GetImpactMarkCount( int entityIndex );
int Ragdoll_GetImpactMark(
	int entityIndex,
	int markIndex,
	int *boneIndex,
	float rayStart[3],
	float worldPosition[3],
	float *radius,
	int *bloodColor,
	int *ragdollID
);
