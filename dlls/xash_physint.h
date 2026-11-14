/*
 * Xash3D FWGS server physics callback ABI.
 *
 * Keep this table in lockstep with engine/physint.h:
 * SV_PHYSICS_INTERFACE_VERSION is frozen at 6, and new callbacks are appended.
 * Only the callbacks used by this module have concrete signatures here; all
 * other slots retain their pointer-sized ABI positions.
 */
#pragma once

#include "extdll.h"

#define XASH_SERVER_PHYSICS_INTERFACE_VERSION 6

typedef int (*xash_sv_create_entity_fn)( edict_t *ent, const char *classname );
typedef int (*xash_sv_physics_entity_fn)( edict_t *ent );
typedef void (*xash_physics_callback_fn)( void );

struct xash_physics_interface_s
{
	int version;
	xash_sv_create_entity_fn SV_CreateEntity;
	xash_sv_physics_entity_fn SV_PhysicsEntity;
	xash_physics_callback_fn SV_LoadEntities;
	xash_physics_callback_fn SV_UpdatePlayerBaseVelocity;
	xash_physics_callback_fn SV_AllowSaveGame;
	xash_physics_callback_fn SV_TriggerTouch;
	xash_physics_callback_fn SV_CheckFeatures;
	xash_physics_callback_fn DrawDebugTriangles;
	xash_physics_callback_fn DrawNormalTriangles;
	xash_physics_callback_fn DrawOrthoTriangles;
	xash_physics_callback_fn ClipMoveToEntity;
	xash_physics_callback_fn ClipPMoveToEntity;
	xash_physics_callback_fn SV_EndFrame;
	xash_physics_callback_fn pfnPrepWorldFrame;
	xash_physics_callback_fn pfnCreateEntitiesInRestoreList;
	xash_physics_callback_fn pfnAllocString;
	xash_physics_callback_fn pfnMakeString;
	xash_physics_callback_fn pfnGetString;
	xash_physics_callback_fn pfnRestoreDecal;
	xash_physics_callback_fn PM_PlayerTouch;
	xash_physics_callback_fn Mod_ProcessUserData;
	xash_physics_callback_fn SV_HullForBsp;
	xash_physics_callback_fn SV_PlayerThink;
	xash_physics_callback_fn pfnVoiceData;
};

int NPCPhysics_GetInterface( int version, void *callbacks );
void NPCPhysics_Shutdown( void );
