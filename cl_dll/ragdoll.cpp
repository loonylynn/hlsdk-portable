#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ragdoll.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const int MAX_RAGDOLL_ENTITIES = 2048;
static const int MAX_RAGDOLL_BONES = 128;
static const float RAGDOLL_SNAPSHOT_INTERVAL = 0.05f;
static const float RAGDOLL_STALE_TIMEOUT = 0.25f;

// The server marks only the original zombie entity as a ragdoll carrier.
static const int RAGDOLL_ENTITY_MARKER = 1;

struct ragdoll_bone_pose_t
{
	float origin[3];
	float rotation[4];
	float previousOrigin[3];
	float previousRotation[4];
	float poseTime;
	bool valid;
};

struct ragdoll_entity_pose_t
{
	int ragdollID;
	int studioBoneCount;
	int bodyCount;
	int receivedBodyCount;
	float lastUpdateTime;
	bool active;
	bool initialSnapshotComplete;
	bool renderReported;
	ragdoll_bone_pose_t bones[MAX_RAGDOLL_BONES];
};

static ragdoll_entity_pose_t g_RagdollPoses[MAX_RAGDOLL_ENTITIES];
static bool g_RagdollMessageHooked = false;

static float Ragdoll_Clamp01( float value )
{
	if( value < 0.0f )
		return 0.0f;
	if( value > 1.0f )
		return 1.0f;
	return value;
}

static void Ragdoll_NlerpQuaternion(
	const float start[4],
	const float target[4],
	float fraction,
	float output[4] )
{
	float adjustedTarget[4];
	float dot = 0.0f;
	float lengthSquared = 0.0f;

	for( int i = 0; i < 4; ++i )
		dot += start[i] * target[i];

	const float sign = dot < 0.0f ? -1.0f : 1.0f;
	for( int i = 0; i < 4; ++i )
	{
		adjustedTarget[i] = target[i] * sign;
		output[i] = start[i] + ( adjustedTarget[i] - start[i] ) * fraction;
		lengthSquared += output[i] * output[i];
	}

	if( lengthSquared < 0.0001f )
	{
		output[0] = 0.0f;
		output[1] = 0.0f;
		output[2] = 0.0f;
		output[3] = 1.0f;
		return;
	}

	const float inverseLength = 1.0f / sqrtf( lengthSquared );
	for( int i = 0; i < 4; ++i )
		output[i] *= inverseLength;
}

static void Ragdoll_GetInterpolatedBonePose(
	const ragdoll_bone_pose_t &bone,
	float now,
	float origin[3],
	float rotation[4] )
{
	const float fraction = Ragdoll_Clamp01(
		( now - bone.poseTime ) / RAGDOLL_SNAPSHOT_INTERVAL
	);

	for( int axis = 0; axis < 3; ++axis )
	{
		origin[axis] = bone.previousOrigin[axis] +
			( bone.origin[axis] - bone.previousOrigin[axis] ) * fraction;
	}

	Ragdoll_NlerpQuaternion(
		bone.previousRotation,
		bone.rotation,
		fraction,
		rotation
	);
}

void Ragdoll_ClearAll( void )
{
	memset( g_RagdollPoses, 0, sizeof( g_RagdollPoses ) );
}

void Ragdoll_Init( void )
{
	if( g_RagdollMessageHooked )
		return;

	Ragdoll_ClearAll();
	gEngfuncs.pfnHookUserMsg( "RagdollPose", Ragdoll_MessageHandler );
	g_RagdollMessageHooked = true;
}

