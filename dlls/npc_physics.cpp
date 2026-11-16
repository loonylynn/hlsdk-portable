/*
 * File-authored NPC ragdolls using Jolt as the rigid-body backend.
 * Each model supplies a sibling .jrag definition; this file contains only
 * the generic loader, validation, Jolt construction and runtime simulation.
 * GoldSrc owns living NPCs; Jolt owns a corpse only after successful handoff.
 */

#include "extdll.h"
#include "util.h"
#include "eiface.h"
#include "enginecallback.h"
#include "xash_physint.h"
#include "studio.h"
#include "cbase.h"
#include "monsters.h"


#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/SkeletonMapper.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstdint>
#include <utility>
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
	static constexpr float RAGDOLL_FIXED_STEP = 1.0f / 120.0f;
	static constexpr int RAGDOLL_MAX_STEPS_PER_FRAME = 8;
	static constexpr float RAGDOLL_ACTIVE_POSE_INTERVAL = 0.05f;
	static constexpr float RAGDOLL_SLEEPING_POSE_INTERVAL = 0.20f;
	static constexpr int RAGDOLL_NETWORK_BODIES_PER_CHUNK = 10;
	static constexpr float RAGDOLL_DEGREES_TO_RADIANS = 0.01745329251994329577f;
	static constexpr float RAGDOLL_VELOCITY_LIMIT = 2000.0f;
	static constexpr float RAGDOLL_ANGULAR_VELOCITY_LIMIT = 35.0f;
	static constexpr int RAGDOLL_DEFINITION_VERSION = 1;
	static constexpr int RAGDOLL_DEFINITION_MAX_BODIES = 64;
	static constexpr int RAGDOLL_DEFINITION_MAX_HULL_POINTS = 32;
	static constexpr int RAGDOLL_DEFINITION_MAX_FILE_BYTES = 1024 * 1024;

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

	struct NPCPhysicsLivePose
	{
		int serialNumber;
		int boneCount;
		float sampleTime;
		::Vector entityOrigin;
		::Vector entityAngles;
		std::vector<Vec3> positions;
		std::vector<Quat> rotations;

		NPCPhysicsLivePose()
			: serialNumber( -1 ), boneCount( 0 ), sampleTime( 0.0f )
		{
		}
	};

	struct RagdollBone
	{
		int studioBoneIndex;
		int parentElementIndex;
		Vec3 localShapeCenter;
	};

	struct NPCPhysicsRagdollPoint
	{
		float x, y, z;
	};

	struct NPCPhysicsRagdollBodyDefinition
	{
		std::string boneName;
		int parentBody;
		NPCPhysicsRagdollPoint localShapeCenter;
		std::vector<NPCPhysicsRagdollPoint> points;
		float jointMinDegrees[3];
		float jointMaxDegrees[3];
		float angularDampingOverride;
	};

	struct NPCPhysicsRagdollDefinition
	{
		float totalMass;
		float inertiaMultiplier;
		float linearDamping;
		float angularDamping;
		float friction;
		float restitution;
		std::vector<NPCPhysicsRagdollBodyDefinition> bodies;
	};

	struct NPCPhysicsCachedRagdollDefinition
	{
		std::string path;
		NPCPhysicsRagdollDefinition definition;
		std::string error;
		bool loaded;
		bool errorReported;

		NPCPhysicsCachedRagdollDefinition()
			: loaded( false ), errorReported( false )
		{
		}
	};

	struct NPCPhysicsBrushModelShape
	{
		int modelIndex;
		Ref<Shape> shape;
	};

	struct NPCPhysicsBrushCollider
	{
		int entityIndex;
		int modelIndex;
		BodyID bodyID;
		Vec3 position;
		Quat rotation;

		NPCPhysicsBrushCollider()
			: entityIndex( -1 ),
			  modelIndex( 0 ),
			  bodyID(),
			  position( 0.0f, 0.0f, 0.0f ),
			  rotation( Quat::sIdentity() )
		{
		}
	};

	struct NPCPhysicsRagdoll
	{
		edict_t *entity;
		int serialNumber;
		int networkID;
		int studioBoneCount;
		int bodyCount;
		int rootElement;
		float nextPoseTime;
		::Vector entityOriginOffset;
		std::vector<RagdollBone> bones;
		std::vector<float> bodyMasses;
		BodyID blastDebugBodyID;
		int blastDebugBodyIndex;
		Vec3 blastDebugBodyStartPosition;
		Vec3 blastDebugRootStartPosition;
		float blastDebugTime;
		bool blastDebugPending;
		Ref<Ragdoll> physicsRagdoll;
		Ref<Skeleton> physicsSkeleton;
		Ref<Skeleton> studioSkeleton;
		Ref<SkeletonMapper> skeletonMapper;
		std::vector<Mat44> studioDeathLocalPose;
		std::vector<Mat44> physicsPoseModelSpace;
		std::vector<Mat44> studioPoseModelSpace;
		bool addedToPhysicsSystem;

		NPCPhysicsRagdoll()
			: entity( NULL ),
			  serialNumber( 0 ),
			  networkID( 0 ),
			  studioBoneCount( 0 ),
			  bodyCount( 0 ),
			  rootElement( -1 ),
			  nextPoseTime( 0.0f ),
			  blastDebugBodyID(),
			  blastDebugBodyIndex( -1 ),
			  blastDebugBodyStartPosition( 0.0f, 0.0f, 0.0f ),
			  blastDebugRootStartPosition( 0.0f, 0.0f, 0.0f ),
			  blastDebugTime( 0.0f ),
			  blastDebugPending( false ),
			  addedToPhysicsSystem( false )
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
		float fixedAccumulator;
		bool initialized;

		NPCPhysicsWorld()
			: jobs(),
			  tempAllocator( NULL ),
			  fixedAccumulator( 0.0f ),
			  initialized( false )
		{
		}
	};

	static NPCPhysicsWorld g_npcPhysics;
	static std::vector<NPCPhysicsRagdoll> g_ragdolls;
	static std::vector<NPCPhysicsCachedRagdollDefinition> g_ragdollDefinitionCache;
	static std::vector<NPCPhysicsLivePose> g_livePoses;
	static int g_nextRagdollID = 1;
	static int g_msgRagdollPose = 0;
	static int g_msgRagdollImpact = 0;
	static xash_server_physics_api_s *g_serverPhysicsAPI = NULL;
	static xash_world_collision_mesh_api_s *g_worldCollisionMeshAPI = NULL;
	static xash_brush_collision_mesh_api_s *g_brushCollisionMeshAPI = NULL;
	static std::vector<NPCPhysicsBrushModelShape> g_brushModelShapes;
	static std::vector<NPCPhysicsBrushCollider> g_brushColliders;
	static bool g_brushCollisionUnavailableLogged = false;
	static bool g_brushCollisionStatusLogged = false;
	static BodyID g_worldCollisionBodyID;
	static Ref<Shape> g_worldCollisionShape;
	static bool g_worldCollisionReady = false;

	static cvar_t g_svNpcVPhysics = { "sv_npc_vphysics", "1", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsMass = { "sv_npc_vphysics_mass", "0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsBulletScale = { "sv_npc_vphysics_bullet_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsCrowbarScale = { "sv_npc_vphysics_crowbar_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsBlastScale = { "sv_npc_vphysics_blast_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsDebug = { "sv_npc_vphysics_debug", "0", FCVAR_SERVER };

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

	static bool NPCPhysics_RagdollSetupFailed( const char *reason )
	{
		ALERT( at_console, "[NPC physics] Ragdoll setup failed: %s.\n", reason );
		return false;
	}

	static bool NPCPhysics_IsEligibleMonster( edict_t *ent )
	{
		return ent && !ent->free &&
			( ent->v.flags & FL_MONSTER ) != 0 &&
			( ent->v.flags & ( FL_CLIENT | FL_KILLME | FL_DORMANT ) ) == 0;
	}

	static const NPCPhysicsRagdollDefinition *NPCPhysics_GetRagdollDefinition(
		edict_t *ent, bool reportErrors );

	static bool NPCPhysics_Initialize()
	{
		if( g_npcPhysics.initialized )
			return true;

		// Jolt's job system allocates during Init(). Register the allocator first.
		RegisterDefaultAllocator();
		g_npcPhysics.jobs.Init( 2048 );

		Factory::sInstance = new Factory();
		RegisterTypes();

		g_npcPhysics.tempAllocator = new TempAllocatorImpl( 16 * 1024 * 1024 );
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
		g_npcPhysics.fixedAccumulator = 0.0f;
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

		// GoldSrc AngleQuaternion order: pitch, yaw, roll.
		Quat result(
			sr * cp * cy - cr * sp * sy,
			cr * sp * cy + sr * cp * sy,
			cr * cp * sy - sr * sp * cy,
			cr * cp * cy + sr * sp * sy
		);
		return result.Normalized();
	}

	static bool NPCPhysics_GetStudioHeader( edict_t *ent, studiohdr_t **headerOut, bool reportErrors = false )
	{
		if( !ent || !headerOut || !g_engfuncs.pfnGetModelPtr )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "studio model lookup is unavailable" ) : false;

		studiohdr_t *header = reinterpret_cast<studiohdr_t *>( GET_MODEL_PTR( ent ) );
		if( !header )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "NPC has no loaded studio model" ) : false;

		if( header->ident != IDSTUDIOHEADER || header->version != STUDIO_VERSION )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "NPC studio model has an unsupported header or version" ) : false;

		if( header->length < (int)sizeof( studiohdr_t ) ||
			header->numbones < 1 ||
			header->numbones > MAXSTUDIOBONES ||
			header->boneindex < (int)sizeof( studiohdr_t ) )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "NPC studio bone table is invalid" ) : false;

		const size_t length = static_cast<size_t>( header->length );
		const size_t boneOffset = static_cast<size_t>( header->boneindex );
		const size_t boneBytes = static_cast<size_t>( header->numbones ) * sizeof( mstudiobone_t );
		if( boneOffset > length || boneBytes > length - boneOffset )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "NPC studio bone table exceeds model bounds" ) : false;

		*headerOut = header;
		return true;
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

	static void NPCPhysics_DestroyRagdoll( NPCPhysicsRagdoll &ragdoll )
	{
		if( ragdoll.physicsRagdoll.GetPtr() )
		{
			if( ragdoll.addedToPhysicsSystem && g_npcPhysics.initialized )
				ragdoll.physicsRagdoll->RemoveFromPhysicsSystem();
			ragdoll.addedToPhysicsSystem = false;
			ragdoll.physicsRagdoll = nullptr;
		}
		ragdoll.skeletonMapper = nullptr;
		ragdoll.physicsSkeleton = nullptr;
		ragdoll.studioSkeleton = nullptr;
		ragdoll.studioDeathLocalPose.clear();
		ragdoll.physicsPoseModelSpace.clear();
		ragdoll.studioPoseModelSpace.clear();
		ragdoll.bones.clear();
		ragdoll.bodyCount = 0;
	}

	static void NPCPhysics_DestroyAllRagdolls()
	{
		for( size_t i = 0; i < g_ragdolls.size(); ++i )
			NPCPhysics_DestroyRagdoll( g_ragdolls[i] );
		g_ragdolls.clear();
	}

	static void NPCPhysics_CaptureLivePose( edict_t *ent )
	{
		if( !g_serverPhysicsAPI || !NPCPhysics_Enabled() || !NPCPhysics_IsEligibleMonster( ent ) ||
			ent->v.deadflag != DEAD_NO || !gpGlobals ||
			!g_engfuncs.pfnGetBonePosition )
			return;

		// Only retain animation history for NPC models that actually opt in
		// through a sibling .jrag definition. Missing definitions are cached.
		if( !NPCPhysics_GetRagdollDefinition( ent, false ) )
			return;

		const int entityIndex = ENTINDEX( ent );
		if( entityIndex < 0 || entityIndex >= (int)g_livePoses.size() )
			return;

		studiohdr_t *header = NULL;
		if( !NPCPhysics_GetStudioHeader( ent, &header ) )
			return;

		NPCPhysicsLivePose &pose = g_livePoses[entityIndex];
		if( pose.serialNumber != ent->serialnumber || pose.boneCount != header->numbones )
		{
			pose.serialNumber = ent->serialnumber;
			pose.boneCount = header->numbones;
			pose.positions.resize( pose.boneCount );
			pose.rotations.resize( pose.boneCount );
			pose.sampleTime = gpGlobals->time;
		}
		else if( gpGlobals->time <= pose.sampleTime + 0.0001f )
		{
			// Do not overwrite the previous frame's useful sample multiple times
			// during one server tick.
			return;
		}

		for( int i = 0; i < pose.boneCount; ++i )
		{
			::Vector position;
			::Vector angles;
			g_engfuncs.pfnGetBonePosition( ent, i, &position[0], &angles[0] );
			pose.positions[i] = Vec3( position[0], position[1], position[2] );
			pose.rotations[i] = NPCPhysics_AnglesToQuaternion( angles );
		}

		pose.entityOrigin = ent->v.origin;
		pose.entityAngles = ent->v.angles;
		pose.sampleTime = gpGlobals->time;
	}

	static void NPCPhysics_DestroyWorldCollider()
	{
		if( g_npcPhysics.initialized && !g_worldCollisionBodyID.IsInvalid() )
		{
			BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
			if( bodyInterface.IsAdded( g_worldCollisionBodyID ) )
				bodyInterface.RemoveBody( g_worldCollisionBodyID );
			bodyInterface.DestroyBody( g_worldCollisionBodyID );
		}

		g_worldCollisionBodyID = BodyID();
		g_worldCollisionShape = Ref<Shape>();
		g_worldCollisionReady = false;
	}

	static bool NPCPhysics_EnsureWorldCollider()
	{
		if( g_worldCollisionReady &&
			g_npcPhysics.initialized &&
			!g_worldCollisionBodyID.IsInvalid() &&
			g_npcPhysics.system.GetBodyInterface().IsAdded( g_worldCollisionBodyID ) )
			return true;

		if( !g_npcPhysics.initialized ||
			!g_worldCollisionMeshAPI ||
			g_worldCollisionMeshAPI->version != XASH_WORLD_COLLISION_MESH_API_VERSION ||
			!g_worldCollisionMeshAPI->GetMeshInfo ||
			!g_worldCollisionMeshAPI->GetMeshData )
		{
			ALERT( at_console, "[NPC physics] Native Xash BSP collision mesh API is unavailable.\n" );
			return false;
		}

		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		if( !g_worldCollisionMeshAPI->GetMeshInfo( &vertexCount, &indexCount ) ||
			vertexCount < 3 || indexCount < 3 || ( indexCount % 3 ) != 0 )
		{
			ALERT( at_console, "[NPC physics] Xash did not provide a valid solid BSP mesh.\n" );
			return false;
		}

		// Bound temporary allocations on unusually large or malformed BSPs.
		// Normal Half-Life maps are well below these limits.
		if( vertexCount > 2000000u || indexCount > 12000000u )
		{
			ALERT(
				at_console,
				"[NPC physics] BSP collision mesh is too large (%u vertices, %u indices).\n",
				vertexCount,
				indexCount
			);
			return false;
		}

		std::vector<float> sourceVertices( static_cast<size_t>( vertexCount ) * 3 );
		std::vector<uint32_t> sourceIndices( indexCount );
		if( !g_worldCollisionMeshAPI->GetMeshData(
			sourceVertices.data(),
			vertexCount,
			sourceIndices.data(),
			indexCount ) )
		{
			ALERT( at_console, "[NPC physics] Failed to read native Xash BSP collision mesh.\n" );
			return false;
		}

		VertexList vertices;
		IndexedTriangleList triangles;
		vertices.reserve( vertexCount );
		triangles.reserve( indexCount / 3 );

		for( uint32_t i = 0; i < vertexCount; ++i )
		{
			const float x = sourceVertices[i * 3 + 0];
			const float y = sourceVertices[i * 3 + 1];
			const float z = sourceVertices[i * 3 + 2];
			if( !std::isfinite( x ) || !std::isfinite( y ) || !std::isfinite( z ) )
			{
				ALERT( at_console, "[NPC physics] BSP collision mesh contains a non-finite vertex.\n" );
				return false;
			}
			vertices.emplace_back( x, y, z );
		}

		for( uint32_t i = 0; i < indexCount; i += 3 )
		{
			const uint32_t i0 = sourceIndices[i + 0];
			const uint32_t i1 = sourceIndices[i + 1];
			const uint32_t i2 = sourceIndices[i + 2];
			if( i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount )
			{
				ALERT( at_console, "[NPC physics] BSP collision mesh contains an invalid triangle index.\n" );
				return false;
			}
			triangles.emplace_back( i0, i1, i2, 0 );
		}

		// The mesh is static and concave, so Jolt can use its BVH directly.
		// Shared BSP vertex indices preserve adjacency for internal-edge
		// handling instead of treating every triangle as a disconnected face.
		MeshShapeSettings meshSettings( std::move( vertices ), std::move( triangles ) );
		meshSettings.mMaxTrianglesPerLeaf = 8;
		meshSettings.mBuildQuality = MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;

		Shape::ShapeResult shapeResult = meshSettings.Create();
		if( !shapeResult.IsValid() )
		{
			ALERT(
				at_console,
				"[NPC physics] Jolt could not build the native BSP mesh: %s\n",
				"mesh shape creation failed"
			);
			return false;
		}

		Ref<Shape> worldShape = shapeResult.Get();
		BodyCreationSettings bodySettings(
			worldShape.GetPtr(),
			RVec3( 0.0f, 0.0f, 0.0f ),
			Quat::sIdentity(),
			EMotionType::Static,
			RAGDOLL_OBJECT_LAYER
		);
		bodySettings.mFriction = 0.9f;
		bodySettings.mRestitution = 0.0f;

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		const BodyID worldBody = bodyInterface.CreateAndAddBody(
			bodySettings,
			EActivation::DontActivate
		);
		if( worldBody.IsInvalid() )
		{
			ALERT( at_console, "[NPC physics] Jolt could not create the native BSP body.\n" );
			return false;
		}

		g_worldCollisionShape = worldShape;
		g_worldCollisionBodyID = worldBody;
		g_worldCollisionReady = true;
		ALERT(
			at_console,
			"[NPC physics] Native BSP collision loaded: %u vertices, %u outward-wound triangles.\n",
			vertexCount,
			indexCount / 3
		);
		return true;
	}

	static void NPCPhysics_DestroyBrushCollider( NPCPhysicsBrushCollider &collider )
	{
		if( !g_npcPhysics.initialized || collider.bodyID.IsInvalid() )
			return;

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		if( bodyInterface.IsAdded( collider.bodyID ) )
			bodyInterface.RemoveBody( collider.bodyID );
		bodyInterface.DestroyBody( collider.bodyID );
		collider.bodyID = BodyID();
	}

	static void NPCPhysics_DestroyBrushColliders()
	{
		for( size_t i = 0; i < g_brushColliders.size(); ++i )
			NPCPhysics_DestroyBrushCollider( g_brushColliders[i] );
		g_brushColliders.clear();
		g_brushModelShapes.clear();
	}

	static int NPCPhysics_FindBrushCollider( int entityIndex )
	{
		for( size_t i = 0; i < g_brushColliders.size(); ++i )
			if( g_brushColliders[i].entityIndex == entityIndex )
				return static_cast<int>( i );
		return -1;
	}

	static Ref<Shape> NPCPhysics_GetBrushModelShape( int modelIndex )
	{
		for( size_t i = 0; i < g_brushModelShapes.size(); ++i )
			if( g_brushModelShapes[i].modelIndex == modelIndex )
				return g_brushModelShapes[i].shape;

		if( !g_brushCollisionMeshAPI || !g_brushCollisionMeshAPI->GetMeshInfo ||
			!g_brushCollisionMeshAPI->GetMeshData )
			return Ref<Shape>();

		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		if( !g_brushCollisionMeshAPI->GetMeshInfo( modelIndex, &vertexCount, &indexCount ) ||
			vertexCount < 3 || indexCount < 3 || ( indexCount % 3 ) != 0 ||
			vertexCount > 500000u || indexCount > 3000000u )
			return Ref<Shape>();

		std::vector<float> sourceVertices( static_cast<size_t>( vertexCount ) * 3 );
		std::vector<uint32_t> sourceIndices( indexCount );
		if( !g_brushCollisionMeshAPI->GetMeshData(
			modelIndex,
			sourceVertices.data(),
			vertexCount,
			sourceIndices.data(),
			indexCount ) )
			return Ref<Shape>();

		VertexList vertices;
		IndexedTriangleList triangles;
		vertices.reserve( vertexCount );
		triangles.reserve( indexCount / 3 );

		for( uint32_t i = 0; i < vertexCount; ++i )
		{
			const float x = sourceVertices[i * 3 + 0];
			const float y = sourceVertices[i * 3 + 1];
			const float z = sourceVertices[i * 3 + 2];
			if( !std::isfinite( x ) || !std::isfinite( y ) || !std::isfinite( z ) )
				return Ref<Shape>();
			vertices.emplace_back( x, y, z );
		}

		for( uint32_t i = 0; i < indexCount; i += 3 )
		{
			const uint32_t i0 = sourceIndices[i + 0];
			const uint32_t i1 = sourceIndices[i + 1];
			const uint32_t i2 = sourceIndices[i + 2];
			if( i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount )
				return Ref<Shape>();
			triangles.emplace_back( i0, i1, i2, 0 );
		}

		MeshShapeSettings meshSettings( std::move( vertices ), std::move( triangles ) );
		meshSettings.mMaxTrianglesPerLeaf = 8;
		meshSettings.mBuildQuality = MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;

		Shape::ShapeResult shapeResult = meshSettings.Create();
		if( !shapeResult.IsValid() )
		{
			ALERT(
				at_console,
				"[NPC physics] Jolt could not build inline brush model %d collision mesh.\n",
				modelIndex
			);
			return Ref<Shape>();
		}

		NPCPhysicsBrushModelShape cachedShape;
		cachedShape.modelIndex = modelIndex;
		cachedShape.shape = shapeResult.Get();
		g_brushModelShapes.push_back( cachedShape );

		ALERT(
			at_console,
			"[NPC physics] Cached inline brush model %d: %u vertices, %u triangles.\n",
			modelIndex,
			vertexCount,
			indexCount / 3
		);
		return cachedShape.shape;
	}

	static bool NPCPhysics_QuaternionsDiffer( const Quat &a, const Quat &b )
	{
		const float epsilon = 0.0001f;
		return fabsf( a.GetX() - b.GetX() ) > epsilon ||
			fabsf( a.GetY() - b.GetY() ) > epsilon ||
			fabsf( a.GetZ() - b.GetZ() ) > epsilon ||
			fabsf( a.GetW() - b.GetW() ) > epsilon;
	}

	static void NPCPhysics_UpdateBrushColliders()
	{
		if( !g_npcPhysics.initialized || !g_brushCollisionMeshAPI ||
			g_brushCollisionMeshAPI->version != XASH_BRUSH_COLLISION_MESH_API_VERSION ||
			!g_brushCollisionMeshAPI->GetEntityCount ||
			!g_brushCollisionMeshAPI->GetEntityInfo )
		{
			if( !g_brushCollisionUnavailableLogged )
			{
				ALERT(
					at_console,
					"[NPC physics] Inline BSP entity collision API is unavailable; func_wall and other brush entities will not collide.\n"
				);
				g_brushCollisionUnavailableLogged = true;
			}
			return;
		}

		const int entityCount = g_brushCollisionMeshAPI->GetEntityCount();
		if( entityCount < 0 || entityCount > 8192 )
			return;
		if( !g_brushCollisionStatusLogged )
		{
			ALERT( at_console, "[NPC physics] Found %d inline SOLID_BSP entity colliders.\n", entityCount );
			g_brushCollisionStatusLogged = true;
		}

		std::vector<int> presentEntities;
		presentEntities.reserve( entityCount );
		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();

		for( int ordinal = 0; ordinal < entityCount; ++ordinal )
		{
			xash_brush_collision_entity_s entityInfo;
			if( !g_brushCollisionMeshAPI->GetEntityInfo( ordinal, &entityInfo ) ||
				entityInfo.entityIndex <= 0 || entityInfo.modelIndex <= 1 )
				continue;

			const Vec3 position(
				entityInfo.origin[0], entityInfo.origin[1], entityInfo.origin[2] );
			const Quat rotation = NPCPhysics_AnglesToQuaternion( ::Vector(
				entityInfo.angles[0], entityInfo.angles[1], entityInfo.angles[2] ) );
			if( !std::isfinite( position.GetX() ) ||
				!std::isfinite( position.GetY() ) ||
				!std::isfinite( position.GetZ() ) )
				continue;

			presentEntities.push_back( entityInfo.entityIndex );
			Ref<Shape> shape = NPCPhysics_GetBrushModelShape( entityInfo.modelIndex );
			if( !shape.GetPtr() )
				continue;
			int colliderIndex = NPCPhysics_FindBrushCollider( entityInfo.entityIndex );
			if( colliderIndex >= 0 &&
				g_brushColliders[colliderIndex].modelIndex != entityInfo.modelIndex )
			{
				NPCPhysics_DestroyBrushCollider( g_brushColliders[colliderIndex] );
				g_brushColliders.erase( g_brushColliders.begin() + colliderIndex );
				colliderIndex = -1;
			}

			if( colliderIndex < 0 )
			{
				BodyCreationSettings bodySettings(
					shape.GetPtr(),
					RVec3( position.GetX(), position.GetY(), position.GetZ() ),
					rotation,
					EMotionType::Static,
					RAGDOLL_OBJECT_LAYER
				);
				bodySettings.mFriction = 0.9f;
				bodySettings.mRestitution = 0.0f;

				const BodyID bodyID = bodyInterface.CreateAndAddBody(
					bodySettings,
					EActivation::Activate
				);
				if( bodyID.IsInvalid() )
				{
					ALERT(
						at_console,
						"[NPC physics] Could not create inline brush collider for entity %d.\n",
						entityInfo.entityIndex
					);
					continue;
				}

				NPCPhysicsBrushCollider collider;
				collider.entityIndex = entityInfo.entityIndex;
				collider.modelIndex = entityInfo.modelIndex;
				collider.bodyID = bodyID;
				collider.position = position;
				collider.rotation = rotation;
				g_brushColliders.push_back( collider );
				ALERT(
					at_console,
					"[NPC physics] Added inline BSP collider: entity %d, model %d.\n",
					entityInfo.entityIndex,
					entityInfo.modelIndex
				);
				continue;
			}

			NPCPhysicsBrushCollider &collider = g_brushColliders[colliderIndex];
			const Vec3 positionDelta = position - collider.position;
			if( positionDelta.LengthSq() > 0.0001f ||
				NPCPhysics_QuaternionsDiffer( rotation, collider.rotation ) )
			{
				bodyInterface.SetPositionAndRotation(
					collider.bodyID,
					RVec3( position.GetX(), position.GetY(), position.GetZ() ),
					rotation,
					EActivation::DontActivate
				);
				collider.position = position;
				collider.rotation = rotation;
			}
		}

		for( size_t i = g_brushColliders.size(); i > 0; --i )
		{
			const int entityIndex = g_brushColliders[i - 1].entityIndex;
			if( std::find( presentEntities.begin(), presentEntities.end(), entityIndex ) == presentEntities.end() )
			{
				NPCPhysics_DestroyBrushCollider( g_brushColliders[i - 1] );
				g_brushColliders.erase( g_brushColliders.begin() + ( i - 1 ) );
			}
		}
	}

	static bool NPCPhysics_NameEquals( const char *a, const char *b )
	{
		if( !a || !b ) return false;
		while( *a && *b )
		{
			char ca = *a++, cb = *b++;
			if( ca >= 'A' && ca <= 'Z' ) ca = char( ca - 'A' + 'a' );
			if( cb >= 'A' && cb <= 'Z' ) cb = char( cb - 'A' + 'a' );
			if( ca != cb ) return false;
		}
		return *a == *b;
	}

	static bool NPCPhysics_NextJragLine(
		std::istringstream &input,
		std::istringstream &line,
		std::string &storage )
	{
		while( std::getline( input, storage ) )
		{
			const size_t comment = storage.find( '#' );
			if( comment != std::string::npos )
				storage.erase( comment );
			line.clear();
			line.str( storage );
			line >> std::ws;
			if( !line.eof() )
				return true;
		}
		return false;
	}

	static bool NPCPhysics_JragLineFinished( std::istringstream &line )
	{
		line >> std::ws;
		return line.eof();
	}

	static bool NPCPhysics_ParseJragDefinition(
		const char *contents,
		int length,
		NPCPhysicsRagdollDefinition &definition,
		std::string &error )
	{
		if( !contents || length <= 0 || length > RAGDOLL_DEFINITION_MAX_FILE_BYTES )
		{
			error = "file is empty or exceeds the 1 MiB limit";
			return false;
		}

		std::istringstream input( std::string( contents, static_cast<size_t>( length ) ) );
		std::istringstream line;
		std::string storage;
		std::string command;
		int version = 0;

		if( !NPCPhysics_NextJragLine( input, line, storage ) ||
			!( line >> command >> version ) ||
			command != "JOLT_RAGDOLL" ||
			version != RAGDOLL_DEFINITION_VERSION ||
			!NPCPhysics_JragLineFinished( line ) )
		{
			error = "expected JOLT_RAGDOLL 1 header";
			return false;
		}

		auto readFloat = [&]( const char *expected, float &value ) -> bool
		{
			if( !NPCPhysics_NextJragLine( input, line, storage ) ||
				!( line >> command >> value ) ||
				command != expected ||
				!NPCPhysics_JragLineFinished( line ) ||
				!std::isfinite( value ) )
				return false;
			return true;
		};

		auto readVector = [&]( const char *expected, float value[3] ) -> bool
		{
			if( !NPCPhysics_NextJragLine( input, line, storage ) ||
				!( line >> command >> value[0] >> value[1] >> value[2] ) ||
				command != expected ||
				!NPCPhysics_JragLineFinished( line ) ||
				!std::isfinite( value[0] ) ||
				!std::isfinite( value[1] ) ||
				!std::isfinite( value[2] ) )
				return false;
			return true;
		};

		if( !readFloat( "mass", definition.totalMass ) ||
			!readFloat( "inertia_multiplier", definition.inertiaMultiplier ) ||
			!readFloat( "linear_damping", definition.linearDamping ) ||
			!readFloat( "angular_damping", definition.angularDamping ) ||
			!readFloat( "friction", definition.friction ) ||
			!readFloat( "restitution", definition.restitution ) )
		{
			error = "missing or invalid global physics property";
			return false;
		}

		if( definition.totalMass <= 0.0f ||
			definition.inertiaMultiplier <= 0.0f ||
			definition.linearDamping < 0.0f ||
			definition.angularDamping < 0.0f ||
			definition.friction < 0.0f ||
			definition.restitution < 0.0f ||
			definition.restitution > 1.0f )
		{
			error = "global mass, damping, friction or restitution is outside its valid range";
			return false;
		}

		int bodyCount = 0;
		if( !NPCPhysics_NextJragLine( input, line, storage ) ||
			!( line >> command >> bodyCount ) ||
			command != "body_count" ||
			!NPCPhysics_JragLineFinished( line ) ||
			bodyCount < 1 ||
			bodyCount > RAGDOLL_DEFINITION_MAX_BODIES )
		{
			error = "body_count must be between 1 and 64";
			return false;
		}

		definition.bodies.clear();
		definition.bodies.reserve( bodyCount );
		int rootCount = 0;

		for( int bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex )
		{
			NPCPhysicsRagdollBodyDefinition body;
			if( !NPCPhysics_NextJragLine( input, line, storage ) ||
				!( line >> command >> std::quoted( body.boneName ) >> body.parentBody ) ||
				command != "body" ||
				!NPCPhysics_JragLineFinished( line ) ||
				body.boneName.empty() ||
				body.boneName.size() > 64 ||
				body.parentBody < -1 ||
				body.parentBody >= bodyIndex )
			{
				error = "invalid body name or parent index";
				return false;
			}

			for( size_t previous = 0; previous < definition.bodies.size(); ++previous )
				if( NPCPhysics_NameEquals(
					definition.bodies[previous].boneName.c_str(), body.boneName.c_str() ) )
				{
					error = "duplicate physical bone name";
					return false;
				}

			if( body.parentBody < 0 )
				++rootCount;

			float center[3];
			if( !readVector( "center", center ) )
			{
				error = "invalid body center";
				return false;
			}
			body.localShapeCenter.x = center[0];
			body.localShapeCenter.y = center[1];
			body.localShapeCenter.z = center[2];

			if( !readVector( "joint_min", body.jointMinDegrees ) ||
				!readVector( "joint_max", body.jointMaxDegrees ) ||
				!readFloat( "angular_damping_override", body.angularDampingOverride ) )
			{
				error = "invalid joint limits or angular damping override";
				return false;
			}
			if( body.angularDampingOverride < 0.0f )
			{
				error = "angular damping override cannot be negative";
				return false;
			}
			for( int axis = 0; axis < 3; ++axis )
				if( body.jointMinDegrees[axis] > body.jointMaxDegrees[axis] )
				{
					error = "joint minimum exceeds joint maximum";
					return false;
				}

			int pointCount = 0;
			if( !NPCPhysics_NextJragLine( input, line, storage ) ||
				!( line >> command >> pointCount ) ||
				command != "hull" ||
				!NPCPhysics_JragLineFinished( line ) ||
				pointCount < 4 ||
				pointCount > RAGDOLL_DEFINITION_MAX_HULL_POINTS )
			{
				error = "hull point count must be between 4 and 32";
				return false;
			}

			body.points.reserve( pointCount );
			for( int pointIndex = 0; pointIndex < pointCount; ++pointIndex )
			{
				NPCPhysicsRagdollPoint point;
				if( !NPCPhysics_NextJragLine( input, line, storage ) ||
					!( line >> command >> point.x >> point.y >> point.z ) ||
					command != "point" ||
					!NPCPhysics_JragLineFinished( line ) ||
					!std::isfinite( point.x ) ||
					!std::isfinite( point.y ) ||
					!std::isfinite( point.z ) )
				{
					error = "invalid or non-finite hull point";
					return false;
				}
				body.points.push_back( point );
			}

			if( !NPCPhysics_NextJragLine( input, line, storage ) ||
				!( line >> command ) ||
				command != "end_body" ||
				!NPCPhysics_JragLineFinished( line ) )
			{
				error = "expected end_body";
				return false;
			}
			definition.bodies.push_back( body );
		}

		if( rootCount != 1 )
		{
			error = "the physical hierarchy must have exactly one root body";
			return false;
		}
		if( !NPCPhysics_NextJragLine( input, line, storage ) ||
			!( line >> command ) ||
			command != "end" ||
			!NPCPhysics_JragLineFinished( line ) )
		{
			error = "expected end after all body definitions";
			return false;
		}
		if( NPCPhysics_NextJragLine( input, line, storage ) )
		{
			error = "unexpected data after end";
			return false;
		}

		return true;
	}

	static bool NPCPhysics_GetRagdollDefinitionPath( edict_t *ent, std::string &path )
	{
		if( !ent || !ent->v.model )
			return false;

		const char *modelName = STRING( ent->v.model );
		if( !modelName || !modelName[0] )
			return false;

		std::string modelPath( modelName );
		if( modelPath.size() < 11 ||
			modelPath.compare( 0, 7, "models/" ) != 0 ||
			!NPCPhysics_NameEquals( modelPath.c_str() + modelPath.size() - 4, ".mdl" ) ||
			modelPath.find( '\\' ) != std::string::npos ||
			modelPath.find( ".." ) != std::string::npos ||
			modelPath.size() >= MAX_PATH )
			return false;

		modelPath.replace( modelPath.size() - 4, 4, ".jrag" );
		path.swap( modelPath );
		return true;
	}

	static bool NPCPhysics_LoadRagdollDefinition(
		const std::string &path,
		NPCPhysicsRagdollDefinition &definition,
		std::string &error )
	{
		if( !g_engfuncs.pfnLoadFileForMe || !g_engfuncs.pfnFreeFile )
		{
			error = "engine file-loading callbacks are unavailable";
			return false;
		}

		int fileLength = 0;
		byte *fileData = LOAD_FILE_FOR_ME( const_cast<char *>( path.c_str() ), &fileLength );
		if( !fileData )
		{
			error = "file was not found in the game search paths";
			return false;
		}
		if( fileLength <= 0 || fileLength > RAGDOLL_DEFINITION_MAX_FILE_BYTES )
		{
			FREE_FILE( fileData );
			error = "file is empty or exceeds the 1 MiB limit";
			return false;
		}

		const bool parsed = NPCPhysics_ParseJragDefinition(
			reinterpret_cast<const char *>( fileData ), fileLength, definition, error );
		FREE_FILE( fileData );
		return parsed;
	}

	static const NPCPhysicsRagdollDefinition *NPCPhysics_GetRagdollDefinition(
		edict_t *ent, bool reportErrors )
	{
		std::string path;
		if( !NPCPhysics_GetRagdollDefinitionPath( ent, path ) )
			return NULL;

		for( size_t i = 0; i < g_ragdollDefinitionCache.size(); ++i )
		{
			NPCPhysicsCachedRagdollDefinition &cached = g_ragdollDefinitionCache[i];
			if( cached.path != path )
				continue;

			if( cached.loaded )
				return &cached.definition;

			if( reportErrors && !cached.errorReported )
			{
				ALERT(
					at_console,
					"[NPC physics] No ragdoll for model '%s': %s. GoldSrc death animation will be used.\n",
					path.c_str(),
					cached.error.c_str()
				);
				cached.errorReported = true;
			}
			return NULL;
		}

		NPCPhysicsCachedRagdollDefinition cached;
		cached.path = path;
		if( !NPCPhysics_LoadRagdollDefinition( path, cached.definition, cached.error ) )
		{
			g_ragdollDefinitionCache.push_back( std::move( cached ) );
			NPCPhysicsCachedRagdollDefinition &failed = g_ragdollDefinitionCache.back();
			if( reportErrors )
			{
				ALERT(
					at_console,
					"[NPC physics] No ragdoll for model '%s': %s. GoldSrc death animation will be used.\n",
					path.c_str(),
					failed.error.c_str()
				);
				failed.errorReported = true;
			}
			return NULL;
		}

		cached.loaded = true;
		g_ragdollDefinitionCache.push_back( std::move( cached ) );
		const NPCPhysicsRagdollDefinition *loaded = &g_ragdollDefinitionCache.back().definition;
		ALERT(
			at_console,
			"[NPC physics] Loaded '%s': %u rigid bodies, mass %.1f.\n",
			path.c_str(),
			static_cast<unsigned int>( loaded->bodies.size() ),
			loaded->totalMass
		);
		return loaded;
	}

	static Vec3 NPCPhysics_ClampVector( const Vec3 &value, float maxLength )
	{
		const float length = value.Length();
		if( length > maxLength && length > 0.0001f )
			return value * ( maxLength / length );
		return value;
	}

	static bool NPCPhysics_CreateRagdoll( edict_t *ent )
	{
		if( !ent || ent->free )
			return NPCPhysics_RagdollSetupFailed( "NPC entity is invalid" );
		const NPCPhysicsRagdollDefinition *physicsDefinition = NPCPhysics_GetRagdollDefinition( ent, true );
		if( !physicsDefinition )
			return false;
		if( !NPCPhysics_Initialize() )
			return NPCPhysics_RagdollSetupFailed( "Jolt initialization failed" );
		if( g_msgRagdollPose <= 0 )
			return NPCPhysics_RagdollSetupFailed( "RagdollPose network message was not registered" );
		if( !g_serverPhysicsAPI )
			return NPCPhysics_RagdollSetupFailed( "Xash server physics API was not received" );
		if( !g_worldCollisionMeshAPI )
			return NPCPhysics_RagdollSetupFailed( "native Xash BSP mesh API is missing (engine build may be outdated)" );
		if( !g_engfuncs.pfnGetBonePosition )
			return NPCPhysics_RagdollSetupFailed( "engine bone-position callback is missing" );
		if( !NPCPhysics_EnsureWorldCollider() )
			return false;
		NPCPhysics_UpdateBrushColliders();

		studiohdr_t *header = NULL;
		if( !NPCPhysics_GetStudioHeader( ent, &header, true ) )
			return false;

		mstudiobone_t *studioBones = reinterpret_cast<mstudiobone_t *>(
			reinterpret_cast<byte *>( header ) + header->boneindex
		);
		NPCPhysicsRagdoll ragdoll;
		ragdoll.entity = ent;
		ragdoll.serialNumber = ent->serialnumber;
		ragdoll.networkID = NPCPhysics_AllocateRagdollID();
		ragdoll.studioBoneCount = header->numbones;
		ragdoll.nextPoseTime = 0.0f;

		std::vector<::Vector> bonePositions( ragdoll.studioBoneCount );
		std::vector<::Vector> boneAngles( ragdoll.studioBoneCount );
		std::vector<Quat> boneRotations( ragdoll.studioBoneCount );
		for( int i = 0; i < ragdoll.studioBoneCount; ++i )
		{
			if( studioBones[i].parent < -1 || studioBones[i].parent >= ragdoll.studioBoneCount || studioBones[i].parent == i )
				return NPCPhysics_RagdollSetupFailed( "NPC studio skeleton contains an invalid parent index" );
			g_engfuncs.pfnGetBonePosition( ent, i, &bonePositions[i][0], &boneAngles[i][0] );
			boneRotations[i] = NPCPhysics_AnglesToQuaternion( boneAngles[i] );
		}

		// Resolve the model-specific physical rig by bone name. The file's
		// physical hierarchy is independent of compiled MDL bone indices.
		ragdoll.bodyCount = static_cast<int>( physicsDefinition->bodies.size() );
		if( ragdoll.bodyCount < 1 || ragdoll.bodyCount > RAGDOLL_DEFINITION_MAX_BODIES )
			return NPCPhysics_RagdollSetupFailed( "loaded ragdoll definition has an invalid body count" );

		std::vector<int> rigStudioBones( ragdoll.bodyCount, -1 );
		std::vector<int> studioBoneToRig( ragdoll.studioBoneCount, -1 );
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const NPCPhysicsRagdollBodyDefinition &bodyDefinition = physicsDefinition->bodies[body];
			const char *requiredName = bodyDefinition.boneName.c_str();
			for( int studio = 0; studio < ragdoll.studioBoneCount; ++studio )
				if( NPCPhysics_NameEquals( studioBones[studio].name, requiredName ) )
				{
					if( rigStudioBones[body] >= 0 )
						return NPCPhysics_RagdollSetupFailed( "model contains duplicate names for a physical bone" );
					rigStudioBones[body] = studio;
				}
			if( rigStudioBones[body] < 0 )
			{
				ALERT(
					at_console,
					"[NPC physics] Ragdoll setup failed: physics bone '%s' is missing from model '%s'.\n",
					requiredName,
					STRING( ent->v.model )
				);
				return false;
			}
			studioBoneToRig[rigStudioBones[body]] = body;
		}

		// Confirm that the compiled MDL hierarchy agrees with the external rig.
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const NPCPhysicsRagdollBodyDefinition &bodyDefinition = physicsDefinition->bodies[body];
			int ancestor = studioBones[rigStudioBones[body]].parent;
			while( ancestor >= 0 && studioBoneToRig[ancestor] < 0 )
				ancestor = studioBones[ancestor].parent;
			const int actualParent = ancestor < 0 ? -1 : studioBoneToRig[ancestor];
			if( actualParent != bodyDefinition.parentBody )
			{
				ALERT(
					at_console,
					"[NPC physics] Ragdoll setup failed: parent mismatch for physics bone '%s'.\n",
					bodyDefinition.boneName.c_str()
				);
				return false;
			}
		}

		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const NPCPhysicsRagdollBodyDefinition &bodyDefinition = physicsDefinition->bodies[body];
			RagdollBone bone;
			bone.studioBoneIndex = rigStudioBones[body];
			bone.parentElementIndex = bodyDefinition.parentBody;
			bone.localShapeCenter = Vec3(
				bodyDefinition.localShapeCenter.x,
				bodyDefinition.localShapeCenter.y,
				bodyDefinition.localShapeCenter.z );
			ragdoll.bones.push_back( bone );
			if( bone.parentElementIndex < 0 )
			{
				if( ragdoll.rootElement >= 0 )
					return NPCPhysics_RagdollSetupFailed( "external rig has multiple root bodies" );
				ragdoll.rootElement = body;
			}
		}
		if( ragdoll.rootElement < 0 )
			return NPCPhysics_RagdollSetupFailed( "external rig has no root body" );

		const int rootStudioBone = ragdoll.bones[ragdoll.rootElement].studioBoneIndex;
		ragdoll.entityOriginOffset = ent->v.origin - bonePositions[rootStudioBone];

		// Build Jolt convex hulls from the vertices in the runtime definition.
		std::vector<Ref<Shape> > physicsShapes( ragdoll.bodyCount );
		std::vector<float> massWeights( ragdoll.bodyCount, 0.0f );
		float totalWeight = 0.0f;
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const NPCPhysicsRagdollBodyDefinition &bodyDefinition = physicsDefinition->bodies[body];
			if( bodyDefinition.points.size() < 4 ||
				bodyDefinition.points.size() > RAGDOLL_DEFINITION_MAX_HULL_POINTS )
				return NPCPhysics_RagdollSetupFailed( "external collision hull has an invalid point count" );
			Vec3 points[RAGDOLL_DEFINITION_MAX_HULL_POINTS];
			for( size_t point = 0; point < bodyDefinition.points.size(); ++point )
			{
				const NPCPhysicsRagdollPoint &p = bodyDefinition.points[point];
				points[point] = Vec3( p.x, p.y, p.z );
			}
			ConvexHullShapeSettings hullSettings(
				points, static_cast<int>( bodyDefinition.points.size() ), 0.05f );
			Shape::ShapeResult result = hullSettings.Create();
			if( !result.IsValid() )
			{
				ALERT( at_console, "[NPC physics] Ragdoll setup failed: Jolt could not build hull '%s'.\n",
					bodyDefinition.boneName.c_str() );
				return false;
			}
			physicsShapes[body] = result.Get();
			massWeights[body] = physicsShapes[body]->GetVolume();
			if( !std::isfinite( massWeights[body] ) || massWeights[body] <= 0.0001f )
				return NPCPhysics_RagdollSetupFailed( "authored NPC collision hull has invalid volume" );
			totalWeight += massWeights[body];
		}
		if( totalWeight <= 0.0001f || !std::isfinite( totalWeight ) )
			return NPCPhysics_RagdollSetupFailed( "authored NPC hull set has zero combined volume" );

		float totalMass = NPCPhysics_GetCvar( "sv_npc_vphysics_mass", 0.0f );
		if( totalMass <= 0.0f )
			totalMass = physicsDefinition->totalMass;
		totalMass = (std::max)( 10.0f, (std::min)( totalMass, 300.0f ) );

		// Build the reduced physical skeleton and complete compiled studio skeleton.
		ragdoll.physicsSkeleton = new Skeleton();
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const NPCPhysicsRagdollBodyDefinition &bodyDefinition = physicsDefinition->bodies[body];
			ragdoll.physicsSkeleton->AddJoint( bodyDefinition.boneName.c_str(), bodyDefinition.parentBody );
		}

		ragdoll.studioSkeleton = new Skeleton();
		for( int studio = 0; studio < ragdoll.studioBoneCount; ++studio )
		{
			const int parent = studioBones[studio].parent;
			if( parent < -1 || parent >= studio )
				return NPCPhysics_RagdollSetupFailed( "compiled NPC studio skeleton is not parent-first" );
			if( !studioBones[studio].name[0] )
				return NPCPhysics_RagdollSetupFailed( "compiled NPC studio skeleton contains an unnamed bone" );
			ragdoll.studioSkeleton->AddJoint( studioBones[studio].name, parent );
		}

		const Vec3 bindRootOrigin(
			bonePositions[rootStudioBone][0],
			bonePositions[rootStudioBone][1],
			bonePositions[rootStudioBone][2] );

		// Both neutral poses use world-oriented matrices translated relative to
		// the pelvis. SkeletonMapper corrects the reduced rig to the MDL rig.
		std::vector<Mat44> studioNeutralModel( ragdoll.studioBoneCount );
		for( int studio = 0; studio < ragdoll.studioBoneCount; ++studio )
		{
			const Vec3 position(
				bonePositions[studio][0],
				bonePositions[studio][1],
				bonePositions[studio][2] );
			studioNeutralModel[studio] = Mat44::sRotationTranslation(
				boneRotations[studio], position - bindRootOrigin );
		}

		// Cache the local death-pose transforms for non-physics bones.
		ragdoll.studioDeathLocalPose.resize( ragdoll.studioBoneCount );
		for( int studio = 0; studio < ragdoll.studioBoneCount; ++studio )
		{
			const int parent = studioBones[studio].parent;
			ragdoll.studioDeathLocalPose[studio] = parent >= 0
				? studioNeutralModel[parent].Inversed() * studioNeutralModel[studio]
				: studioNeutralModel[studio];
		}

		std::vector<Mat44> physicsNeutralModel( ragdoll.bodyCount );
		for( int body = 0; body < ragdoll.bodyCount; ++body )
			physicsNeutralModel[body] = studioNeutralModel[ragdoll.bones[body].studioBoneIndex];

		ragdoll.skeletonMapper = new SkeletonMapper();
		// GoldSrc model compilers can change the capitalization of bone names.
		// Use the same case-insensitive match as the authored rig resolver.
		SkeletonMapper::CanMapJoint canMapJoint =
			[]( const Skeleton *source, int sourceIndex, const Skeleton *target, int targetIndex ) -> bool
			{
				return NPCPhysics_NameEquals(
					source->GetJoint( sourceIndex ).mName.c_str(),
					target->GetJoint( targetIndex ).mName.c_str() );
			};
		ragdoll.skeletonMapper->Initialize(
			ragdoll.physicsSkeleton.GetPtr(), physicsNeutralModel.data(),
			ragdoll.studioSkeleton.GetPtr(), studioNeutralModel.data(), canMapJoint );
		if( ragdoll.skeletonMapper->GetMappings().size() != static_cast<size_t>( ragdoll.bodyCount ) )
			return NPCPhysics_RagdollSetupFailed( "Jolt could not map every authored NPC body to its studio bone" );
		ragdoll.skeletonMapper->LockAllTranslations(
			ragdoll.studioSkeleton.GetPtr(), studioNeutralModel.data() );
		ragdoll.physicsPoseModelSpace.resize( ragdoll.bodyCount );
		ragdoll.studioPoseModelSpace.resize( ragdoll.studioBoneCount );


		ragdoll.bodyMasses.resize( ragdoll.bodyCount, 0.0f );
		Ref<RagdollSettings> settings = new RagdollSettings();
		settings->mSkeleton = ragdoll.physicsSkeleton;
		settings->mParts.resize( ragdoll.bodyCount );
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const NPCPhysicsRagdollBodyDefinition &bodyDefinition = physicsDefinition->bodies[body];
			const RagdollBone &bone = ragdoll.bones[body];
			const int studioBone = bone.studioBoneIndex;
			const Quat rotation = boneRotations[studioBone];
			const Vec3 boneOrigin(
				bonePositions[studioBone][0],
				bonePositions[studioBone][1],
				bonePositions[studioBone][2] );
			const Vec3 bodyCenter = boneOrigin + rotation * bone.localShapeCenter;
			const float bodyMass = totalMass * massWeights[body] / totalWeight;

			RagdollSettings::Part &part = settings->mParts[body];
			part.SetShape( physicsShapes[body].GetPtr() );
			part.mPosition = RVec3( bodyCenter );
			part.mRotation = rotation;
			part.mMotionType = EMotionType::Dynamic;
			part.mObjectLayer = RAGDOLL_OBJECT_LAYER;
			part.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
			part.mInertiaMultiplier = physicsDefinition->inertiaMultiplier;
			const float physicalBodyMass = (std::max)( 0.25f, bodyMass );
			ragdoll.bodyMasses[body] = physicalBodyMass;
			part.mMassPropertiesOverride.mMass = physicalBodyMass;
			part.mFriction = physicsDefinition->friction;
			part.mRestitution = physicsDefinition->restitution;
			part.mLinearDamping = physicsDefinition->linearDamping;
			part.mAngularDamping = bodyDefinition.angularDampingOverride > 0.0f
				? bodyDefinition.angularDampingOverride : physicsDefinition->angularDamping;
			part.mMaxLinearVelocity = RAGDOLL_VELOCITY_LIMIT;
			part.mMaxAngularVelocity = RAGDOLL_ANGULAR_VELOCITY_LIMIT;
			part.mMotionQuality = EMotionQuality::LinearCast;
			part.mEnhancedInternalEdgeRemoval = true;

			if( bone.parentElementIndex >= 0 )
			{
				SixDOFConstraintSettings *constraint = new SixDOFConstraintSettings();
				constraint->mSpace = EConstraintSpace::WorldSpace;
				const Vec3 joint(
					bonePositions[studioBone][0],
					bonePositions[studioBone][1],
					bonePositions[studioBone][2] );
				constraint->mPosition1 = RVec3( joint );
				constraint->mPosition2 = RVec3( joint );
				const Vec3 axisX = rotation.RotateAxisX().Normalized();
				const Vec3 axisY = rotation.RotateAxisY().Normalized();
				constraint->mAxisX1 = axisX;
				constraint->mAxisY1 = axisY;
				constraint->mAxisX2 = axisX;
				constraint->mAxisY2 = axisY;
				constraint->MakeFixedAxis( SixDOFConstraintSettings::TranslationX );
				constraint->MakeFixedAxis( SixDOFConstraintSettings::TranslationY );
				constraint->MakeFixedAxis( SixDOFConstraintSettings::TranslationZ );
				constraint->mSwingType = ESwingType::Pyramid;
				const SixDOFConstraintSettings::EAxis axes[3] = {
					SixDOFConstraintSettings::RotationX,
					SixDOFConstraintSettings::RotationY,
					SixDOFConstraintSettings::RotationZ };
				for( int axis = 0; axis < 3; ++axis )
				{
					const float minDegrees = bodyDefinition.jointMinDegrees[axis];
					const float maxDegrees = bodyDefinition.jointMaxDegrees[axis];
					const float minRadians = minDegrees <= -1.0e20f
						? -FLT_MAX : minDegrees * RAGDOLL_DEGREES_TO_RADIANS;
					const float maxRadians = maxDegrees >= 1.0e20f
						? FLT_MAX : maxDegrees * RAGDOLL_DEGREES_TO_RADIANS;
					constraint->SetLimitedAxis( axes[axis], minRadians, maxRadians );
				}
				part.mToParent = constraint;
			}
		}

		// Jolt's native articulated ragdoll owns body/constraint creation, collision
		// filtering, stabilization and lifetime from this point onward.
		if( !settings->Stabilize() )
			return NPCPhysics_RagdollSetupFailed( "Jolt could not stabilize the authored ragdoll" );
		settings->CalculateConstraintPriorities();
		settings->DisableParentChildCollisions();
		settings->CalculateBodyIndexToConstraintIndex();

		ragdoll.physicsRagdoll = settings->CreateRagdoll(
			static_cast<CollisionGroup::GroupID>( ragdoll.networkID ),
			static_cast<uint64>( ragdoll.networkID ), &g_npcPhysics.system );
		if( !ragdoll.physicsRagdoll.GetPtr() )
			return NPCPhysics_RagdollSetupFailed( "Jolt could not allocate native ragdoll bodies" );
		ragdoll.physicsRagdoll->AddToPhysicsSystem( EActivation::Activate );
		ragdoll.addedToPhysicsSystem = true;

		NPCPhysicsLivePose *livePose = NULL;
		const int entityIndex = ENTINDEX( ent );
		if( entityIndex >= 0 && entityIndex < (int)g_livePoses.size() )
		{
			NPCPhysicsLivePose &candidate = g_livePoses[entityIndex];
			if( candidate.serialNumber == ent->serialnumber &&
				candidate.boneCount == ragdoll.studioBoneCount &&
				!candidate.positions.empty() )
				livePose = &candidate;
		}

		float animationDeltaTime = gpGlobals ? gpGlobals->time : 0.0f;
		if( livePose && gpGlobals )
		{
			animationDeltaTime -= livePose->sampleTime;
			if( animationDeltaTime < 0.001f || animationDeltaTime > 0.15f )
				animationDeltaTime = gpGlobals->frametime;
		}
		if( animationDeltaTime < 0.001f || animationDeltaTime > 0.15f )
			animationDeltaTime = 1.0f / 60.0f;

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const RagdollBone &bone = ragdoll.bones[body];
			const int boneIndex = bone.studioBoneIndex;
			const BodyID bodyID = ragdoll.physicsRagdoll->GetBodyID( body );
			const Quat rotation = boneRotations[boneIndex];
			const Vec3 boneOrigin(
				bonePositions[boneIndex][0], bonePositions[boneIndex][1], bonePositions[boneIndex][2] );
			const Vec3 bodyCenter = boneOrigin + rotation * bone.localShapeCenter;
			Vec3 linearVelocity(
				ent->v.velocity[0], ent->v.velocity[1], ent->v.velocity[2] );
			Vec3 angularVelocity(
				ent->v.avelocity[0] * RAGDOLL_DEGREES_TO_RADIANS,
				ent->v.avelocity[1] * RAGDOLL_DEGREES_TO_RADIANS,
				ent->v.avelocity[2] * RAGDOLL_DEGREES_TO_RADIANS );

			if( livePose && boneIndex < (int)livePose->positions.size() &&
				boneIndex < (int)livePose->rotations.size() )
			{
				const Vec3 previousCenter =
					livePose->positions[boneIndex] + livePose->rotations[boneIndex] * bone.localShapeCenter;
				const Vec3 entityLinearVelocity(
					( ent->v.origin[0] - livePose->entityOrigin[0] ) / animationDeltaTime,
					( ent->v.origin[1] - livePose->entityOrigin[1] ) / animationDeltaTime,
					( ent->v.origin[2] - livePose->entityOrigin[2] ) / animationDeltaTime );
				linearVelocity += ( bodyCenter - previousCenter ) / animationDeltaTime - entityLinearVelocity;
				Quat deltaRotation = ( rotation * livePose->rotations[boneIndex].Conjugated() ).Normalized();
				if( deltaRotation.GetW() < 0.0f )
					deltaRotation = Quat( -deltaRotation.GetX(), -deltaRotation.GetY(), -deltaRotation.GetZ(), -deltaRotation.GetW() );
				Quat entityDeltaRotation = (
					NPCPhysics_AnglesToQuaternion( ent->v.angles ) *
					NPCPhysics_AnglesToQuaternion( livePose->entityAngles ).Conjugated() ).Normalized();
				if( entityDeltaRotation.GetW() < 0.0f )
					entityDeltaRotation = Quat( -entityDeltaRotation.GetX(), -entityDeltaRotation.GetY(), -entityDeltaRotation.GetZ(), -entityDeltaRotation.GetW() );
				angularVelocity +=
					deltaRotation.GetAngularVelocity( animationDeltaTime ) -
					entityDeltaRotation.GetAngularVelocity( animationDeltaTime );
			}

			linearVelocity = NPCPhysics_ClampVector( linearVelocity, RAGDOLL_VELOCITY_LIMIT );
			angularVelocity = NPCPhysics_ClampVector( angularVelocity, RAGDOLL_ANGULAR_VELOCITY_LIMIT );
			bodyInterface.SetLinearVelocity( bodyID, linearVelocity );
			bodyInterface.SetAngularVelocity( bodyID, angularVelocity );
		}
		// Only replace the animated corpse after all physics bodies and joints
		// were built successfully. Freeze the death frame so non-physics bones
		// remain a stable fallback and inherit from simulated parents.
		ent->v.iuser4 = 1;
		ent->v.solid = SOLID_NOT;
		ent->v.movetype = MOVETYPE_NONE;
		ent->v.takedamage = DAMAGE_NO;
		ent->v.deadflag = DEAD_DEAD;
		ent->v.nextthink = -1.0f;
		ent->v.framerate = 0.0f;
		ent->v.animtime = gpGlobals ? gpGlobals->time : ent->v.animtime;
		ent->v.velocity = g_vecZero;
		ent->v.avelocity = g_vecZero;

		g_ragdolls.push_back( ragdoll );
		ALERT(
			at_console,
			"[NPC physics] Created articulated Jolt ragdoll: %d physics bodies for %d studio bones.\n",
			ragdoll.bodyCount,
			ragdoll.studioBoneCount
		);
		return true;
	}


	static Vec3 NPCPhysics_GetRootBoneOrigin(
		NPCPhysicsRagdoll &ragdoll, BodyInterface &bodyInterface )
	{
		const BodyID bodyID = ragdoll.physicsRagdoll->GetBodyID( ragdoll.rootElement );
		const RVec3 position = bodyInterface.GetPosition( bodyID );
		const Quat rotation = bodyInterface.GetRotation( bodyID ).Normalized();
		return Vec3(
			float( position.GetX() ), float( position.GetY() ), float( position.GetZ() )
		) - rotation * ragdoll.bones[ragdoll.rootElement].localShapeCenter;
	}

	static void NPCPhysics_UpdateMappedStudioPose(
		NPCPhysicsRagdoll &ragdoll, BodyInterface &bodyInterface, const Vec3 &rootBoneOrigin )
	{
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const RagdollBone &bone = ragdoll.bones[body];
			const BodyID bodyID = ragdoll.physicsRagdoll->GetBodyID( body );
			const RVec3 position = bodyInterface.GetPosition( bodyID );
			const Quat rotation = bodyInterface.GetRotation( bodyID ).Normalized();
			const Vec3 bodyPosition(
				float( position.GetX() ), float( position.GetY() ), float( position.GetZ() ) );
			const Vec3 boneOrigin = bodyPosition - rotation * bone.localShapeCenter;
			ragdoll.physicsPoseModelSpace[body] = Mat44::sRotationTranslation(
				rotation, boneOrigin - rootBoneOrigin );
		}
		ragdoll.skeletonMapper->Map(
			ragdoll.physicsPoseModelSpace.data(),
			ragdoll.studioDeathLocalPose.data(),
			ragdoll.studioPoseModelSpace.data() );
	}

	static short NPCPhysics_QuantizeQuaternion( float value )
	{
		value = (std::max)( -1.0f, (std::min)( 1.0f, value ) );
		return static_cast<short>( value * 32767.0f );
	}

	static void NPCPhysics_SendRagdollPose(
		NPCPhysicsRagdoll &ragdoll, BodyInterface &bodyInterface )
	{
		if( g_msgRagdollPose <= 0 || !ragdoll.entity || ragdoll.entity->free ||
			!ragdoll.physicsRagdoll.GetPtr() || !ragdoll.skeletonMapper.GetPtr() )
			return;

		const Vec3 rootBoneOrigin = NPCPhysics_GetRootBoneOrigin( ragdoll, bodyInterface );
		NPCPhysics_UpdateMappedStudioPose( ragdoll, bodyInterface, rootBoneOrigin );
		const int entityIndex = ENTINDEX( ragdoll.entity );
		const int snapshotBoneCount = ragdoll.studioBoneCount;

		for( int start = 0; start < snapshotBoneCount; start += RAGDOLL_NETWORK_BODIES_PER_CHUNK )
		{
			const int count = (std::min)(
				RAGDOLL_NETWORK_BODIES_PER_CHUNK, snapshotBoneCount - start );
			MESSAGE_BEGIN( MSG_PVS, g_msgRagdollPose, ragdoll.entity->v.origin );
				WRITE_SHORT( entityIndex );
				WRITE_SHORT( ragdoll.networkID );
				WRITE_BYTE( ragdoll.studioBoneCount );
				WRITE_BYTE( snapshotBoneCount );
				WRITE_BYTE( start );
				WRITE_BYTE( count );
				for( int i = 0; i < count; ++i )
				{
					const int studioBone = start + i;
					const Mat44 &transform = ragdoll.studioPoseModelSpace[studioBone];
					const Vec3 bonePosition = rootBoneOrigin + transform.GetTranslation();
					const Quat rotation = transform.GetQuaternion().Normalized();
					WRITE_BYTE( studioBone );
					WRITE_COORD( bonePosition.GetX() );
					WRITE_COORD( bonePosition.GetY() );
					WRITE_COORD( bonePosition.GetZ() );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetX() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetY() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetZ() ) );
					WRITE_SHORT( NPCPhysics_QuantizeQuaternion( rotation.GetW() ) );
				}
			MESSAGE_END();
		}
	}

	static int NPCPhysics_PhysicsEntity( edict_t *ent )
	{
		// Capture live animation transforms only. Return 0 so GoldSrc remains the
		// sole owner of living NPC movement, AI, contacts and collision.
		NPCPhysics_CaptureLivePose( ent );
		return 0;
	}

	static void NPCPhysics_EndFrame( void )
	{
		if( !g_npcPhysics.initialized || g_ragdolls.empty() || !gpGlobals )
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

		float gravity = NPCPhysics_GetCvar( "sv_gravity", 800.0f );
		if( gravity < 0.0f )
			gravity = 800.0f;
		g_npcPhysics.system.SetGravity( Vec3( 0.0f, 0.0f, -gravity ) );

		NPCPhysics_UpdateBrushColliders();

		float frameTime = gpGlobals->frametime;
		if( frameTime < 0.0f )
			frameTime = 0.0f;
		if( frameTime > 0.1f )
			frameTime = 0.1f;

		g_npcPhysics.fixedAccumulator += frameTime;
		const float maxAccumulatedTime = RAGDOLL_FIXED_STEP * RAGDOLL_MAX_STEPS_PER_FRAME;
		if( g_npcPhysics.fixedAccumulator > maxAccumulatedTime )
			g_npcPhysics.fixedAccumulator = maxAccumulatedTime;

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		int steps = 0;
		while( g_npcPhysics.fixedAccumulator >= RAGDOLL_FIXED_STEP &&
			steps < RAGDOLL_MAX_STEPS_PER_FRAME )
		{
			g_npcPhysics.system.Update(
				RAGDOLL_FIXED_STEP,
				1,
				g_npcPhysics.tempAllocator,
				&g_npcPhysics.jobs
			);

			g_npcPhysics.fixedAccumulator -= RAGDOLL_FIXED_STEP;
			++steps;
		}

		for( size_t r = 0; r < g_ragdolls.size(); ++r )
		{
			NPCPhysicsRagdoll &ragdoll = g_ragdolls[r];

			// One-shot blast telemetry: compare the selected body and the actual
			// ragdoll root after 100 ms of Jolt simulation.
			if( ragdoll.blastDebugPending &&
				gpGlobals->time - ragdoll.blastDebugTime >= 0.10f &&
				ragdoll.blastDebugBodyID.IsInvalid() == false )
			{
				const RVec3 bodyPosition = bodyInterface.GetPosition( ragdoll.blastDebugBodyID );
				const Vec3 bodyDelta(
					float( bodyPosition.GetX() ) - ragdoll.blastDebugBodyStartPosition.GetX(),
					float( bodyPosition.GetY() ) - ragdoll.blastDebugBodyStartPosition.GetY(),
					float( bodyPosition.GetZ() ) - ragdoll.blastDebugBodyStartPosition.GetZ() );
				const Vec3 bodyVelocity = bodyInterface.GetLinearVelocity( ragdoll.blastDebugBodyID );
				const Vec3 bodyAngular = bodyInterface.GetAngularVelocity( ragdoll.blastDebugBodyID );
				Vec3 rootDelta( 0.0f, 0.0f, 0.0f );
				Vec3 rootVelocity( 0.0f, 0.0f, 0.0f );
				if( ragdoll.rootElement >= 0 && ragdoll.rootElement < ragdoll.bodyCount )
				{
					const BodyID rootBody = ragdoll.physicsRagdoll->GetBodyID( ragdoll.rootElement );
					const RVec3 rootPosition = bodyInterface.GetPosition( rootBody );
					rootDelta = Vec3(
						float( rootPosition.GetX() ) - ragdoll.blastDebugRootStartPosition.GetX(),
						float( rootPosition.GetY() ) - ragdoll.blastDebugRootStartPosition.GetY(),
						float( rootPosition.GetZ() ) - ragdoll.blastDebugRootStartPosition.GetZ() );
					rootVelocity = bodyInterface.GetLinearVelocity( rootBody );
				}
				ALERT(
					at_console,
					"[NPC blast] Post-step: ent=%d id=%d body=%d dt=%.2f body-delta=(%.1f %.1f %.1f) body-v=(%.1f %.1f %.1f) body-w=(%.1f %.1f %.1f) root-delta=(%.1f %.1f %.1f) root-v=(%.1f %.1f %.1f).\n",
					ENTINDEX( ragdoll.entity ), ragdoll.networkID, ragdoll.blastDebugBodyIndex,
					gpGlobals->time - ragdoll.blastDebugTime,
					bodyDelta.GetX(), bodyDelta.GetY(), bodyDelta.GetZ(),
					bodyVelocity.GetX(), bodyVelocity.GetY(), bodyVelocity.GetZ(),
					bodyAngular.GetX(), bodyAngular.GetY(), bodyAngular.GetZ(),
					rootDelta.GetX(), rootDelta.GetY(), rootDelta.GetZ(),
					rootVelocity.GetX(), rootVelocity.GetY(), rootVelocity.GetZ()
				);
				ragdoll.blastDebugPending = false;
			}

			// Move the carrier edict with the simulated root element. Besides
			// keeping the corpse's network origin useful, this keeps renderer
			// culling and the entity's PVS bounds near a ragdoll that has fallen.
			if( ragdoll.rootElement >= 0 &&
				ragdoll.rootElement < ragdoll.bodyCount &&
				g_engfuncs.pfnSetOrigin )
			{
				const Vec3 rootBoneOrigin = NPCPhysics_GetRootBoneOrigin( ragdoll, bodyInterface );

				const float entityOrigin[3] = {
					rootBoneOrigin.GetX() + ragdoll.entityOriginOffset[0],
					rootBoneOrigin.GetY() + ragdoll.entityOriginOffset[1],
					rootBoneOrigin.GetZ() + ragdoll.entityOriginOffset[2]
				};
				g_engfuncs.pfnSetOrigin( ragdoll.entity, entityOrigin );
			}

			if( gpGlobals->time >= ragdoll.nextPoseTime )
			{
				// A settled corpse needs only an occasional full-pose heartbeat.
				// Keep the original 20 Hz stream while any articulated body moves.
				bool isActive = false;
				for( int body = 0; body < ragdoll.bodyCount; ++body )
				{
					if( bodyInterface.IsActive( ragdoll.physicsRagdoll->GetBodyID( body ) ) )
					{
						isActive = true;
						break;
					}
				}

				NPCPhysics_SendRagdollPose( ragdoll, bodyInterface );
				ragdoll.nextPoseTime = gpGlobals->time + ( isActive
					? RAGDOLL_ACTIVE_POSE_INTERVAL
					: RAGDOLL_SLEEPING_POSE_INTERVAL );
			}
		}
	}
}

