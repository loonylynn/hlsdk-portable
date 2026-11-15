/*
 * Source-style articulated zombie ragdolls using Jolt as the rigid-body backend.
 *
 * This follows the Source ragdoll data flow:
 *   - one physics element per hitbox-bearing studio bone
 *   - physical parent links skip non-physical intermediary bones
 *   - constrained joints use limited angular motion and friction
 *   - client rendering overrides only simulated bones; other bones inherit
 *     from their simulated parent using the frozen death animation pose
 *   - animation pose derivatives seed the initial linear/angular velocities
 *
 * GoldSrc studio hitboxes are used as the available collision source. Unlike
 * Source models, standard HL1 MDLs do not carry Source VCollide/.phy solids or
 * ragdollconstraint KeyValues, so this is an architecture-faithful conversion,
 * not a byte-for-byte reproduction of Source's Havok collision assets.
 */

#include "extdll.h"
#include "util.h"
#include "eiface.h"
#include "enginecallback.h"
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
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <string>
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
	static constexpr float RAGDOLL_POSE_INTERVAL = 0.05f;
	static constexpr int RAGDOLL_NETWORK_BODIES_PER_CHUNK = 10;
	static constexpr float RAGDOLL_DEGREES_TO_RADIANS = 0.01745329251994329577f;
	static constexpr float RAGDOLL_VELOCITY_LIMIT = 2000.0f;
	static constexpr float RAGDOLL_ANGULAR_VELOCITY_LIMIT = 35.0f;

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

	struct BoneHitboxBounds
	{
		float mins[3];
		float maxs[3];
		bool valid;

		BoneHitboxBounds()
			: valid( false )
		{
			for( int i = 0; i < 3; ++i )
			{
				mins[i] = FLT_MAX;
				maxs[i] = -FLT_MAX;
			}
		}
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
		BodyID bodyID;
		int studioBoneIndex;
		int parentElementIndex;
		Vec3 localShapeCenter;
		Vec3 halfExtents;
		Quat initialRotation;
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
		std::vector<Ref<TwoBodyConstraint> > constraints;
		Ref<GroupFilterTable> groupFilter;

		NPCPhysicsRagdoll()
			: entity( NULL ),
			  serialNumber( 0 ),
			  networkID( 0 ),
			  studioBoneCount( 0 ),
			  bodyCount( 0 ),
			  rootElement( -1 ),
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
	static std::vector<NPCPhysicsLivePose> g_livePoses;
	static int g_nextRagdollID = 1;
	static int g_msgRagdollPose = 0;
	static xash_server_physics_api_s *g_serverPhysicsAPI = NULL;
	static xash_world_collision_mesh_api_s *g_worldCollisionMeshAPI = NULL;
	static BodyID g_worldCollisionBodyID;
	static Ref<Shape> g_worldCollisionShape;
	static bool g_worldCollisionReady = false;

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

	static bool NPCPhysics_RagdollSetupFailed( const char *reason )
	{
		ALERT( at_console, "[NPC physics] Ragdoll setup failed: %s.\n", reason );
		return false;
	}

	static bool NPCPhysics_IsZombie( edict_t *ent )
	{
		return ent && !ent->free &&
			( ent->v.flags & FL_MONSTER ) != 0 &&
			( ent->v.flags & ( FL_CLIENT | FL_KILLME | FL_DORMANT ) ) == 0 &&
			FStrEq( STRING( ent->v.classname ), "monster_zombie" );
	}

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
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie has no loaded studio model" ) : false;

		if( header->ident != IDSTUDIOHEADER || header->version != STUDIO_VERSION )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie studio model has an unsupported header or version" ) : false;

		if( header->length < (int)sizeof( studiohdr_t ) ||
			header->numbones < 1 ||
			header->numbones > MAXSTUDIOBONES ||
			header->boneindex < (int)sizeof( studiohdr_t ) )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie studio bone table is invalid" ) : false;

		const size_t length = static_cast<size_t>( header->length );
		const size_t boneOffset = static_cast<size_t>( header->boneindex );
		const size_t boneBytes = static_cast<size_t>( header->numbones ) * sizeof( mstudiobone_t );
		if( boneOffset > length || boneBytes > length - boneOffset )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie studio bone table exceeds model bounds" ) : false;

		if( header->numhitboxes <= 0 || header->numhitboxes > 1024 )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie model has no usable hitboxes" ) : false;

		if( header->hitboxindex < (int)sizeof( studiohdr_t ) )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie studio hitbox table offset is invalid" ) : false;

		const size_t hitboxOffset = static_cast<size_t>( header->hitboxindex );
		const size_t hitboxBytes = static_cast<size_t>( header->numhitboxes ) * sizeof( mstudiobbox_t );
		if( hitboxOffset > length || hitboxBytes > length - hitboxOffset )
			return reportErrors ? NPCPhysics_RagdollSetupFailed( "zombie studio hitbox table exceeds model bounds" ) : false;

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
		if( !g_npcPhysics.initialized )
			return;

		for( size_t i = 0; i < ragdoll.constraints.size(); ++i )
		{
			if( ragdoll.constraints[i].GetPtr() )
				g_npcPhysics.system.RemoveConstraint( ragdoll.constraints[i].GetPtr() );
		}
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
		ragdoll.groupFilter = Ref<GroupFilterTable>();
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
		if( !g_serverPhysicsAPI || !NPCPhysics_IsZombie( ent ) ||
			ent->v.deadflag != DEAD_NO || !gpGlobals ||
			!g_engfuncs.pfnGetBonePosition )
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

	static void NPCPhysics_GetJointLimits(
		const char *boneName,
		float &twistLimit,
		float &swingLimit,
		float &friction )
	{
		std::string name = boneName ? boneName : "";
		for( size_t i = 0; i < name.size(); ++i )
		{
			if( name[i] >= 'A' && name[i] <= 'Z' )
				name[i] = char( name[i] - 'A' + 'a' );
		}

		// Defaults are deliberately broad. Model-specific constraints can replace
		// these when a matching Source-style ragdoll definition is supplied.
		twistLimit = 1.0f;
		swingLimit = 0.9f;
		friction = 4.0f;

		if( name.find( "elbow" ) != std::string::npos ||
			name.find( "forearm" ) != std::string::npos ||
			name.find( "knee" ) != std::string::npos ||
			name.find( "calf" ) != std::string::npos )
		{
			twistLimit = 0.45f;
			swingLimit = 0.55f;
			friction = 3.0f;
		}
		else if( name.find( "shoulder" ) != std::string::npos ||
			name.find( "upperarm" ) != std::string::npos ||
			name.find( "thigh" ) != std::string::npos ||
			name.find( "hip" ) != std::string::npos )
		{
			twistLimit = 1.25f;
			swingLimit = 1.35f;
			friction = 5.0f;
		}
		else if( name.find( "head" ) != std::string::npos ||
			name.find( "neck" ) != std::string::npos )
		{
			twistLimit = 0.8f;
			swingLimit = 0.7f;
			friction = 2.5f;
		}
		else if( name.find( "spine" ) != std::string::npos ||
			name.find( "chest" ) != std::string::npos ||
			name.find( "pelvis" ) != std::string::npos )
		{
			twistLimit = 0.5f;
			swingLimit = 0.4f;
			friction = 6.0f;
		}
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
			"[NPC physics] Native BSP collision loaded: %u vertices, %u triangles.\n",
			vertexCount,
			indexCount / 3
		);
		return true;
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
			return NPCPhysics_RagdollSetupFailed( "zombie entity is invalid" );
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

		studiohdr_t *header = NULL;
		if( !NPCPhysics_GetStudioHeader( ent, &header, true ) )
			return false;

		mstudiobone_t *studioBones = reinterpret_cast<mstudiobone_t *>(
			reinterpret_cast<byte *>( header ) + header->boneindex
		);
		mstudiobbox_t *hitboxes = reinterpret_cast<mstudiobbox_t *>(
			reinterpret_cast<byte *>( header ) + header->hitboxindex
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
		std::vector<BoneHitboxBounds> bounds( ragdoll.studioBoneCount );
		std::vector<int> boneToElement( ragdoll.studioBoneCount, -1 );

		for( int i = 0; i < ragdoll.studioBoneCount; ++i )
		{
			if( studioBones[i].parent < -1 ||
				studioBones[i].parent >= ragdoll.studioBoneCount ||
				studioBones[i].parent == i )
				return NPCPhysics_RagdollSetupFailed( "zombie studio skeleton contains an invalid parent index" );

			g_engfuncs.pfnGetBonePosition(
				ent,
				i,
				&bonePositions[i][0],
				&boneAngles[i][0]
			);
			boneRotations[i] = NPCPhysics_AnglesToQuaternion( boneAngles[i] );
		}

		// Source's RagdollExtractBoneIndices maps physics solids to named bones.
		// HL1 MDLs lack Source VCollide solids, so each hitbox-bearing studio bone
		// becomes one box-shaped physics element. Multiple hitboxes on the same
		// bone are unioned into one local-space box.
		for( int i = 0; i < header->numhitboxes; ++i )
		{
			const int boneIndex = hitboxes[i].bone;
			if( boneIndex < 0 || boneIndex >= ragdoll.studioBoneCount )
				continue;

			BoneHitboxBounds &boneBounds = bounds[boneIndex];
			for( int axis = 0; axis < 3; ++axis )
			{
				boneBounds.mins[axis] = std::min( boneBounds.mins[axis], hitboxes[i].bbmin[axis] );
				boneBounds.maxs[axis] = std::max( boneBounds.maxs[axis], hitboxes[i].bbmax[axis] );
			}
			boneBounds.valid = true;
		}

		for( int i = 0; i < ragdoll.studioBoneCount; ++i )
		{
			if( !bounds[i].valid )
				continue;

			RagdollBone bone;
			bone.studioBoneIndex = i;
			bone.parentElementIndex = -1;
			bone.bodyID = BodyID();
			bone.initialRotation = boneRotations[i];

			const Vec3 localMin(
				bounds[i].mins[0],
				bounds[i].mins[1],
				bounds[i].mins[2]
			);
			const Vec3 localMax(
				bounds[i].maxs[0],
				bounds[i].maxs[1],
				bounds[i].maxs[2]
			);
			bone.localShapeCenter = ( localMin + localMax ) * 0.5f;
			bone.halfExtents = ( localMax - localMin ) * 0.5f;

			bone.halfExtents = Vec3(
				std::max( 0.75f, bone.halfExtents.GetX() ),
				std::max( 0.75f, bone.halfExtents.GetY() ),
				std::max( 0.75f, bone.halfExtents.GetZ() )
			);

			boneToElement[i] = static_cast<int>( ragdoll.bones.size() );
			ragdoll.bones.push_back( bone );
		}

		ragdoll.bodyCount = static_cast<int>( ragdoll.bones.size() );
		if( ragdoll.bodyCount <= 0 || ragdoll.bodyCount > 128 )
			return NPCPhysics_RagdollSetupFailed( "zombie model produced no valid hitbox bodies or too many bodies" );

		// Match Source's hierarchy collapse: a physical element is connected to
		// the nearest ancestor that also owns a physical element, not blindly to
		// the immediately preceding studio bone.
		for( int i = 0; i < ragdoll.bodyCount; ++i )
		{
			RagdollBone &bone = ragdoll.bones[i];
			int parentBone = studioBones[bone.studioBoneIndex].parent;

			while( parentBone >= 0 && parentBone < ragdoll.studioBoneCount )
			{
				if( boneToElement[parentBone] >= 0 )
				{
					bone.parentElementIndex = boneToElement[parentBone];
					break;
				}
				parentBone = studioBones[parentBone].parent;
			}

			if( bone.parentElementIndex < 0 && ragdoll.rootElement < 0 )
				ragdoll.rootElement = i;
		}

		if( ragdoll.rootElement < 0 )
			return NPCPhysics_RagdollSetupFailed( "no hitbox body could be selected as the ragdoll root" );

		// Keep the edict origin near the moving physics assembly so Xash's
		// entity culling/PVS bounds follow the corpse. The custom bone matrices
		// remain absolute world transforms and do not depend on this offset.
		const int rootStudioBone = ragdoll.bones[ragdoll.rootElement].studioBoneIndex;
		ragdoll.entityOriginOffset = ent->v.origin - bonePositions[rootStudioBone];

		float totalWeight = 0.0f;
		std::vector<float> massWeights( ragdoll.bodyCount, 0.0f );
		for( int i = 0; i < ragdoll.bodyCount; ++i )
		{
			const Vec3 &half = ragdoll.bones[i].halfExtents;
			const float weight = std::max(
				1.0f,
				8.0f * half.GetX() * half.GetY() * half.GetZ()
			);
			massWeights[i] = weight;
			totalWeight += weight;
		}

		float totalMass = NPCPhysics_GetCvar( "sv_npc_vphysics_mass", 80.0f );
		if( totalMass <= 0.0f )
			totalMass = 80.0f;
		totalMass = std::max( 10.0f, std::min( totalMass, 300.0f ) );

		ragdoll.groupFilter = new GroupFilterTable( static_cast<uint>( ragdoll.bodyCount ) );
		for( int i = 0; i < ragdoll.bodyCount; ++i )
		{
			const int parent = ragdoll.bones[i].parentElementIndex;
			if( parent >= 0 && parent < ragdoll.bodyCount )
			{
				// Source's default ragdoll collision set lets non-adjacent parts
				// collide and suppresses collisions for constrained parent pairs.
				ragdoll.groupFilter->DisableCollision(
					static_cast<CollisionGroup::SubGroupID>( i ),
					static_cast<CollisionGroup::SubGroupID>( parent )
				);
			}
		}

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

		ragdoll.bones.shrink_to_fit();

		BodyInterface &bodyInterface = g_npcPhysics.system.GetBodyInterface();
		for( int i = 0; i < ragdoll.bodyCount; ++i )
		{
			RagdollBone &bone = ragdoll.bones[i];
			const int boneIndex = bone.studioBoneIndex;
			const Quat rotation = boneRotations[boneIndex];
			const Vec3 boneOrigin(
				bonePositions[boneIndex][0],
				bonePositions[boneIndex][1],
				bonePositions[boneIndex][2]
			);
			const Vec3 bodyCenter = boneOrigin + rotation * bone.localShapeCenter;
			const float bodyMass = totalMass * massWeights[i] / totalWeight;

			BodyCreationSettings settings(
				new BoxShape( bone.halfExtents, 0.05f ),
				RVec3( bodyCenter ),
				rotation,
				EMotionType::Dynamic,
				RAGDOLL_OBJECT_LAYER
			);
			settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
			settings.mMassPropertiesOverride.mMass = std::max( 0.25f, bodyMass );
			settings.mCollisionGroup = CollisionGroup(
				ragdoll.groupFilter.GetPtr(),
				static_cast<CollisionGroup::GroupID>( ragdoll.networkID ),
				static_cast<CollisionGroup::SubGroupID>( i )
			);
			settings.mFriction = 0.8f;
			settings.mRestitution = 0.0f;
			settings.mLinearDamping = 0.025f;
			settings.mAngularDamping = 0.06f;
			settings.mMaxLinearVelocity = RAGDOLL_VELOCITY_LIMIT;
			settings.mMaxAngularVelocity = RAGDOLL_ANGULAR_VELOCITY_LIMIT;
			settings.mMotionQuality = EMotionQuality::LinearCast;
			settings.mEnhancedInternalEdgeRemoval = true;

			const BodyID bodyID = bodyInterface.CreateAndAddBody( settings, EActivation::Activate );
			if( bodyID.IsInvalid() )
			{
				NPCPhysics_DestroyRagdoll( ragdoll );
				ALERT( at_console, "[NPC physics] Ragdoll setup failed: Jolt could not create body for studio bone %d.\n", boneIndex );
				return false;
			}

			bone.bodyID = bodyID;

			Vec3 linearVelocity(
				ent->v.velocity[0],
				ent->v.velocity[1],
				ent->v.velocity[2]
			);
			Vec3 angularVelocity(
				ent->v.avelocity[0] * RAGDOLL_DEGREES_TO_RADIANS,
				ent->v.avelocity[1] * RAGDOLL_DEGREES_TO_RADIANS,
				ent->v.avelocity[2] * RAGDOLL_DEGREES_TO_RADIANS
			);

			// Source's RagdollApplyAnimationAsVelocity seeds each physics object
			// from the previous/current animation bone transforms. The GoldSrc
			// hook stores the latest live pose while the zombie is still alive.
			if( livePose && boneIndex < (int)livePose->positions.size() &&
				boneIndex < (int)livePose->rotations.size() )
			{
				const Vec3 previousCenter =
					livePose->positions[boneIndex] +
					livePose->rotations[boneIndex] * bone.localShapeCenter;

				// The animation derivative is an absolute world-space velocity,
				// as in Source's CalcBoneDerivatives. Add only the bone-local part
				// to the entity's current velocity; otherwise locomotion gets
				// counted twice. This also preserves a death impulse already put
				// into pev->velocity by GoldSrc.
				const Vec3 entityLinearVelocity(
					( ent->v.origin[0] - livePose->entityOrigin[0] ) / animationDeltaTime,
					( ent->v.origin[1] - livePose->entityOrigin[1] ) / animationDeltaTime,
					( ent->v.origin[2] - livePose->entityOrigin[2] ) / animationDeltaTime
				);
				linearVelocity +=
					( bodyCenter - previousCenter ) / animationDeltaTime -
					entityLinearVelocity;

				Quat deltaRotation = rotation * livePose->rotations[boneIndex].Conjugated();
				deltaRotation = deltaRotation.Normalized();
				if( deltaRotation.GetW() < 0.0f )
				{
					deltaRotation = Quat(
						-deltaRotation.GetX(),
						-deltaRotation.GetY(),
						-deltaRotation.GetZ(),
						-deltaRotation.GetW()
					);
				}

				Quat entityDeltaRotation =
					NPCPhysics_AnglesToQuaternion( ent->v.angles ) *
					NPCPhysics_AnglesToQuaternion( livePose->entityAngles ).Conjugated();
				entityDeltaRotation = entityDeltaRotation.Normalized();
				if( entityDeltaRotation.GetW() < 0.0f )
				{
					entityDeltaRotation = Quat(
						-entityDeltaRotation.GetX(),
						-entityDeltaRotation.GetY(),
						-entityDeltaRotation.GetZ(),
						-entityDeltaRotation.GetW()
					);
				}
				angularVelocity +=
					deltaRotation.GetAngularVelocity( animationDeltaTime ) -
					entityDeltaRotation.GetAngularVelocity( animationDeltaTime );
			}

			linearVelocity = NPCPhysics_ClampVector( linearVelocity, RAGDOLL_VELOCITY_LIMIT );
			angularVelocity = NPCPhysics_ClampVector( angularVelocity, RAGDOLL_ANGULAR_VELOCITY_LIMIT );
			bodyInterface.SetLinearVelocity( bodyID, linearVelocity );
			bodyInterface.SetAngularVelocity( bodyID, angularVelocity );
		}

		// Create articulated 6-DOF joints. Translation is locked at the joint;
		// angular motion is limited and damped instead of allowing the free
		// pivot swing produced by the old point constraints.
		for( int i = 0; i < ragdoll.bodyCount; ++i )
		{
			const int parentIndex = ragdoll.bones[i].parentElementIndex;
			if( parentIndex < 0 || parentIndex >= ragdoll.bodyCount )
				continue;

			RagdollBone &childBone = ragdoll.bones[i];
			RagdollBone &parentBone = ragdoll.bones[parentIndex];
			const BodyID bodyIDs[2] = { parentBone.bodyID, childBone.bodyID };

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
				ALERT( at_console, "[NPC physics] Ragdoll setup failed: could not lock parent/child bodies for joint %d.\n", i );
				return false;
			}

			const int childStudioBone = childBone.studioBoneIndex;
			const Vec3 joint(
				bonePositions[childStudioBone][0],
				bonePositions[childStudioBone][1],
				bonePositions[childStudioBone][2]
			);

			SixDOFConstraintSettings constraintSettings;
			constraintSettings.mSpace = EConstraintSpace::WorldSpace;
			constraintSettings.mPosition1 = RVec3( joint );
			constraintSettings.mPosition2 = RVec3( joint );

			// The two constraint frames are aligned in world space at creation,
			// preserving the animation's initial relative bone orientation.
			const Vec3 axisX = parentBone.initialRotation.RotateAxisX().Normalized();
			const Vec3 axisY = parentBone.initialRotation.RotateAxisY().Normalized();
			constraintSettings.mAxisX1 = axisX;
			constraintSettings.mAxisY1 = axisY;
			constraintSettings.mAxisX2 = axisX;
			constraintSettings.mAxisY2 = axisY;

			constraintSettings.MakeFixedAxis( SixDOFConstraintSettings::TranslationX );
			constraintSettings.MakeFixedAxis( SixDOFConstraintSettings::TranslationY );
			constraintSettings.MakeFixedAxis( SixDOFConstraintSettings::TranslationZ );

			float twistLimit, swingLimit, friction;
			NPCPhysics_GetJointLimits(
				studioBones[childStudioBone].name,
				twistLimit,
				swingLimit,
				friction
			);
			constraintSettings.mSwingType = ESwingType::Cone;
			constraintSettings.SetLimitedAxis(
				SixDOFConstraintSettings::RotationX,
				-twistLimit,
				twistLimit
			);
			// Jolt's cone swing convention takes a non-negative half-angle;
			// the cone is symmetric about zero internally.
			constraintSettings.SetLimitedAxis(
				SixDOFConstraintSettings::RotationY,
				0.0f,
				swingLimit
			);
			constraintSettings.SetLimitedAxis(
				SixDOFConstraintSettings::RotationZ,
				0.0f,
				swingLimit
			);
			constraintSettings.mMaxFriction[SixDOFConstraintSettings::RotationX] = friction;
			constraintSettings.mMaxFriction[SixDOFConstraintSettings::RotationY] = friction;
			constraintSettings.mMaxFriction[SixDOFConstraintSettings::RotationZ] = friction;

			Ref<TwoBodyConstraint> constraint = constraintSettings.Create( *parentBody, *childBody );
			if( constraint.GetPtr() == NULL )
			{
				bodyLock.ReleaseLocks();
				NPCPhysics_DestroyRagdoll( ragdoll );
				ALERT( at_console, "[NPC physics] Ragdoll setup failed: Jolt could not create joint for studio bone %d.\n", childStudioBone );
				return false;
			}

			g_npcPhysics.system.AddConstraint( constraint.GetPtr() );
			ragdoll.constraints.push_back( constraint );
			bodyLock.ReleaseLocks();
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


	static short NPCPhysics_QuantizeQuaternion( float value )
	{
		value = std::max( -1.0f, std::min( 1.0f, value ) );
		return static_cast<short>( value * 32767.0f );
	}

	static void NPCPhysics_SendRagdollPose(
		NPCPhysicsRagdoll &ragdoll,
		BodyInterface &bodyInterface )
	{
		if( g_msgRagdollPose <= 0 || !ragdoll.entity || ragdoll.entity->free )
			return;

		const int entityIndex = ENTINDEX( ragdoll.entity );
		for( int start = 0; start < ragdoll.bodyCount; start += RAGDOLL_NETWORK_BODIES_PER_CHUNK )
		{
			const int count = std::min(
				RAGDOLL_NETWORK_BODIES_PER_CHUNK,
				ragdoll.bodyCount - start
			);

			// The carrier edict follows the simulated root. Anchor snapshots to
			// that live origin so clients entering the new PVS keep receiving poses.
			MESSAGE_BEGIN( MSG_PVS, g_msgRagdollPose, ragdoll.entity->v.origin );
				WRITE_SHORT( entityIndex );
				WRITE_SHORT( ragdoll.networkID );
				WRITE_BYTE( ragdoll.studioBoneCount );
				WRITE_BYTE( ragdoll.bodyCount );
				WRITE_BYTE( start );
				WRITE_BYTE( count );

				for( int i = 0; i < count; ++i )
				{
					const RagdollBone &bone = ragdoll.bones[start + i];
					const RVec3 bodyPosition = bodyInterface.GetPosition( bone.bodyID );
					const Quat rotation = bodyInterface.GetRotation( bone.bodyID ).Normalized();
					const Vec3 bonePosition =
						Vec3(
							float( bodyPosition.GetX() ),
							float( bodyPosition.GetY() ),
							float( bodyPosition.GetZ() )
						) - rotation * bone.localShapeCenter;

					WRITE_BYTE( bone.studioBoneIndex );
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

			// Move the carrier edict with the simulated root element. Besides
			// keeping the corpse's network origin useful, this keeps renderer
			// culling and the entity's PVS bounds near a ragdoll that has fallen.
			if( ragdoll.rootElement >= 0 &&
				ragdoll.rootElement < ragdoll.bodyCount &&
				g_engfuncs.pfnSetOrigin )
			{
				const RagdollBone &rootBone = ragdoll.bones[ragdoll.rootElement];
				const RVec3 rootBodyPosition = bodyInterface.GetPosition( rootBone.bodyID );
				const Quat rootRotation = bodyInterface.GetRotation( rootBone.bodyID );
				const Vec3 rootBoneOrigin =
					Vec3(
						float( rootBodyPosition.GetX() ),
						float( rootBodyPosition.GetY() ),
						float( rootBodyPosition.GetZ() )
					) - rootRotation * rootBone.localShapeCenter;

				const float entityOrigin[3] = {
					rootBoneOrigin.GetX() + ragdoll.entityOriginOffset[0],
					rootBoneOrigin.GetY() + ragdoll.entityOriginOffset[1],
					rootBoneOrigin.GetZ() + ragdoll.entityOriginOffset[2]
				};
				g_engfuncs.pfnSetOrigin( ragdoll.entity, entityOrigin );
			}

			if( gpGlobals->time >= ragdoll.nextPoseTime )
			{
				NPCPhysics_SendRagdollPose( ragdoll, bodyInterface );
				ragdoll.nextPoseTime = gpGlobals->time + RAGDOLL_POSE_INTERVAL;
			}
		}
	}
}

int NPCPhysics_TryCreateRagdoll( edict_t *ent )
{
	if( !NPCPhysics_Enabled() || !NPCPhysics_IsZombie( ent ) )
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

	g_serverPhysicsAPI = reinterpret_cast<xash_server_physics_api_s *>( serverAPI );
	g_worldCollisionMeshAPI = NULL;
	if( g_serverPhysicsAPI->pfnGetNativeObject )
	{
		g_worldCollisionMeshAPI = reinterpret_cast<xash_world_collision_mesh_api_s *>(
			g_serverPhysicsAPI->pfnGetNativeObject( "xash_world_collision_mesh_v1" )
		);
		if( g_worldCollisionMeshAPI &&
			( g_worldCollisionMeshAPI->version != XASH_WORLD_COLLISION_MESH_API_VERSION ||
			  !g_worldCollisionMeshAPI->GetMeshInfo || !g_worldCollisionMeshAPI->GetMeshData ) )
			g_worldCollisionMeshAPI = NULL;
	}
	if( gpGlobals )
	{
		const int maxEntities = std::max( 2048, gpGlobals->maxEntities + 1 );
		g_livePoses.resize( maxEntities );
	}

	if( !g_msgRagdollPose && g_engfuncs.pfnRegUserMsg )
		g_msgRagdollPose = g_engfuncs.pfnRegUserMsg( "RagdollPose", -1 );

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
	NPCPhysics_DestroyWorldCollider();
	for( size_t i = 0; i < g_livePoses.size(); ++i )
	{
		g_livePoses[i].serialNumber = -1;
		g_livePoses[i].boneCount = 0;
		g_livePoses[i].sampleTime = 0.0f;
		g_livePoses[i].positions.clear();
		g_livePoses[i].rotations.clear();
	}
	g_npcPhysics.fixedAccumulator = 0.0f;
}

void NPCPhysics_Shutdown( void )
{
	NPCPhysics_DestroyAllRagdolls();
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
	g_livePoses.clear();
	g_msgRagdollPose = 0;
}
