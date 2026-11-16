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
#include "basemonster.h"


#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/SkeletonMapper.h>

#include <algorithm>
#include <cmath>
#include <cfloat>
#include <iomanip>
#include <mutex>
#include <memory>
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
	static constexpr ObjectLayer WORLD_STATIC_OBJECT_LAYER = 1;
	// Shared by moving BSP brushes and the living-actor hull proxies.
	static constexpr ObjectLayer BRUSH_KINEMATIC_OBJECT_LAYER = 2;
	static constexpr uint32_t RAGDOLL_MAX_BODIES = 4096;
	static constexpr uint32_t RAGDOLL_MAX_BODY_PAIRS = 8192;
	static constexpr uint32_t RAGDOLL_MAX_CONTACT_CONSTRAINTS = 8192;
	// The bridge keeps geometry, gravity and velocities in GoldSrc map units
	// (1 unit = 1 inch). Jolt's absolute PhysicsSettings defaults are in meters,
	// so convert those distances/speeds instead of using SI-sized tolerances
	// against inch-scaled world geometry.
	static constexpr float RAGDOLL_SOURCE_UNITS_PER_METER = 39.3700787f;
	static constexpr float RAGDOLL_FIXED_STEP = 1.0f / 120.0f;
	static constexpr int RAGDOLL_MAX_STEPS_PER_FRAME = 8;
	// Articulated ragdolls need extra position iterations when multiple limbs
	// are constrained against static BSP surfaces in tight corners.
	static constexpr uint32_t RAGDOLL_CONSTRAINT_POSITION_STEPS = 8;
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

	// Sampled on the main thread before each Jolt frame; declared before the
	// contact listener that reads it during Jolt callbacks.
	static bool g_npcPhysicsContactDebugEnabled = false;

	class RagdollBroadPhaseLayers final : public BroadPhaseLayerInterface
	{
	public:
		uint GetNumBroadPhaseLayers() const override { return 1; }
		BroadPhaseLayer GetBroadPhaseLayer( ObjectLayer ) const override { return BroadPhaseLayer( 0 ); }
	};

	class RagdollObjectLayerFilter final : public ObjectLayerPairFilter
	{
	public:
		bool ShouldCollide( ObjectLayer layer1, ObjectLayer layer2 ) const override
		{
			// The world BSP and inline brush models are triangle meshes. Jolt
			// does not support mesh-vs-mesh contacts when one mesh is kinematic.
			// Living actors are represented by kinematic box hulls on the same
			// layer. Only convex articulated ragdoll bodies need contacts with
			// environment geometry or actor hulls.
			return layer1 == RAGDOLL_OBJECT_LAYER || layer2 == RAGDOLL_OBJECT_LAYER;
		}
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
		bool isKinematic;
		bool wasMoving;
		bool pendingStaticTransition;

		NPCPhysicsBrushCollider()
			: entityIndex( -1 ),
			  modelIndex( 0 ),
			  bodyID(),
			  position( 0.0f, 0.0f, 0.0f ),
			  rotation( Quat::sIdentity() ),
			  isKinematic( false ),
			  wasMoving( false ),
			  pendingStaticTransition( false )
		{
		}
	};

	struct NPCPhysicsActorCollider
	{
		int entityIndex;
		int serialNumber;
		BodyID bodyID;
		Vec3 position;
		Vec3 centerOffset;
		Vec3 halfExtents;
		bool wasMoving;

		NPCPhysicsActorCollider()
			: entityIndex( -1 ),
			  serialNumber( 0 ),
			  bodyID(),
			  position( 0.0f, 0.0f, 0.0f ),
			  centerOffset( 0.0f, 0.0f, 0.0f ),
			  halfExtents( 0.0f, 0.0f, 0.0f ),
			  wasMoving( false )
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
		float nextCollisionDamageTime;
		std::vector<float> nextContactDebugTimes;
		float nextPlayerPushDebugTime;
		bool firstPoseSendReported;
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
			  nextCollisionDamageTime( 0.0f ),
			  nextPlayerPushDebugTime( 0.0f ),
			  firstPoseSendReported( false ),
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

	struct NPCPhysicsPendingCollisionDamage
	{
		uint64_t ragdollID;
		BodyID ragdollBodyID;
		int otherEntityIndex;
		float velocityLoss;
		Vec3 contactPoint;

		NPCPhysicsPendingCollisionDamage()
			: ragdollID( 0 ),
			  ragdollBodyID(),
			  otherEntityIndex( 0 ),
			  velocityLoss( 0.0f ),
			  contactPoint( 0.0f, 0.0f, 0.0f )
		{
		}
	};

	struct NPCPhysicsPendingContactDiagnostic
	{
		uint64_t ragdollID;
		BodyID ragdollBodyID;
		int otherLayer;
		int otherEntityIndex;
		float penetrationDepth;
		float closingSpeed;
		unsigned int sampleCount;
		bool persisted;
		Vec3 contactPoint;
		Vec3 normal;
		Vec3 ragdollCenterOfMass;
		Quat ragdollRotation;
		Vec3 ragdollBoundsMin;
		Vec3 ragdollBoundsMax;
		int ragdollShapeSubType;
		Vec3 otherCenterOfMass;
		Quat otherRotation;
		Vec3 otherBoundsMin;
		Vec3 otherBoundsMax;
		int otherShapeSubType;
		uint32_t ragdollSubShapeID;
		uint32_t otherSubShapeID;

		NPCPhysicsPendingContactDiagnostic()
			: ragdollID( 0 ),
			  ragdollBodyID(),
			  otherLayer( 0 ),
			  otherEntityIndex( 0 ),
			  penetrationDepth( 0.0f ),
			  closingSpeed( 0.0f ),
			  sampleCount( 0 ),
			  persisted( false ),
			  contactPoint( 0.0f, 0.0f, 0.0f ),
			  normal( 0.0f, 0.0f, 0.0f ),
			  ragdollCenterOfMass( 0.0f, 0.0f, 0.0f ),
			  ragdollRotation( Quat::sIdentity() ),
			  ragdollBoundsMin( 0.0f, 0.0f, 0.0f ),
			  ragdollBoundsMax( 0.0f, 0.0f, 0.0f ),
			  ragdollShapeSubType( -1 ),
			  otherCenterOfMass( 0.0f, 0.0f, 0.0f ),
			  otherRotation( Quat::sIdentity() ),
			  otherBoundsMin( 0.0f, 0.0f, 0.0f ),
			  otherBoundsMax( 0.0f, 0.0f, 0.0f ),
			  otherShapeSubType( -1 ),
			  ragdollSubShapeID( 0xFFFFFFFFu ),
			  otherSubShapeID( 0xFFFFFFFFu )
		{
		}
	};

	static const char *NPCPhysics_ShapeSubTypeName( int shapeSubType )
	{
		switch( static_cast<EShapeSubType>( shapeSubType ) )
		{
		case EShapeSubType::Sphere: return "sphere";
		case EShapeSubType::Box: return "box";
		case EShapeSubType::Triangle: return "triangle";
		case EShapeSubType::Capsule: return "capsule";
		case EShapeSubType::TaperedCapsule: return "tapered-capsule";
		case EShapeSubType::Cylinder: return "cylinder";
		case EShapeSubType::ConvexHull: return "convex-hull";
		case EShapeSubType::StaticCompound: return "static-compound";
		case EShapeSubType::MutableCompound: return "mutable-compound";
		case EShapeSubType::RotatedTranslated: return "rotated-translated";
		case EShapeSubType::Scaled: return "scaled";
		case EShapeSubType::OffsetCenterOfMass: return "offset-center-of-mass";
		case EShapeSubType::Mesh: return "mesh";
		case EShapeSubType::HeightField: return "heightfield";
		case EShapeSubType::SoftBody: return "soft-body";
		default: return "other";
		}
	}

	static void NPCPhysics_CaptureContactShapeData(
		const Body &body, Vec3 &centerOfMass, Quat &rotation,
		Vec3 &boundsMin, Vec3 &boundsMax, int &shapeSubType )
	{
		const RVec3 position = body.GetCenterOfMassPosition();
		centerOfMass = Vec3( float(position.GetX()), float(position.GetY()), float(position.GetZ()) );
		rotation = body.GetRotation();
		const Shape *shape = body.GetShape();
		if( !shape )
		{
			boundsMin = Vec3::sZero();
			boundsMax = Vec3::sZero();
			shapeSubType = -1;
			return;
		}
		shapeSubType = static_cast<int>( shape->GetSubType() );
		// Jolt local bounds are expressed relative to the shape's center of mass.
		const AABox bounds = shape->GetLocalBounds();
		boundsMin = bounds.mMin;
		boundsMax = bounds.mMax;
	}

	struct NPCPhysicsPendingPlayerPush
	{
		uint64_t ragdollID;
		BodyID ragdollBodyID;
		int playerEntityIndex;
		Vec3 pushDirection;
		Vec3 contactPoint;
		Vec3 ragdollPointVelocityBefore;
		Vec3 playerPointVelocityBefore;

		NPCPhysicsPendingPlayerPush()
			: ragdollID( 0 ),
			  ragdollBodyID(),
			  playerEntityIndex( 0 ),
			  pushDirection( 0.0f, 0.0f, 0.0f ),
			  contactPoint( 0.0f, 0.0f, 0.0f ),
			  ragdollPointVelocityBefore( 0.0f, 0.0f, 0.0f ),
			  playerPointVelocityBefore( 0.0f, 0.0f, 0.0f )
		{
		}
	};

	class NPCPhysicsContactListener final : public ContactListener
	{
	public:
		void OnContactAdded(
			const Body &body1,
			const Body &body2,
			const ContactManifold &manifold,
			ContactSettings &settings ) override
		{
			const ObjectLayer layer1 = body1.GetObjectLayer();
			const ObjectLayer layer2 = body2.GetObjectLayer();
			const bool body1IsRagdoll = layer1 == RAGDOLL_OBJECT_LAYER;
			const bool body2IsRagdoll = layer2 == RAGDOLL_OBJECT_LAYER;

			// This pass models Source's ragdoll-vs-world/brush/actor impact path.
			// Ragdoll-vs-ragdoll damage is deliberately left out for now.
			if( body1IsRagdoll == body2IsRagdoll )
				return;

			const Body &ragdollBody = body1IsRagdoll ? body1 : body2;
			const Body &otherBody = body1IsRagdoll ? body2 : body1;
			const ObjectLayer otherLayer = body1IsRagdoll ? layer2 : layer1;
			if( otherLayer != WORLD_STATIC_OBJECT_LAYER &&
				otherLayer != BRUSH_KINEMATIC_OBJECT_LAYER )
				return;
			if( ragdollBody.GetMotionType() != EMotionType::Dynamic ||
				manifold.mRelativeContactPointsOn1.size() == 0 )
				return;

			const uint64_t ragdollID = ragdollBody.GetUserData();
			if( ragdollID == 0 )
				return;

			QueuePlayerPushLimit( body1, body2, manifold );
			QueueContactDiagnostic( body1, body2, manifold, false );

			const RVec3 contact = body1IsRagdoll
				? manifold.GetWorldSpaceContactPointOn1( 0 )
				: manifold.GetWorldSpaceContactPointOn2( 0 );
			const Vec3 normal = manifold.mWorldSpaceNormal;
			const Vec3 relativeVelocityBefore =
				body2.GetPointVelocity( contact ) - body1.GetPointVelocity( contact );
			const float closingSpeedBefore =
				(std::max)( 0.0f, -relativeVelocityBefore.Dot( normal ) );

			// Collision damage is reserved for actual closing impacts with a
			// usable manifold. Diagnostic contacts above also include slow or
			// resting contacts, which are useful when investigating wall clips.
			if( manifold.mPenetrationDepth < 0.0f || closingSpeedBefore < 1.0f )
				return;

			CollisionEstimationResult estimate;
			EstimateCollisionResponse(
				body1, body2, manifold, estimate,
				settings.mCombinedFriction, settings.mCombinedRestitution,
				1.0f, 2 );

			const RVec3 center1 = body1.GetCenterOfMassPosition();
			const RVec3 center2 = body2.GetCenterOfMassPosition();
			const Vec3 offset1(
				float( contact.GetX() - center1.GetX() ),
				float( contact.GetY() - center1.GetY() ),
				float( contact.GetZ() - center1.GetZ() ) );
			const Vec3 offset2(
				float( contact.GetX() - center2.GetX() ),
				float( contact.GetY() - center2.GetY() ),
				float( contact.GetZ() - center2.GetZ() ) );

			const Vec3 velocity1After = estimate.mLinearVelocity1 +
				estimate.mAngularVelocity1.Cross( offset1 );
			const Vec3 velocity2After = estimate.mLinearVelocity2 +
				estimate.mAngularVelocity2.Cross( offset2 );
			const Vec3 relativeVelocityAfter = velocity2After - velocity1After;
			const float closingSpeedAfter =
				(std::max)( 0.0f, -relativeVelocityAfter.Dot( normal ) );
			const float velocityLoss = closingSpeedBefore - closingSpeedAfter;
			if( !std::isfinite( velocityLoss ) || velocityLoss < 1.0f )
				return;

			NPCPhysicsPendingCollisionDamage event;
			event.ragdollID = ragdollID;
			event.ragdollBodyID = ragdollBody.GetID();
			event.otherEntityIndex = otherLayer == BRUSH_KINEMATIC_OBJECT_LAYER
				? static_cast<int>( otherBody.GetUserData() ) : 0;
			event.velocityLoss = velocityLoss;
			event.contactPoint = Vec3(
				float( contact.GetX() ),
				float( contact.GetY() ),
				float( contact.GetZ() ) );

			std::lock_guard<std::mutex> lock( m_mutex );
			m_pending.push_back( event );
		}

		void OnContactPersisted(
			const Body &body1,
			const Body &body2,
			const ContactManifold &manifold,
			ContactSettings & ) override
		{
			QueuePlayerPushLimit( body1, body2, manifold );
			QueueContactDiagnostic( body1, body2, manifold, true );
		}

		std::vector<NPCPhysicsPendingPlayerPush> DrainPlayerPushLimits()
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			std::vector<NPCPhysicsPendingPlayerPush> result;
			result.swap( m_playerPushLimits );
			return result;
		}

		std::vector<NPCPhysicsPendingCollisionDamage> Drain()
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			std::vector<NPCPhysicsPendingCollisionDamage> result;
			result.swap( m_pending );
			return result;
		}

		std::vector<NPCPhysicsPendingContactDiagnostic> DrainDiagnostics()
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			std::vector<NPCPhysicsPendingContactDiagnostic> result;
			result.swap( m_contactDiagnostics );
			return result;
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			m_pending.clear();
			m_contactDiagnostics.clear();
			m_playerPushLimits.clear();
		}

	private:
		static bool IsContactDiagnosticMoreSevere(
			const NPCPhysicsPendingContactDiagnostic &a,
			const NPCPhysicsPendingContactDiagnostic &b )
		{
			if( a.penetrationDepth != b.penetrationDepth )
				return a.penetrationDepth > b.penetrationDepth;
			return a.closingSpeed > b.closingSpeed;
		}

		void QueueContactDiagnostic(
			const Body &body1,
			const Body &body2,
			const ContactManifold &manifold,
			bool persisted )
		{
			const ObjectLayer layer1 = body1.GetObjectLayer();
			const ObjectLayer layer2 = body2.GetObjectLayer();
			const bool body1IsRagdoll = layer1 == RAGDOLL_OBJECT_LAYER;
			const bool body2IsRagdoll = layer2 == RAGDOLL_OBJECT_LAYER;
			if( body1IsRagdoll == body2IsRagdoll )
				return;

			const Body &ragdollBody = body1IsRagdoll ? body1 : body2;
			const Body &otherBody = body1IsRagdoll ? body2 : body1;
			const ObjectLayer otherLayer = body1IsRagdoll ? layer2 : layer1;
			if( ( otherLayer != WORLD_STATIC_OBJECT_LAYER &&
				  otherLayer != BRUSH_KINEMATIC_OBJECT_LAYER ) ||
				ragdollBody.GetMotionType() != EMotionType::Dynamic ||
				manifold.mRelativeContactPointsOn1.size() == 0 )
				return;

			const uint64_t ragdollID = ragdollBody.GetUserData();
			if( ragdollID == 0 || !g_npcPhysicsContactDebugEnabled )
				return;

			const RVec3 contact = body1IsRagdoll
				? manifold.GetWorldSpaceContactPointOn1( 0 )
				: manifold.GetWorldSpaceContactPointOn2( 0 );
			const Vec3 manifoldNormal = manifold.mWorldSpaceNormal;
			const Vec3 relativeVelocity =
				body2.GetPointVelocity( contact ) - body1.GetPointVelocity( contact );
			const float closingSpeed =
				(std::max)( 0.0f, -relativeVelocity.Dot( manifoldNormal ) );
			if( !std::isfinite( manifold.mPenetrationDepth ) ||
				!std::isfinite( closingSpeed ) )
				return;

			NPCPhysicsPendingContactDiagnostic diagnostic;
			diagnostic.ragdollID = ragdollID;
			diagnostic.ragdollBodyID = ragdollBody.GetID();
			diagnostic.otherLayer = static_cast<int>( otherLayer );
			diagnostic.otherEntityIndex = otherLayer == BRUSH_KINEMATIC_OBJECT_LAYER
				? static_cast<int>( otherBody.GetUserData() ) : 0;
			diagnostic.penetrationDepth = manifold.mPenetrationDepth;
			diagnostic.closingSpeed = closingSpeed;
			diagnostic.sampleCount = 1;
			diagnostic.persisted = persisted;
			diagnostic.contactPoint = Vec3(
				float( contact.GetX() ),
				float( contact.GetY() ),
				float( contact.GetZ() ) );
			// Jolt's manifold normal points in the direction that moves body 2
			// out of the collision. Orient this diagnostic normal toward the
			// ragdoll so its meaning is stable regardless of body ordering.
			diagnostic.normal = body1IsRagdoll ? -manifoldNormal : manifoldNormal;
			diagnostic.ragdollSubShapeID = body1IsRagdoll
				? manifold.mSubShapeID1.GetValue() : manifold.mSubShapeID2.GetValue();
			diagnostic.otherSubShapeID = body1IsRagdoll
				? manifold.mSubShapeID2.GetValue() : manifold.mSubShapeID1.GetValue();

			// Source's player shadow update checks the penetrating state and
			// inspects intersecting entities before reconciling movement.
			// Capture both Jolt bodies so we can diagnose overlap without
			// changing collision response.
			NPCPhysics_CaptureContactShapeData(
				ragdollBody, diagnostic.ragdollCenterOfMass, diagnostic.ragdollRotation,
				diagnostic.ragdollBoundsMin, diagnostic.ragdollBoundsMax,
				diagnostic.ragdollShapeSubType );
			NPCPhysics_CaptureContactShapeData(
				otherBody, diagnostic.otherCenterOfMass, diagnostic.otherRotation,
				diagnostic.otherBoundsMin, diagnostic.otherBoundsMax,
				diagnostic.otherShapeSubType );

			// Retain the deepest contact per ragdoll body, opposing entity/layer
			// and callback stage; aggregating by ragdoll alone hid individual limbs.
			const size_t maxContactDiagnostics = 256;
			std::lock_guard<std::mutex> lock( m_mutex );
			for( size_t i = 0; i < m_contactDiagnostics.size(); ++i )
			{
				NPCPhysicsPendingContactDiagnostic &existing = m_contactDiagnostics[i];
				if( existing.ragdollID != diagnostic.ragdollID ||
					existing.ragdollBodyID != diagnostic.ragdollBodyID ||
					existing.otherLayer != diagnostic.otherLayer ||
					existing.otherEntityIndex != diagnostic.otherEntityIndex ||
					existing.persisted != diagnostic.persisted )
					continue;

				if( existing.sampleCount < 0xFFFFFFFFu )
					++existing.sampleCount;
				if( IsContactDiagnosticMoreSevere( diagnostic, existing ) )
				{
					diagnostic.sampleCount = existing.sampleCount;
					existing = diagnostic;
				}
				return;
			}

			if( m_contactDiagnostics.size() < maxContactDiagnostics )
			{
				m_contactDiagnostics.push_back( diagnostic );
				return;
			}

			size_t leastSevereIndex = 0;
			for( size_t i = 1; i < m_contactDiagnostics.size(); ++i )
			{
				if( IsContactDiagnosticMoreSevere(
						m_contactDiagnostics[leastSevereIndex],
						m_contactDiagnostics[i] ) )
					leastSevereIndex = i;
			}
			if( IsContactDiagnosticMoreSevere(
					diagnostic, m_contactDiagnostics[leastSevereIndex] ) )
				m_contactDiagnostics[leastSevereIndex] = diagnostic;
		}

		void QueuePlayerPushLimit(
			const Body &body1,
			const Body &body2,
			const ContactManifold &manifold )
		{
			const ObjectLayer layer1 = body1.GetObjectLayer();
			const ObjectLayer layer2 = body2.GetObjectLayer();
			const bool body1IsRagdoll = layer1 == RAGDOLL_OBJECT_LAYER;
			const bool body2IsRagdoll = layer2 == RAGDOLL_OBJECT_LAYER;
			if( body1IsRagdoll == body2IsRagdoll )
				return;

			const Body &ragdollBody = body1IsRagdoll ? body1 : body2;
			const Body &playerBody = body1IsRagdoll ? body2 : body1;
			if( playerBody.GetObjectLayer() != BRUSH_KINEMATIC_OBJECT_LAYER ||
				ragdollBody.GetMotionType() != EMotionType::Dynamic ||
				manifold.mRelativeContactPointsOn1.size() == 0 ||
				!gpGlobals )
				return;

			const int playerEntityIndex = static_cast<int>( playerBody.GetUserData() );
			if( playerEntityIndex <= 0 || playerEntityIndex >= gpGlobals->maxEntities )
				return;

			edict_t *player = INDEXENT( playerEntityIndex );
			if( !player || player->free || !( player->v.flags & FL_CLIENT ) )
				return;

			const RVec3 contact = body1IsRagdoll
				? manifold.GetWorldSpaceContactPointOn1( 0 )
				: manifold.GetWorldSpaceContactPointOn2( 0 );
			// Jolt's manifold normal points from body 1 toward body 2.
			// Convert it into the direction from the player into the ragdoll.
			const Vec3 pushDirection = body1IsRagdoll
				? -manifold.mWorldSpaceNormal
				: manifold.mWorldSpaceNormal;
			const Vec3 ragdollPointVelocity =
				ragdollBody.GetPointVelocity( contact );
			const Vec3 playerPointVelocity =
				playerBody.GetPointVelocity( contact );
			const float approachSpeed =
				( playerPointVelocity - ragdollPointVelocity ).Dot( pushDirection );
			if( !std::isfinite( approachSpeed ) || approachSpeed < 1.0f )
				return;

			NPCPhysicsPendingPlayerPush event;
			event.ragdollID = ragdollBody.GetUserData();
			event.ragdollBodyID = ragdollBody.GetID();
			event.playerEntityIndex = playerEntityIndex;
			event.pushDirection = pushDirection;
			event.contactPoint = Vec3(
				float( contact.GetX() ),
				float( contact.GetY() ),
				float( contact.GetZ() ) );
			event.ragdollPointVelocityBefore = ragdollPointVelocity;
			event.playerPointVelocityBefore = playerPointVelocity;

			std::lock_guard<std::mutex> lock( m_mutex );
			m_playerPushLimits.push_back( event );
		}

		std::mutex m_mutex;
		std::vector<NPCPhysicsPendingCollisionDamage> m_pending;
		std::vector<NPCPhysicsPendingContactDiagnostic> m_contactDiagnostics;
		std::vector<NPCPhysicsPendingPlayerPush> m_playerPushLimits;
	};

	// The creation path logs restored velocities before the diagnostic helper's
	// implementation later in this file.
	static void NPCPhysics_LogRagdollVelocitySummary(
		const char *stage,
		edict_t *ent,
		const NPCPhysicsRagdoll &ragdoll );

	struct NPCPhysicsWorld
	{
		RagdollBroadPhaseLayers broadPhaseLayers;
		RagdollObjectLayerFilter objectLayerFilter;
		RagdollBroadPhaseFilter broadPhaseFilter;
		NPCPhysicsContactListener contactListener;
		std::unique_ptr<PhysicsSystem> system;
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
	static std::vector<NPCPhysicsActorCollider> g_actorColliders;
	static bool g_brushCollisionUnavailableLogged = false;
	static bool g_brushCollisionStatusLogged = false;
	static BodyID g_worldCollisionBodyID;
	static Ref<Shape> g_worldCollisionShape;
	static bool g_worldCollisionReady = false;

	static cvar_t g_svNpcVPhysics = { "sv_npc_vphysics", "1", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsMass = { "sv_npc_vphysics_mass", "0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsBulletScale = { "sv_npc_vphysics_bullet_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsCrowbarScale = { "sv_npc_vphysics_crowbar_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsCrossbowScale = { "sv_npc_vphysics_crossbow_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsBlastScale = { "sv_npc_vphysics_blast_scale", "1.0", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsImpactDamageScale = { "sv_npc_vphysics_impact_damage_scale", "0.25", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsPlayerPushMass = { "sv_npc_vphysics_player_push_mass", "350", FCVAR_SERVER };
	static cvar_t g_svNpcVPhysicsPlayerPushSpeed = { "sv_npc_vphysics_player_push_speed", "50", FCVAR_SERVER };
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

	// Source's default NPC physics impact table is keyed by squared velocity
	// lost during contact. Keep its thresholds with a conservative scale so
	// ordinary corpse settling does not repeatedly cause damage.
	static float NPCPhysics_CalculateCollisionDamage( float velocityLoss )
	{
		const float scale = NPCPhysics_GetCvar(
			"sv_npc_vphysics_impact_damage_scale", 0.25f );
		if( !std::isfinite( velocityLoss ) || velocityLoss <= 0.0f ||
			!std::isfinite( scale ) || scale <= 0.0f )
			return 0.0f;

		const float impactEnergy = velocityLoss * velocityLoss * scale;
		if( impactEnergy < 150.0f * 150.0f )
			return 0.0f;
		if( impactEnergy < 250.0f * 250.0f )
			return 5.0f;
		if( impactEnergy < 350.0f * 350.0f )
			return 10.0f;
		if( impactEnergy < 500.0f * 500.0f )
			return 50.0f;
		if( impactEnergy < 1000.0f * 1000.0f )
			return 100.0f;
		return 500.0f;
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
		g_npcPhysics.system.reset( new PhysicsSystem() );
		g_npcPhysics.system->Init(
			RAGDOLL_MAX_BODIES,
			0,
			RAGDOLL_MAX_BODY_PAIRS,
			RAGDOLL_MAX_CONTACT_CONSTRAINTS,
			g_npcPhysics.broadPhaseLayers,
			g_npcPhysics.broadPhaseFilter,
			g_npcPhysics.objectLayerFilter
		);

		// Jolt's documented defaults assume meters. The bridge intentionally
		// uses GoldSrc inches throughout (including sv_gravity=800), so scale
		// every linear tolerance and squared-distance threshold to that space.
		PhysicsSettings sourceUnitSettings = g_npcPhysics.system->GetPhysicsSettings();
		const float sourceScale = RAGDOLL_SOURCE_UNITS_PER_METER;
		const float sourceScaleSquared = sourceScale * sourceScale;
		sourceUnitSettings.mSpeculativeContactDistance *= sourceScale;
		sourceUnitSettings.mPenetrationSlop *= sourceScale;
		sourceUnitSettings.mManifoldTolerance *= sourceScale;
		sourceUnitSettings.mMaxPenetrationDistance *= sourceScale;
		sourceUnitSettings.mMinVelocityForRestitution *= sourceScale;
		sourceUnitSettings.mPointVelocitySleepThreshold *= sourceScale;
		sourceUnitSettings.mBodyPairCacheMaxDeltaPositionSq *= sourceScaleSquared;
		sourceUnitSettings.mContactPointPreserveLambdaMaxDistSq *= sourceScaleSquared;
		sourceUnitSettings.mInternalEdgeRemovalVertexToleranceSq *= sourceScaleSquared;

		// Articulated ragdolls can be wedged between the player hull and BSP
		// surfaces. Give the contact/joint island more position-solver passes
		// so deep transient contacts are corrected more completely per step.
		sourceUnitSettings.mNumPositionSteps = 4;
		g_npcPhysics.system->SetPhysicsSettings( sourceUnitSettings );
		g_npcPhysics.system->SetContactListener( &g_npcPhysics.contactListener );

		g_npcPhysics.system->SetGravity( Vec3( 0.0f, 0.0f, -800.0f ) );
		ALERT(
			at_console,
			"[NPC physics] Jolt GoldSrc-unit scaling: units/m=%.3f max-penetration-correction=%.2f speculative-distance=%.2f position-steps=%u constraint-position-steps=%u.\n",
			sourceScale,
			sourceUnitSettings.mMaxPenetrationDistance,
			sourceUnitSettings.mSpeculativeContactDistance,
			sourceUnitSettings.mNumPositionSteps,
			RAGDOLL_CONSTRAINT_POSITION_STEPS
		);
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
			BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
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
			g_npcPhysics.system->GetBodyInterface().IsAdded( g_worldCollisionBodyID ) )
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
			WORLD_STATIC_OBJECT_LAYER
		);
		bodySettings.mFriction = 0.9f;
		bodySettings.mRestitution = 0.0f;

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
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

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
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
		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();

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
				// Most inline BSP entities are stationary. Create them as static
				// bodies first; only promote a body to kinematic when its actual
				// transform changes. Reserve motion properties so Jolt can switch
				// the same body to kinematic without rebuilding its mesh shape.
				BodyCreationSettings bodySettings(
					shape.GetPtr(),
					RVec3( position.GetX(), position.GetY(), position.GetZ() ),
					rotation,
					EMotionType::Static,
					BRUSH_KINEMATIC_OBJECT_LAYER
				);
				bodySettings.mAllowDynamicOrKinematic = true;
				// Mesh shapes do not have a closed volume from which to derive
				// mass properties. Supply valid values for the optional kinematic
				// motion properties; static bodies do not use these values.
				bodySettings.mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided;
				bodySettings.mMassPropertiesOverride.mMass = 1.0f;
				bodySettings.mMassPropertiesOverride.mInertia = Mat44::sIdentity();
				bodySettings.mUserData = static_cast<uint64_t>( entityInfo.entityIndex );
				bodySettings.mFriction = 0.9f;
				bodySettings.mRestitution = 0.0f;

				const BodyID bodyID = bodyInterface.CreateAndAddBody(
					bodySettings,
					EActivation::DontActivate
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
					"[NPC physics] Added static inline BSP collider: entity %d, model %d.\n",
					entityInfo.entityIndex,
					entityInfo.modelIndex
				);
				continue;
			}

			NPCPhysicsBrushCollider &collider = g_brushColliders[colliderIndex];
			const Vec3 positionDelta = position - collider.position;
			const bool transformChanged = positionDelta.LengthSq() > 0.0001f ||
				NPCPhysics_QuaternionsDiffer( rotation, collider.rotation );

			float frameTime = gpGlobals ? gpGlobals->frametime : 0.0f;
			if( frameTime < 0.0f )
				frameTime = 0.0f;
			if( frameTime > 0.1f )
				frameTime = 0.1f;

			// Only entities whose transforms actually change need a kinematic
			// surface velocity. This avoids keeping every stationary func_wall
			// and other inline BSP entity in the moving broadphase.
			if( frameTime > 0.0001f && transformChanged )
			{
				if( !collider.isKinematic )
				{
					bodyInterface.SetMotionType(
						collider.bodyID, EMotionType::Kinematic, EActivation::Activate );
					collider.isKinematic = true;
					ALERT(
						at_console,
						"[NPC physics] Promoted inline BSP collider to kinematic: entity %d, model %d.\n",
						collider.entityIndex, collider.modelIndex
					);
				}

				// MoveKinematic derives linear/angular surface velocity from the
				// transform delta so moving doors and platforms can push ragdolls.
				bodyInterface.MoveKinematic(
					collider.bodyID,
					RVec3( position.GetX(), position.GetY(), position.GetZ() ),
					rotation,
					frameTime
				);
				collider.position = position;
				collider.rotation = rotation;
				collider.wasMoving = true;
				collider.pendingStaticTransition = false;
			}
			else if( frameTime > 0.0001f && collider.wasMoving )
			{
				// Give the kinematic body one unchanged target to clear the
				// velocity calculated on its last moving frame. Switch it back
				// to static on the following frame, after that final step.
				bodyInterface.MoveKinematic(
					collider.bodyID,
					RVec3( position.GetX(), position.GetY(), position.GetZ() ),
					rotation,
					frameTime
				);
				collider.wasMoving = false;
				collider.pendingStaticTransition = true;
			}
			else if( collider.pendingStaticTransition )
			{
				bodyInterface.SetMotionType(
					collider.bodyID, EMotionType::Static, EActivation::DontActivate );
				collider.isKinematic = false;
				collider.pendingStaticTransition = false;
				ALERT(
					at_console,
					"[NPC physics] Settled inline BSP collider to static: entity %d, model %d.\n",
					collider.entityIndex, collider.modelIndex
				);
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

	static void NPCPhysics_DestroyActorCollider( NPCPhysicsActorCollider &collider )
	{
		if( !g_npcPhysics.initialized || collider.bodyID.IsInvalid() )
			return;

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
		if( bodyInterface.IsAdded( collider.bodyID ) )
			bodyInterface.RemoveBody( collider.bodyID );
		bodyInterface.DestroyBody( collider.bodyID );
		collider.bodyID = BodyID();
	}

	static void NPCPhysics_DestroyActorColliders()
	{
		for( size_t i = 0; i < g_actorColliders.size(); ++i )
			NPCPhysics_DestroyActorCollider( g_actorColliders[i] );
		g_actorColliders.clear();
	}

	static int NPCPhysics_FindActorCollider( int entityIndex )
	{
		for( size_t i = 0; i < g_actorColliders.size(); ++i )
			if( g_actorColliders[i].entityIndex == entityIndex )
				return static_cast<int>( i );
		return -1;
	}

	static bool NPCPhysics_IsActorColliderCandidate( edict_t *ent )
	{
		if( !ent || ent->free )
			return false;

		const int flags = ent->v.flags;
		if( !( flags & ( FL_CLIENT | FL_MONSTER ) ) ||
			( flags & ( FL_KILLME | FL_DORMANT ) ) ||
			NPCPhysics_IsRagdollEntity( ent ) )
			return false;

		if( ent->v.deadflag != DEAD_NO || ent->v.health <= 0.0f )
			return false;

		if( ent->v.movetype != MOVETYPE_WALK &&
			ent->v.movetype != MOVETYPE_STEP &&
			ent->v.movetype != MOVETYPE_FLY )
			return false;

		return ent->v.solid == SOLID_SLIDEBOX || ent->v.solid == SOLID_BBOX;
	}

	static void NPCPhysics_UpdateActorColliders()
	{
		if( !g_npcPhysics.initialized || !gpGlobals )
			return;

		std::vector<int> presentEntities;
		presentEntities.reserve( 32 );
		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();

		float frameTime = gpGlobals->frametime;
		if( frameTime < 0.0f )
			frameTime = 0.0f;
		if( frameTime > 0.1f )
			frameTime = 0.1f;

		for( int entityIndex = 1; entityIndex < gpGlobals->maxEntities; ++entityIndex )
		{
			edict_t *ent = INDEXENT( entityIndex );
			if( !NPCPhysics_IsActorColliderCandidate( ent ) )
				continue;

			const Vec3 mins( ent->v.mins[0], ent->v.mins[1], ent->v.mins[2] );
			const Vec3 maxs( ent->v.maxs[0], ent->v.maxs[1], ent->v.maxs[2] );
			const Vec3 halfExtents = ( maxs - mins ) * 0.5f;
			const Vec3 centerOffset = ( maxs + mins ) * 0.5f;
			if( !std::isfinite( halfExtents.GetX() ) ||
				!std::isfinite( halfExtents.GetY() ) ||
				!std::isfinite( halfExtents.GetZ() ) ||
				!std::isfinite( centerOffset.GetX() ) ||
				!std::isfinite( centerOffset.GetY() ) ||
				!std::isfinite( centerOffset.GetZ() ) ||
				halfExtents.GetX() < 1.0f || halfExtents.GetY() < 1.0f ||
				halfExtents.GetZ() < 1.0f ||
				halfExtents.GetX() > 128.0f || halfExtents.GetY() > 128.0f ||
				halfExtents.GetZ() > 128.0f )
				continue;

			presentEntities.push_back( entityIndex );
			const Vec3 position(
				ent->v.origin[0] + centerOffset.GetX(),
				ent->v.origin[1] + centerOffset.GetY(),
				ent->v.origin[2] + centerOffset.GetZ() );

			int colliderIndex = NPCPhysics_FindActorCollider( entityIndex );
			if( colliderIndex >= 0 )
			{
				NPCPhysicsActorCollider &existing = g_actorColliders[colliderIndex];
				const Vec3 extentDelta = halfExtents - existing.halfExtents;
				if( existing.serialNumber != ent->serialnumber ||
					extentDelta.LengthSq() > 0.0001f )
				{
					NPCPhysics_DestroyActorCollider( existing );
					g_actorColliders.erase( g_actorColliders.begin() + colliderIndex );
					colliderIndex = -1;
				}
			}

			if( colliderIndex < 0 )
			{
				BoxShapeSettings shapeSettings( halfExtents );
				Shape::ShapeResult shapeResult = shapeSettings.Create();
				if( !shapeResult.IsValid() )
					continue;

				BodyCreationSettings bodySettings(
					shapeResult.Get(),
					RVec3( position.GetX(), position.GetY(), position.GetZ() ),
					Quat::sIdentity(),
					EMotionType::Kinematic,
					BRUSH_KINEMATIC_OBJECT_LAYER
				);
				bodySettings.mUserData = static_cast<uint64_t>( entityIndex );
				bodySettings.mFriction = 0.65f;
				bodySettings.mRestitution = 0.0f;

				const BodyID bodyID = bodyInterface.CreateAndAddBody(
					bodySettings,
					EActivation::Activate
				);
				if( bodyID.IsInvalid() )
					continue;

				NPCPhysicsActorCollider collider;
				collider.entityIndex = entityIndex;
				collider.serialNumber = ent->serialnumber;
				collider.bodyID = bodyID;
				collider.position = position;
				collider.centerOffset = centerOffset;
				collider.halfExtents = halfExtents;
				g_actorColliders.push_back( collider );

				ALERT(
					at_console,
					"[NPC physics] Added living actor hull: entity=%d type=%s half=(%.1f %.1f %.1f).\n",
					entityIndex,
					( ent->v.flags & FL_CLIENT ) ? "player" : "NPC",
					halfExtents.GetX(), halfExtents.GetY(), halfExtents.GetZ()
				);
				continue;
			}

			NPCPhysicsActorCollider &collider = g_actorColliders[colliderIndex];
			const Vec3 positionDelta = position - collider.position;
			const bool transformChanged = positionDelta.LengthSq() > 0.0001f;

			// MoveKinematic turns GoldSrc actor displacement into surface
			// velocity. A single unchanged target after movement stops clears
			// that velocity; stationary hulls do not need per-frame updates.
			if( frameTime > 0.0001f && ( transformChanged || collider.wasMoving ) )
			{
				if( positionDelta.LengthSq() > 256.0f * 256.0f )
				{
					// Spawn/teleport changes must not launch nearby ragdolls.
					bodyInterface.SetPositionAndRotation(
						collider.bodyID,
						RVec3( position.GetX(), position.GetY(), position.GetZ() ),
						Quat::sIdentity(),
						EActivation::Activate
					);
				}
				else
				{
					bodyInterface.MoveKinematic(
						collider.bodyID,
						RVec3( position.GetX(), position.GetY(), position.GetZ() ),
						Quat::sIdentity(),
						frameTime
					);
				}

				collider.position = position;
				collider.wasMoving = transformChanged;
			}
			collider.centerOffset = centerOffset;
		}

		for( size_t i = g_actorColliders.size(); i > 0; --i )
		{
			const int entityIndex = g_actorColliders[i - 1].entityIndex;
			if( std::find( presentEntities.begin(), presentEntities.end(), entityIndex ) == presentEntities.end() )
			{
				NPCPhysics_DestroyActorCollider( g_actorColliders[i - 1] );
				g_actorColliders.erase( g_actorColliders.begin() + ( i - 1 ) );
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

	static bool NPCPhysics_CreateRagdoll( edict_t *ent, bool restorePersistentState = false )
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

		CBaseMonster *persistentMonster = static_cast<CBaseMonster *>( CBaseEntity::Instance( ent ) );
		bool hasPersistentPose = restorePersistentState && persistentMonster &&
			persistentMonster->m_iNpcRagdollPersistVersion == NPC_RAGDOLL_PERSIST_VERSION &&
			persistentMonster->m_iNpcRagdollPersistBoneCount == ragdoll.studioBoneCount &&
			persistentMonster->m_iNpcRagdollPersistBodyCount ==
				static_cast<int>( physicsDefinition->bodies.size() ) &&
			ragdoll.studioBoneCount <= NPC_RAGDOLL_PERSIST_MAX_STUDIO_BONES &&
			static_cast<int>( physicsDefinition->bodies.size() ) <= NPC_RAGDOLL_PERSIST_MAX_BODIES;

		// Validate the full pose and velocity snapshot before using any of it.
		// Positions are offsets from pev->origin, which the save system rebases
		// automatically across a landmark transition.
		if( hasPersistentPose )
		{
			for( int bone = 0; bone < ragdoll.studioBoneCount; ++bone )
			{
				const float *saved = &persistentMonster->m_flNpcRagdollPersistStudioPose[
					bone * NPC_RAGDOLL_PERSIST_STUDIO_STRIDE];
				float quaternionLengthSquared = 0.0f;
				for( int component = 0; component < NPC_RAGDOLL_PERSIST_STUDIO_STRIDE; ++component )
					if( !std::isfinite( saved[component] ) )
						hasPersistentPose = false;
				for( int component = 3; component < 7; ++component )
					quaternionLengthSquared += saved[component] * saved[component];
				if( !std::isfinite( quaternionLengthSquared ) || quaternionLengthSquared < 1.0e-8f )
					hasPersistentPose = false;
				if( !hasPersistentPose )
					break;
			}

			for( int body = 0; hasPersistentPose &&
				body < persistentMonster->m_iNpcRagdollPersistBodyCount; ++body )
			{
				const float *saved = &persistentMonster->m_flNpcRagdollPersistBodyVelocity[
					body * NPC_RAGDOLL_PERSIST_BODY_VELOCITY_STRIDE];
				for( int component = 0; component < NPC_RAGDOLL_PERSIST_BODY_VELOCITY_STRIDE; ++component )
					if( !std::isfinite( saved[component] ) )
						hasPersistentPose = false;
			}
		}

		if( restorePersistentState && !hasPersistentPose )
			return NPCPhysics_RagdollSetupFailed(
				"saved ragdoll snapshot is incompatible with this model or contains invalid data" );

		if( hasPersistentPose )
		{
			const ::Vector carrierOrigin = ent->v.origin;
			for( int bone = 0; bone < ragdoll.studioBoneCount; ++bone )
			{
				const float *saved = &persistentMonster->m_flNpcRagdollPersistStudioPose[
					bone * NPC_RAGDOLL_PERSIST_STUDIO_STRIDE];
				bonePositions[bone] = carrierOrigin + ::Vector( saved[0], saved[1], saved[2] );
				boneRotations[bone] = Quat( saved[3], saved[4], saved[5], saved[6] ).Normalized();
			}
			ALERT( at_console,
				"[NPC physics] Restoring saved ragdoll pose: entity=%d bones=%d bodies=%d.\n",
				ENTINDEX( ent ), ragdoll.studioBoneCount,
				static_cast<int>( physicsDefinition->bodies.size() ) );
		}

		// Resolve the model-specific physical rig by bone name. The file's
		// physical hierarchy is independent of compiled MDL bone indices.
		ragdoll.bodyCount = static_cast<int>( physicsDefinition->bodies.size() );
		if( ragdoll.bodyCount < 1 || ragdoll.bodyCount > RAGDOLL_DEFINITION_MAX_BODIES )
			return NPCPhysics_RagdollSetupFailed( "loaded ragdoll definition has an invalid body count" );
		// Four contact channels per body: world/brush x added/persisted.
		ragdoll.nextContactDebugTimes.assign(
			static_cast<size_t>( ragdoll.bodyCount ) * 4, 0.0f );

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
			// The hull vertices are already in GoldSrc units, so keep this
			// padding in the same unit space. Do not convert the SI default:
			// Jolt subtracts the convex radius before rounding the hull.
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
				// Raise the solver iteration count for the entire articulated
				// island, including its environment contacts. This improves
				// convergence when several limbs are trapped against corner faces.
				constraint->mNumPositionStepsOverride = RAGDOLL_CONSTRAINT_POSITION_STEPS;
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

		// Build the initial body transforms used by Jolt's pose-aware collision
		// filter. The hull vertices are authored around each body's local shape
		// center, so offset each bone transform by the same center used for
		// RagdollSettings::Part::mPosition above.
		std::vector<Mat44> physicsBodyTransforms = physicsNeutralModel;
		for( int body = 0; body < ragdoll.bodyCount; ++body )
			physicsBodyTransforms[body] =
				physicsBodyTransforms[body].PreTranslated( ragdoll.bones[body].localShapeCenter );

		// Disable collisions not only between direct parent/child bodies, but
		// also between other hull pairs that overlap in the initial pose. This
		// prevents adjacent limbs and torso parts from pushing against each other
		// while the ragdoll is trying to articulate naturally.
		if( !settings->Stabilize() )
			return NPCPhysics_RagdollSetupFailed( "Jolt could not stabilize the authored ragdoll" );
		settings->CalculateConstraintPriorities();
		settings->DisableParentChildCollisions( physicsBodyTransforms.data(), 1.0f );
		settings->CalculateBodyIndexToConstraintIndex();

		ragdoll.physicsRagdoll = settings->CreateRagdoll(
			static_cast<CollisionGroup::GroupID>( ragdoll.networkID ),
			static_cast<uint64>( ragdoll.networkID ), g_npcPhysics.system.get() );
		if( !ragdoll.physicsRagdoll.GetPtr() )
			return NPCPhysics_RagdollSetupFailed( "Jolt could not allocate native ragdoll bodies" );
		ragdoll.physicsRagdoll->AddToPhysicsSystem( EActivation::Activate );
		ragdoll.addedToPhysicsSystem = true;

		NPCPhysicsLivePose *livePose = NULL;
		const int entityIndex = ENTINDEX( ent );
		if( !hasPersistentPose && entityIndex >= 0 && entityIndex < (int)g_livePoses.size() )
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

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
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

		// A transferred corpse resumes with the velocity of each original body,
		// not with the default animation-derived velocity of the carrier entity.
		if( hasPersistentPose )
		{
			for( int body = 0; body < ragdoll.bodyCount; ++body )
			{
				const float *saved = &persistentMonster->m_flNpcRagdollPersistBodyVelocity[
					body * NPC_RAGDOLL_PERSIST_BODY_VELOCITY_STRIDE];
				Vec3 linearVelocity( saved[0], saved[1], saved[2] );
				Vec3 angularVelocity( saved[3], saved[4], saved[5] );
				linearVelocity = NPCPhysics_ClampVector( linearVelocity, RAGDOLL_VELOCITY_LIMIT );
				angularVelocity = NPCPhysics_ClampVector( angularVelocity, RAGDOLL_ANGULAR_VELOCITY_LIMIT );
				const BodyID bodyID = ragdoll.physicsRagdoll->GetBodyID( body );
				bodyInterface.SetLinearVelocity( bodyID, linearVelocity );
				bodyInterface.SetAngularVelocity( bodyID, angularVelocity );
				bodyInterface.ActivateBody( bodyID );
			}
			NPCPhysics_LogRagdollVelocitySummary( "Restored", ent, ragdoll );
		}

		// Only replace the animated corpse after all physics bodies and joints
		// were built successfully. Freeze the death frame so non-physics bones
		// remain a stable fallback and inherit from simulated parents.
		ent->v.iuser4 = 1;
		ent->v.effects &= ~EF_NODRAW;
		ent->v.solid = SOLID_NOT;
		ent->v.movetype = MOVETYPE_NONE;
		// Keep the corpse damageable so deferred Jolt impacts use the normal
		// CBaseMonster::DeadTakeDamage corpse-gib rules.
		ent->v.takedamage = DAMAGE_YES;
		ent->v.deadflag = DEAD_DEAD;
		ent->v.nextthink = -1.0f;
		ent->v.framerate = 0.0f;
		ent->v.animtime = gpGlobals ? gpGlobals->time : ent->v.animtime;
		ent->v.velocity = g_vecZero;
		ent->v.avelocity = g_vecZero;

		g_ragdolls.push_back( ragdoll );
		ALERT(
			at_console,
			hasPersistentPose
				? "[NPC physics] Restored articulated Jolt ragdoll: entity=%d model='%s' id=%d bodies=%d studio-bones=%d.\n"
				: "[NPC physics] Created articulated Jolt ragdoll: entity=%d model='%s' id=%d bodies=%d studio-bones=%d.\n",
			ENTINDEX( ent ),
			STRING( ent->v.model ),
			ragdoll.networkID,
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

		if( !ragdoll.firstPoseSendReported )
		{
			ALERT(
				at_console,
				"[NPC physics] Sending first ragdoll pose: entity=%d model='%s' id=%d bodies=%d studio-bones=%d pvs-origin=(%.1f %.1f %.1f).\n",
				entityIndex,
				STRING( ragdoll.entity->v.model ),
				ragdoll.networkID,
				ragdoll.bodyCount,
				ragdoll.studioBoneCount,
				ragdoll.entity->v.origin[0],
				ragdoll.entity->v.origin[1],
				ragdoll.entity->v.origin[2]
			);
			ragdoll.firstPoseSendReported = true;
		}

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

	static void NPCPhysics_LogRagdollVelocitySummary(
		const char *stage,
		edict_t *ent,
		const NPCPhysicsRagdoll &ragdoll )
	{
		if( !ent || !ragdoll.physicsRagdoll.GetPtr() || ragdoll.bodyCount <= 0 ||
			NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) < 0.5f )
			return;

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
		const int rootBodyIndex = ragdoll.rootElement >= 0 && ragdoll.rootElement < ragdoll.bodyCount
			? ragdoll.rootElement : 0;
		const BodyID rootBodyID = ragdoll.physicsRagdoll->GetBodyID( rootBodyIndex );
		const Vec3 rootLinear = bodyInterface.GetLinearVelocity( rootBodyID );
		const Vec3 rootAngular = bodyInterface.GetAngularVelocity( rootBodyID );

		int fastestBodyIndex = rootBodyIndex;
		float fastestSpeedSquared = -1.0f;
		Vec3 fastestLinear( 0.0f, 0.0f, 0.0f );
		Vec3 fastestAngular( 0.0f, 0.0f, 0.0f );
		for( int body = 0; body < ragdoll.bodyCount; ++body )
		{
			const BodyID bodyID = ragdoll.physicsRagdoll->GetBodyID( body );
			const Vec3 linear = bodyInterface.GetLinearVelocity( bodyID );
			const float speedSquared =
				linear.GetX() * linear.GetX() +
				linear.GetY() * linear.GetY() +
				linear.GetZ() * linear.GetZ();
			if( speedSquared > fastestSpeedSquared )
			{
				fastestSpeedSquared = speedSquared;
				fastestBodyIndex = body;
				fastestLinear = linear;
				fastestAngular = bodyInterface.GetAngularVelocity( bodyID );
			}
		}

		ALERT(
			at_console,
			"[NPC physics] %s ragdoll velocities: entity=%d root-body=%d root-v=(%.1f %.1f %.1f) root-w=(%.2f %.2f %.2f) fastest-body=%d speed=%.1f v=(%.1f %.1f %.1f) w=(%.2f %.2f %.2f).\n",
			stage, ENTINDEX( ent ), rootBodyIndex,
			rootLinear.GetX(), rootLinear.GetY(), rootLinear.GetZ(),
			rootAngular.GetX(), rootAngular.GetY(), rootAngular.GetZ(),
			fastestBodyIndex, std::sqrt( (std::max)( 0.0f, fastestSpeedSquared ) ),
			fastestLinear.GetX(), fastestLinear.GetY(), fastestLinear.GetZ(),
			fastestAngular.GetX(), fastestAngular.GetY(), fastestAngular.GetZ()
		);
	}

	static void NPCPhysics_SaveRagdollStateInternal( edict_t *ent )
	{
		if( !ent || ent->free )
			return;

		CBaseMonster *monster = static_cast<CBaseMonster *>( CBaseEntity::Instance( ent ) );
		if( !monster )
			return;

		const bool hasPriorRagdollSnapshot =
			monster->m_iNpcRagdollPersistVersion != 0 ||
			monster->m_iNpcRagdollPersistBoneCount != 0 ||
			monster->m_iNpcRagdollPersistBodyCount != 0;
		const bool debug = NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f;
		if( !g_npcPhysics.initialized || g_ragdolls.empty() )
		{
			if( hasPriorRagdollSnapshot && debug )
				ALERT( at_console,
					"[NPC physics] Existing ragdoll snapshot for entity %d could not be refreshed: Jolt world has no active ragdolls.\n",
					ENTINDEX( ent ) );
			return;
		}

		NPCPhysicsRagdoll *ragdoll = NULL;
		for( size_t i = 0; i < g_ragdolls.size(); ++i )
			if( g_ragdolls[i].entity == ent &&
				g_ragdolls[i].serialNumber == ent->serialnumber &&
				g_ragdolls[i].addedToPhysicsSystem &&
				g_ragdolls[i].physicsRagdoll.GetPtr() )
			{
				ragdoll = &g_ragdolls[i];
				break;
			}

		if( !ragdoll )
		{
			if( hasPriorRagdollSnapshot && debug )
				ALERT( at_console,
					"[NPC physics] Existing ragdoll snapshot for entity %d could not be refreshed: live Jolt state is not registered.\n",
					ENTINDEX( ent ) );
			return;
		}

		if( ragdoll->studioBoneCount <= 0 ||
			ragdoll->studioBoneCount > NPC_RAGDOLL_PERSIST_MAX_STUDIO_BONES ||
			ragdoll->bodyCount <= 0 ||
			ragdoll->bodyCount > NPC_RAGDOLL_PERSIST_MAX_BODIES )
		{
			monster->m_iNpcRagdollPersistVersion = 0;
			return;
		}

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
		const Vec3 rootBoneOrigin = NPCPhysics_GetRootBoneOrigin( *ragdoll, bodyInterface );
		NPCPhysics_UpdateMappedStudioPose( *ragdoll, bodyInterface, rootBoneOrigin );
		const Vec3 carrierOrigin( ent->v.origin[0], ent->v.origin[1], ent->v.origin[2] );

		for( int bone = 0; bone < ragdoll->studioBoneCount; ++bone )
		{
			const Mat44 &transform = ragdoll->studioPoseModelSpace[bone];
			const Vec3 relativePosition =
				rootBoneOrigin + transform.GetTranslation() - carrierOrigin;
			const Quat rotation = transform.GetQuaternion().Normalized();
			float *saved = &monster->m_flNpcRagdollPersistStudioPose[
				bone * NPC_RAGDOLL_PERSIST_STUDIO_STRIDE];
			saved[0] = relativePosition.GetX();
			saved[1] = relativePosition.GetY();
			saved[2] = relativePosition.GetZ();
			saved[3] = rotation.GetX();
			saved[4] = rotation.GetY();
			saved[5] = rotation.GetZ();
			saved[6] = rotation.GetW();
		}

		for( int body = 0; body < ragdoll->bodyCount; ++body )
		{
			const BodyID bodyID = ragdoll->physicsRagdoll->GetBodyID( body );
			const Vec3 linear = bodyInterface.GetLinearVelocity( bodyID );
			const Vec3 angular = bodyInterface.GetAngularVelocity( bodyID );
			float *saved = &monster->m_flNpcRagdollPersistBodyVelocity[
				body * NPC_RAGDOLL_PERSIST_BODY_VELOCITY_STRIDE];
			saved[0] = linear.GetX();
			saved[1] = linear.GetY();
			saved[2] = linear.GetZ();
			saved[3] = angular.GetX();
			saved[4] = angular.GetY();
			saved[5] = angular.GetZ();
		}
		NPCPhysics_LogRagdollVelocitySummary( "Saved", ent, *ragdoll );

		monster->m_iNpcRagdollPersistVersion = NPC_RAGDOLL_PERSIST_VERSION;
		monster->m_iNpcRagdollPersistBoneCount = ragdoll->studioBoneCount;
		monster->m_iNpcRagdollPersistBodyCount = ragdoll->bodyCount;

		if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT( at_console,
				"[NPC physics] Saved transferable ragdoll: entity=%d bones=%d bodies=%d.\n",
				ENTINDEX( ent ), ragdoll->studioBoneCount, ragdoll->bodyCount );
	}

	static int NPCPhysics_PhysicsEntity( edict_t *ent )
	{
		// Capture live animation transforms only. Return 0 so GoldSrc remains the
		// sole owner of living NPC movement, AI, contacts and collision.
		NPCPhysics_CaptureLivePose( ent );
		return 0;
	}

	static void NPCPhysics_ProcessContactDiagnostics()
	{
		std::vector<NPCPhysicsPendingContactDiagnostic> pending =
			g_npcPhysics.contactListener.DrainDiagnostics();
		if( pending.empty() || !gpGlobals ||
			NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) < 0.5f )
			return;

		for( size_t i = 0; i < pending.size(); ++i )
		{
			const NPCPhysicsPendingContactDiagnostic &event = pending[i];
			NPCPhysicsRagdoll *ragdoll = NULL;
			for( size_t r = 0; r < g_ragdolls.size(); ++r )
			{
				if( g_ragdolls[r].networkID == static_cast<int>( event.ragdollID ) &&
					g_ragdolls[r].addedToPhysicsSystem &&
					g_ragdolls[r].physicsRagdoll.GetPtr() )
				{
					ragdoll = &g_ragdolls[r];
					break;
				}
			}
			if( !ragdoll || !ragdoll->entity || ragdoll->entity->free ||
				ragdoll->entity->serialnumber != ragdoll->serialNumber )
				continue;

			int bodyIndex = -1;
			for( int body = 0; body < ragdoll->bodyCount; ++body )
			{
				if( ragdoll->physicsRagdoll->GetBodyID( body ) == event.ragdollBodyID )
				{
					bodyIndex = body;
					break;
				}
			}
			if( bodyIndex < 0 )
				continue;

			// Source keeps static world geometry distinct from moving brush
			// and actor hull contacts. Throttle each body/channel separately
			// so one busy limb cannot hide another body's penetration data.
			const int debugChannel = ( event.otherLayer == WORLD_STATIC_OBJECT_LAYER ? 0 : 2 ) +
				( event.persisted ? 1 : 0 );
			const size_t debugSlot = static_cast<size_t>( bodyIndex ) * 4 +
				static_cast<size_t>( debugChannel );
			if( debugSlot >= ragdoll->nextContactDebugTimes.size() )
				continue;
			float &nextDebugTime = ragdoll->nextContactDebugTimes[debugSlot];
			if( gpGlobals->time < nextDebugTime )
				continue;

			const char *otherKind = event.otherLayer == WORLD_STATIC_OBJECT_LAYER
				? "world" : "kinematic";
			if( event.otherLayer == BRUSH_KINEMATIC_OBJECT_LAYER &&
				event.otherEntityIndex > 0 &&
				event.otherEntityIndex < gpGlobals->maxEntities )
			{
				edict_t *other = INDEXENT( event.otherEntityIndex );
				if( !FNullEnt( other ) && !other->free )
				{
					if( other->v.flags & FL_CLIENT )
						otherKind = "player";
					else if( other->v.flags & FL_MONSTER )
						otherKind = "actor";
					else
						otherKind = "brush";
				}
			}

			const char *boneName = ragdoll->physicsSkeleton.GetPtr()
				? ragdoll->physicsSkeleton->GetJoint( bodyIndex ).mName.c_str()
				: "unknown";
			const float bodyMass = bodyIndex < static_cast<int>( ragdoll->bodyMasses.size() )
				? ragdoll->bodyMasses[bodyIndex] : 0.0f;

			ALERT(
				at_console,
				"[NPC physics] Contact %s: ent=%d id=%d body=%d bone='%s' other=%s/%d samples=%u jolt-depth=%.3f closing=%.1f point=(%.1f %.1f %.1f) ragdoll-normal=(%.2f %.2f %.2f) subshape-ragdoll=%u subshape-other=%u.\n",
				event.persisted ? "persisted" : "added",
				ENTINDEX( ragdoll->entity ), ragdoll->networkID, bodyIndex, boneName,
				otherKind, event.otherEntityIndex, event.sampleCount,
				event.penetrationDepth, event.closingSpeed,
				event.contactPoint.GetX(), event.contactPoint.GetY(), event.contactPoint.GetZ(),
				event.normal.GetX(), event.normal.GetY(), event.normal.GetZ(),
				static_cast<unsigned int>( event.ragdollSubShapeID ),
				static_cast<unsigned int>( event.otherSubShapeID )
			);
			ALERT(
				at_console,
				"[NPC physics] Contact shape detail: ent=%d id=%d body=%d mass=%.1f ragdoll-shape=%s com=(%.1f %.1f %.1f) rot=(%.3f %.3f %.3f %.3f) bounds-com=(%.1f %.1f %.1f)-(%.1f %.1f %.1f) other-shape=%s other-com=(%.1f %.1f %.1f) other-rot=(%.3f %.3f %.3f %.3f) other-bounds-com=(%.1f %.1f %.1f)-(%.1f %.1f %.1f).\n",
				ENTINDEX( ragdoll->entity ), ragdoll->networkID, bodyIndex, bodyMass,
				NPCPhysics_ShapeSubTypeName( event.ragdollShapeSubType ),
				event.ragdollCenterOfMass.GetX(), event.ragdollCenterOfMass.GetY(), event.ragdollCenterOfMass.GetZ(),
				event.ragdollRotation.GetX(), event.ragdollRotation.GetY(),
				event.ragdollRotation.GetZ(), event.ragdollRotation.GetW(),
				event.ragdollBoundsMin.GetX(), event.ragdollBoundsMin.GetY(), event.ragdollBoundsMin.GetZ(),
				event.ragdollBoundsMax.GetX(), event.ragdollBoundsMax.GetY(), event.ragdollBoundsMax.GetZ(),
				NPCPhysics_ShapeSubTypeName( event.otherShapeSubType ),
				event.otherCenterOfMass.GetX(), event.otherCenterOfMass.GetY(), event.otherCenterOfMass.GetZ(),
				event.otherRotation.GetX(), event.otherRotation.GetY(),
				event.otherRotation.GetZ(), event.otherRotation.GetW(),
				event.otherBoundsMin.GetX(), event.otherBoundsMin.GetY(), event.otherBoundsMin.GetZ(),
				event.otherBoundsMax.GetX(), event.otherBoundsMax.GetY(), event.otherBoundsMax.GetZ()
			);

			nextDebugTime = gpGlobals->time + 0.5f;
		}
	}

	static void NPCPhysics_ProcessPlayerPushLimits( BodyInterface &bodyInterface )
	{
		std::vector<NPCPhysicsPendingPlayerPush> pending =
			g_npcPhysics.contactListener.DrainPlayerPushLimits();
		if( pending.empty() || g_ragdolls.empty() )
			return;

		float massLimit = NPCPhysics_GetCvar( "sv_npc_vphysics_player_push_mass", 350.0f );
		float speedLimit = NPCPhysics_GetCvar( "sv_npc_vphysics_player_push_speed", 50.0f );
		if( !std::isfinite( massLimit ) )
			massLimit = 350.0f;
		if( !std::isfinite( speedLimit ) )
			speedLimit = 50.0f;
		massLimit = (std::max)( 0.0f, massLimit );
		speedLimit = (std::max)( 0.0f, speedLimit );

		for( size_t i = 0; i < pending.size(); ++i )
		{
			const NPCPhysicsPendingPlayerPush &event = pending[i];
			if( event.ragdollBodyID.IsInvalid() ||
				!bodyInterface.IsAdded( event.ragdollBodyID ) )
				continue;

			NPCPhysicsRagdoll *ragdoll = NULL;
			for( size_t r = 0; r < g_ragdolls.size(); ++r )
			{
				if( g_ragdolls[r].networkID == static_cast<int>( event.ragdollID ) &&
					g_ragdolls[r].addedToPhysicsSystem &&
					g_ragdolls[r].physicsRagdoll.GetPtr() )
				{
					ragdoll = &g_ragdolls[r];
					break;
				}
			}
			if( !ragdoll || !ragdoll->entity || ragdoll->entity->free ||
				ragdoll->entity->serialnumber != ragdoll->serialNumber ||
				ragdoll->bodyMasses.size() != static_cast<size_t>( ragdoll->bodyCount ) )
				continue;

			int ragdollBodyIndex = -1;
			for( int body = 0; body < ragdoll->bodyCount; ++body )
			{
				if( ragdoll->physicsRagdoll->GetBodyID( body ) == event.ragdollBodyID )
				{
					ragdollBodyIndex = body;
					break;
				}
			}
			if( ragdollBodyIndex < 0 )
				continue;

			// Source's player controller checks the mass of the contacted
			// physics object, so use this ragdoll body rather than group mass.
			const float contactMass = ragdoll->bodyMasses[ragdollBodyIndex];
			if( !std::isfinite( contactMass ) || contactMass <= 0.0f )
				continue;

			const float initialPushSpeed =
				event.ragdollPointVelocityBefore.Dot( event.pushDirection );
			const float playerPushSpeed =
				event.playerPointVelocityBefore.Dot( event.pushDirection );
			if( playerPushSpeed <= initialPushSpeed + 0.5f )
				continue;

			const RVec3 contactPoint(
				event.contactPoint.GetX(),
				event.contactPoint.GetY(),
				event.contactPoint.GetZ() );
			const Vec3 currentPointVelocity =
				bodyInterface.GetPointVelocity( event.ragdollBodyID, contactPoint );
			const float currentPushSpeed =
				currentPointVelocity.Dot( event.pushDirection );

			// Source's player controller refuses to push objects over its mass
			// threshold and limits the push response to a normal speed. The Jolt
			// actor is still kinematic, so apply that limit to the velocity gained
			// by the contacted ragdoll body rather than feeding movement back into
			// GoldSrc's player movement code.
			const bool exceedsMassLimit = contactMass > massLimit;
			const float maxAllowedPushSpeed = exceedsMassLimit
				? initialPushSpeed
				: ( initialPushSpeed < speedLimit ? speedLimit : initialPushSpeed );
			if( currentPushSpeed <= maxAllowedPushSpeed + 0.01f )
				continue;

			const Vec3 currentLinearVelocity =
				bodyInterface.GetLinearVelocity( event.ragdollBodyID );
			const Vec3 correctedLinearVelocity = currentLinearVelocity +
				event.pushDirection * ( maxAllowedPushSpeed - currentPushSpeed );
			bodyInterface.SetLinearVelocity(
				event.ragdollBodyID, correctedLinearVelocity );

			if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f &&
				gpGlobals && gpGlobals->time >= ragdoll->nextPlayerPushDebugTime )
			{
				ALERT(
					at_console,
					"[NPC physics] Player push limited: ragdoll=%d id=%d body=%d player=%d mass=%.1f push-speed=%.1f->%.1f limit=%.1f%s.\n",
					ENTINDEX( ragdoll->entity ), ragdoll->networkID,
					ragdollBodyIndex, event.playerEntityIndex,
					contactMass, currentPushSpeed, maxAllowedPushSpeed,
					speedLimit, exceedsMassLimit ? " (mass limit)" : ""
				);
				ragdoll->nextPlayerPushDebugTime = gpGlobals->time + 0.5f;
			}
		}
	}

	static void NPCPhysics_ProcessCollisionDamage()
	{
		std::vector<NPCPhysicsPendingCollisionDamage> pending =
			g_npcPhysics.contactListener.Drain();
		if( pending.empty() || !NPCPhysics_Enabled() )
			return;

		// One strongest impact per corpse per server frame. This collapses
		// multi-point manifolds and prevents a single landing from multiplying
		// damage by the number of contact points and articulated bones.
		std::vector<NPCPhysicsPendingCollisionDamage> strongest;
		for( size_t i = 0; i < pending.size(); ++i )
		{
			NPCPhysicsPendingCollisionDamage &event = pending[i];
			size_t existing = strongest.size();
			for( size_t j = 0; j < strongest.size(); ++j )
				if( strongest[j].ragdollID == event.ragdollID )
				{
					existing = j;
					break;
				}
			if( existing == strongest.size() )
				strongest.push_back( event );
			else if( event.velocityLoss > strongest[existing].velocityLoss )
				strongest[existing] = event;
		}

		for( size_t i = 0; i < strongest.size(); ++i )
		{
			const NPCPhysicsPendingCollisionDamage &event = strongest[i];
			NPCPhysicsRagdoll *ragdoll = NULL;
			for( size_t r = 0; r < g_ragdolls.size(); ++r )
				if( g_ragdolls[r].networkID == static_cast<int>( event.ragdollID ) &&
					g_ragdolls[r].addedToPhysicsSystem &&
					g_ragdolls[r].physicsRagdoll.GetPtr() )
				{
					ragdoll = &g_ragdolls[r];
					break;
				}
			if( !ragdoll || !ragdoll->entity || ragdoll->entity->free ||
				ragdoll->entity->serialnumber != ragdoll->serialNumber ||
				gpGlobals->time < ragdoll->nextCollisionDamageTime )
				continue;

			const float damage = NPCPhysics_CalculateCollisionDamage( event.velocityLoss );
			if( damage <= 0.0f || ragdoll->entity->v.takedamage == DAMAGE_NO )
				continue;

			int attackerIndex = event.otherEntityIndex;

			// World contacts use entity index 0. CBaseEntity::TakeDamage reads
			// both inflictor and attacker entvars when deriving the attack
			// direction, so passing NULL for a world impact can dereference a
			// null pointer. Use worldspawn as the safe fallback, then prefer a
			// live brush/actor entity when the contact supplied one.
			edict_t *attacker = INDEXENT( 0 );
			// FNullEnt treats edict offset 0 as null, but index 0 is the
			// actual worldspawn edict. For this one special case, test the
			// pointer and free flag directly instead of using FNullEnt.
			if( !attacker || attacker->free )
				attacker = NULL;
			if( attackerIndex > 0 && attackerIndex < gpGlobals->maxEntities )
			{
				edict_t *candidate = INDEXENT( attackerIndex );
				if( !FNullEnt( candidate ) && !candidate->free )
					attacker = candidate;
			}
			if( !attacker )
			{
				if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
					ALERT(
						at_console,
						"[NPC physics] Skipped collision damage: no valid world or contact attacker (entity=%d id=%d other=%d).\n",
						ENTINDEX( ragdoll->entity ), ragdoll->networkID, attackerIndex
					);
				continue;
			}

			entvars_t *attackerVars = &attacker->v;
			CBaseEntity *victim = CBaseEntity::Instance( ragdoll->entity );
			if( !victim )
				continue;

			// DeadTakeDamage accumulates DMG_GIB_CORPSE damage and invokes the
			// normal monster gib path only after the corpse's remaining health
			// has been exhausted.
			ragdoll->nextCollisionDamageTime = gpGlobals->time + 0.25f;
			if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
				ALERT(
					at_console,
					"[NPC physics] Applying collision damage: entity=%d id=%d jolt-body=%u inflictor=%d attacker=%d delta-v=%.1f damage=%.1f health=%.1f.\n",
					ENTINDEX( ragdoll->entity ), ragdoll->networkID,
					event.ragdollBodyID.GetIndex(), ENTINDEX( attacker ),
					ENTINDEX( attacker ), event.velocityLoss, damage,
					ragdoll->entity->v.health
				);
			victim->TakeDamage( attackerVars, attackerVars, damage, DMG_GIB_CORPSE );

			if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
				ALERT(
					at_console,
					"[NPC physics] Collision impact damage applied: entity=%d id=%d jolt-body=%u attacker=%d delta-v=%.1f damage=%.1f health=%.1f.\n",
					ENTINDEX( ragdoll->entity ), ragdoll->networkID,
					event.ragdollBodyID.GetIndex(), ENTINDEX( attacker ),
					event.velocityLoss, damage, ragdoll->entity->v.health
				);
		}
	}

	static void NPCPhysics_RemoveGibbedRagdolls()
	{
		for( size_t i = g_ragdolls.size(); i > 0; --i )
		{
			NPCPhysicsRagdoll &ragdoll = g_ragdolls[i - 1];
			// The ragdoll carrier is intentionally SOLID_NOT from the moment
			// its Jolt bodies are created. Only remove it when the edict is gone,
			// its serial changes, or the normal gib path hides it with EF_NODRAW.
			if( !ragdoll.entity || ragdoll.entity->free ||
				ragdoll.entity->serialnumber != ragdoll.serialNumber ||
				( ragdoll.entity->v.effects & EF_NODRAW ) )
			{
				NPCPhysics_DestroyRagdoll( ragdoll );
				g_ragdolls.erase( g_ragdolls.begin() + ( i - 1 ) );
			}
		}
	}

	static void NPCPhysics_EndFrame( void )
	{
		if( !g_npcPhysics.initialized || !gpGlobals )
			return;

		g_npcPhysicsContactDebugEnabled =
			NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f;

		NPCPhysics_RemoveGibbedRagdolls();
		if( g_ragdolls.empty() )
		{
			g_npcPhysics.contactListener.Clear();
			NPCPhysics_DestroyActorColliders();
			return;
		}

		float gravity = NPCPhysics_GetCvar( "sv_gravity", 800.0f );
		if( gravity < 0.0f )
			gravity = 800.0f;
		g_npcPhysics.system->SetGravity( Vec3( 0.0f, 0.0f, -gravity ) );

		NPCPhysics_UpdateBrushColliders();
		NPCPhysics_UpdateActorColliders();

		float frameTime = gpGlobals->frametime;
		if( frameTime < 0.0f )
			frameTime = 0.0f;
		if( frameTime > 0.1f )
			frameTime = 0.1f;

		g_npcPhysics.fixedAccumulator += frameTime;
		const float maxAccumulatedTime = RAGDOLL_FIXED_STEP * RAGDOLL_MAX_STEPS_PER_FRAME;
		if( g_npcPhysics.fixedAccumulator > maxAccumulatedTime )
			g_npcPhysics.fixedAccumulator = maxAccumulatedTime;

		BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
		int steps = 0;
		while( g_npcPhysics.fixedAccumulator >= RAGDOLL_FIXED_STEP &&
			steps < RAGDOLL_MAX_STEPS_PER_FRAME )
		{
			g_npcPhysics.system->Update(
				RAGDOLL_FIXED_STEP,
				1,
				g_npcPhysics.tempAllocator,
				&g_npcPhysics.jobs
			);

			NPCPhysics_ProcessPlayerPushLimits( bodyInterface );
			g_npcPhysics.fixedAccumulator -= RAGDOLL_FIXED_STEP;
			++steps;
		}

		NPCPhysics_ProcessContactDiagnostics();
		NPCPhysics_ProcessCollisionDamage();
		NPCPhysics_RemoveGibbedRagdolls();
		if( g_ragdolls.empty() )
		{
			NPCPhysics_DestroyActorColliders();
			return;
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

void NPCPhysics_SaveRagdollState( edict_t *ent )
{
	NPCPhysics_SaveRagdollStateInternal( ent );
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
	const float *targetPoint,
	float radius,
	float blastDamage )
{
	const float blastScale = NPCPhysics_GetCvar( "sv_npc_vphysics_blast_scale", 1.0f );
	if( !NPCPhysics_Enabled() || !g_npcPhysics.initialized || g_ragdolls.empty() ||
		!targetEntity || targetEntity->free || !origin || !targetPoint ||
		!std::isfinite( radius ) || radius <= 0.001f ||
		!std::isfinite( blastDamage ) || blastDamage <= 0.0f ||
		!std::isfinite( blastScale ) || blastScale <= 0.0f ||
		!std::isfinite( origin[0] ) || !std::isfinite( origin[1] ) ||
		!std::isfinite( origin[2] ) ||
		!std::isfinite( targetPoint[0] ) || !std::isfinite( targetPoint[1] ) ||
		!std::isfinite( targetPoint[2] ) )
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
		targetRagdoll->bodyMasses.size() != static_cast<size_t>( targetRagdoll->bodyCount ) ||
		targetRagdoll->rootElement < 0 ||
		targetRagdoll->rootElement >= targetRagdoll->bodyCount )
		return 0;

	BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
	const BodyID rootBody = targetRagdoll->physicsRagdoll->GetBodyID( targetRagdoll->rootElement );
	if( rootBody.IsInvalid() )
		return 0;

	float totalRagdollMass = 0.0f;
	for( size_t i = 0; i < targetRagdoll->bodyMasses.size(); ++i )
	{
		const float mass = targetRagdoll->bodyMasses[i];
		if( std::isfinite( mass ) && mass > 0.0f )
			totalRagdollMass += mass;
	}

	// A ragdoll is a constrained group of independent bodies. Sending the
	// entire character impulse into the small pelvis body saturates its linear
	// velocity limit and creates a large off-center torque that the joints
	// and floor contacts immediately fight. Apply most of the same total
	// momentum across all body masses (a shared character-level delta-v), then
	// apply a smaller share to the body nearest BodyTarget for a localized
	// rotational response.
	Vec3 blastDirection(
		targetPoint[0] - origin[0],
		targetPoint[1] - origin[1],
		targetPoint[2] - origin[2] );
	const float directionLength = blastDirection.Length();
	if( !std::isfinite( directionLength ) || directionLength <= 0.001f )
		blastDirection = Vec3( 0.0f, 0.0f, 1.0f );
	else
		blastDirection /= directionLength;

	// Source's explosive force uses a 75 kg reference character at
	// 4 units/sec per damage point, capped at a 400 units/sec reference speed.
	const float baseImpulse = (std::min)( blastDamage * 300.0f, 30000.0f );
	const float randomScale = RANDOM_FLOAT( 0.85f, 1.15f );
	const float scaledImpulse = baseImpulse * randomScale * blastScale;
	if( !std::isfinite( scaledImpulse ) || scaledImpulse <= 0.0f )
		return 0;

	const Vec3 impulse = blastDirection * scaledImpulse;
	const Vec3 distributedImpulse = impulse * 0.85f;
	const Vec3 sharedVelocityDelta = distributedImpulse / totalRagdollMass;

	// BodyTarget is normally around the torso. Find the physical part closest
	// to that point instead of always striking the pelvis.
	int impactBodyIndex = targetRagdoll->rootElement;
	float closestDistanceSquared = FLT_MAX;
	for( int body = 0; body < targetRagdoll->bodyCount; ++body )
	{
		const BodyID bodyID = targetRagdoll->physicsRagdoll->GetBodyID( body );
		if( bodyID.IsInvalid() )
			continue;

		const RVec3 position = bodyInterface.GetPosition( bodyID );
		const Vec3 offset(
			targetPoint[0] - float( position.GetX() ),
			targetPoint[1] - float( position.GetY() ),
			targetPoint[2] - float( position.GetZ() ) );
		const float distanceSquared = offset.LengthSq();
		if( std::isfinite( distanceSquared ) && distanceSquared < closestDistanceSquared )
		{
			closestDistanceSquared = distanceSquared;
			impactBodyIndex = body;
		}
	}

	const BodyID impactBody = targetRagdoll->physicsRagdoll->GetBodyID( impactBodyIndex );
	const float impactBodyMass = targetRagdoll->bodyMasses[impactBodyIndex];
	if( impactBody.IsInvalid() || !std::isfinite( impactBodyMass ) || impactBodyMass <= 0.0f )
		return 0;

	const Vec3 velocityBefore = bodyInterface.GetLinearVelocity( impactBody );
	const Vec3 angularBefore = bodyInterface.GetAngularVelocity( impactBody );

	for( int body = 0; body < targetRagdoll->bodyCount; ++body )
	{
		const float mass = targetRagdoll->bodyMasses[body];
		const BodyID bodyID = targetRagdoll->physicsRagdoll->GetBodyID( body );
		if( bodyID.IsInvalid() || !std::isfinite( mass ) || mass <= 0.0f )
			continue;

		const RVec3 position = bodyInterface.GetPosition( bodyID );
		const Vec3 bodyImpulse = sharedVelocityDelta * mass;
		bodyInterface.ActivateBody( bodyID );
		bodyInterface.AddImpulse( bodyID, bodyImpulse, position );
	}

	// Keep 15% of the total impulse localized at BodyTarget. Clamping the
	// lever arm prevents a distant explosion origin from spinning the pelvis
	// at its angular-velocity ceiling.
	const RVec3 impactPosition = bodyInterface.GetPosition( impactBody );
	Vec3 leverArm(
		targetPoint[0] - float( impactPosition.GetX() ),
		targetPoint[1] - float( impactPosition.GetY() ),
		targetPoint[2] - float( impactPosition.GetZ() ) );
	const float leverLength = leverArm.Length();
	if( leverLength > 16.0f )
		leverArm *= 16.0f / leverLength;
	const RVec3 applicationPoint(
		float( impactPosition.GetX() ) + leverArm.GetX(),
		float( impactPosition.GetY() ) + leverArm.GetY(),
		float( impactPosition.GetZ() ) + leverArm.GetZ() );

	const Vec3 localizedImpulse = impulse * 0.15f;
	bodyInterface.ActivateBody( impactBody );
	bodyInterface.AddImpulse( impactBody, localizedImpulse, applicationPoint );

	const Vec3 velocityAfter = bodyInterface.GetLinearVelocity( impactBody );
	const Vec3 angularAfter = bodyInterface.GetAngularVelocity( impactBody );
	const RVec3 bodyPosition = bodyInterface.GetPosition( impactBody );
	const RVec3 rootPosition = bodyInterface.GetPosition( rootBody );
	targetRagdoll->blastDebugBodyID = impactBody;
	targetRagdoll->blastDebugBodyIndex = impactBodyIndex;
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
			ENTINDEX( targetEntity ), impactBodyIndex,
			impulse.GetX(), impulse.GetY(), impulse.GetZ(),
			velocityBefore.GetX(), velocityBefore.GetY(), velocityBefore.GetZ(),
			velocityAfter.GetX(), velocityAfter.GetY(), velocityAfter.GetZ(),
			angularBefore.GetX(), angularBefore.GetY(), angularBefore.GetZ(),
			angularAfter.GetX(), angularAfter.GetY(), angularAfter.GetZ()
		);

	if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
		ALERT(
			at_console,
			"[NPC blast] Distributed character impulse: ent=%d id=%d body=%d mass=%.1f total-mass=%.1f damage=%.1f radius=%.1f impulse=%.1f random=%.2f scale=%.2f origin=(%.1f %.1f %.1f) target=(%.1f %.1f %.1f) dir=(%.2f %.2f %.2f).\n",
			ENTINDEX( targetEntity ), targetRagdoll->networkID,
			impactBodyIndex, impactBodyMass, totalRagdollMass,
			blastDamage, radius, scaledImpulse, randomScale, blastScale,
			origin[0], origin[1], origin[2],
			targetPoint[0], targetPoint[1], targetPoint[2],
			blastDirection.GetX(), blastDirection.GetY(), blastDirection.GetZ()
		);

	if( gpGlobals && targetRagdoll->nextPoseTime > gpGlobals->time )
		targetRagdoll->nextPoseTime = gpGlobals->time;

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
		impulseMagnitude < 0.0f )
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

	if( !g_npcPhysics.system->GetNarrowPhaseQuery().CastRay( ray, hit ) )
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
	if( impulseMagnitude > 0.0f )
	{
		JPH::Vec3 impulse = ( rayDirection / rayLength ) * impulseMagnitude;
		impulse = NPCPhysics_ClampVector( impulse, 3000.0f );
		JPH::BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
		// Source's client ragdoll applies a localized force to the struck physics
		// object. Explicit activation guarantees that a sleeping Jolt limb wakes.
		bodyInterface.ActivateBody( hit.mBodyID );
		bodyInterface.AddImpulse( hit.mBodyID, impulse, impactPoint );
	}

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

int NPCPhysics_ApplyImpactImpulseToRagdoll(
	edict_t *targetEntity,
	const float *impactPosition,
	const float *direction,
	float impulseMagnitude,
	const char *impactName )
{
	if( !NPCPhysics_Enabled() || !g_npcPhysics.initialized || !targetEntity ||
		targetEntity->free || !impactPosition || !direction ||
		!std::isfinite( impulseMagnitude ) || impulseMagnitude <= 0.0f )
		return FALSE;

	NPCPhysicsRagdoll *targetRagdoll = NULL;
	for( size_t i = 0; i < g_ragdolls.size(); ++i )
	{
		NPCPhysicsRagdoll &ragdoll = g_ragdolls[i];
		if( ragdoll.entity == targetEntity &&
			ragdoll.serialNumber == targetEntity->serialnumber &&
			ragdoll.addedToPhysicsSystem &&
			ragdoll.physicsRagdoll.GetPtr() )
		{
			targetRagdoll = &ragdoll;
			break;
		}
	}
	if( !targetRagdoll || targetRagdoll->bodyCount <= 0 )
		return FALSE;

	Vec3 impulseDirection( direction[0], direction[1], direction[2] );
	if( !std::isfinite( impulseDirection.GetX() ) ||
		!std::isfinite( impulseDirection.GetY() ) ||
		!std::isfinite( impulseDirection.GetZ() ) )
		return FALSE;
	const float directionLength = impulseDirection.Length();
	if( !std::isfinite( directionLength ) || directionLength <= 0.001f )
		return FALSE;
	impulseDirection = impulseDirection * ( 1.0f / directionLength );

	Vec3 impulse = impulseDirection * impulseMagnitude;
	impulse = NPCPhysics_ClampVector( impulse, 3000.0f );
	if( impulse.Length() <= 1.0e-3f )
		return FALSE;

	BodyInterface &bodyInterface = g_npcPhysics.system->GetBodyInterface();
	int impactBodyIndex = -1;
	float closestDistanceSquared = FLT_MAX;
	RVec3 closestBodyPosition( 0.0, 0.0, 0.0 );
	for( int body = 0; body < targetRagdoll->bodyCount; ++body )
	{
		const BodyID bodyID = targetRagdoll->physicsRagdoll->GetBodyID( body );
		const RVec3 bodyPosition = bodyInterface.GetPosition( bodyID );
		const float dx = float( bodyPosition.GetX() ) - impactPosition[0];
		const float dy = float( bodyPosition.GetY() ) - impactPosition[1];
		const float dz = float( bodyPosition.GetZ() ) - impactPosition[2];
		const float distanceSquared = dx * dx + dy * dy + dz * dz;
		if( distanceSquared < closestDistanceSquared )
		{
			closestDistanceSquared = distanceSquared;
			impactBodyIndex = body;
			closestBodyPosition = bodyPosition;
		}
	}
	if( impactBodyIndex < 0 )
		return FALSE;

	const BodyID impactBody = targetRagdoll->physicsRagdoll->GetBodyID( impactBodyIndex );
	Vec3 leverArm(
		impactPosition[0] - float( closestBodyPosition.GetX() ),
		impactPosition[1] - float( closestBodyPosition.GetY() ),
		impactPosition[2] - float( closestBodyPosition.GetZ() ) );
	const float leverLength = leverArm.Length();
	if( leverLength > 16.0f )
		leverArm *= 16.0f / leverLength;

	const RVec3 applicationPoint(
		float( closestBodyPosition.GetX() ) + leverArm.GetX(),
		float( closestBodyPosition.GetY() ) + leverArm.GetY(),
		float( closestBodyPosition.GetZ() ) + leverArm.GetZ() );

	bodyInterface.ActivateBody( impactBody );
	bodyInterface.AddImpulse( impactBody, impulse, applicationPoint );

	if( gpGlobals && targetRagdoll->nextPoseTime > gpGlobals->time )
		targetRagdoll->nextPoseTime = gpGlobals->time;

	if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
	{
		const float bodyMass = impactBodyIndex < static_cast<int>( targetRagdoll->bodyMasses.size() )
			? targetRagdoll->bodyMasses[impactBodyIndex] : 0.0f;
		ALERT(
			at_console,
			"[NPC %s] Applied deferred lethal-hit impulse: entity=%d id=%d body=%d mass=%.2f impulse=(%.1f %.1f %.1f) point=(%.1f %.1f %.1f) dir=(%.2f %.2f %.2f).\n",
			( impactName && impactName[0] ) ? impactName : "weapon",
			ENTINDEX( targetEntity ), targetRagdoll->networkID,
			impactBodyIndex, bodyMass,
			impulse.GetX(), impulse.GetY(), impulse.GetZ(),
			impactPosition[0], impactPosition[1], impactPosition[2],
			impulseDirection.GetX(), impulseDirection.GetY(), impulseDirection.GetZ()
		);
	}

	return TRUE;
}

int NPCPhysics_ApplyBulletImpulseToRagdoll(
	edict_t *targetEntity,
	const float *impactPosition,
	const float *direction,
	float impulseMagnitude )
{
	return NPCPhysics_ApplyImpactImpulseToRagdoll(
		targetEntity, impactPosition, direction, impulseMagnitude, "bullet" );
}

int NPCPhysics_HasRagdollDefinition( edict_t *ent )
{
	if( !NPCPhysics_Enabled() || !NPCPhysics_IsEligibleMonster( ent ) )
		return FALSE;

	return NPCPhysics_GetRagdollDefinition( ent, false ) ? TRUE : FALSE;
}

int NPCPhysics_RestorePersistentRagdoll( edict_t *ent )
{
	if( !NPCPhysics_Enabled() || !NPCPhysics_IsEligibleMonster( ent ) ||
		NPCPhysics_IsRagdollEntity( ent ) )
		return FALSE;

	CBaseMonster *monster = static_cast<CBaseMonster *>( CBaseEntity::Instance( ent ) );
	if( !monster )
		return FALSE;

	const bool hasAnyPersistentRagdollData =
		monster->m_iNpcRagdollPersistVersion != 0 ||
		monster->m_iNpcRagdollPersistBoneCount != 0 ||
		monster->m_iNpcRagdollPersistBodyCount != 0;
	if( monster->m_iNpcRagdollPersistVersion != NPC_RAGDOLL_PERSIST_VERSION ||
		monster->m_iNpcRagdollPersistBoneCount <= 0 ||
		monster->m_iNpcRagdollPersistBoneCount > NPC_RAGDOLL_PERSIST_MAX_STUDIO_BONES ||
		monster->m_iNpcRagdollPersistBodyCount <= 0 ||
		monster->m_iNpcRagdollPersistBodyCount > NPC_RAGDOLL_PERSIST_MAX_BODIES )
	{
		if( hasAnyPersistentRagdollData &&
			NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
			ALERT( at_console,
				"[NPC physics] Entity %d has incomplete ragdoll snapshot data (version=%d bones=%d bodies=%d).\n",
				ENTINDEX( ent ), monster->m_iNpcRagdollPersistVersion,
				monster->m_iNpcRagdollPersistBoneCount, monster->m_iNpcRagdollPersistBodyCount );
		return FALSE;
	}

	if( NPCPhysics_GetCvar( "sv_npc_vphysics_debug", 0.0f ) >= 0.5f )
		ALERT( at_console,
			"[NPC physics] Found transferred ragdoll state: entity=%d bones=%d bodies=%d.\n",
			ENTINDEX( ent ), monster->m_iNpcRagdollPersistBoneCount,
			monster->m_iNpcRagdollPersistBodyCount );

	if( !NPCPhysics_CreateRagdoll( ent, true ) )
	{
		ALERT( at_console,
			"[NPC physics] Could not restore transferred ragdoll for entity %d; keeping saved pose data for retry.\n",
			ENTINDEX( ent ) );
		return FALSE;
	}

	monster->m_iNpcRagdollPersistVersion = 0;
	monster->m_iNpcRagdollPersistBoneCount = 0;
	monster->m_iNpcRagdollPersistBodyCount = 0;
	return TRUE;
}

int NPCPhysics_TryCreateRagdoll( edict_t *ent )
{
	if( !NPCPhysics_Enabled() || !NPCPhysics_IsEligibleMonster( ent ) )
		return FALSE;

	if( !NPCPhysics_CreateRagdoll( ent, false ) )
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
	CVAR_REGISTER( &g_svNpcVPhysicsCrossbowScale );
	CVAR_REGISTER( &g_svNpcVPhysicsBlastScale );
	CVAR_REGISTER( &g_svNpcVPhysicsImpactDamageScale );
	CVAR_REGISTER( &g_svNpcVPhysicsPlayerPushMass );
	CVAR_REGISTER( &g_svNpcVPhysicsPlayerPushSpeed );
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
	NPCPhysics_DestroyAllRagdolls();
	NPCPhysics_DestroyActorColliders();
	NPCPhysics_DestroyBrushColliders();
	NPCPhysics_DestroyWorldCollider();
	g_npcPhysics.contactListener.Clear();
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
	if( g_npcPhysics.system )
		g_npcPhysics.system->SetContactListener( NULL );
	g_npcPhysics.contactListener.Clear();
	NPCPhysics_DestroyAllRagdolls();
	NPCPhysics_DestroyActorColliders();
	NPCPhysics_DestroyBrushColliders();
	NPCPhysics_DestroyWorldCollider();

	// PhysicsSystem owns Jolt's contact caches. Destroy it while the Jolt
	// allocator and registered types are still alive; leaving it as a global
	// value would defer its destructor until dlclose(), after this shutdown.
	g_npcPhysics.system.reset();

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
