/*
 * Xash3D FWGS server physics callback ABI.
 *
 * Keep this table in lockstep with engine/physint.h. SV_PHYSICS_INTERFACE_VERSION
 * is frozen at 6; the callback structure only uses pointer-sized slots except
 * for its leading version integer.
 */
#pragma once

#include "extdll.h"
#include <stdint.h>

#define XASH_SERVER_PHYSICS_INTERFACE_VERSION 6
#define XASH_WORLD_COLLISION_MESH_API_VERSION 1

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
 * followed by pfnTrace and pfnTraceNoEnts. The tail mirrors the remaining
 * eight pointer slots and pfnGetNativeObject in engine/physint.h.
 */
typedef void (*xash_server_api_reserved_fn)( void );
typedef void *(*xash_get_native_object_fn)( const char *object );

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
	xash_server_api_reserved_fn reservedTail[8];
	xash_get_native_object_fn pfnGetNativeObject;
};

#define XASH_BRUSH_COLLISION_MESH_API_VERSION 1

struct xash_brush_collision_entity_s
{
	int entityIndex;
	int modelIndex;
	float origin[3];
	float angles[3];
};

struct xash_brush_collision_mesh_api_s
{
	int version;
	int (*GetEntityCount)( void );
	int (*GetEntityInfo)( int ordinal, xash_brush_collision_entity_s *info );
	int (*GetMeshInfo)( int modelIndex, uint32_t *vertexCount, uint32_t *indexCount );
	int (*GetMeshData)(
		int modelIndex,
		float *vertices,
		uint32_t vertexCapacity,
		uint32_t *indices,
		uint32_t indexCapacity
	);
};

struct xash_world_collision_mesh_api_s
{
	int version;
	int (*GetMeshInfo)( uint32_t *vertexCount, uint32_t *indexCount );
	int (*GetMeshData)(
		float *vertices,
		uint32_t vertexCapacity,
		uint32_t *indices,
		uint32_t indexCapacity
	);
};

#if defined(__cplusplus)
static_assert(
	offsetof( xash_server_physics_api_s, pfnTrace ) == sizeof(void *) * 21,
	"Xash server physics API trace callback offset mismatch"
);
static_assert(
	offsetof( xash_server_physics_api_s, pfnTraceNoEnts ) == sizeof(void *) * 22,
	"Xash server physics API trace-no-entities callback offset mismatch"
);
static_assert(
	offsetof( xash_server_physics_api_s, pfnGetNativeObject ) == sizeof(void *) * 31,
	"Xash server physics API native-object callback offset mismatch"
);
#endif

typedef xash_trace_s xash_trace_t;

int NPCPhysics_GetInterface( int version, void *serverAPI, void *callbacks );
int NPCPhysics_HasRagdollDefinition( edict_t *ent );
int NPCPhysics_TryCreateRagdoll( edict_t *ent );
int NPCPhysics_RestorePersistentRagdoll( edict_t *ent );
void NPCPhysics_SaveRagdollState( edict_t *ent );
int NPCPhysics_IsRagdollEntity( edict_t *ent );
int NPCPhysics_ApplyPushableRagdollPush( edict_t *pushable, const float *velocity );
int NPCPhysics_ApplyBlastImpulse(
	edict_t *targetEntity,
	const float *origin,
	const float *targetPoint,
	float radius,
	float blastDamage
);
int NPCPhysics_TraceAndApplyImpact(
	const float *start,
	const float *end,
	float maxFraction,
	float impulseMagnitude,
	float *hitFraction,
	float *hitPosition,
	edict_t **hitEntity
);
int NPCPhysics_TraceRagdollImpact(
	const float *start,
	const float *end,
	float maxFraction,
	float *hitFraction,
	float *hitPosition,
	edict_t **hitEntity
);
int NPCPhysics_TraceTargetAndApplyImpact(
	edict_t *targetEntity,
	const float *start,
	const float *end,
	float maxFraction,
	float impulseMagnitude
);
int NPCPhysics_TriggerHornetProjectileImpact(
	edict_t *projectile,
	edict_t *target,
	const float *start,
	const float *end,
	float maxFraction
);
int NPCPhysics_SendRagdollImpactMark(
	edict_t *targetEntity,
	const float *start,
	const float *end,
	float maxFraction
);
int NPCPhysics_IsExplosiveProjectile( edict_t *projectile );
int NPCPhysics_TriggerExplosiveProjectileImpact(
	edict_t *projectile,
	edict_t *target,
	const float *start,
	const float *end,
	float hitFraction,
	const float *hitPosition
);
// Source SDK ammo-force conversion: grains + ft/s -> kg*inch/s.
float NPCPhysics_CalculateSourceBulletImpulse( float grains, float feetPerSecond );
int NPCPhysics_ApplyImpactImpulseToRagdoll(
	edict_t *targetEntity,
	const float *impactPosition,
	const float *direction,
	float impulseMagnitude,
	const char *impactName
);
int NPCPhysics_ApplyBulletImpulseToRagdoll(
	edict_t *targetEntity,
	const float *impactPosition,
	const float *direction,
	float impulseMagnitude
);
void NPCPhysics_ResetMap( void );
void NPCPhysics_Shutdown( void );
