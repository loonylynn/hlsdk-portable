/*
 * Death-only Jolt ragdolls for GoldSrc monsters.
 *
 * Living NPC movement, AI, animation and collision remain entirely in GoldSrc.
 * Jolt is entered only from CBaseMonster::Killed(), after the final animated
 * pose and the ordinary corpse launch velocity have been captured.
 *
 * The first pass uses one spherical rigid body per studio bone and point
 * constraints at parent/child joints. GoldSrc traces each body against the
 * world after the Jolt step; this is a transitional world-collision bridge,
 * not a full BSP shape integration.
 */

#include "extdll.h"
#include "eiface.h"
#include "enginecallback.h"
#include "cbase.h"
#include "xash_physint.h"
#include "studio.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

extern enginefuncs_t g_engfuncs;
extern globalvars_t *gpGlobals;

namespace
{
	using namespace JPH;

	static constexpr ObjectLayer RAGDOLL_OBJECT_LAYER = 0;
	static constexpr uint32_t RAGDOLL_MAX_BODIES = 4096;
	static constexpr uint32_t RAGDOLL_MAX_BODY_PAIRS = 8192;
	static constexpr uint32_t RAGDOLL_MAX_CONTACT_CONSTRAINTS = 8192;
	static constexpr float RAGDOLL_MAX_FRAME_TIME = 0.05f;
	static constexpr float RAGDOLL_POSE_INTERVAL = 0.05f;
	static constexpr int RAGDOLL_NETWORK_BONES_PER_CHUNK = 10;
	static constexpr float RAGDOLL_DEGREES_TO_RADIANS = 0.01745329251994329577f;

	class RagdollBroadPhaseLayers final : public BroadPhaseLayerInterface
	{
	public:
		uint GetNumBroadPhaseLayers() const override { return 1; }
		BroadPhaseLayer GetBroadPhaseLayer( ObjectLayer ) const override { return BroadPhaseLayer( 0 ); }
	};