int NPCPhysics_IsRagdollEntity( edict_t *ent )
{
	if( !ent || ent->free )
		return FALSE;

	for( size_t i = 0; i < g_ragdolls.size(); ++i )
	{
		const NPCPhysicsRagdoll &ragdoll = g_ragdolls[i];
		if( ragdoll.entity == ent && ragdoll.serialNumber == ent->serialnumber &&
			ragdoll.physicsRagdoll.GetPtr() && ragdoll.addedToPhysicsSystem )
			return TRUE;
	}

	return FALSE;
}

int NPCPhysics_ApplyBlastImpulse(
	edict_t *targetEntity,
	const float *origin,
	float radius,
	float blastDamage )
{
	const float blastScale = NPCPhysics_GetCvar( "sv_npc_vphysics_blast_scale", 1.0f );
	if( !NPCPhysics_Enabled() || !g_npcPhysics.initialized || g_ragdolls.empty() ||
		!targetEntity || targetEntity->free || !origin ||
		!std::isfinite( radius ) || radius <= 0.001f ||
		!std::isfinite( blastDamage ) || blastDamage <= 0.0f ||
		!std::isfinite( blastScale ) || blastScale <= 0.0f ||
		!std::isfinite( origin[0] ) || !std::isfinite( origin[1] ) ||
		!std::isfinite( origin[2] ) )
		return 0;

	NPCPhysicsRagdoll *targetRagdoll = NULL;
	for( size_t i = 0; i < g_ragdolls.size(); ++i )
	{
		NPCPhysicsRagdoll &ragdoll = g_ragdolls[i];
		if( ragdoll.entity == targetEntity &&
			ragdoll.serialNumber == targetEntity->serialnumber &&
			ragdoll.addedToPhysicsSystem && ragdoll.physicsRagdoll.GetPtr() )
		{
			targetRagdoll = &ragdoll;
			break;
		}
	}

	if( !targetRagdoll || targetRagdoll->bodyCount <= 0 ||
		targetRagdoll->bodyMasses.size() != static_cast<size_t>( targetRagdoll->bodyCount ) )
		return 0;

	BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
	const RVec3 blastOrigin( origin[0], origin[1], origin[2] );

	// First find every body the blast can actually see. The mass-weighted
	// center of this exposed set supplies one coherent outward blast direction.
	// We deliberately do not distribute the full blast budget over every limb:
	// one exposed, substantial body receives the main impulse and Jolt's
	// articulated constraints transmit the reaction through the ragdoll.
	BodyID impulseBody;
	RVec3 impulsePoint = blastOrigin;
	float impulseBodyMass = 0.0f;
	float impulseFalloff = 0.0f;
	float bestBodyScore = 0.0f;
	Vec3 exposedMassCenter( 0.0f, 0.0f, 0.0f );
	float totalRagdollMass = 0.0f;
	float exposedMass = 0.0f;
	int exposedBodies = 0;
	int impulseBodyIndex = -1;

	for( int body = 0; body < targetRagdoll->bodyCount; ++body )
	{
		const float bodyMass = targetRagdoll->bodyMasses[body];
		if( !std::isfinite( bodyMass ) || bodyMass <= 0.0f )
			continue;
		totalRagdollMass += bodyMass;

		const BodyID bodyID = targetRagdoll->physicsRagdoll->GetBodyID( body );
		const RVec3 bodyPosition = bodyInterface.GetPosition( bodyID );
		const Vec3 offset(
			float( bodyPosition.GetX() ) - origin[0],
			float( bodyPosition.GetY() ) - origin[1],
			float( bodyPosition.GetZ() ) - origin[2] );
		const float distance = offset.Length();
		if( !std::isfinite( distance ) || distance >= radius )
			continue;

		RVec3 surfacePoint = bodyPosition;
		if( distance > 0.001f )
		{
			// The ray must reach this body before any other Jolt collider.
			// The scene includes the native BSP world and inline brush models.
			const RRayCast ray( blastOrigin, offset );
			RayCastResult hit;
			hit.mFraction = 1.0f;
			if( !g_npcPhysics.system.GetNarrowPhaseQuery().CastRay( ray, hit ) ||
				hit.mBodyID != bodyID )
				continue;
			surfacePoint = ray.GetPointOnRay( hit.mFraction );
		}

		const float falloff = 1.0f - distance / radius;
		if( falloff <= 0.0f )
			continue;

		const Vec3 bodyCenter(
			float( bodyPosition.GetX() ),
			float( bodyPosition.GetY() ),
			float( bodyPosition.GetZ() ) );
		exposedMassCenter += bodyCenter * bodyMass;
		exposedMass += bodyMass;
		++exposedBodies;

		// Prefer a substantial exposed body close to the blast. This avoids
		// spending the main impulse on a hand, foot, or other low-mass extremity.
		const float bodyScore = bodyMass * falloff;
		if( bodyScore > bestBodyScore )
		{
			bestBodyScore = bodyScore;
			impulseBody = bodyID;
			impulsePoint = surfacePoint;
			impulseBodyMass = bodyMass;
			impulseFalloff = falloff;
			impulseBodyIndex = body;
		}
	}

	if( exposedBodies == 0 || exposedMass <= 0.0f || bestBodyScore <= 0.0f ||
		impulseBodyIndex < 0 )
	{
		if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT(
				at_console,
				"[NPC blast] No visible ragdoll bodies in range: ent=%d id=%d radius=%.1f.\n",
				ENTINDEX( targetEntity ), targetRagdoll->networkID, radius
			);
		return 0;
	}

	const Vec3 exposedCenter = exposedMassCenter / exposedMass;
	Vec3 blastDirection(
		exposedCenter.GetX() - origin[0],
		exposedCenter.GetY() - origin[1],
		exposedCenter.GetZ() - origin[2] );
	const float directionLength = blastDirection.Length();
	if( !std::isfinite( directionLength ) || directionLength <= 0.001f )
		blastDirection = Vec3( 0.0f, 0.0f, 1.0f );
	else
		blastDirection /= directionLength;

	// A single-body impulse budget keeps the kick energetic enough to launch
	// the articulated assembly. The body mass controls the resulting velocity;
	// distance attenuation and the user scale remain independent adjustments.
	const float baseImpulse = (std::min)( blastDamage * 300.0f, 30000.0f );
	const float scaledImpulse = baseImpulse * impulseFalloff * blastScale;
	if( !std::isfinite( scaledImpulse ) || scaledImpulse <= 0.0f )
		return 0;

	const Vec3 impulse = blastDirection * scaledImpulse;
	const Vec3 velocityBefore = bodyInterface.GetLinearVelocity( impulseBody );
	const Vec3 angularBefore = bodyInterface.GetAngularVelocity( impulseBody );
	bodyInterface.ActivateBody( impulseBody );
	// Apply at the exposed surface point, not at the body's center. Because
	// direction follows the ragdoll's exposed mass center, an asymmetric hit
	// produces a lever arm and angular response without an arbitrary spin.
	bodyInterface.AddImpulse( impulseBody, impulse, impulsePoint );

	const Vec3 velocityAfter = bodyInterface.GetLinearVelocity( impulseBody );
	const Vec3 angularAfter = bodyInterface.GetAngularVelocity( impulseBody );
	const RVec3 bodyPosition = bodyInterface.GetPosition( impulseBody );
	const BodyID rootBody = targetRagdoll->physicsRagdoll->GetBodyID( targetRagdoll->rootElement );
	const RVec3 rootPosition = bodyInterface.GetPosition( rootBody );
	targetRagdoll->blastDebugBodyID = impulseBody;
	targetRagdoll->blastDebugBodyIndex = impulseBodyIndex;
	targetRagdoll->blastDebugBodyStartPosition = Vec3(
		float( bodyPosition.GetX() ), float( bodyPosition.GetY() ), float( bodyPosition.GetZ() ) );
	targetRagdoll->blastDebugRootStartPosition = Vec3(
		float( rootPosition.GetX() ), float( rootPosition.GetY() ), float( rootPosition.GetZ() ) );
	targetRagdoll->blastDebugTime = gpGlobals ? gpGlobals->time : 0.0f;
	targetRagdoll->blastDebugPending = true;

	if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
		ALERT(
			at_console,
			"[NPC blast] Immediate velocity: ent=%d body=%d impulse=(%.1f %.1f %.1f) v-before=(%.1f %.1f %.1f) v-after=(%.1f %.1f %.1f) w-before=(%.1f %.1f %.1f) w-after=(%.1f %.1f %.1f).\n",
			ENTINDEX( targetEntity ), impulseBodyIndex,
			impulse.GetX(), impulse.GetY(), impulse.GetZ(),
			velocityBefore.GetX(), velocityBefore.GetY(), velocityBefore.GetZ(),
			velocityAfter.GetX(), velocityAfter.GetY(), velocityAfter.GetZ(),
			angularBefore.GetX(), angularBefore.GetY(), angularBefore.GetZ(),
			angularAfter.GetX(), angularAfter.GetY(), angularAfter.GetZ()
		);

	if( gpGlobals && targetRagdoll->nextPoseTime > gpGlobals->time )
		targetRagdoll->nextPoseTime = gpGlobals->time;

	if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
		ALERT(
			at_console,
			"[NPC blast] Applied focused impulse: ent=%d id=%d body=%d mass=%.1f exposed=%d/%d exposed-mass=%.1f/%.1f radius=%.1f falloff=%.2f impulse=%.1f scale=%.2f.\n",
			ENTINDEX( targetEntity ), targetRagdoll->networkID,
			impulseBodyIndex, impulseBodyMass,
			exposedBodies, targetRagdoll->bodyCount,
			exposedMass, totalRagdollMass, radius,
			impulseFalloff, scaledImpulse, blastScale
		);

	return 1;
}

