/*
 * Jolt-backed NPC shadow physics for Xash3D FWGS.
 *
 * GoldSrc remains authoritative for monster AI, navigation, animation and
 * ordinary world movement. Jolt supplies dynamic contact resolution between
 * living step/fly/toss monsters and existing toss/bounce/pushstep props. The resulting NPC
 * displacement is clipped through the engine's own monster hull trace before
 * it is written back to the edict.
 *
 * This is intentionally a first-stage bridge, not a wholesale VPhysics clone:
 * BSP collision, source-style ragdolls, constraints and articulated bodies are
 * not part of this implementation yet.
 */

#include "extdll.h"
#include "eiface.h"
#include "enginecallback.h"
#include "xash_physint.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>

#include <algorithm>
#include <cmath>
#include <vector>

extern enginefuncs_t g_engfuncs;
extern globalvars_t *gpGlobals;

namespace
{
	using namespace JPH;

	static constexpr ObjectLayer NPC_PHYSICS_LAYER = 0;
	static constexpr uint32_t NPC_PHYSICS_MAX_BODIES = 1024;
	static constexpr float NPC_PHYSICS_MAX_FRAME_TIME = 0.05f;
	static constexpr float NPC_PHYSICS_MIN_EXTENT = 1.0f;

	class NPCBroadPhaseLayers final : public BroadPhaseLayerInterface
	{
	public:
		uint GetNumBroadPhaseLayers() const override
		{
			return 1;
		}

		BroadPhaseLayer GetBroadPhaseLayer( ObjectLayer ) const override
		{
			return BroadPhaseLayer( 0 );
		}
	};