	class RagdollObjectLayerFilter final : public ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide( ObjectLayer, ObjectLayer ) const override { return true; }
	};

	class RagdollBroadPhaseFilter final : public ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide( ObjectLayer, BroadPhaseLayer ) const override { return true; }
	};

	struct RagdollBone
	{
		BodyID bodyID;
		int parent;
		float radius;
		Vec3 initialPosition;
	};

	struct NPCPhysicsRagdoll
	{
		edict_t *entity;
		int serialNumber;
		int networkID;
		int boneCount;
		float nextPoseTime;
		::Vector entityOriginOffset;
		std::vector<RagdollBone> bones;
		std::vector<Ref<TwoBodyConstraint> > constraints;
		Ref<GroupFilterTable> groupFilter;
		std::vector<Vec3> previousPositions;

		NPCPhysicsRagdoll()
			: entity( NULL ),
			  serialNumber( 0 ),
			  networkID( 0 ),
			  boneCount( 0 ),
			  nextPoseTime( 0.0f )
		{
		}
	};

	struct NPCPhysicsWorld
	{
		RagdollBroadPhaseLayers broadPhaseLayers;
		RagdollObjectLayerFilter objectLayerFilter;
		RagdollBroadPhaseFilter broadPhaseFilter;
		PhysicsSystem system;
		JobSystemSingleThreaded jobs;
		TempAllocatorImpl *tempAllocator;
		bool initialized;

		NPCPhysicsWorld()
			: jobs(),
			  tempAllocator( NULL ),
			  initialized( false )
		{
		}
	};

	static NPCPhysicsWorld g_npcPhysics;
	static std::vector<NPCPhysicsRagdoll> g_ragdolls;
	static int g_nextRagdollID = 1;
	static int g_msgRagdollPose = 0;

	static cvar_t g_svNpcVPhysics = { "sv_npc_vphysics", "1", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsMass = { "sv_npc_vphysics_mass", "80", FCVAR_SERVER };

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

		// Jolt's job system allocates during Init(). Register the allocator
		// first; doing this in a global constructor caused a startup crash.
		RegisterDefaultAllocator();
		g_npcPhysics.jobs.Init( 2048 );

		Factory::sInstance = new Factory();
		RegisterTypes();

		g_npcPhysics.tempAllocator = new TempAllocatorImpl( 10 * 1024 * 1024 );
		g_npcPhysics.system.Init(
			RAGDOLL_MAX_BODIES,
			0,
			RAGDOLL_MAX_BODY_PAIRS,
			RAGDOLL_MAX_CONTACT_CONSTRAINTS,
			g_npcPhysics.broadPhaseLayers,
			g_npcPhysics.broadPhaseFilter,
			g_npcPhysics.objectLayerFilter
		);
		g_npcPhysics.system.SetGravity( Vec3( 0.0f, 0.0f, -800.0f ) );
		g_npcPhysics.initialized = true;
		return true;
	}

	static Quat NPCPhysics_AnglesToQuaternion( const ::Vector &angles )
	{
		const float halfPitch = angles[0] * RAGDOLL_DEGREES_TO_RADIANS * 0.5f;
		const float halfYaw = angles[1] * RAGDOLL_DEGREES_TO_RADIANS * 0.5f;
		const float halfRoll = angles[2] * RAGDOLL_DEGREES_TO_RADIANS * 0.5f;

		const float sp = sinf( halfPitch );
		const float cp = cosf( halfPitch );
		const float sy = sinf( halfYaw );
		const float cy = cosf( halfYaw );
		const float sr = sinf( halfRoll );
		const float cr = cosf( halfRoll );

		// GoldSrc QAngle order: pitch, yaw, roll. This is the same
		// AngleQuaternion convention used by the studio model pipeline.
		// Match the GoldSrc studio AngleQuaternion implementation:
		// angle[0] = pitch, angle[1] = yaw, angle[2] = roll.
		Quat result(
			sr * cp * cy - cr * sp * sy,
			cr * sp * cy + sr * cp * sy,
			cr * cp * sy - sr * sp * cy,
			cr * cp * cy + sr * sp * sy
		);
		result = result.Normalized();
		return result;
	}

	static bool NPCPhysics_GetStudioHeader( edict_t *ent, studiohdr_t **headerOut )
	{
		if( !ent || !headerOut || !g_engfuncs.pfnGetModelPtr )
			return false;

		studiohdr_t *header = reinterpret_cast<studiohdr_t *>( GET_MODEL_PTR( ent ) );
		if( !header )
			return false;

		if( header->ident != IDSTUDIOHEADER || header->version != STUDIO_VERSION )
			return false;

		if( header->length < (int)sizeof( studiohdr_t ) ||
			header->numbones < 1 ||
			header->numbones > MAXSTUDIOBONES ||
			header->boneindex < (int)sizeof( studiohdr_t ) )
			return false;

		const size_t length = static_cast<size_t>( header->length );
		const size_t boneOffset = static_cast<size_t>( header->boneindex );
		const size_t boneBytes = static_cast<size_t>( header->numbones ) * sizeof( mstudiobone_t );
		if( boneOffset > length || boneBytes > length - boneOffset )
			return false;

		if( header->numhitboxes < 0 || header->numhitboxes > 1024 )
			return false;

		if( header->numhitboxes > 0 )
		{
			if( header->hitboxindex < (int)sizeof( studiohdr_t ) )
				return false;

			const size_t hitboxOffset = static_cast<size_t>( header->hitboxindex );
			const size_t hitboxBytes = static_cast<size_t>( header->numhitboxes ) * sizeof( mstudiobbox_t );
			if( hitboxOffset > length || hitboxBytes > length - hitboxOffset )
				return false;
		}

		*headerOut = header;
		return true;
	}

	static void NPCPhysics_DestroyRagdoll( NPCPhysicsRagdoll &ragdoll )
	{
		if( !g_npcPhysics.initialized )
			return;

		for( size_t i = 0; i < ragdoll.constraints.size(); ++i )
			g_npcPhysics.system.RemoveConstraint( ragdoll.constraints[i].Get() );
		ragdoll.constraints.clear();

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		for( size_t i = 0; i < ragdoll.bones.size(); ++i )
		{
			const BodyID bodyID = ragdoll.bones[i].bodyID;
			if( bodyID.IsInvalid() )
				continue;

			if( bodyInterface.IsAdded( bodyID ) )
				bodyInterface.RemoveBody( bodyID );
			bodyInterface.DestroyBody( bodyID );
			ragdoll.bones[i].bodyID = BodyID();
		}

		ragdoll.bones.clear();
		ragdoll.previousPositions.clear();
		ragdoll.groupFilter = Ref<GroupFilterTable>();
	}

	static void NPCPhysics_DestroyAllRagdolls()
	{
		for( size_t i = 0; i < g_ragdolls.size(); ++i )
			NPCPhysics_DestroyRagdoll( g_ragdolls[i] );
		g_ragdolls.clear();
	}

	static float NPCPhysics_BoneRadius( const mstudiobbox_t *hitboxes, int hitboxCount, int boneIndex )
	{
		float radius = 2.5f;

		for( int i = 0; i < hitboxCount; ++i )
		{
			if( hitboxes[i].bone != boneIndex )
				continue;

			const float x = hitboxes[i].bbmax[0] - hitboxes[i].bbmin[0];
			const float y = hitboxes[i].bbmax[1] - hitboxes[i].bbmin[1];
			const float z = hitboxes[i].bbmax[2] - hitboxes[i].bbmin[2];
			const float candidate = sqrtf( x * x + y * y + z * z ) * 0.25f;
			radius = std::max( radius, std::min( 9.0f, candidate ) );
		}

		return std::max( 2.0f, std::min( 9.0f, radius ) );
	}

	static int NPCPhysics_AllocateRagdollID()
	{
		int result = g_nextRagdollID++;
		if( g_nextRagdollID > 30000 )
			g_nextRagdollID = 1;
		if( result <= 0 )
			result = 1;
		return result;
	}

	static bool NPCPhysics_CreateRagdoll( edict_t *ent )
	{
		if( !ent || ent->free || !NPCPhysics_Initialize() || g_msgRagdollPose <= 0 )
			return false;

		studiohdr_t *header = NULL;
		if( !NPCPhysics_GetStudioHeader( ent, &header ) )
			return false;

		CBaseEntity *baseEntity = CBaseEntity::Instance( ent );
		if( !baseEntity )
			return false;

		CBaseAnimating *animating = static_cast<CBaseAnimating *>( baseEntity );
		mstudiobone_t *studioBones = reinterpret_cast<mstudiobone_t *>(
			reinterpret_cast<byte *>( header ) + header->boneindex
		);
		mstudiobbox_t *hitboxes = NULL;
		if( header->numhitboxes > 0 )
		{
			hitboxes = reinterpret_cast<mstudiobbox_t *>(
				reinterpret_cast<byte *>( header ) + header->hitboxindex
			);
		}

		NPCPhysicsRagdoll ragdoll;
		ragdoll.entity = ent;
		ragdoll.serialNumber = ent->serialnumber;
		ragdoll.networkID = NPCPhysics_AllocateRagdollID();
		ragdoll.boneCount = header->numbones;
		ragdoll.nextPoseTime = 0.0f;
		ragdoll.entityOriginOffset = ent->v.origin;

		std::vector<::Vector> bonePositions( ragdoll.boneCount );
		std::vector<::Vector> boneAngles( ragdoll.boneCount );
		std::vector<float> boneMassWeights( ragdoll.boneCount, 0.0f );

		for( int i = 0; i < ragdoll.boneCount; ++i )
		{
			animating->GetBonePosition( i, bonePositions[i], boneAngles[i] );

			if( studioBones[i].parent < -1 || studioBones[i].parent >= ragdoll.boneCount )
				return false;

			const float radius = NPCPhysics_BoneRadius( hitboxes, header->numhitboxes, i );
			boneMassWeights[i] = radius * radius * radius;
		}

		ragdoll.entityOriginOffset -= bonePositions[0];

		float totalWeight = 0.0f;
		for( int i = 0; i < ragdoll.boneCount; ++i )
			totalWeight += boneMassWeights[i];
		if( totalWeight <= 0.0f )
			return false;

		float totalMass = NPCPhysics_GetCvar( "sv_npc_vphysics_mass", 80.0f );
		if( totalMass <= 0.0f )
			totalMass = 80.0f;
		totalMass = std::max( 10.0f, std::min( totalMass, 300.0f ) );

		ragdoll.groupFilter = new GroupFilterTable( static_cast<uint>( ragdoll.boneCount ) );
		for( int i = 0; i < ragdoll.boneCount; ++i )
		{
			const int parent = studioBones[i].parent;
			if( parent >= 0 && parent != i )
			{
				ragdoll.groupFilter->DisableCollision(
					static_cast<CollisionGroup::SubGroupID>( i ),
					static_cast<CollisionGroup::SubGroupID>( parent )
				);
			}
		}

		ragdoll.bones.resize( ragdoll.boneCount );
		ragdoll.previousPositions.resize( ragdoll.boneCount );

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		for( int i = 0; i < ragdoll.boneCount; ++i )
		{
			const float radius = NPCPhysics_BoneRadius( hitboxes, header->numhitboxes, i );
			const float boneMass = totalMass * boneMassWeights[i] / totalWeight;

			const Vec3 position(
				bonePositions[i][0],
				bonePositions[i][1],
				bonePositions[i][2]
			);
			const Quat rotation = NPCPhysics_AnglesToQuaternion( boneAngles[i] );

			BodyCreationSettings settings(
				new SphereShape( radius ),
				RVec3( position ),
				rotation,
				EMotionType::Dynamic,
				RAGDOLL_OBJECT_LAYER
			);
			settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = std::max( 0.1f, boneMass );
			settings.mCollisionGroup = CollisionGroup(
				ragdoll.groupFilter.Get(),
				static_cast<CollisionGroup::GroupID>( ragdoll.networkID ),
				static_cast<CollisionGroup::SubGroupID>( i )
			);
			settings.mFriction = 0.85f;
			settings.mRestitution = 0.02f;
			settings.mLinearDamping = 0.04f;
			settings.mAngularDamping = 0.12f;
			settings.mMaxLinearVelocity = 1800.0f;
			settings.mMaxAngularVelocity = 35.0f;

			const BodyID bodyID = bodyInterface.CreateAndAddBody( settings, EActivation::Activate );
			if( bodyID.IsInvalid() )
			{
				NPCPhysics_DestroyRagdoll( ragdoll );
				return false;
			}

			ragdoll.bones[i].bodyID = bodyID;
			ragdoll.bones[i].parent = studioBones[i].parent;
			ragdoll.bones[i].radius = radius;
			ragdoll.bones[i].initialPosition = position;

			bodyInterface.SetLinearVelocity(
				bodyID,
				Vec3( ent->v.velocity[0], ent->v.velocity[1], ent->v.velocity[2] )
			);
			bodyInterface.SetAngularVelocity(
				bodyID,
				Vec3(
					ent->v.avelocity[0] * RAGDOLL_DEGREES_TO_RADIANS,
					ent->v.avelocity[1] * RAGDOLL_DEGREES_TO_RADIANS,
					ent->v.avelocity[2] * RAGDOLL_DEGREES_TO_RADIANS
				)
			);
		}

		for( int i = 0; i < ragdoll.boneCount; ++i )
		{
			const int parent = ragdoll.bones[i].parent;
			if( parent < 0 || parent >= ragdoll.boneCount || parent == i )
				continue;

			const BodyID bodyIDs[2] = {
				ragdoll.bones[parent].bodyID,
				ragdoll.bones[i].bodyID
			};

			BodyLockMultiWrite bodyLock(
				g_npcPhysics.system.GetBodyLockInterface(),
				bodyIDs,
				2
			);
			Body *parentBody = bodyLock.GetBody( 0 );
			Body *childBody = bodyLock.GetBody( 1 );
			if( !parentBody || !childBody )
			{
				bodyLock.ReleaseLocks();
				NPCPhysics_DestroyRagdoll( ragdoll );
				return false;
			}

			const ::Vector &jointPosition = bonePositions[i];
			const RVec3 joint(
				jointPosition[0],
				jointPosition[1],
				jointPosition[2]
			);

			PointConstraintSettings constraintSettings;
			constraintSettings.mSpace = EConstraintSpace::WorldSpace;
			constraintSettings.mPoint1 = joint;
			constraintSettings.mPoint2 = joint;

			Ref<TwoBodyConstraint> constraint = constraintSettings.Create( *parentBody, *childBody );
			if( constraint.Get() == nullptr )
			{
				bodyLock.ReleaseLocks();
				NPCPhysics_DestroyRagdoll( ragdoll );
				return false;
			}

			g_npcPhysics.system.AddConstraint( constraint.Get() );
			ragdoll.constraints.push_back( constraint );
			bodyLock.ReleaseLocks();
		}

		// Only mutate the edict after every body and joint exists. Any earlier
		// failure leaves the regular GoldSrc death animation path untouched.
		ent->v.iuser4 = ragdoll.networkID;
		ent->v.solid = SOLID_NOT;
		ent->v.movetype = MOVETYPE_NONE;
		ent->v.takedamage = DAMAGE_NO;
		ent->v.deadflag = DEAD_DEAD;
		ent->v.nextthink = -1.0f;
		ent->v.velocity = g_vecZero;
		ent->v.avelocity = g_vecZero;

		g_ragdolls.push_back( ragdoll );
		ALERT( at_console, "[NPC physics] Created Jolt ragdoll for monster_zombie (%d bones).\n", ragdoll.boneCount );
		return true;
	}

	static void NPCPhysics_ClipRagdollToWorld( NPCPhysicsRagdoll &ragdoll, BodyInterface &bodyInterface )
	{
		if( !g_engfuncs.pfnTraceLine )
			return;

		for( int i = 0; i < ragdoll.boneCount; ++i )
		{
			const RagdollBone &bone = ragdoll.bones[i];
			const Vec3 &oldPosition = ragdoll.previousPositions[i];
			const RVec3 currentPosition = bodyInterface.GetPosition( bone.bodyID );

			const float start[3] = {
				oldPosition.GetX(),
				oldPosition.GetY(),
				oldPosition.GetZ()
			};
			const float end[3] = {
				float( currentPosition.GetX() ),
				float( currentPosition.GetY() ),
				float( currentPosition.GetZ() )
			};

			TraceResult trace;
			g_engfuncs.pfnTraceLine( start, end, 1, ragdoll.entity, &trace );
			if( trace.flFraction >= 1.0f || trace.fAllSolid )
				continue;

			Vec3 normal(
				trace.vecPlaneNormal[0],
				trace.vecPlaneNormal[1],
				trace.vecPlaneNormal[2]
			);
			if( normal.LengthSq() < 0.01f )
				continue;
			normal = normal.Normalized();

			const Vec3 clippedPosition(
				trace.vecEndPos[0] + normal.GetX() * bone.radius,
				trace.vecEndPos[1] + normal.GetY() * bone.radius,
				trace.vecEndPos[2] + normal.GetZ() * bone.radius
			);
			bodyInterface.SetPosition(
				bone.bodyID,
				RVec3( clippedPosition ),
				EActivation::Activate
			);

			Vec3 velocity = bodyInterface.GetLinearVelocity( bone.bodyID );
			const float intoSurface = velocity.Dot( normal );
			if( intoSurface < 0.0f )
			{
				velocity -= normal * intoSurface;
				velocity *= 0.82f;
				bodyInterface.SetLinearVelocity( bone.bodyID, velocity );
			}
		}
	}

	static short NPCPhysics_QuantizeQuaternion( float value )
	{
		value = std::max( -1.0f, std::min( 1.0f, value ) );
		return static_cast<short>( value * 32767.0f );
	}

	static void NPCPhysics_SendRagdollPose( NPCPhysicsRagdoll &ragdoll, BodyInterface &bodyInterface )
	{
		if( g_msgRagdollPose <= 0 || !ragdoll.entity || ragdoll.entity->free )
			return;

		const int entityIndex = ENTINDEX( ragdoll.entity );
		for( int start = 0; start < ragdoll.boneCount; start += RAGDOLL_NETWORK_BONES_PER_CHUNK )
		{
			const int count = std::min(
				RAGDOLL_NETWORK_BONES_PER_CHUNK,
				ragdoll.boneCount - start
			);

			MESSAGE_BEGIN( MSG_PVS, g_msgRagdollPose, ragdoll.entity->v.origin );
				WRITE_SHORT( entityIndex );
				WRITE_SHORT( ragdoll.networkID );
				WRITE_BYTE( ragdoll.boneCount );
				WRITE_BYTE( start );
				WRITE_BYTE( count );

				for( int i = 0; i < count; ++i )
				{
					const RagdollBone &bone = ragdoll.bones[start + i];
					const RVec3 position = bodyInterface.GetPosition( bone.bodyID );
					Quat rotation = bodyInterface.GetRotation( bone.bodyID );
					rotation = rotation.Normalized();

					WRITE_COORD( float( position.GetX() ) );
					WRITE_COORD( float( position.GetY() ) );
					WRITE_COORD( float( position.GetZ() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetX() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetY() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetZ() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetW() ) );
				}
			MESSAGE_END();
		}
	}

	static int NPCPhysics_PhysicsEntity( edict_t * )
	{
		// Intentionally do not intercept or replace any living NPC or prop
		// physics. Ragdolls are created explicitly from the death callback.
		return 0;
	}

	static void NPCPhysics_EndFrame( void )
	{
		if( !g_npcPhysics.initialized || g_ragdolls.empty() )
			return;

		for( size_t i = g_ragdolls.size(); i > 0; --i )
		{
			NPCPhysicsRagdoll &ragdoll = g_ragdolls[i - 1];
			if( !ragdoll.entity || ragdoll.entity->free ||
				ragdoll.entity->serialnumber != ragdoll.serialNumber )
			{
				NPCPhysics_DestroyRagdoll( ragdoll );
				g_ragdolls.erase( g_ragdolls.begin() + ( i - 1 ) );
			}
		}

		if( g_ragdolls.empty() )
			return;

		float deltaTime = gpGlobals ? gpGlobals->frametime : ( 1.0f / 60.0f );
		if( deltaTime <= 0.0f )
			return;
		deltaTime = std::min( deltaTime, RAGDOLL_MAX_FRAME_TIME );

		// GoldSrc's sv_gravity is already expressed in world units/sec^2
		// (normally 800), not as a normalized multiplier.
		float gravity = NPCPhysics_GetCvar( "sv_gravity", 800.0f );
		if( gravity < 0.0f )
			gravity = 800.0f;
		g_npcPhysics.system.SetGravity( Vec3( 0.0f, 0.0f, -gravity ) );

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		for( size_t r = 0; r < g_ragdolls.size(); ++r )
		{
			NPCPhysicsRagdoll &ragdoll = g_ragdolls[r];
			for( int i = 0; i < ragdoll.boneCount; ++i )
			{
				const RVec3 position = bodyInterface.GetPosition( ragdoll.bones[i].bodyID );
				ragdoll.previousPositions[i] = Vec3(
					float( position.GetX() ),
					float( position.GetY() ),
					float( position.GetZ() )
				);
			}
		}

		const uint collisionSteps = std::max( 1, std::min( 4, int( ceilf( deltaTime * 60.0f ) ) ) );
		g_npcPhysics.system.Update(
			deltaTime,
			collisionSteps,
			g_npcPhysics.tempAllocator,
			&g_npcPhysics.jobs
		);

		for( size_t r = 0; r < g_ragdolls.size(); ++r )
		{
			NPCPhysicsRagdoll &ragdoll = g_ragdolls[r];
			NPCPhysics_ClipRagdollToWorld( ragdoll, bodyInterface );

			const RVec3 rootPosition = bodyInterface.GetPosition( ragdoll.bones[0].bodyID );
			const float entityOrigin[3] = {
				float( rootPosition.GetX() ) + ragdoll.entityOriginOffset[0],
				float( rootPosition.GetY() ) + ragdoll.entityOriginOffset[1],
				float( rootPosition.GetZ() ) + ragdoll.entityOriginOffset[2]
			};
			if( g_engfuncs.pfnSetOrigin )
				g_engfuncs.pfnSetOrigin( ragdoll.entity, entityOrigin );

			if( gpGlobals && gpGlobals->time >= ragdoll.nextPoseTime )
			{
				NPCPhysics_SendRagdollPose( ragdoll, bodyInterface );
				ragdoll.nextPoseTime = gpGlobals->time + RAGDOLL_POSE_INTERVAL;
			}
		}
	}
}