int NPCPhysics_TraceAndApplyImpact(
	const float *start,
	const float *end,
	float maxFraction,
	float impulseMagnitude,
	float *hitFraction,
	float *hitPosition,
	edict_t **hitEntity )
{
	if( !NPCPhysics_Enabled() || !g_npcPhysics.initialized || g_ragdolls.empty() ||
		!start || !end || !hitFraction || !hitPosition || !hitEntity ||
		!std::isfinite( maxFraction ) || !std::isfinite( impulseMagnitude ) ||
		impulseMagnitude <= 0.0f )
		return FALSE;

	maxFraction = (std::max)( 0.0f, (std::min)( maxFraction, 1.0f ) );
	const JPH::Vec3 rayDirection(
		end[0] - start[0],
		end[1] - start[1],
		end[2] - start[2] );
	const float rayLength = rayDirection.Length();
	if( !std::isfinite( rayLength ) || rayLength <= 0.001f )
		return FALSE;

	const JPH::RRayCast ray(
		JPH::RVec3( start[0], start[1], start[2] ),
		rayDirection );
	JPH::RayCastResult hit;
	hit.mFraction = maxFraction;
	if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
		ALERT(
			at_console,
			"[NPC decals] Bullet trace: start=(%.1f %.1f %.1f) end=(%.1f %.1f %.1f) base-fraction=%.3f jolt-length=%.1f.\n",
			start[0], start[1], start[2], end[0], end[1], end[2],
			maxFraction, rayLength
		);

	if( !g_npcPhysics.system.GetNarrowPhaseQuery().CastRay( ray, hit ) )
	{
		if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT( at_console, "[NPC decals] Jolt ray did not hit any physics body.\n" );
		return FALSE;
	}

	// The Jolt world and brush colliders participate in the ray test too.
	// Only accept a body that belongs to one of our articulated NPC ragdolls.
	edict_t *targetEntity = NULL;
	NPCPhysicsRagdoll *targetRagdoll = NULL;
	int targetBodyIndex = -1;
	bool foundRagdollBody = false;
	for( size_t ragdollIndex = 0; ragdollIndex < g_ragdolls.size(); ++ragdollIndex )
	{
		NPCPhysicsRagdoll &ragdoll = g_ragdolls[ragdollIndex];
		if( !ragdoll.entity || ragdoll.entity->free ||
			ragdoll.entity->serialnumber != ragdoll.serialNumber ||
			!ragdoll.addedToPhysicsSystem || !ragdoll.physicsRagdoll.GetPtr() )
			continue;

		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			if( ragdoll.physicsRagdoll->GetBodyID( body ) == hit.mBodyID )
			{
				targetEntity = ragdoll.entity;
				targetRagdoll = &ragdoll;
				targetBodyIndex = body;
				foundRagdollBody = true;
				break;
			}
		}
		if( foundRagdollBody )
			break;
	}

	if( !foundRagdollBody || !targetEntity )
	{
		if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT(
				at_console,
				"[NPC decals] Jolt ray hit a non-ragdoll body (fraction %.4f).\n",
				hit.mFraction
			);
		return FALSE;
	}

	const JPH::RVec3 impactPoint = ray.GetPointOnRay( hit.mFraction );
	JPH::Vec3 impulse = ( rayDirection / rayLength ) * impulseMagnitude;
	impulse = NPCPhysics_ClampVector( impulse, 3000.0f );

	JPH::BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
	// Source's client ragdoll applies a localized force to the struck physics
	// object. Explicit activation guarantees that a sleeping Jolt limb wakes.
	bodyInterface.ActivateBody( hit.mBodyID );
	bodyInterface.AddImpulse( hit.mBodyID, impulse, impactPoint );

	// A sleeping corpse normally sends only a 5 Hz heartbeat. A weapon strike
	// should be visible on the next end-frame snapshot, not after that interval.
	if( gpGlobals && targetRagdoll->nextPoseTime > gpGlobals->time )
		targetRagdoll->nextPoseTime = gpGlobals->time;

	// Send a world-space impact mark. The client projects it onto the
	// articulated studio mesh, then attaches the result to its skinned triangles.
	if( g_msgRagdollImpact > 0 && targetBodyIndex >= 0 &&
		targetBodyIndex < targetRagdoll->bodyCount )
	{
		CBaseEntity *hitObject = CBaseEntity::Instance( targetEntity );
		const int bloodColor = hitObject ? hitObject->BloodColor() : DONT_BLEED;
		if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT(
				at_console,
				"[NPC decals] Hit ragdoll ent=%d id=%d body=%d fraction=%.4f blood=%d msg=%d.\n",
				ENTINDEX( targetEntity ), targetRagdoll->networkID,
				targetBodyIndex, hit.mFraction, bloodColor, g_msgRagdollImpact
			);

		if( bloodColor != DONT_BLEED )
		{
			const RagdollBone &bone = targetRagdoll->bones[targetBodyIndex];
			if( bone.studioBoneIndex >= 0 &&
				bone.studioBoneIndex < targetRagdoll->studioBoneCount )
			{
				// Mirror Source's AddStudioDecal input: preserve the shot ray and
				// Jolt contact so the client can intersect the actual skinned mesh.
				const float markRadius = (std::max)(
					3.0f, (std::min)( 6.0f, 3.0f + impulseMagnitude * 0.00045f ) );
				const int encodedRadius = static_cast<int>( markRadius * 4.0f + 0.5f );
				const int markColor = bloodColor == BLOOD_COLOR_GREEN ? 1 :
					( bloodColor == BLOOD_COLOR_YELLOW ? 2 : 0 );
				const float markOrigin[3] = {
					float( impactPoint.GetX() ),
					float( impactPoint.GetY() ),
					float( impactPoint.GetZ() )
				};

				MESSAGE_BEGIN( MSG_PVS, g_msgRagdollImpact, markOrigin );
					WRITE_SHORT( ENTINDEX( targetEntity ) );
					WRITE_SHORT( targetRagdoll->networkID );
					WRITE_BYTE( bone.studioBoneIndex );
					WRITE_COORD( start[0] );
					WRITE_COORD( start[1] );
					WRITE_COORD( start[2] );
					WRITE_COORD( impactPoint.GetX() );
					WRITE_COORD( impactPoint.GetY() );
					WRITE_COORD( impactPoint.GetZ() );
					WRITE_BYTE( (std::max)( 1, (std::min)( 255, encodedRadius ) ) );
					WRITE_BYTE( markColor );
				MESSAGE_END();

				if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
					ALERT(
						at_console,
						"[NPC decals] Sent RagdollImpact: ent=%d id=%d studio-bone=%d radius=%.2f color=%d.\n",
						ENTINDEX( targetEntity ), targetRagdoll->networkID,
						bone.studioBoneIndex, markRadius, markColor
					);
			}
			else if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
				ALERT( at_console, "[NPC decals] Hit body has invalid studio bone %d.\n", bone.studioBoneIndex );
		}
		else if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT( at_console, "[NPC decals] Ragdoll blood color is DONT_BLEED; no mark sent.\n" );
	}
	else if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
		ALERT( at_console, "[NPC decals] Impact usermessage is not registered or body index is invalid.\n" );

	*hitFraction = hit.mFraction;
	hitPosition[0] = float( impactPoint.GetX() );
	hitPosition[1] = float( impactPoint.GetY() );
	hitPosition[2] = float( impactPoint.GetZ() );
	*hitEntity = targetEntity;
	return TRUE;
}

