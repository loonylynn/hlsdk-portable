#pragma once

// Client-side snapshot receiver and studio-bone override for server ragdolls.
void Ragdoll_Init( void );
void Ragdoll_ClearAll( void );
int Ragdoll_MessageHandler( const char *name, int size, void *buffer );
void Ragdoll_DebugRenderGate( int entityIndex, int ragdollMarker, int hardwareRenderer );
int Ragdoll_GetBoneTransform( int entityIndex, int ragdollMarker, int boneIndex, float transform[3][4] );