	class NPCObjectLayerFilter final : public ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide( ObjectLayer, ObjectLayer ) const override
		{
			return true;
		}
	};

	class NPCBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide( ObjectLayer, BroadPhaseLayer ) const override
		{
			return true;
		}
	};

	enum NPCPhysicsBodyKind
	{
		NPC_PHYSICS_MONSTER,
		NPC_PHYSICS_PROP
	};

	struct NPCPhysicsBody
	{
		edict_t *entity;
		int serialNumber;
		BodyID bodyID;
		::Vector mins;
		::Vector maxs;
		::Vector center;
		NPCPhysicsBodyKind kind;
		uint32_t seenFrame;
		float mass;
	};

	struct NPCPhysicsWorld
	{
		NPCBroadPhaseLayers broadPhaseLayers;
		NPCObjectLayerFilter objectLayerFilter;
		NPCBroadPhaseFilter broadPhaseFilter;
		PhysicsSystem system;
		JobSystemSingleThreaded jobs;
		TempAllocatorImpl *tempAllocator;
		bool initialized;

		NPCPhysicsWorld()
			: jobs( 2048 ),
			  tempAllocator( NULL ),
			  initialized( false )
		{
		}
	};

	static NPCPhysicsWorld g_npcPhysics;
	static std::vector<NPCPhysicsBody> g_npcBodies;
	static uint32_t g_npcPhysicsFrame = 0;

	static cvar_t g_svNpcVPhysics = { "sv_npc_vphysics", "1", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsMass = { "sv_npc_vphysics_mass", "80", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsPropMass = { "sv_npc_vphysics_prop_mass", "25", FCVAR_SERVER };

	static float NPCPhysics_GetCvar( const char *name, float fallback )
	{
		if( !g_engfuncs.pfnCVarGetFloat )
			return fallback;

		return g_engfuncs.pfnCVarGetFloat( name );
	}

	static bool NPCPhysics_Enabled()
	{
		return NPCPhysics_GetCvar( "sv_npc_vphysics", 1.0f ) >= 0.5f;
	}

	static bool NPCPhysics_Initialize()
	{
		if( g_npcPhysics.initialized )
			return true;

		RegisterDefaultAllocator();
		Factory::sInstance = new Factory();
		RegisterTypes();

		g_npcPhysics.tempAllocator = new TempAllocatorImpl( 10 * 1024 * 1024 );
		g_npcPhysics.system.Init(
			NPC_PHYSICS_MAX_BODIES,
			0,
			2048,
			2048,
			g_npcPhysics.broadPhaseLayers,
			g_npcPhysics.broadPhaseFilter,
			g_npcPhysics.objectLayerFilter
		);
		g_npcPhysics.system.SetGravity( Vec3::sZero() );
		g_npcPhysics.initialized = true;
		return true;
	}

	static void NPCPhysics_DestroyBody( NPCPhysicsBody &record )
	{
		if( !g_npcPhysics.initialized || record.bodyID.IsInvalid() )
			return;

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		if( bodyInterface.IsAdded( record.bodyID ) )
			bodyInterface.RemoveBody( record.bodyID );
		bodyInterface.DestroyBody( record.bodyID );
		record.bodyID = BodyID();
	}

	static bool NPCPhysics_IsAliveMonster( const edict_t *ent )
	{
		if( !ent || ent->free )
			return false;

		const entvars_t &v = ent->v;
		if( ( v.flags & FL_MONSTER ) == 0 || ( v.flags & ( FL_CLIENT | FL_KILLME | FL_DORMANT ) ) != 0 )
			return false;

		// Match Source's shadow-hull eligibility for movable NPCs. GoldSrc
		// also uses MOVETYPE_TOSS for living creatures such as leeches when
		// they are outside their normal movement state.
		if( ( v.movetype != MOVETYPE_STEP &&
			v.movetype != MOVETYPE_FLY &&
			v.movetype != MOVETYPE_FLYMISSILE &&
			v.movetype != MOVETYPE_TOSS ) ||
			v.solid == SOLID_NOT || v.solid == SOLID_TRIGGER )
			return false;

		return v.health > 0.0f && v.deadflag == DEAD_NO;
	}

	static bool NPCPhysics_IsMovableProp( const edict_t *ent )
	{
		if( !ent || ent->free )
			return false;

		const entvars_t &v = ent->v;
		if( ( v.flags & ( FL_MONSTER | FL_CLIENT | FL_KILLME | FL_DORMANT ) ) != 0 )
			return false;

		if( v.movetype != MOVETYPE_TOSS && v.movetype != MOVETYPE_BOUNCE && v.movetype != MOVETYPE_PUSHSTEP )
			return false;

		if( v.solid != SOLID_BBOX || v.modelindex == 0 )
			return false;

		// Include GoldSrc func_pushable (MOVETYPE_PUSHSTEP) and ordinary toss /
		// bounce objects. Exclude tiny debris and point entities so they cannot
		// flood the body budget or turn every effect sprite into a rigid body.
		if( v.size[0] < 8.0f || v.size[1] < 8.0f || v.size[2] < 8.0f )
			return false;

		return true;
	}

	static bool NPCPhysics_BoundsChanged( const NPCPhysicsBody &record, const entvars_t *pev )
	{
		const float epsilon = 0.5f;
		for( int i = 0; i < 3; ++i )
		{
			if( fabsf( record.mins[i] - pev->mins[i] ) > epsilon ||
				fabsf( record.maxs[i] - pev->maxs[i] ) > epsilon )
				return true;
		}
		return false;
	}

	static NPCPhysicsBody *NPCPhysics_FindBody( edict_t *ent )
	{
		for( size_t i = 0; i < g_npcBodies.size(); ++i )
		{
			if( g_npcBodies[i].entity == ent && g_npcBodies[i].serialNumber == ent->serialnumber )
				return &g_npcBodies[i];
		}
		return NULL;
	}

	static bool NPCPhysics_CreateBody( edict_t *ent, NPCPhysicsBodyKind kind, uint32_t seenFrame )
	{
		if( !g_npcPhysics.initialized || !ent || ent->free )
			return false;

		const entvars_t *pev = &ent->v;
		::Vector center = ( pev->mins + pev->maxs ) * 0.5f;
		::Vector half = ( pev->maxs - pev->mins ) * 0.5f;

		half[0] = std::max( NPC_PHYSICS_MIN_EXTENT, half[0] );
		half[1] = std::max( NPC_PHYSICS_MIN_EXTENT, half[1] );
		half[2] = std::max( NPC_PHYSICS_MIN_EXTENT, half[2] );

		const float convexRadius = std::min( 0.5f, std::min( half[0], std::min( half[1], half[2] ) ) * 0.1f );
		float mass = kind == NPC_PHYSICS_MONSTER
			? NPCPhysics_GetCvar( "sv_npc_vphysics_mass", 80.0f )
			: NPCPhysics_GetCvar( "sv_npc_vphysics_prop_mass", 25.0f );
		mass = std::max( 1.0f, std::min( mass, 500.0f ) );

		const Vec3 position(
			pev->origin[0] + center[0],
			pev->origin[1] + center[1],
			pev->origin[2] + center[2]
		);

		BodyCreationSettings settings(
			new BoxShape( Vec3( half[0], half[1], half[2] ), convexRadius ),
			RVec3( position ),
			Quat::sIdentity(),
			EMotionType::Dynamic,
			NPC_PHYSICS_LAYER
		);
		settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
		settings.mMassPropertiesOverride.mMass = mass;
		// GoldSrc continues to own entity angles and angular motion. Keep the
		// shadow bodies translation-only so Jolt's contact response cannot rotate
		// a proxy independently of the engine's collision bounds.
		settings.mAllowedDOFs = EAllowedDOFs::TranslationX | EAllowedDOFs::TranslationY | EAllowedDOFs::TranslationZ;
		settings.mGravityFactor = 0.0f; // GoldSrc continues to own gravity and world movement.
		settings.mAllowSleeping = false;
		settings.mFriction = kind == NPC_PHYSICS_MONSTER ? 0.8f : 0.6f;
		settings.mRestitution = 0.0f;
		settings.mLinearDamping = 0.0f;
		settings.mAngularDamping = 0.05f;
		settings.mMaxLinearVelocity = 1200.0f;

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		BodyID bodyID = bodyInterface.CreateAndAddBody( settings, EActivation::Activate );
		if( bodyID.IsInvalid() )
			return false;

		NPCPhysicsBody record;
		record.entity = ent;
		record.serialNumber = ent->serialnumber;
		record.bodyID = bodyID;
		record.mins = pev->mins;
		record.maxs = pev->maxs;
		record.center = center;
		record.kind = kind;
		record.seenFrame = seenFrame;
		record.mass = mass;
		g_npcBodies.push_back( record );
		return true;
	}

	static void NPCPhysics_MarkEntity( edict_t *ent, NPCPhysicsBodyKind kind, uint32_t seenFrame )
	{
		NPCPhysicsBody *record = NPCPhysics_FindBody( ent );
		if( record && NPCPhysics_BoundsChanged( *record, &ent->v ) )
		{
			NPCPhysics_DestroyBody( *record );
			for( size_t i = 0; i < g_npcBodies.size(); ++i )
			{
				if( &g_npcBodies[i] == record )
				{
					g_npcBodies.erase( g_npcBodies.begin() + i );
					break;
				}
			}
			record = NULL;
		}

		if( !record )
			NPCPhysics_CreateBody( ent, kind, seenFrame );
		else
			record->seenFrame = seenFrame;
	}

	static Vec3 NPCPhysics_EntityVelocity( const entvars_t *pev )
	{
		return Vec3( pev->velocity[0], pev->velocity[1], pev->velocity[2] );
	}

	static int NPCPhysics_PhysicsEntity( edict_t *ent )
	{
		if( !NPCPhysics_Enabled() || !NPCPhysics_Initialize() )
			return 0;

		const uint32_t seenFrame = g_npcPhysicsFrame + 1;
		if( NPCPhysics_IsAliveMonster( ent ) )
			NPCPhysics_MarkEntity( ent, NPC_PHYSICS_MONSTER, seenFrame );
		else if( NPCPhysics_IsMovableProp( ent ) )
			NPCPhysics_MarkEntity( ent, NPC_PHYSICS_PROP, seenFrame );

		// Returning 0 is essential: the normal GoldSrc MOVETYPE_STEP / TOSS
		// simulation remains active and continues to own gravity and world clips.
		return 0;
	}

	static void NPCPhysics_ApplyMonsterShadow( NPCPhysicsBody &record, float deltaTime, BodyInterface &bodyInterface )
	{
		edict_t *ent = record.entity;
		if( !NPCPhysics_IsAliveMonster( ent ) )
			return;

		const entvars_t *pev = &ent->v;
		const RVec3 physicalPosition = bodyInterface.GetPosition( record.bodyID );
		const Vec3 currentVelocity = bodyInterface.GetLinearVelocity( record.bodyID );

		const Vec3 targetPosition(
			pev->origin[0] + record.center[0],
			pev->origin[1] + record.center[1],
			pev->origin[2] + record.center[2]
		);

		Vec3 error(
			targetPosition.GetX() - float( physicalPosition.GetX() ),
			targetPosition.GetY() - float( physicalPosition.GetY() ),
			targetPosition.GetZ() - float( physicalPosition.GetZ() )
		);

		// Teleports, map transitions and scripted repositioning should rebase
		// the shadow body rather than producing a huge catch-up impulse.
		if( error.LengthSq() > 128.0f * 128.0f )
		{
			bodyInterface.SetPositionAndRotation(
				record.bodyID,
				RVec3( targetPosition ),
				Quat::sIdentity(),
				EActivation::Activate
			);
			bodyInterface.SetLinearVelocity( record.bodyID, NPCPhysics_EntityVelocity( pev ) );
			return;
		}

		// A damped velocity controller is the GoldSrc-side equivalent of an
		// NPC shadow controller: AI supplies a target, Jolt supplies contact
		// response. Limit the correction so a heavy prop can still displace it.
		Vec3 desiredVelocity = NPCPhysics_EntityVelocity( pev ) + error * 8.0f;
		const float desiredSpeed = desiredVelocity.Length();
		if( desiredSpeed > 600.0f )
			desiredVelocity *= 600.0f / desiredSpeed;

		Vec3 impulse = ( desiredVelocity - currentVelocity ) * ( record.mass * 0.30f );
		const float maxImpulse = record.mass * std::max( 30.0f, deltaTime * 1800.0f );
		const float impulseLength = impulse.Length();
		if( impulseLength > maxImpulse && impulseLength > 0.0f )
			impulse *= maxImpulse / impulseLength;

		bodyInterface.AddImpulse( record.bodyID, impulse );
	}

	static void NPCPhysics_SyncPropToJolt( NPCPhysicsBody &record, BodyInterface &bodyInterface )
	{
		edict_t *ent = record.entity;
		if( !NPCPhysics_IsMovableProp( ent ) )
			return;

		const entvars_t *pev = &ent->v;
		const RVec3 position(
			pev->origin[0] + record.center[0],
			pev->origin[1] + record.center[1],
			pev->origin[2] + record.center[2]
		);

		// Existing GoldSrc prop movement remains authoritative. Jolt only
		// resolves the prop's contact with the NPC shadow bodies this frame.
		bodyInterface.SetPositionAndRotation(
			record.bodyID,
			position,
			Quat::sIdentity(),
			EActivation::Activate
		);
		bodyInterface.SetLinearVelocity( record.bodyID, NPCPhysics_EntityVelocity( pev ) );
	}

	static void NPCPhysics_ClipMonsterToWorld( NPCPhysicsBody &record, BodyInterface &bodyInterface )
	{
		edict_t *ent = record.entity;
		if( !NPCPhysics_IsAliveMonster( ent ) || !g_engfuncs.pfnTraceMonsterHull || !g_engfuncs.pfnSetOrigin )
			return;

		const RVec3 physicalPosition = bodyInterface.GetPosition( record.bodyID );
		float start[3] = {
			ent->v.origin[0],
			ent->v.origin[1],
			ent->v.origin[2]
		};
		float end[3] = {
			float( physicalPosition.GetX() ) - record.center[0],
			float( physicalPosition.GetY() ) - record.center[1],
			float( physicalPosition.GetZ() ) - record.center[2]
		};

		const float displacement =
			( end[0] - start[0] ) * ( end[0] - start[0] ) +
			( end[1] - start[1] ) * ( end[1] - start[1] ) +
			( end[2] - start[2] ) * ( end[2] - start[2] );

		if( displacement < 0.01f )
			return;

		TraceResult trace;
		memset( &trace, 0, sizeof( trace ) );
		g_engfuncs.pfnTraceMonsterHull( ent, start, end, 0, ent, &trace );

		float resolved[3];
		if( trace.fAllSolid || trace.fStartSolid )
		{
			resolved[0] = start[0];
			resolved[1] = start[1];
			resolved[2] = start[2];
		}
		else
		{
			resolved[0] = trace.vecEndPos[0];
			resolved[1] = trace.vecEndPos[1];
			resolved[2] = trace.vecEndPos[2];
		}

		g_engfuncs.pfnSetOrigin( ent, resolved );

		// Keep the shadow body on the safe side of the GoldSrc world hull.
		const RVec3 clippedPosition(
			resolved[0] + record.center[0],
			resolved[1] + record.center[1],
			resolved[2] + record.center[2]
		);
		bodyInterface.SetPositionAndRotation(
			record.bodyID,
			clippedPosition,
			Quat::sIdentity(),
			EActivation::Activate
		);

		if( trace.flFraction < 1.0f && !trace.fAllSolid && !trace.fStartSolid )
		{
			Vec3 velocity = bodyInterface.GetLinearVelocity( record.bodyID );
			const Vec3 normal(
				trace.vecPlaneNormal[0],
				trace.vecPlaneNormal[1],
				trace.vecPlaneNormal[2]
			);
			const float intoSurface = velocity.Dot( normal );
			if( intoSurface < 0.0f )
			{
				velocity -= normal * intoSurface;
				bodyInterface.SetLinearVelocity( record.bodyID, velocity );
			}
		}
	}

	static void NPCPhysics_CopyMonsterVelocityToGoldSrc( NPCPhysicsBody &record, BodyInterface &bodyInterface )
	{
		edict_t *ent = record.entity;
		if( !NPCPhysics_IsAliveMonster( ent ) )
			return;

		// Feed the contact-resolved shadow velocity back into GoldSrc. Without
		// this, the next MOVETYPE_STEP tick would continue using the AI's old
		// velocity and partially undo the impulse delivered by a physical prop.
		const Vec3 linear = bodyInterface.GetLinearVelocity( record.bodyID );
		ent->v.velocity[0] = linear.GetX();
		ent->v.velocity[1] = linear.GetY();
		ent->v.velocity[2] = linear.GetZ();
	}

	static void NPCPhysics_CopyPropVelocityToGoldSrc( NPCPhysicsBody &record, BodyInterface &bodyInterface )
	{
		edict_t *ent = record.entity;
		if( !NPCPhysics_IsMovableProp( ent ) )
			return;

		const Vec3 linear = bodyInterface.GetLinearVelocity( record.bodyID );

		ent->v.velocity[0] = linear.GetX();
		ent->v.velocity[1] = linear.GetY();
		ent->v.velocity[2] = linear.GetZ();
	}

	static void NPCPhysics_EndFrame( void )
	{
		if( !g_npcPhysics.initialized )
			return;

		if( !NPCPhysics_Enabled() )
		{
			for( size_t i = 0; i < g_npcBodies.size(); ++i )
				NPCPhysics_DestroyBody( g_npcBodies[i] );
			g_npcBodies.clear();
			return;
		}

		const uint32_t frame = g_npcPhysicsFrame + 1;
		for( size_t i = g_npcBodies.size(); i > 0; --i )
		{
			NPCPhysicsBody &record = g_npcBodies[i - 1];
			if( record.seenFrame != frame || !record.entity || record.entity->free ||
				record.entity->serialnumber != record.serialNumber )
			{
				NPCPhysics_DestroyBody( record );
				g_npcBodies.erase( g_npcBodies.begin() + ( i - 1 ) );
			}
		}

		g_npcPhysicsFrame = frame;
		if( g_npcBodies.empty() )
			return;

		float deltaTime = gpGlobals ? gpGlobals->frametime : ( 1.0f / 60.0f );
		if( deltaTime <= 0.0f )
			return;
		deltaTime = std::min( deltaTime, NPC_PHYSICS_MAX_FRAME_TIME );

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		for( size_t i = 0; i < g_npcBodies.size(); ++i )
		{
			NPCPhysicsBody &record = g_npcBodies[i];
			if( record.kind == NPC_PHYSICS_MONSTER )
				NPCPhysics_ApplyMonsterShadow( record, deltaTime, bodyInterface );
			else
				NPCPhysics_SyncPropToJolt( record, bodyInterface );
		}

		const uint collisionSteps = std::max( 1, std::min( 4, int( ceilf( deltaTime * 60.0f ) ) ) );
		g_npcPhysics.system.Update(
			deltaTime,
			collisionSteps,
			g_npcPhysics.tempAllocator,
			&g_npcPhysics.jobs
		);

		for( size_t i = 0; i < g_npcBodies.size(); ++i )
		{
			NPCPhysicsBody &record = g_npcBodies[i];
			if( record.kind == NPC_PHYSICS_MONSTER )
			{
				NPCPhysics_ClipMonsterToWorld( record, bodyInterface );
				NPCPhysics_CopyMonsterVelocityToGoldSrc( record, bodyInterface );
			}
			else
				NPCPhysics_CopyPropVelocityToGoldSrc( record, bodyInterface );
		}
	}
}

int NPCPhysics_GetInterface( int version, void *callbacks )
{
	if( version != XASH_SERVER_PHYSICS_INTERFACE_VERSION || !callbacks )
		return FALSE;

	CVAR_REGISTER( &g_svNpcVPhysics );
	CVAR_REGISTER( &g_svNpcVPhysicsMass );
	CVAR_REGISTER( &g_svNpcVPhysicsPropMass );

	xash_physics_interface_s *physics = reinterpret_cast<xash_physics_interface_s *>( callbacks );
	if( !physics )
		return FALSE;
	memset( physics, 0, sizeof( *physics ) );
	physics->SV_PhysicsEntity = NPCPhysics_PhysicsEntity;
	physics->SV_EndFrame = NPCPhysics_EndFrame;

	if( NPCPhysics_Enabled() )
	{
		if( !NPCPhysics_Initialize() )
			return FALSE;

		ALERT( at_console, "[NPC physics] Jolt shadow-body bridge initialized.\n" );
	}
	return TRUE;
}

void NPCPhysics_ResetMap( void )
{
	if( !g_npcPhysics.initialized )
		return;

	for( size_t i = 0; i < g_npcBodies.size(); ++i )
		NPCPhysics_DestroyBody( g_npcBodies[i] );
	g_npcBodies.clear();
	g_npcPhysicsFrame = 0;
}

void NPCPhysics_Shutdown( void )
{
	for( size_t i = 0; i < g_npcBodies.size(); ++i )
		NPCPhysics_DestroyBody( g_npcBodies[i] );
	g_npcBodies.clear();

	if( g_npcPhysics.initialized )
	{
		UnregisterTypes();
		delete Factory::sInstance;
		Factory::sInstance = NULL;
		delete g_npcPhysics.tempAllocator;
		g_npcPhysics.tempAllocator = NULL;
		g_npcPhysics.initialized = false;
	}
	g_npcPhysicsFrame = 0;
}