int NPCPhysics_TryCreateRagdoll( edict_t *ent )
{
	if( !NPCPhysics_Enabled() || !NPCPhysics_IsEligibleMonster( ent ) )
		return FALSE;

	if( !NPCPhysics_CreateRagdoll( ent ) )
	{
		ALERT( at_console, "[NPC physics] Jolt ragdoll creation failed; using GoldSrc death animation.\n" );
		return FALSE;
	}

	return TRUE;
}

int NPCPhysics_GetInterface( int version, void *serverAPI, void *callbacks )
{
	if( version != XASH_SERVER_PHYSICS_INTERFACE_VERSION ||
		!serverAPI || !callbacks )
		return FALSE;

	CVAR_REGISTER( &g_svNpcVPhysics );
	CVAR_REGISTER( &g_svNpcVPhysicsMass );
	CVAR_REGISTER( &g_svNpcVPhysicsBulletScale );
	CVAR_REGISTER( &g_svNpcVPhysicsCrowbarScale );
	CVAR_REGISTER( &g_svNpcVPhysicsBlastScale );
	CVAR_REGISTER( &g_svNpcVPhysicsDebug );

	g_serverPhysicsAPI = reinterpret_cast<xash_server_physics_api_s *>( serverAPI );
	g_worldCollisionMeshAPI = NULL;
	g_brushCollisionMeshAPI = NULL;
	g_brushCollisionUnavailableLogged = false;
	g_brushCollisionStatusLogged = false;

	ALERT(
		at_console,
		"[NPC physics] Native-object bridge: serverAPI=%p getter=%s.\n",
		serverAPI,
		g_serverPhysicsAPI->pfnGetNativeObject ? "present" : "NULL"
	);

	if( g_serverPhysicsAPI->pfnGetNativeObject )
	{
		void *worldObject = g_serverPhysicsAPI->pfnGetNativeObject( "xash_world_collision_mesh_v1" );
		g_worldCollisionMeshAPI = reinterpret_cast<xash_world_collision_mesh_api_s *>( worldObject );
		ALERT(
			at_console,
			"[NPC physics] World mesh API lookup: ptr=%p%s.\n",
			worldObject,
			g_worldCollisionMeshAPI
				? ( g_worldCollisionMeshAPI->version == XASH_WORLD_COLLISION_MESH_API_VERSION &&
					g_worldCollisionMeshAPI->GetMeshInfo && g_worldCollisionMeshAPI->GetMeshData
					? " valid" : " INVALID" )
				: " NULL"
		);
		if( g_worldCollisionMeshAPI &&
			( g_worldCollisionMeshAPI->version != XASH_WORLD_COLLISION_MESH_API_VERSION ||
			  !g_worldCollisionMeshAPI->GetMeshInfo || !g_worldCollisionMeshAPI->GetMeshData ) )
			g_worldCollisionMeshAPI = NULL;

		void *brushObject = g_serverPhysicsAPI->pfnGetNativeObject( "xash_brush_collision_mesh_v1" );
		g_brushCollisionMeshAPI = reinterpret_cast<xash_brush_collision_mesh_api_s *>( brushObject );
		if( g_brushCollisionMeshAPI )
		{
			const bool versionOK = g_brushCollisionMeshAPI->version == XASH_BRUSH_COLLISION_MESH_API_VERSION;
			const bool callbacksOK = g_brushCollisionMeshAPI->GetEntityCount &&
				g_brushCollisionMeshAPI->GetEntityInfo &&
				g_brushCollisionMeshAPI->GetMeshInfo &&
				g_brushCollisionMeshAPI->GetMeshData;
			ALERT(
				at_console,
				"[NPC physics] Brush mesh API lookup: ptr=%p version=%d expected=%d callbacks=%d/%d/%d/%d.\n",
				brushObject,
				g_brushCollisionMeshAPI->version,
				XASH_BRUSH_COLLISION_MESH_API_VERSION,
				g_brushCollisionMeshAPI->GetEntityCount ? 1 : 0,
				g_brushCollisionMeshAPI->GetEntityInfo ? 1 : 0,
				g_brushCollisionMeshAPI->GetMeshInfo ? 1 : 0,
				g_brushCollisionMeshAPI->GetMeshData ? 1 : 0
			);
			if( !versionOK || !callbacksOK )
				g_brushCollisionMeshAPI = NULL;
		}
		else
		{
			ALERT( at_console, "[NPC physics] Brush mesh API lookup returned NULL.\n" );
		}
	}
	else
	{
		ALERT( at_console, "[NPC physics] Native-object bridge has no getter callback.\n" );
	}
	if( gpGlobals )
	{
		const int maxEntities = (std::max)( 2048, gpGlobals->maxEntities + 1 );
		g_livePoses.resize( maxEntities );
	}

	if( !g_msgRagdollPose && g_engfuncs.pfnRegUserMsg )
		g_msgRagdollPose = g_engfuncs.pfnRegUserMsg( "RagdollPose", -1 );
	if( !g_msgRagdollImpact && g_engfuncs.pfnRegUserMsg )
		g_msgRagdollImpact = g_engfuncs.pfnRegUserMsg( "RagdollImpact", -1 );

	xash_physics_interface_s *physics =
		reinterpret_cast<xash_physics_interface_s *>( callbacks );
	memset( physics, 0, sizeof( *physics ) );
	physics->version = version;
	physics->SV_PhysicsEntity = NPCPhysics_PhysicsEntity;
	physics->SV_EndFrame = NPCPhysics_EndFrame;
	return TRUE;
}

