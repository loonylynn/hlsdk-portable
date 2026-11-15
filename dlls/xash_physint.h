/*
 * Xash3D FWGS server physics callback ABI.
 *
 * Keep this table in lockstep with engine/physint.h. SV_PHYSICS_INTERFACE_VERSION
 * is frozen at 6; the callback structure only uses pointer-sized slots except
 * for its leading version integer.
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

/*
 * server_physics_api_t begins with 21 pointer-sized callbacks/data fields,
 * followed by pfnTrace and pfnTraceNoEnts. The trace_t layout mirrors
 * common/const.h in Xash3D FWGS. The engine passes this table to
 * Server_GetPhysicsInterface before any server physics callbacks are run.
 */
typedef void (*xash_server_api_reserved_fn)( void );

struct xash_trace_plane_s
{
	float normal[3];
	float dist;
};

struct xash_trace_s
{
	int allsolid;
	int startsolid;
	int inopen;
	int inwater;
	float fraction;
	float endpos[3];
	xash_trace_plane_s plane;
	edict_t *ent;
	int hitgroup;
};

typedef xash_trace_s (*xash_trace_fn)(
	const float *start,
	float *mins,
	float *maxs,
	const float *end,
	int traceType,
	edict_t *ignore
);

struct xash_server_physics_api_s
{
	xash_server_api_reserved_fn reserved[21];
	xash_trace_fn pfnTrace;
	xash_trace_fn pfnTraceNoEnts;
};

typedef xash_trace_s xash_trace_t;

int NPCPhysics_GetInterface( int version, void *serverAPI, void *callbacks );
int NPCPhysics_TryCreateRagdoll( edict_t *ent );
void NPCPhysics_ResetMap( void );
void NPCPhysics_Shutdown( void );
