#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "ragdoll.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const int MAX_RAGDOLL_ENTITIES = 2048;
static const int MAX_RAGDOLL_BONES = 128;

struct ragdoll_bone_pose_t
{
	float origin[3];
	float rotation[4];
	bool valid;
};

struct ragdoll_entity_pose_t
{
	int ragdollID;
	int boneCount;
	float lastUpdateTime;
	bool active;
	bool renderReported;
	ragdoll_bone_pose_t bones[MAX_RAGDOLL_BONES];
};

static ragdoll_entity_pose_t g_RagdollPoses[MAX_RAGDOLL_ENTITIES];
static bool g_RagdollMessageHooked = false;

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
	if( !buffer || size < 7 )
		return 0;

	BEGIN_READ( buffer, size );

	const int entityIndex = READ_SHORT();
	const int ragdollID = READ_SHORT();
	const int boneCount = READ_BYTE();
	const int startBone = READ_BYTE();
	const int chunkCount = READ_BYTE();

	if( entityIndex <= 0 || entityIndex >= MAX_RAGDOLL_ENTITIES ||
		ragdollID <= 0 ||
		boneCount <= 0 || boneCount > MAX_RAGDOLL_BONES ||
		startBone < 0 || chunkCount <= 0 ||
		startBone + chunkCount > boneCount ||
		size < 7 + chunkCount * 14 )
		return 0;

	ragdoll_entity_pose_t &entry = g_RagdollPoses[entityIndex];
	if( !entry.active || entry.ragdollID != ragdollID || entry.boneCount != boneCount )
	{
		memset( &entry, 0, sizeof( entry ) );
		entry.active = true;
		entry.ragdollID = ragdollID;
		entry.boneCount = boneCount;
		char message[128];
		snprintf( message, sizeof( message ), "[NPC ragdoll] Receiving entity %d, ID %d (%d bones).\n", entityIndex, ragdollID, boneCount );
		ConsolePrint( message );
	}

	entry.lastUpdateTime = gEngfuncs.GetClientTime();

	for( int i = 0; i < chunkCount; ++i )
	{
		ragdoll_bone_pose_t &bone = entry.bones[startBone + i];

		bone.origin[0] = READ_COORD();
		bone.origin[1] = READ_COORD();
		bone.origin[2] = READ_COORD();

		bone.rotation[0] = float( READ_SHORT() ) / 32767.0f;
		bone.rotation[1] = float( READ_SHORT() ) / 32767.0f;
		bone.rotation[2] = float( READ_SHORT() ) / 32767.0f;
		bone.rotation[3] = float( READ_SHORT() ) / 32767.0f;

		float lengthSquared =
			bone.rotation[0] * bone.rotation[0] +
			bone.rotation[1] * bone.rotation[1] +
			bone.rotation[2] * bone.rotation[2] +
			bone.rotation[3] * bone.rotation[3];

		if( lengthSquared < 0.0001f )
		{
			bone.rotation[0] = 0.0f;
			bone.rotation[1] = 0.0f;
			bone.rotation[2] = 0.0f;
			bone.rotation[3] = 1.0f;
		}
		else
		{
			const float inverseLength = 1.0f / sqrtf( lengthSquared );
			for( int axis = 0; axis < 4; ++axis )
				bone.rotation[axis] *= inverseLength;
		}

		bone.valid = true;
	}

	return 1;
}

int Ragdoll_GetBoneTransform( int entityIndex, int boneIndex, float transform[3][4] )
{
	if( !transform || entityIndex <= 0 || entityIndex >= MAX_RAGDOLL_ENTITIES ||
		boneIndex < 0 || boneIndex >= MAX_RAGDOLL_BONES )
		return 0;

	ragdoll_entity_pose_t &entry = g_RagdollPoses[entityIndex];
	if( !entry.active || entry.ragdollID <= 0 ||
		boneIndex >= entry.boneCount || !entry.bones[boneIndex].valid )
		return 0;

	// Snapshots are sent at 20 Hz. Expire stale mappings so a recycled entity
	// slot cannot inherit an old ragdoll pose indefinitely.
	if( gEngfuncs.GetClientTime() - entry.lastUpdateTime > 0.25f )
		return 0;

	// Do not mix a partial first network chunk with GoldSrc local animation
	// matrices. Wait until the first complete skeletal snapshot has arrived.
	for( int i = 0; i < entry.boneCount; ++i )
	{
		if( !entry.bones[i].valid )
			return 0;
	}

	if( !entry.renderReported )
	{
		char message[128];
		snprintf( message, sizeof( message ), "[NPC ragdoll] Applying pose for entity %d, ID %d.\n", entityIndex, entry.ragdollID );
		ConsolePrint( message );
		entry.renderReported = true;
	}

	const ragdoll_bone_pose_t &bone = entry.bones[boneIndex];
	const float x = bone.rotation[0];
	const float y = bone.rotation[1];
	const float z = bone.rotation[2];
	const float w = bone.rotation[3];

	// Same 3x4 quaternion matrix layout consumed by StudioModelRenderer.
	transform[0][0] = 1.0f - 2.0f * ( y * y + z * z );
	transform[0][1] = 2.0f * ( x * y - z * w );
	transform[0][2] = 2.0f * ( x * z + y * w );
	transform[0][3] = bone.origin[0];

	transform[1][0] = 2.0f * ( x * y + z * w );
	transform[1][1] = 1.0f - 2.0f * ( x * x + z * z );
	transform[1][2] = 2.0f * ( y * z - x * w );
	transform[1][3] = bone.origin[1];

	transform[2][0] = 2.0f * ( x * z - y * w );
	transform[2][1] = 2.0f * ( y * z + x * w );
	transform[2][2] = 1.0f - 2.0f * ( x * x + y * y );
	transform[2][3] = bone.origin[2];

	return 1;
}