void NPCPhysics_ResetMap( void )
{
	for( size_t i = 0; i < g_ragdolls.size(); ++i )
	{
		NPCPhysicsRagdoll &ragdoll = g_ragdolls[i];
		if( ragdoll.entity && !ragdoll.entity->free &&
			ragdoll.entity->serialnumber == ragdoll.serialNumber )
		{
			ragdoll.entity->v.effects |= EF_NODRAW;
			ragdoll.entity->v.iuser4 = 0;
			ragdoll.entity->v.solid = SOLID_NOT;
			ragdoll.entity->v.movetype = MOVETYPE_NONE;
		}
	}

	NPCPhysics_DestroyAllRagdolls();
	NPCPhysics_DestroyBrushColliders();
	NPCPhysics_DestroyWorldCollider();
	for( size_t i = 0; i < g_livePoses.size(); ++i )
	{
		g_livePoses[i].serialNumber = -1;
		g_livePoses[i].boneCount = 0;
		g_livePoses[i].sampleTime = 0.0f;
		g_livePoses[i].positions.clear();
		g_livePoses[i].rotations.clear();
	}
	g_ragdollDefinitionCache.clear();
	g_npcPhysics.fixedAccumulator = 0.0f;
}

void NPCPhysics_Shutdown( void )
{
	NPCPhysics_DestroyAllRagdolls();
	NPCPhysics_DestroyBrushColliders();
	NPCPhysics_DestroyWorldCollider();

	if( g_npcPhysics.initialized )
	{
		UnregisterTypes();
		delete Factory::sInstance;
		Factory::sInstance = NULL;
		delete g_npcPhysics.tempAllocator;
		g_npcPhysics.tempAllocator = NULL;
		g_npcPhysics.initialized = false;
		g_npcPhysics.fixedAccumulator = 0.0f;
	}

	g_serverPhysicsAPI = NULL;
	g_worldCollisionMeshAPI = NULL;
	g_brushCollisionMeshAPI = NULL;
	g_brushCollisionUnavailableLogged = false;
	g_brushCollisionStatusLogged = false;
	g_livePoses.clear();
	g_ragdollDefinitionCache.clear();
	g_msgRagdollPose = 0;
	g_msgRagdollImpact = 0;
}