int NPCPhysics_TryCreateRagdoll( edict_t *ent )
{
	if( !NPCPhysics_Enabled() || !ent || ent->free )
		return FALSE;

	if( ( ent->v.flags & FL_MONSTER ) == 0 ||
		( ent->v.flags & ( FL_CLIENT | FL_KILLME | FL_DORMANT ) ) != 0 )
		return FALSE;

	// First pass is deliberately limited to the standard zombie. Other
	// monster skeletons can be enabled after their bone layouts are verified.
	if( !FStrEq( STRING( ent->v.classname ), "monster_zombie" ) )
		return FALSE;

	if( !NPCPhysics_CreateRagdoll( ent ) )
	{
		ALERT( at_console, "[NPC physics] Jolt ragdoll creation failed; using GoldSrc death animation.\n" );
		return FALSE;
	}

	return TRUE;
}

int NPCPhysics_GetInterface( int version, void *callbacks )
{
	if( version != XASH_SERVER_PHYSICS_INTERFACE_VERSION || !callbacks )
		return FALSE;

	CVAR_REGISTER( &g_svNpcVPhysics );
	CVAR_REGISTER( &g_svNpcVPhysicsMass );

	if( !g_msgRagdollPose && g_engfuncs.pfnRegUserMsg )
		g_msgRagdollPose = g_engfuncs.pfnRegUserMsg( "RagdollPose", -1 );

	xash_physics_interface_s *physics = reinterpret_cast<xash_physics_interface_s *>( callbacks );
	memset( physics, 0, sizeof( *physics ) );
	physics->version = version;
	physics->SV_PhysicsEntity = NPCPhysics_PhysicsEntity;
	physics->SV_EndFrame = NPCPhysics_EndFrame;
	return TRUE;
}

void NPCPhysics_ResetMap( void )
{
	NPCPhysics_DestroyAllRagdolls();
}

void NPCPhysics_Shutdown( void )
{
	NPCPhysics_DestroyAllRagdolls();

	if( g_npcPhysics.initialized )
	{
		UnregisterTypes();
		delete Factory::sInstance;
		Factory::sInstance = NULL;
		delete g_npcPhysics.tempAllocator;
		g_npcPhysics.tempAllocator = NULL;
		g_npcPhysics.initialized = false;
	}

	g_msgRagdollPose = 0;
}