int Ragdoll_MessageHandler( const char *, int size, void *buffer )
{
	// Packet header:
	// entity, ragdoll ID, studio bone count, physics body count,
	// starting physics element, element count. Each element carries the
	// studio bone index plus world position and quaternion.
	if( !buffer || size < 8 )
		return 0;

	BEGIN_READ( buffer, size );

	const int entityIndex = READ_SHORT();
	const int ragdollID = READ_SHORT();
	const int studioBoneCount = READ_BYTE();
	const int bodyCount = READ_BYTE();
	const int startBody = READ_BYTE();
	const int chunkCount = READ_BYTE();

	if( entityIndex <= 0 || entityIndex >= MAX_RAGDOLL_ENTITIES ||
		ragdollID <= 0 ||
		studioBoneCount <= 0 || studioBoneCount > MAX_RAGDOLL_BONES ||
		bodyCount <= 0 || bodyCount > studioBoneCount ||
		startBody < 0 || chunkCount <= 0 ||
		startBody + chunkCount > bodyCount ||
		size < 8 + chunkCount * 15 )
		return 0;

	const float now = (float)gEngfuncs.GetClientTime();
	ragdoll_entity_pose_t &entry = g_RagdollPoses[entityIndex];

	if( entry.active &&
		( now < entry.lastUpdateTime || now - entry.lastUpdateTime > RAGDOLL_STALE_TIMEOUT ) )
	{
		memset( &entry, 0, sizeof( entry ) );
	}

	if( !entry.active ||
		entry.ragdollID != ragdollID ||
		entry.studioBoneCount != studioBoneCount ||
		entry.bodyCount != bodyCount )
	{
		memset( &entry, 0, sizeof( entry ) );
		entry.active = true;
		entry.ragdollID = ragdollID;
		entry.studioBoneCount = studioBoneCount;
		entry.bodyCount = bodyCount;

		char message[144];
		snprintf(
			message,
			sizeof( message ),
			"[NPC ragdoll] Receiving entity %d, ID %d (%d/%d simulated bones).\n",
			entityIndex,
			ragdollID,
			bodyCount,
			studioBoneCount
		);
		ConsolePrint( message );
	}

	entry.lastUpdateTime = now;

	for( int i = 0; i < chunkCount; ++i )
	{
		const int boneIndex = READ_BYTE();
		if( boneIndex < 0 || boneIndex >= studioBoneCount )
			return 0;

		ragdoll_bone_pose_t &bone = entry.bones[boneIndex];
		float receivedOrigin[3];
		float receivedRotation[4];

		receivedOrigin[0] = READ_COORD();
		receivedOrigin[1] = READ_COORD();
		receivedOrigin[2] = READ_COORD();

		receivedRotation[0] = float( READ_SHORT() ) / 32767.0f;
		receivedRotation[1] = float( READ_SHORT() ) / 32767.0f;
		receivedRotation[2] = float( READ_SHORT() ) / 32767.0f;
		receivedRotation[3] = float( READ_SHORT() ) / 32767.0f;

		float lengthSquared =
			receivedRotation[0] * receivedRotation[0] +
			receivedRotation[1] * receivedRotation[1] +
			receivedRotation[2] * receivedRotation[2] +
			receivedRotation[3] * receivedRotation[3];

		if( lengthSquared < 0.0001f )
		{
			receivedRotation[0] = 0.0f;
			receivedRotation[1] = 0.0f;
			receivedRotation[2] = 0.0f;
			receivedRotation[3] = 1.0f;
		}
		else
		{
			const float inverseLength = 1.0f / sqrtf( lengthSquared );
			for( int axis = 0; axis < 4; ++axis )
				receivedRotation[axis] *= inverseLength;
		}

		if( bone.valid )
		{
			float interpolatedOrigin[3];
			float interpolatedRotation[4];
			Ragdoll_GetInterpolatedBonePose(
				bone,
				now,
				interpolatedOrigin,
				interpolatedRotation
			);

			memcpy( bone.previousOrigin, interpolatedOrigin, sizeof( interpolatedOrigin ) );
			memcpy( bone.previousRotation, interpolatedRotation, sizeof( interpolatedRotation ) );
		}
		else
		{
			memcpy( bone.previousOrigin, receivedOrigin, sizeof( receivedOrigin ) );
			memcpy( bone.previousRotation, receivedRotation, sizeof( receivedRotation ) );
			++entry.receivedBodyCount;
		}

		memcpy( bone.origin, receivedOrigin, sizeof( receivedOrigin ) );
		memcpy( bone.rotation, receivedRotation, sizeof( receivedRotation ) );
		bone.poseTime = now;
		bone.valid = true;
	}

	entry.initialSnapshotComplete = ( entry.receivedBodyCount >= entry.bodyCount );
	return 1;
}

int Ragdoll_GetBoneTransform(
	int entityIndex,
	int ragdollMarker,
	int boneIndex,
	float transform[3][4] )
{
	if( !transform ||
		entityIndex <= 0 ||
		entityIndex >= MAX_RAGDOLL_ENTITIES ||
		ragdollMarker != RAGDOLL_ENTITY_MARKER ||
		boneIndex < 0 ||
		boneIndex >= MAX_RAGDOLL_BONES )
		return 0;

	ragdoll_entity_pose_t &entry = g_RagdollPoses[entityIndex];
	if( !entry.active ||
		entry.ragdollID <= 0 ||
		boneIndex >= entry.studioBoneCount ||
		!entry.initialSnapshotComplete ||
		!entry.bones[boneIndex].valid )
		return 0;

	const float now = (float)gEngfuncs.GetClientTime();
	if( now < entry.lastUpdateTime || now - entry.lastUpdateTime > RAGDOLL_STALE_TIMEOUT )
	{
		memset( &entry, 0, sizeof( entry ) );
		return 0;
	}

	if( !entry.renderReported )
	{
		char message[128];
		snprintf(
			message,
			sizeof( message ),
			"[NPC ragdoll] Applying articulated pose for entity %d, ID %d.\n",
			entityIndex,
			entry.ragdollID
		);
		ConsolePrint( message );
		entry.renderReported = true;
	}

	const ragdoll_bone_pose_t &bone = entry.bones[boneIndex];
	float origin[3];
	float rotation[4];
	Ragdoll_GetInterpolatedBonePose( bone, now, origin, rotation );

	const float x = rotation[0];
	const float y = rotation[1];
	const float z = rotation[2];
	const float w = rotation[3];

	transform[0][0] = 1.0f - 2.0f * ( y * y + z * z );
	transform[0][1] = 2.0f * ( x * y - z * w );
	transform[0][2] = 2.0f * ( x * z + y * w );
	transform[0][3] = origin[0];

	transform[1][0] = 2.0f * ( x * y + z * w );
	transform[1][1] = 1.0f - 2.0f * ( x * x + z * z );
	transform[1][2] = 2.0f * ( y * z - x * w );
	transform[1][3] = origin[1];

	transform[2][0] = 2.0f * ( x * z - y * w );
	transform[2][1] = 2.0f * ( y * z + x * w );
	transform[2][2] = 1.0f - 2.0f * ( x * x + y * y );
	transform[2][3] = origin[2];

	return 1;
}
