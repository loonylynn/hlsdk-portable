#pragma once

// Client-side snapshot receiver and studio-bone override for server ragdolls.
void Ragdoll_Init( void );
void Ragdoll_ClearAll( void );
int Ragdoll_MessageHandler( const char *name, int size, void *buffer );
int Ragdoll_ImpactMessageHandler( const char *name, int size, void *buffer );
void Ragdoll_DebugRenderGate( int entityIndex, int ragdollMarker, int hardwareRenderer );
int Ragdoll_GetBoneTransform( int entityIndex, int boneIndex, float transform[3][4] );
int Ragdoll_GetImpactMarkCount( int entityIndex );
int Ragdoll_GetImpactMark(
	int entityIndex,
	int markIndex,
	int *boneIndex,
	float localPosition[3],
	float localNormal[3],
	float *radius,
	int *bloodColor
);
