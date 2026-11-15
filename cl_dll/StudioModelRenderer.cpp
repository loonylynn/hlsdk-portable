//========= Copyright (c) 1996-2002, Valve LLC, All rights reserved. ============
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================

// studio_model.cpp
// routines for setting up to draw 3DStudio models

#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "com_model.h"
#include "studio.h"
#include "entity_state.h"
#include "cl_entity.h"
#include "dlight.h"
#include "triangleapi.h"
#include "byteswap.h"
#include "pm_defs.h"
#include "event_api.h"
#include "xash_render_api.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

#include "studio_util.h"
#include "r_studioint.h"

#include "StudioModelRenderer.h"
#include "GameStudioModelRenderer.h"
#include "ragdoll.h"

// Global engine <-> studio model rendering code interface
engine_studio_api_t IEngineStudio;

static vec3_t g_BlobShadowStudioOrigin = { 0.0f, 0.0f, 0.0f };
static int g_BlobShadowStudioEntity = -1;

static cvar_t *cl_blobshadows = NULL;
static cvar_t *cl_blobshadows_opacity = NULL;
static cvar_t *cl_ragdoll_bloodmarks = NULL;
static cvar_t *cl_ragdoll_bloodmarks_debug = NULL;
static model_t *cl_blobshadow_sprite = NULL;

// Minimal Xash sprite metadata mirrors. We temporarily substitute the frame's
// texture index so TriAPI can bind our generated alpha-mask texture.
typedef struct xash_mspriteframe_v37_s
{
	int width;
	int height;
	float up, down, left, right;
	int gl_texturenum;
} xash_mspriteframe_v37_t;

typedef struct xash_mspriteframedesc_v37_s
{
	int type;
	xash_mspriteframe_v37_t *frameptr;
} xash_mspriteframedesc_v37_t;

typedef struct xash_msprite_v37_s
{
	short type;
	short texFormat;
	int maxwidth;
	int maxheight;
	int numframes;
	int radius;
	int facecull;
	int synctype;
	xash_mspriteframedesc_v37_t frames[1];
} xash_msprite_v37_t;

/*
====================
StudioGetBlobShadowTexture

Create a small white RGBA texture whose alpha is a radial falloff. glow01.spr
is an additive glow sprite with a black background, so it cannot be used as
an ordinary alpha-blended shadow mask.
====================
*/
static int StudioGetBlobShadowTexture( void )
{
	static const char textureName[] = "cl_blobshadow_radial_v1";
	static byte pixels[64 * 64 * 4];
	static qboolean pixelsBuilt = false;
	int texture;
	int x, y;

	if( !gXashRenderAPI || !gXashRenderAPI->GL_FindTexture ||
		!gXashRenderAPI->GL_CreateTexture )
		return 0;

	// Texture indices belong to the active renderer resource table and can
	// change when a map tears down/reloads resources. Always resolve by name.
	texture = gXashRenderAPI->GL_FindTexture( textureName );
	if( texture > 0 )
		return texture;

	if( !pixelsBuilt )
	{
		for( y = 0; y < 64; y++ )
		{
			for( x = 0; x < 64; x++ )
			{
				float nx = ((float)x + 0.5f - 32.0f) / 32.0f;
				float ny = ((float)y + 0.5f - 32.0f) / 32.0f;
				float radius = sqrtf( nx * nx + ny * ny );
				float fade = 0.0f;
				int alpha;
				int offset = ( y * 64 + x ) * 4;

				if( radius < 1.0f )
				{
					float edge = 1.0f - radius;
					fade = edge * edge * ( 3.0f - 2.0f * edge );
				}

				alpha = (int)( fade * 255.0f + 0.5f );
				pixels[offset + 0] = 255;
				pixels[offset + 1] = 255;
				pixels[offset + 2] = 255;
				pixels[offset + 3] = (byte)alpha;
			}
		}
		pixelsBuilt = true;
	}

	// TF_CLAMP | TF_NOMIPMAP | TF_HAS_ALPHA from Xash render_api.h.
	texture = gXashRenderAPI->GL_CreateTexture(
		textureName, 64, 64, pixels, ( 1 << 11 ) | ( 1 << 12 ) | ( 1 << 16 )
	);

	return texture;
}

/*
====================
StudioBindBlobShadowTexture

TriAPI's SpriteTexture accepts a model rather than a raw GL texture ID.
The studio API's Mod_Extradata is studio-only in Xash, so access the sprite's
generic model cache directly and temporarily replace frame zero's texture.
====================
*/
static qboolean StudioBindBlobShadowTexture( triangleapi_t *tri, int texture )
{
	xash_msprite_v37_t *sprite;
	xash_mspriteframe_v37_t *frame;
	int originalTexture;
	int result;

	if( !tri || !tri->SpriteTexture || !cl_blobshadow_sprite ||
		!cl_blobshadow_sprite->cache.data )
		return false;

	sprite = (xash_msprite_v37_t *)cl_blobshadow_sprite->cache.data;
	if( sprite->numframes < 1 || sprite->frames[0].type != 0 )
		return false;

	frame = sprite->frames[0].frameptr;
	if( !frame )
		return false;

	originalTexture = frame->gl_texturenum;
	frame->gl_texturenum = texture;
	result = tri->SpriteTexture( cl_blobshadow_sprite, 0 );
	frame->gl_texturenum = originalTexture;

	return result ? true : false;
}

/*
====================
StudioGetRagdollBloodTexture

Build a small irregular alpha mask for triangle-clipped studio impact decals.
The renderer tints this white texture with the model's blood palette.
====================
*/
static int StudioGetRagdollBloodTexture( void )
{
	static const char textureName[] = "cl_ragdoll_bloodmark_v1";
	static byte pixels[64 * 64 * 4];
	int texture;

	if( !gXashRenderAPI || !gXashRenderAPI->GL_FindTexture ||
		!gXashRenderAPI->GL_CreateTexture )
		return 0;

	texture = gXashRenderAPI->GL_FindTexture( textureName );
	if( texture > 0 )
		return texture;

	for( int y = 0; y < 64; ++y )
	{
		for( int x = 0; x < 64; ++x )
		{
			const float nx = ( (float)x + 0.5f - 32.0f ) / 32.0f;
			const float ny = ( (float)y + 0.5f - 32.0f ) / 32.0f;
			const float radius = sqrtf( nx * nx + ny * ny );
			const float angle = atan2f( ny, nx );
			const float edge = 0.74f +
				0.12f * sinf( angle * 5.0f ) +
				0.06f * cosf( angle * 11.0f ) +
				0.035f * sinf( angle * 17.0f );
			float alpha = ( edge - radius ) * 15.0f;
			if( alpha < 0.0f ) alpha = 0.0f;
			if( alpha > 1.0f ) alpha = 1.0f;
			alpha = alpha * alpha * ( 3.0f - 2.0f * alpha );

			const int offset = ( y * 64 + x ) * 4;
			pixels[offset + 0] = 255;
			pixels[offset + 1] = 255;
			pixels[offset + 2] = 255;
			pixels[offset + 3] = (byte)( alpha * 230.0f + 0.5f );
		}
	}

	// TF_CLAMP | TF_NOMIPMAP | TF_HAS_ALPHA from Xash render_api.h.
	return gXashRenderAPI->GL_CreateTexture(
		textureName, 64, 64, pixels, ( 1 << 11 ) | ( 1 << 12 ) | ( 1 << 16 )
	);
}

/*
====================
StudioForEachRagdollModelTriangle

Walk the active GoldSrc studio submodels and decode their triangle
strip/fan command streams. Vertices are transformed with the exact bone
matrices used by the studio renderer, including the replicated Jolt pose.
====================
*/
/*
 * Ragdoll studio decals are clipped only once per impact. Each clipped
 * polygon stores barycentric coordinates inside its source studio triangle;
 * rendering then transforms only those triangles' three vertices, rather
 * than walking the entire model for every decal on every frame.
 */
#define STUDIO_RAGDOLL_DECAL_CACHE_COUNT 128
#define STUDIO_RAGDOLL_DECAL_MAX_FRAGMENTS 32
#define STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS 10

typedef struct studio_ragdoll_triangle_s
{
	float world[3][3];
	float local[3][3];
	int bone[3];
} studio_ragdoll_triangle_t;

typedef void (*studio_ragdoll_triangle_callback_t)(
	const studio_ragdoll_triangle_t *triangle, void *context );

static qboolean StudioGetRagdollWorldVertex(
	CStudioModelRenderer *renderer,
	const byte *vertexBones,
	const vec3_t *vertices,
	int vertexIndex,
	vec3_t localPosition,
	vec3_t worldPosition,
	int *boneIndex )
{
	if( !renderer || !renderer->m_pStudioHeader || !renderer->m_pbonetransform ||
		!renderer->m_pSubModel || !vertexBones || !vertices ||
		vertexIndex < 0 || vertexIndex >= renderer->m_pSubModel->numverts )
		return false;

	const int index = vertexBones[vertexIndex];
	if( index < 0 || index >= renderer->m_pStudioHeader->numbones )
		return false;

	if( localPosition )
		VectorCopy( vertices[vertexIndex], localPosition );
	if( worldPosition )
		VectorTransform(
			vertices[vertexIndex],
			(*renderer->m_pbonetransform)[index],
			worldPosition
		);
	if( boneIndex )
		*boneIndex = index;
	return true;
}

static void StudioForEachRagdollModelTriangle(
	CStudioModelRenderer *renderer,
	studio_ragdoll_triangle_callback_t callback,
	void *context )
{
	if( !renderer || !renderer->m_pStudioHeader ||
		!IEngineStudio.StudioSetupModel || !callback )
		return;

	mstudiobodyparts_t *savedBodyPart = renderer->m_pBodyPart;
	mstudiomodel_t *savedSubModel = renderer->m_pSubModel;

	const int bodyPartCount = renderer->m_pStudioHeader->numbodyparts;
	if( bodyPartCount <= 0 || bodyPartCount > MAXSTUDIOBODYPARTS )
		return;

	for( int bodyPartIndex = 0; bodyPartIndex < bodyPartCount; ++bodyPartIndex )
	{
		void *bodyPart = NULL;
		void *subModel = NULL;
		IEngineStudio.StudioSetupModel( bodyPartIndex, &bodyPart, &subModel );
		renderer->m_pBodyPart = (mstudiobodyparts_t *)bodyPart;
		renderer->m_pSubModel = (mstudiomodel_t *)subModel;

		mstudiomodel_t *model = renderer->m_pSubModel;
		if( !model || model->numverts <= 0 ||
			model->numverts > MAXSTUDIOVERTS ||
			model->nummesh <= 0 || model->nummesh > MAXSTUDIOMESHES ||
			model->vertinfoindex <= 0 || model->vertindex <= 0 ||
			model->meshindex <= 0 )
			continue;

		const byte *vertexBones =
			(const byte *)renderer->m_pStudioHeader + model->vertinfoindex;
		const vec3_t *vertices =
			(const vec3_t *)((const byte *)renderer->m_pStudioHeader + model->vertindex);
		const mstudiomesh_t *meshes =
			(const mstudiomesh_t *)((const byte *)renderer->m_pStudioHeader + model->meshindex);

		for( int meshIndex = 0; meshIndex < model->nummesh; ++meshIndex )
		{
			const mstudiomesh_t *mesh = &meshes[meshIndex];
			if( mesh->numtris <= 0 || mesh->numtris > MAXSTUDIOTRIANGLES ||
				mesh->triindex <= 0 )
				continue;

			const short *commands =
				(const short *)((const byte *)renderer->m_pStudioHeader + mesh->triindex);
			int trianglesVisited = 0;

			for( int commandIndex = 0;
				commandIndex <= mesh->numtris && trianglesVisited < mesh->numtris;
				++commandIndex )
			{
				const int signedVertexCount = *commands++;
				if( signedVertexCount == 0 )
					break;

				const qboolean isFan = signedVertexCount < 0;
				const int vertexCount = isFan ? -signedVertexCount : signedVertexCount;
				if( vertexCount < 3 || vertexCount > MAXSTUDIOVERTS )
					break;

				const mstudiotrivert_t *commandVertices =
					(const mstudiotrivert_t *)commands;

				for( int vertex = 2;
					vertex < vertexCount && trianglesVisited < mesh->numtris;
					++vertex )
				{
					int indices[3];
					if( isFan )
					{
						indices[0] = 0;
						indices[1] = vertex - 1;
						indices[2] = vertex;
					}
					else if( vertex & 1 )
					{
						indices[0] = vertex - 1;
						indices[1] = vertex - 2;
						indices[2] = vertex;
					}
					else
					{
						indices[0] = vertex - 2;
						indices[1] = vertex - 1;
						indices[2] = vertex;
					}

					studio_ragdoll_triangle_t triangle;
					qboolean valid = true;
					for( int corner = 0; corner < 3; ++corner )
					{
						if( !StudioGetRagdollWorldVertex(
								renderer, vertexBones, vertices,
								commandVertices[indices[corner]].vertindex,
								triangle.local[corner],
								triangle.world[corner],
								&triangle.bone[corner] ) )
						{
							valid = false;
							break;
						}
					}

					if( valid )
						callback( &triangle, context );
					++trianglesVisited;
				}

				commands += vertexCount * 4;
			}
		}
	}

	renderer->m_pBodyPart = savedBodyPart;
	renderer->m_pSubModel = savedSubModel;
}

static float StudioDot3( const float *a, const float *b )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void StudioNormalize3( float vector[3] )
{
	const float lengthSquared = StudioDot3( vector, vector );
	if( lengthSquared <= 0.000001f )
		return;

	const float inverseLength = 1.0f / sqrtf( lengthSquared );
	for( int axis = 0; axis < 3; ++axis )
		vector[axis] *= inverseLength;
}

typedef struct studio_ragdoll_decal_vertex_s
{
	float barycentric[3];
	float u, v, depth;
} studio_ragdoll_decal_vertex_t;

typedef struct studio_ragdoll_decal_fragment_s
{
	int bone[3];
	float local[3][3];
	int vertexCount;
	float barycentric[STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS][3];
	float uv[STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS][2];
} studio_ragdoll_decal_fragment_t;

typedef struct studio_ragdoll_mark_cache_s
{
	qboolean valid;
	int entityIndex;
	model_t *model;
	int boneIndex;
	float rayStart[3];
	float worldPosition[3];
	float radius;
	int bloodColor;
	unsigned int age;
	int fragmentCount;
	int projectionAttempts;
	float retryTime;
	studio_ragdoll_decal_fragment_t fragments[STUDIO_RAGDOLL_DECAL_MAX_FRAGMENTS];
} studio_ragdoll_mark_cache_t;

static studio_ragdoll_mark_cache_t
	g_StudioRagdollDecalCache[STUDIO_RAGDOLL_DECAL_CACHE_COUNT];
static unsigned int g_StudioRagdollDecalCacheClock;

typedef struct studio_ragdoll_decal_build_s
{
	studio_ragdoll_mark_cache_t *cache;
	float center[3];
	float normal[3];
	float tangent[3];
	float bitangent[3];
	float radius;
	float depthTolerance;
	int triangleCount;
} studio_ragdoll_decal_build_t;

/*
 * Jolt ragdoll collision shapes are deliberately coarse. Their ray hit can
 * lie several units inside or outside the visible studio skin, so projecting
 * only around the shot line can miss every rendered triangle. Find the
 * nearest point on the currently skinned mesh and use that as the decal
 * anchor; this is the studio-renderer equivalent of Source's model trace
 * fallback when the physics proxy does not exactly match the render mesh.
 */
typedef struct studio_ragdoll_surface_anchor_s
{
	const float *point;
	float closest[3];
	float normal[3];
	float distanceSquared;
	int boneIndex;
	int triangleCount;
	qboolean found;
} studio_ragdoll_surface_anchor_t;

static qboolean StudioVectorIsFinite3( const float vector[3] )
{
	for( int axis = 0; axis < 3; ++axis )
	{
		if( !( vector[axis] == vector[axis] ) ||
			fabsf( vector[axis] ) > 1.0e20f )
			return false;
	}
	return true;
}

static double StudioDot3Double( const double *a, const double *b )
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void StudioClosestPointOnRagdollSegment(
	const float point[3],
	const float a[3],
	const float b[3],
	float closest[3] )
{
	double direction[3], offset[3];
	for( int axis = 0; axis < 3; ++axis )
	{
		direction[axis] = (double)b[axis] - (double)a[axis];
		offset[axis] = (double)point[axis] - (double)a[axis];
	}
	const double lengthSquared = StudioDot3Double( direction, direction );
	double fraction = 0.0;
	if( lengthSquared > 1.0e-24 )
	{
		fraction = StudioDot3Double( offset, direction ) / lengthSquared;
		if( fraction < 0.0 ) fraction = 0.0;
		if( fraction > 1.0 ) fraction = 1.0;
	}
	for( int axis = 0; axis < 3; ++axis )
		closest[axis] = (float)( (double)a[axis] + direction[axis] * fraction );
}

static qboolean StudioClosestPointOnRagdollTriangle(
	const float point[3],
	const float a[3],
	const float b[3],
	const float c[3],
	float closest[3] )
{
	if( !StudioVectorIsFinite3( point ) ||
		!StudioVectorIsFinite3( a ) ||
		!StudioVectorIsFinite3( b ) ||
		!StudioVectorIsFinite3( c ) )
		return false;

	double ab[3], ac[3], ap[3], normal[3];
	for( int axis = 0; axis < 3; ++axis )
	{
		ab[axis] = (double)b[axis] - (double)a[axis];
		ac[axis] = (double)c[axis] - (double)a[axis];
		ap[axis] = (double)point[axis] - (double)a[axis];
	}
	normal[0] = ab[1] * ac[2] - ab[2] * ac[1];
	normal[1] = ab[2] * ac[0] - ab[0] * ac[2];
	normal[2] = ab[0] * ac[1] - ab[1] * ac[0];
	const double areaSquared = StudioDot3Double( normal, normal );

	// Project onto the triangle plane, then test its barycentric coordinates.
	// Use double precision here: very skinny SMD triangles can make the
	// float Ericson denominator cancel to zero and produce an infinite point.
	if( areaSquared > 1.0e-24 )
	{
		const double planeFraction = StudioDot3Double( ap, normal ) / areaSquared;
		double projected[3], fromA[3];
		for( int axis = 0; axis < 3; ++axis )
		{
			projected[axis] = (double)point[axis] - normal[axis] * planeFraction;
			fromA[axis] = projected[axis] - (double)a[axis];
		}

		const double d00 = StudioDot3Double( ab, ab );
		const double d01 = StudioDot3Double( ab, ac );
		const double d11 = StudioDot3Double( ac, ac );
		const double d20 = StudioDot3Double( fromA, ab );
		const double d21 = StudioDot3Double( fromA, ac );
		const double denominator = d00 * d11 - d01 * d01;
		if( denominator > 1.0e-24 )
		{
			const double v = ( d11 * d20 - d01 * d21 ) / denominator;
			const double w = ( d00 * d21 - d01 * d20 ) / denominator;
			const double u = 1.0 - v - w;
			if( u >= -1.0e-7 && v >= -1.0e-7 && w >= -1.0e-7 )
			{
				for( int axis = 0; axis < 3; ++axis )
					closest[axis] = (float)projected[axis];
				return StudioVectorIsFinite3( closest );
			}
		}
	}

	// Outside the triangle, or nearly degenerate: the nearest point lies on
	// one of its edges. Compare all three segment candidates without division
	// by a near-zero triangle-area denominator.
	float candidate[3];
	double bestDistanceSquared = 1.0e300;
	const float *edgeStarts[3] = { a, b, c };
	const float *edgeEnds[3] = { b, c, a };
	for( int edge = 0; edge < 3; ++edge )
	{
		StudioClosestPointOnRagdollSegment(
			point, edgeStarts[edge], edgeEnds[edge], candidate );
		double delta[3];
		for( int axis = 0; axis < 3; ++axis )
			delta[axis] = (double)candidate[axis] - (double)point[axis];
		const double distanceSquared = StudioDot3Double( delta, delta );
		if( distanceSquared < bestDistanceSquared )
		{
			bestDistanceSquared = distanceSquared;
			VectorCopy( candidate, closest );
		}
	}
	return StudioVectorIsFinite3( closest );
}

typedef struct studio_ragdoll_anchor_diagnostics_s
{
	int invalidVertices;
	int degenerateNormals;
	int invalidClosestPoints;
	int invalidDistances;
	int usableTriangles;
	float minimum[3];
	float maximum[3];
	qboolean haveBounds;
} studio_ragdoll_anchor_diagnostics_t;

typedef struct studio_ragdoll_anchor_context_s
{
	studio_ragdoll_surface_anchor_t anchor;
	studio_ragdoll_anchor_diagnostics_t diagnostics;
} studio_ragdoll_anchor_context_t;

static void StudioFindNearestRagdollSurface(
	const studio_ragdoll_triangle_t *triangle,
	void *opaqueContext )
{
	studio_ragdoll_anchor_context_t *context =
		(studio_ragdoll_anchor_context_t *)opaqueContext;
	studio_ragdoll_surface_anchor_t *anchor = &context->anchor;
	studio_ragdoll_anchor_diagnostics_t *diagnostics = &context->diagnostics;
	++anchor->triangleCount;

	for( int vertex = 0; vertex < 3; ++vertex )
	{
		if( !StudioVectorIsFinite3( triangle->world[vertex] ) )
		{
			++diagnostics->invalidVertices;
			return;
		}
		if( !diagnostics->haveBounds )
		{
			for( int axis = 0; axis < 3; ++axis )
			{
				diagnostics->minimum[axis] = triangle->world[vertex][axis];
				diagnostics->maximum[axis] = triangle->world[vertex][axis];
			}
			diagnostics->haveBounds = true;
		}
		else
		{
			for( int axis = 0; axis < 3; ++axis )
			{
				if( triangle->world[vertex][axis] < diagnostics->minimum[axis] )
					diagnostics->minimum[axis] = triangle->world[vertex][axis];
				if( triangle->world[vertex][axis] > diagnostics->maximum[axis] )
					diagnostics->maximum[axis] = triangle->world[vertex][axis];
			}
		}
	}

	// Compute the surface normal and its magnitude in double precision.
	// A float cross product can overflow while all of its source coordinates
	// are still finite, preventing every triangle from becoming an anchor.
	double edge1[3], edge2[3], faceNormalDouble[3];
	for( int axis = 0; axis < 3; ++axis )
	{
		edge1[axis] =
			(double)triangle->world[1][axis] - (double)triangle->world[0][axis];
		edge2[axis] =
			(double)triangle->world[2][axis] - (double)triangle->world[0][axis];
	}
	faceNormalDouble[0] = edge1[1] * edge2[2] - edge1[2] * edge2[1];
	faceNormalDouble[1] = edge1[2] * edge2[0] - edge1[0] * edge2[2];
	faceNormalDouble[2] = edge1[0] * edge2[1] - edge1[1] * edge2[0];
	const double normalLengthSquared =
		StudioDot3Double( faceNormalDouble, faceNormalDouble );
	if( !( normalLengthSquared > 1.0e-12 ) ||
		!( normalLengthSquared < 1.0e300 ) )
	{
		++diagnostics->degenerateNormals;
		return;
	}

	float closest[3];
	if( !StudioClosestPointOnRagdollTriangle(
			anchor->point,
			triangle->world[0],
			triangle->world[1],
			triangle->world[2],
			closest ) )
	{
		++diagnostics->invalidClosestPoints;
		return;
	}

	double delta[3];
	for( int axis = 0; axis < 3; ++axis )
		delta[axis] = (double)closest[axis] - (double)anchor->point[axis];
	const double distanceSquaredDouble = StudioDot3Double( delta, delta );
	if( !( distanceSquaredDouble >= 0.0 ) ||
		!( distanceSquaredDouble < 1.0e300 ) )
	{
		++diagnostics->invalidDistances;
		return;
	}
	++diagnostics->usableTriangles;
	if( anchor->found && distanceSquaredDouble >= (double)anchor->distanceSquared )
		return;

	const double inverseNormalLength = 1.0 / sqrt( normalLengthSquared );
	float faceNormal[3] = {
		(float)( faceNormalDouble[0] * inverseNormalLength ),
		(float)( faceNormalDouble[1] * inverseNormalLength ),
		(float)( faceNormalDouble[2] * inverseNormalLength )
	};
	anchor->found = true;
	anchor->distanceSquared = (float)distanceSquaredDouble;
	anchor->boneIndex = triangle->bone[0];
	VectorCopy( closest, anchor->closest );
	VectorCopy( faceNormal, anchor->normal );
}

static float StudioRagdollDecalClipDistance(
	const studio_ragdoll_decal_vertex_t &vertex,
	int plane,
	float depthTolerance )
{
	switch( plane )
	{
	case 0: return vertex.u + 1.0f;
	case 1: return 1.0f - vertex.u;
	case 2: return vertex.v + 1.0f;
	case 3: return 1.0f - vertex.v;
	case 4: return vertex.depth + depthTolerance;
	default: return depthTolerance - vertex.depth;
	}
}

static int StudioClipRagdollDecalPolygon(
	const studio_ragdoll_decal_vertex_t *input,
	int inputCount,
	studio_ragdoll_decal_vertex_t *output,
	int plane,
	float depthTolerance )
{
	int outputCount = 0;
	if( inputCount <= 0 )
		return 0;

	studio_ragdoll_decal_vertex_t previous = input[inputCount - 1];
	float previousDistance =
		StudioRagdollDecalClipDistance( previous, plane, depthTolerance );
	qboolean previousInside = previousDistance >= 0.0f;

	for( int index = 0; index < inputCount; ++index )
	{
		const studio_ragdoll_decal_vertex_t current = input[index];
		const float currentDistance =
			StudioRagdollDecalClipDistance( current, plane, depthTolerance );
		const qboolean currentInside = currentDistance >= 0.0f;

		if( previousInside != currentInside &&
			outputCount < STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS )
		{
			const float denominator = previousDistance - currentDistance;
			if( fabsf( denominator ) > 0.000001f )
			{
				const float fraction = previousDistance / denominator;
				studio_ragdoll_decal_vertex_t intersection;
				for( int axis = 0; axis < 3; ++axis )
					intersection.barycentric[axis] =
						previous.barycentric[axis] +
						( current.barycentric[axis] -
						  previous.barycentric[axis] ) * fraction;
				intersection.u = previous.u + ( current.u - previous.u ) * fraction;
				intersection.v = previous.v + ( current.v - previous.v ) * fraction;
				intersection.depth = previous.depth +
					( current.depth - previous.depth ) * fraction;
				output[outputCount++] = intersection;
			}
		}

		if( currentInside &&
			outputCount < STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS )
			output[outputCount++] = current;

		previous = current;
		previousDistance = currentDistance;
		previousInside = currentInside;
	}

	return outputCount;
}

static void StudioBuildRagdollImpactFragment(
	const studio_ragdoll_triangle_t *triangle,
	void *opaqueContext )
{
	studio_ragdoll_decal_build_t *context =
		(studio_ragdoll_decal_build_t *)opaqueContext;
	++context->triangleCount;
	if( context->cache->fragmentCount >= STUDIO_RAGDOLL_DECAL_MAX_FRAGMENTS )
		return;

	float edge1[3], edge2[3], faceNormal[3];
	for( int axis = 0; axis < 3; ++axis )
	{
		edge1[axis] = triangle->world[1][axis] - triangle->world[0][axis];
		edge2[axis] = triangle->world[2][axis] - triangle->world[0][axis];
	}
	faceNormal[0] = edge1[1] * edge2[2] - edge1[2] * edge2[1];
	faceNormal[1] = edge1[2] * edge2[0] - edge1[0] * edge2[2];
	faceNormal[2] = edge1[0] * edge2[1] - edge1[1] * edge2[0];
	if( StudioDot3( faceNormal, faceNormal ) < 0.000001f )
		return;
	StudioNormalize3( faceNormal );

	// Studio triangle winding varies between models. Accept either winding,
	// then orient the tiny polygon bias toward the impact-facing side.
	const float facing = StudioDot3( faceNormal, context->normal );
	if( fabsf( facing ) < 0.05f )
		return;
	if( facing < 0.0f )
		for( int axis = 0; axis < 3; ++axis )
			faceNormal[axis] = -faceNormal[axis];

	studio_ragdoll_decal_vertex_t first[STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS];
	studio_ragdoll_decal_vertex_t second[STUDIO_RAGDOLL_DECAL_MAX_POLY_VERTS];
	for( int vertex = 0; vertex < 3; ++vertex )
	{
		float delta[3];
		for( int axis = 0; axis < 3; ++axis )
			delta[axis] = triangle->world[vertex][axis] - context->center[axis];

		first[vertex].barycentric[0] = vertex == 0 ? 1.0f : 0.0f;
		first[vertex].barycentric[1] = vertex == 1 ? 1.0f : 0.0f;
		first[vertex].barycentric[2] = vertex == 2 ? 1.0f : 0.0f;
		first[vertex].u = StudioDot3( delta, context->tangent ) / context->radius;
		first[vertex].v = StudioDot3( delta, context->bitangent ) / context->radius;
		first[vertex].depth = StudioDot3( delta, context->normal );
	}

	int vertexCount = 3;
	studio_ragdoll_decal_vertex_t *input = first;
	studio_ragdoll_decal_vertex_t *output = second;
	for( int plane = 0; plane < 6; ++plane )
	{
		vertexCount = StudioClipRagdollDecalPolygon(
			input, vertexCount, output, plane, context->depthTolerance );
		if( vertexCount < 3 )
			return;
		studio_ragdoll_decal_vertex_t *swap = input;
		input = output;
		output = swap;
	}

	studio_ragdoll_decal_fragment_t &fragment =
		context->cache->fragments[context->cache->fragmentCount++];
	for( int corner = 0; corner < 3; ++corner )
	{
		fragment.bone[corner] = triangle->bone[corner];
		VectorCopy( triangle->local[corner], fragment.local[corner] );
	}
	fragment.vertexCount = vertexCount;
	for( int vertex = 0; vertex < vertexCount; ++vertex )
	{
		for( int axis = 0; axis < 3; ++axis )
			fragment.barycentric[vertex][axis] = input[vertex].barycentric[axis];
		fragment.uv[vertex][0] = input[vertex].u * 0.5f + 0.5f;
		fragment.uv[vertex][1] = input[vertex].v * 0.5f + 0.5f;
	}
}

static qboolean StudioSameRagdollMark(
	const studio_ragdoll_mark_cache_t *cache,
	int entityIndex,
	model_t *model,
	int boneIndex,
	const float rayStart[3],
	const float worldPosition[3],
	float radius,
	int bloodColor )
{
	if( !cache->valid || cache->entityIndex != entityIndex ||
		cache->model != model || cache->boneIndex != boneIndex ||
		cache->bloodColor != bloodColor ||
		fabsf( cache->radius - radius ) > 0.001f )
		return false;

	for( int axis = 0; axis < 3; ++axis )
	{
		if( fabsf( cache->rayStart[axis] - rayStart[axis] ) > 0.001f ||
			fabsf( cache->worldPosition[axis] - worldPosition[axis] ) > 0.001f )
			return false;
	}
	return true;
}

static studio_ragdoll_mark_cache_t *StudioFindRagdollMarkCache(
	int entityIndex,
	model_t *model,
	int boneIndex,
	const float rayStart[3],
	const float worldPosition[3],
	float radius,
	int bloodColor )
{
	for( int i = 0; i < STUDIO_RAGDOLL_DECAL_CACHE_COUNT; ++i )
	{
		studio_ragdoll_mark_cache_t *cache = &g_StudioRagdollDecalCache[i];
		if( StudioSameRagdollMark(
				cache, entityIndex, model, boneIndex,
				rayStart, worldPosition, radius, bloodColor ) )
		{
			cache->age = ++g_StudioRagdollDecalCacheClock;
			return cache;
		}
	}
	return NULL;
}

static studio_ragdoll_mark_cache_t *StudioAllocateRagdollMarkCache( void )
{
	studio_ragdoll_mark_cache_t *oldest = &g_StudioRagdollDecalCache[0];
	for( int i = 0; i < STUDIO_RAGDOLL_DECAL_CACHE_COUNT; ++i )
	{
		studio_ragdoll_mark_cache_t *cache = &g_StudioRagdollDecalCache[i];
		if( !cache->valid )
			return cache;
		if( cache->age < oldest->age )
			oldest = cache;
	}
	return oldest;
}

static studio_ragdoll_mark_cache_t *StudioBuildRagdollMarkCache(
	CStudioModelRenderer *renderer,
	int entityIndex,
	int boneIndex,
	const float rayStart[3],
	const float worldPosition[3],
	float sourceRadius,
	int bloodColor,
	int projectionAttempt )
{
	// Source's CStudioRender::AddDecal projects a decal plane along the
	// incoming ray. In particular, ordinary studio decals are not restricted
	// to a thin depth slab around the physics contact: coarse collision hulls
	// can place that contact well behind the visible skin along the same ray.
	// Cache the clipped geometry in the source mesh triangles so it follows
	// the current interpolated ragdoll pose.
	studio_ragdoll_mark_cache_t *cache = StudioAllocateRagdollMarkCache();
	memset( cache, 0, sizeof( *cache ) );
	cache->valid = true;
	cache->entityIndex = entityIndex;
	cache->model = renderer->m_pRenderModel;
	cache->boneIndex = boneIndex;
	cache->radius = sourceRadius;
	if( cache->radius < 3.0f ) cache->radius = 3.0f;
	if( cache->radius > 6.0f ) cache->radius = 6.0f;
	cache->bloodColor = bloodColor;
	cache->projectionAttempts = projectionAttempt;
	for( int axis = 0; axis < 3; ++axis )
	{
		cache->rayStart[axis] = rayStart[axis];
		cache->worldPosition[axis] = worldPosition[axis];
	}

	float rayDirection[3];
	float rayLengthSquared = 0.0f;
	for( int axis = 0; axis < 3; ++axis )
	{
		rayDirection[axis] = worldPosition[axis] - rayStart[axis];
		rayLengthSquared += rayDirection[axis] * rayDirection[axis];
	}
	if( rayLengthSquared < 0.0001f )
	{
		cache->age = ++g_StudioRagdollDecalCacheClock;
		return cache;
	}
	const float rayLength = sqrtf( rayLengthSquared );
	for( int axis = 0; axis < 3; ++axis )
		rayDirection[axis] /= rayLength;

	// First locate the real render-surface point nearest the Jolt contact.
	// The proxy hull and the skinned zombie mesh are not coincident, so the
	// shot ray alone can pass outside the small decal footprint.
	studio_ragdoll_anchor_context_t anchorContext;
	memset( &anchorContext, 0, sizeof( anchorContext ) );
	studio_ragdoll_surface_anchor_t &anchor = anchorContext.anchor;
	anchor.point = worldPosition;
	anchor.distanceSquared = 1.0e30f;
	StudioForEachRagdollModelTriangle(
		renderer, StudioFindNearestRagdollSurface, &anchor );

	studio_ragdoll_decal_build_t build;
	memset( &build, 0, sizeof( build ) );
	build.cache = cache;
	build.radius = cache->radius;
	// Source's standard model decal path leaves projection depth open. Here
	// the projector is surface-anchored to compensate for the coarser Jolt
	// proxy; an effectively unbounded depth preserves that behavior.
	build.depthTolerance = 1.0e6f;
	if( anchor.found )
	{
		for( int axis = 0; axis < 3; ++axis )
		{
			build.center[axis] = anchor.closest[axis];
			build.normal[axis] = anchor.normal[axis];
		}
		// Face the mark toward the incoming shot so it remains visible from
		// the side that actually hit the corpse.
		float toShooter[3];
		for( int axis = 0; axis < 3; ++axis )
			toShooter[axis] = rayStart[axis] - anchor.closest[axis];
		if( StudioDot3( build.normal, toShooter ) < 0.0f )
			for( int axis = 0; axis < 3; ++axis )
				build.normal[axis] = -build.normal[axis];
	}
	else
	{
		for( int axis = 0; axis < 3; ++axis )
		{
			build.center[axis] = worldPosition[axis];
			build.normal[axis] = -rayDirection[axis];
		}
	}

	// This is the same decal basis used by CStudioRender::ComputePoseToDecal:
	// N = -ray direction, U = up x N, V = N x U.
	float reference[3] = { 0.0f, 0.0f, 1.0f };
	if( fabsf( build.normal[2] ) > 0.90f )
	{
		reference[0] = 0.0f;
		reference[1] = 1.0f;
		reference[2] = 0.0f;
	}
	build.tangent[0] = reference[1] * build.normal[2] - reference[2] * build.normal[1];
	build.tangent[1] = reference[2] * build.normal[0] - reference[0] * build.normal[2];
	build.tangent[2] = reference[0] * build.normal[1] - reference[1] * build.normal[0];
	StudioNormalize3( build.tangent );
	build.bitangent[0] = build.normal[1] * build.tangent[2] - build.normal[2] * build.tangent[1];
	build.bitangent[1] = build.normal[2] * build.tangent[0] - build.normal[0] * build.tangent[2];
	build.bitangent[2] = build.normal[0] * build.tangent[1] - build.normal[1] * build.tangent[0];

	StudioForEachRagdollModelTriangle(
		renderer, StudioBuildRagdollImpactFragment, &build );
	cache->age = ++g_StudioRagdollDecalCacheClock;
	if( cache->fragmentCount <= 0 && projectionAttempt < 4 )
		cache->retryTime = (float)gEngfuncs.GetClientTime() + 0.20f;
	if( cl_ragdoll_bloodmarks_debug && cl_ragdoll_bloodmarks_debug->value > 0.0f &&
		gEngfuncs.Con_Printf )
		gEngfuncs.Con_Printf(
			"[Ragdoll decals] entity %d bone %d: scanned %d, usable %d, bad-vertices %d, degenerate %d, bad-closest %d, bad-distance %d, bounds [%.1f %.1f %.1f]-[%.1f %.1f %.1f], anchor %s (%.2f units, bone %d), fragments %d, radius %.2f, ray %.1f\n",
			entityIndex, boneIndex, build.triangleCount,
			anchorContext.diagnostics.usableTriangles,
			anchorContext.diagnostics.invalidVertices,
			anchorContext.diagnostics.degenerateNormals,
			anchorContext.diagnostics.invalidClosestPoints,
			anchorContext.diagnostics.invalidDistances,
			anchorContext.diagnostics.haveBounds ? anchorContext.diagnostics.minimum[0] : 0.0f,
			anchorContext.diagnostics.haveBounds ? anchorContext.diagnostics.minimum[1] : 0.0f,
			anchorContext.diagnostics.haveBounds ? anchorContext.diagnostics.minimum[2] : 0.0f,
			anchorContext.diagnostics.haveBounds ? anchorContext.diagnostics.maximum[0] : 0.0f,
			anchorContext.diagnostics.haveBounds ? anchorContext.diagnostics.maximum[1] : 0.0f,
			anchorContext.diagnostics.haveBounds ? anchorContext.diagnostics.maximum[2] : 0.0f,
			anchor.found ? "found" : "missing",
			anchor.found ? sqrtf( anchor.distanceSquared ) : -1.0f,
			anchor.found ? anchor.boneIndex : -1,
			cache->fragmentCount, cache->radius, rayLength );
	return cache;
}

static void StudioDrawCachedRagdollMark(
	CStudioModelRenderer *renderer,
	triangleapi_t *tri,
	const studio_ragdoll_mark_cache_t *cache )
{
	if( !renderer || !renderer->m_pbonetransform || !tri ||
		!cache || !cache->valid || cache->fragmentCount <= 0 )
		return;

	float red = 0.30f, green = 0.012f, blue = 0.004f;
	if( cache->bloodColor == 1 )
	{
		red = 0.025f;
		green = 0.24f;
		blue = 0.018f;
	}
	else if( cache->bloodColor == 2 )
	{
		red = 0.30f;
		green = 0.22f;
		blue = 0.012f;
	}

	tri->Color4f( red, green, blue, 0.88f );
	tri->Begin( TRI_TRIANGLES );

	for( int fragmentIndex = 0;
		fragmentIndex < cache->fragmentCount;
		++fragmentIndex )
	{
		const studio_ragdoll_decal_fragment_t &fragment =
			cache->fragments[fragmentIndex];
		float worldTriangle[3][3];
		qboolean valid = true;
		for( int corner = 0; corner < 3; ++corner )
		{
			if( fragment.bone[corner] < 0 ||
				fragment.bone[corner] >= renderer->m_pStudioHeader->numbones )
			{
				valid = false;
				break;
			}
			VectorTransform(
				fragment.local[corner],
				(*renderer->m_pbonetransform)[fragment.bone[corner]],
				worldTriangle[corner] );
		}
		if( !valid )
			continue;

		float edge1[3], edge2[3], normal[3];
		for( int axis = 0; axis < 3; ++axis )
		{
			edge1[axis] = worldTriangle[1][axis] - worldTriangle[0][axis];
			edge2[axis] = worldTriangle[2][axis] - worldTriangle[0][axis];
		}
		normal[0] = edge1[1] * edge2[2] - edge1[2] * edge2[1];
		normal[1] = edge1[2] * edge2[0] - edge1[0] * edge2[2];
		normal[2] = edge1[0] * edge2[1] - edge1[1] * edge2[0];
		if( StudioDot3( normal, normal ) < 0.000001f )
			continue;
		StudioNormalize3( normal );

		for( int fanVertex = 1;
			fanVertex + 1 < fragment.vertexCount;
			++fanVertex )
		{
			const int triangleIndices[3] = { 0, fanVertex, fanVertex + 1 };
			for( int corner = 0; corner < 3; ++corner )
			{
				const int polygonIndex = triangleIndices[corner];
				const float *bary = fragment.barycentric[polygonIndex];
				float position[3];
				for( int axis = 0; axis < 3; ++axis )
				{
					position[axis] =
						worldTriangle[0][axis] * bary[0] +
						worldTriangle[1][axis] * bary[1] +
						worldTriangle[2][axis] * bary[2] +
						normal[axis] * 0.08f;
				}
				tri->TexCoord2f(
					fragment.uv[polygonIndex][0],
					fragment.uv[polygonIndex][1] );
				tri->Vertex3fv( position );
			}
		}
	}

	tri->End();
}

/*
====================
StudioDrawRagdollImpactMarks

The expensive projection/clipping is cached per impact. Rendering each
subsequent frame only transforms the few source triangles touched by each
mark and interpolates their cached barycentric decal vertices.
====================
*/
static void StudioDrawRagdollImpactMarks( CStudioModelRenderer *renderer )
{
	triangleapi_t *tri = gEngfuncs.pTriAPI;
	cl_entity_t *entity = renderer ? renderer->m_pCurrentEntity : NULL;
	if( !renderer || !entity || entity->index <= 0 || !tri ||
		!cl_ragdoll_bloodmarks || cl_ragdoll_bloodmarks->value <= 0.0f )
		return;

	const int markCount = Ragdoll_GetImpactMarkCount( entity->index );
	if( markCount <= 0 )
	{
		for( int i = 0; i < STUDIO_RAGDOLL_DECAL_CACHE_COUNT; ++i )
			if( g_StudioRagdollDecalCache[i].entityIndex == entity->index )
				g_StudioRagdollDecalCache[i].valid = false;
		return;
	}

	if( !cl_blobshadow_sprite )
		cl_blobshadow_sprite = IEngineStudio.Mod_ForName( "sprites/glow01.spr", false );
	if( !cl_blobshadow_sprite || cl_blobshadow_sprite->type != mod_sprite ||
		!cl_blobshadow_sprite->cache.data || !tri->SpriteTexture )
		return;

	const int texture = StudioGetRagdollBloodTexture();
	if( texture <= 0 || !StudioBindBlobShadowTexture( tri, texture ) )
		return;

	tri->CullFace( TRI_NONE );
	tri->RenderMode( kRenderTransAlpha );

	for( int markIndex = 0; markIndex < markCount; ++markIndex )
	{
		int boneIndex = -1;
		float rayStart[3];
		float worldPosition[3];
		float sourceRadius = 0.0f;
		int bloodColor = 0;
		if( !Ragdoll_GetImpactMark(
				entity->index, markIndex, &boneIndex,
				rayStart, worldPosition, &sourceRadius, &bloodColor ) )
			continue;

		studio_ragdoll_mark_cache_t *cache = StudioFindRagdollMarkCache(
			entity->index, renderer->m_pRenderModel, boneIndex,
			rayStart, worldPosition, sourceRadius, bloodColor );
		if( !cache )
		{
			cache = StudioBuildRagdollMarkCache(
				renderer, entity->index, boneIndex,
				rayStart, worldPosition, sourceRadius, bloodColor, 0 );
		}
		else if( cache->fragmentCount <= 0 && cache->retryTime > 0.0f &&
			(float)gEngfuncs.GetClientTime() >= cache->retryTime )
		{
			const int nextAttempt = cache->projectionAttempts + 1;
			cache->valid = false;
			cache = StudioBuildRagdollMarkCache(
				renderer, entity->index, boneIndex,
				rayStart, worldPosition, sourceRadius, bloodColor, nextAttempt );
		}

		if( cache && cache->fragmentCount > 0 )
			StudioDrawCachedRagdollMark( renderer, tri, cache );
	}

	tri->Color4f( 1.0f, 1.0f, 1.0f, 1.0f );
	tri->RenderMode( kRenderNormal );
	tri->CullFace( TRI_FRONT );
}

/*
====================
StudioDrawBlobShadow

Adapt Quake III's cg_shadows 1 path:
- trace down 128 units from the entity's feet;
- use FWGS custom trace bounds for the exact 30 x 30 x 2 box sweep;
- use 1 - trace.fraction for mark opacity;
- clip the 48-unit radial mark against nearby BSP faces via Xash RenderAPI.

The radial mark does not need yaw rotation. Point traces are used only
to discover neighboring BSP faces for clipping.
====================
*/
static void StudioDrawBlobShadow( cl_entity_t *entity, const vec3_t modelOrigin )
{
	static const float sampleOffsets[5] = { -24.0f, -15.0f, 0.0f, 15.0f, 24.0f };
	static const float q3TraceMins[3] = { -15.0f, -15.0f, 0.0f };
	static const float q3TraceMaxs[3] = {  15.0f,  15.0f, 2.0f };
	static qboolean warnedTexture = false;

	pmtrace_t trace, bestTrace;
	vec3_t traceStart, traceEnd;
	vec3_t normal, center, shadowOrigin;
	vec3_t surfacePoints[25];
	vec3_t surfaceNormals[25];
	msurface_t *surfaces[25];
	xash_decal_v37_t decal;
	float *verts;
	float alpha, strength, opacity, planeZ, depth, length;
	float gridX, gridY;
	int texture, textureWidth, textureHeight;
	int surfaceCount = 0;
	int numVerts;
	int i, j, k, n;
	triangleapi_t *tri = gEngfuncs.pTriAPI;

	if( !cl_blobshadows || cl_blobshadows->value <= 0.0f )
		return;

	if( !entity || !entity->model || !tri || !gEngfuncs.pEventAPI ||
		!gXashRenderAPI || !gXashRenderAPI->RenderGetParm ||
		!gXashRenderAPI->GL_FindTexture || !gXashRenderAPI->GL_CreateTexture ||
		!gXashRenderAPI->R_DecalSetupVerts )
		return;

	if( entity->index == 0 || ( gEngfuncs.GetViewModel && entity == gEngfuncs.GetViewModel() ))
		return;

	if( entity->curstate.rendermode != kRenderNormal ||
		entity->curstate.renderfx != kRenderFxNone ||
		( entity->curstate.effects & ( EF_NOSHADOW | EF_NODRAW )))
		return;

	if( !gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction ||
		!gEngfuncs.pEventAPI->EV_PushPMStates ||
		!gEngfuncs.pEventAPI->EV_SetTraceHull ||
		!gEngfuncs.pEventAPI->EV_PlayerTrace ||
		!gEngfuncs.pEventAPI->EV_TraceSurface ||
		!gEngfuncs.pEventAPI->EV_PushTraceBounds ||
		!gEngfuncs.pEventAPI->EV_PopTraceBounds ||
		!gEngfuncs.pEventAPI->EV_PopPMStates ||
		!IEngineStudio.Mod_ForName )
		return;

	// Mod_ForName resolves the current model-cache entry. Do not retain a
	// model_t pointer across level changes, when the previous cache can be freed.
	cl_blobshadow_sprite = IEngineStudio.Mod_ForName( "sprites/glow01.spr", false );

	if( !cl_blobshadow_sprite || cl_blobshadow_sprite->type != mod_sprite ||
		!cl_blobshadow_sprite->cache.data || !tri->SpriteTexture )
		return;

	// Build a real alpha-mask texture; the stock glow sprite has a black
	// background and produces opaque black quads with kRenderTransAlpha.
	texture = StudioGetBlobShadowTexture();
	if( texture <= 0 )
	{
		if( !warnedTexture && gEngfuncs.Con_Printf )
		{
			gEngfuncs.Con_Printf( "[Blob shadows] Could not create radial alpha texture\n" );
			warnedTexture = true;
		}
		return;
	}

	textureWidth = (int)gXashRenderAPI->RenderGetParm( XASH_RENDER_PARM_TEX_WIDTH, texture );
	textureHeight = (int)gXashRenderAPI->RenderGetParm( XASH_RENDER_PARM_TEX_HEIGHT, texture );
	if( textureWidth <= 0 || textureHeight <= 0 )
		return;

	memset( &decal, 0, sizeof( decal ));
	decal.texture = (short)texture;
	decal.scale = (float)textureWidth / 48.0f; // 48 world units, matching Q3's radius-24 mark.

	memset( &bestTrace, 0, sizeof( bestTrace ));

	// Use the exact world-space origin saved by StudioSetUpTransform().
	if( modelOrigin )
	{
		VectorCopy( modelOrigin, shadowOrigin );
	}
	else
	{
		VectorCopy( entity->origin, shadowOrigin );
	}

	/*
	 * Match Quake III CG_PlayerShadow exactly: sweep a 30 x 30 x 2 box
	 * down 128 units from the entity origin. FWGS exposes custom trace
	 * bounds, so this no longer needs a point-trace approximation.
	 */
	gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction( false, true );
	gEngfuncs.pEventAPI->EV_PushPMStates();
	gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );
	gEngfuncs.pEventAPI->EV_PushTraceBounds( 2, q3TraceMins, q3TraceMaxs );

	VectorCopy( shadowOrigin, traceStart );
	VectorCopy( shadowOrigin, traceEnd );
	traceEnd[2] -= 128.0f;

	memset( &bestTrace, 0, sizeof( bestTrace ));
	gEngfuncs.pEventAPI->EV_PlayerTrace(
		traceStart, traceEnd, PM_WORLD_ONLY, -1, &bestTrace
	);
	gEngfuncs.pEventAPI->EV_PopTraceBounds();

	if( bestTrace.fraction >= 1.0f || bestTrace.startsolid ||
		bestTrace.allsolid || bestTrace.inwater ||
		bestTrace.plane.normal[2] < 0.5f )
	{
		// If the renderer transform cannot be traced (e.g. interpolation
		// extrapolated beyond the current floor), retry from the network origin.
		VectorCopy( entity->origin, shadowOrigin );
		VectorCopy( shadowOrigin, traceStart );
		VectorCopy( shadowOrigin, traceEnd );
		traceEnd[2] -= 128.0f;

		gEngfuncs.pEventAPI->EV_PushTraceBounds( 2, q3TraceMins, q3TraceMaxs );
		memset( &bestTrace, 0, sizeof( bestTrace ));
		gEngfuncs.pEventAPI->EV_PlayerTrace(
			traceStart, traceEnd, PM_WORLD_ONLY, -1, &bestTrace
		);
		gEngfuncs.pEventAPI->EV_PopTraceBounds();

		if( bestTrace.fraction >= 1.0f || bestTrace.startsolid ||
			bestTrace.allsolid || bestTrace.inwater ||
			bestTrace.plane.normal[2] < 0.5f )
		{
			gEngfuncs.pEventAPI->EV_PopPMStates();
			return;
		}
	}

	/*
	 * Sample the mark footprint with point traces to discover every nearby
	 * BSP face for clipping. The center/radius samples are not used to
	 * calculate the Q3 trace fraction; that now comes from the exact box trace.
	 */
	gEngfuncs.pEventAPI->EV_SetTraceHull( 2 );

	for( i = 0; i < 5; i++ )
	{
		for( j = 0; j < 5; j++ )
		{
			gridX = sampleOffsets[i];
			gridY = sampleOffsets[j];

			traceStart[0] = shadowOrigin[0] + gridX;
			traceStart[1] = shadowOrigin[1] + gridY;
			traceStart[2] = shadowOrigin[2] + 1.0f;

			traceEnd[0] = traceStart[0];
			traceEnd[1] = traceStart[1];
			traceEnd[2] = traceStart[2] - 128.0f;

			memset( &trace, 0, sizeof( trace ));
			gEngfuncs.pEventAPI->EV_PlayerTrace(
				traceStart, traceEnd, PM_WORLD_ONLY, -1, &trace
			);

			if( trace.fraction >= 1.0f || trace.startsolid || trace.allsolid ||
				trace.inwater || trace.plane.normal[2] < 0.5f )
				continue;

			// EV_TraceSurface resolves the actual BSP face for this hit physent.
			if( trace.ent < 0 )
				continue;

			{
				msurface_t *surface = gEngfuncs.pEventAPI->EV_TraceSurface(
					trace.ent, traceStart, traceEnd
				);
				int alreadyAdded = 0;

				if( !surface )
					continue;

				for( k = 0; k < surfaceCount; k++ )
				{
					if( surfaces[k] == surface )
					{
						alreadyAdded = 1;
						break;
					}
				}

				if( alreadyAdded || surfaceCount >= 25 )
					continue;

				surfaces[surfaceCount] = surface;
				VectorCopy( trace.endpos, surfacePoints[surfaceCount] );
				VectorCopy( trace.plane.normal, surfaceNormals[surfaceCount] );

				length = sqrtf( DotProduct( surfaceNormals[surfaceCount], surfaceNormals[surfaceCount] ));
				if( length > 0.001f )
				{
					surfaceNormals[surfaceCount][0] /= length;
					surfaceNormals[surfaceCount][1] /= length;
					surfaceNormals[surfaceCount][2] /= length;
				}

				surfaceCount++;
			}
		}
	}

	gEngfuncs.pEventAPI->EV_PopPMStates();

	if( surfaceCount == 0 )
		return;

	// Q3 cg_shadows 1 uses alpha = 1 - swept-box trace fraction.
	// A modest default boost makes the shadow easier to read in bright rooms.
	alpha = 1.0f - bestTrace.fraction;
	strength = cl_blobshadows->value;
	if( strength > 1.0f ) strength = 1.0f;
	if( strength < 0.0f ) strength = 0.0f;

	opacity = cl_blobshadows_opacity ? cl_blobshadows_opacity->value : 0.53f;
	if( opacity < 0.0f ) opacity = 0.0f;
	if( opacity > 2.0f ) opacity = 2.0f;
	alpha *= strength * opacity;
	if( alpha > 1.0f ) alpha = 1.0f;
	if( alpha <= 0.0f )
		return;

	VectorCopy( bestTrace.plane.normal, normal );
	length = sqrtf( DotProduct( normal, normal ));
	if( length <= 0.001f || normal[2] < 0.5f )
		return;

	normal[0] /= length;
	normal[1] /= length;
	normal[2] /= length;

	// Reconstruct the impact point at the entity center from the hit plane.
	planeZ = ( bestTrace.plane.dist -
		normal[0] * shadowOrigin[0] -
		normal[1] * shadowOrigin[1] ) / normal[2];

	center[0] = shadowOrigin[0] + normal[0];
	center[1] = shadowOrigin[1] + normal[1];
	center[2] = planeZ + normal[2];

	VectorCopy( center, decal.position );

	if( !StudioBindBlobShadowTexture( tri, texture ))
		return;

	tri->CullFace( TRI_NONE );
	tri->RenderMode( kRenderTransAlpha );
	// StudioBindBlobShadowTexture already bound the generated radial mask.
	tri->Color4f( 0.0f, 0.0f, 0.0f, alpha );

	for( n = 0; n < surfaceCount; n++ )
	{
		// Q3's CG_ImpactMark projects fragments 20 units along -normal.
		// Keep faces inside that projection depth and reject floors above it.
		depth =
			( center[0] - surfacePoints[n][0] ) * normal[0] +
			( center[1] - surfacePoints[n][1] ) * normal[1] +
			( center[2] - surfacePoints[n][2] ) * normal[2];

		if( depth < -0.5f || depth > 21.0f )
			continue;

		verts = gXashRenderAPI->R_DecalSetupVerts(
			&decal, surfaces[n], texture, &numVerts
		);
		if( !verts || numVerts < 3 )
			continue;

		tri->Begin( TRI_POLYGON );
		for( k = 0; k < numVerts; k++, verts += 7 )
		{
			vec3_t vertex;

			// Small normal bias prevents depth fighting with the BSP face.
			vertex[0] = verts[0] + surfaceNormals[n][0] * 0.5f;
			vertex[1] = verts[1] + surfaceNormals[n][1] * 0.5f;
			vertex[2] = verts[2] + surfaceNormals[n][2] * 0.5f;

			tri->TexCoord2f( verts[3], verts[4] );
			tri->Vertex3fv( vertex );
		}
		tri->End();
	}

	tri->Color4f( 1.0f, 1.0f, 1.0f, 1.0f );
	tri->RenderMode( kRenderNormal );
	tri->CullFace( TRI_FRONT );
}

/////////////////////
// Implementation of CStudioModelRenderer.h
#define LEGS_BONES_COUNT	8

// enumerate all the bones that used for gait animation
const char *legs_bones[] =
{
	"Bip01",
	"Bip01 Pelvis",
	"Bip01 L Leg",
	"Bip01 L Leg1",
	"Bip01 L Foot",
	"Bip01 R Leg",
	"Bip01 R Leg1",
	"Bip01 R Foot" 
};

/*
====================
Init

====================
*/
void CStudioModelRenderer::Init( void )
{
	// Set up some variables shared with engine
	m_pCvarHiModels			= IEngineStudio.GetCvar( "cl_himodels" );
	m_pCvarDeveloper		= IEngineStudio.GetCvar( "developer" );
	m_pCvarDrawEntities		= IEngineStudio.GetCvar( "r_drawentities" );

	if( !cl_blobshadows )
		cl_blobshadows = CVAR_CREATE( "cl_blobshadows", "1", FCVAR_ARCHIVE );
	if( !cl_blobshadows_opacity )
		cl_blobshadows_opacity = CVAR_CREATE( "cl_blobshadows_opacity", "0.53", FCVAR_ARCHIVE );
	if( !cl_ragdoll_bloodmarks )
		cl_ragdoll_bloodmarks = CVAR_CREATE( "cl_ragdoll_bloodmarks", "1", FCVAR_ARCHIVE );
	if( !cl_ragdoll_bloodmarks_debug )
		cl_ragdoll_bloodmarks_debug = CVAR_CREATE( "cl_ragdoll_bloodmarks_debug", "0", FCVAR_ARCHIVE );

	m_pChromeSprite			= IEngineStudio.GetChromeSprite();

	IEngineStudio.GetModelCounters( &m_pStudioModelCount, &m_pModelsDrawn );

	// Get pointers to engine data structures
	m_pbonetransform		= (float (*)[MAXSTUDIOBONES][3][4])IEngineStudio.StudioGetBoneTransform();
	m_plighttransform		= (float (*)[MAXSTUDIOBONES][3][4])IEngineStudio.StudioGetLightTransform();
	m_paliastransform		= (float (*)[3][4])IEngineStudio.StudioGetAliasTransform();
	m_protationmatrix		= (float (*)[3][4])IEngineStudio.StudioGetRotationMatrix();
}

/*
====================
CStudioModelRenderer

====================
*/
CStudioModelRenderer::CStudioModelRenderer( void )
{
	m_fDoInterp		= 1;
	m_fGaitEstimation	= 1;
	m_pCurrentEntity	= NULL;
	m_pCvarHiModels		= NULL;
	m_pCvarDeveloper	= NULL;
	m_pCvarDrawEntities	= NULL;
	m_pChromeSprite		= NULL;
	m_pStudioModelCount	= NULL;
	m_pModelsDrawn		= NULL;
	m_protationmatrix	= NULL;
	m_paliastransform	= NULL;
	m_pbonetransform	= NULL;
	m_plighttransform	= NULL;
	m_pStudioHeader		= NULL;
	m_pBodyPart		= NULL;
	m_pSubModel		= NULL;
	m_pPlayerInfo		= NULL;
	m_pRenderModel		= NULL;
}

/*
====================
~CStudioModelRenderer

====================
*/
CStudioModelRenderer::~CStudioModelRenderer( void )
{
}

/*
====================
StudioCalcBoneAdj

====================
*/
void CStudioModelRenderer::StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	int i, j;
	float value;
	mstudiobonecontroller_t *pbonecontroller;

	pbonecontroller = (mstudiobonecontroller_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->bonecontrollerindex );

	for( j = 0; j < m_pStudioHeader->numbonecontrollers; j++ )
	{
		i = pbonecontroller[j].index;
		if( i <= 3 )
		{
			// check for 360% wrapping
			if( pbonecontroller[j].type & STUDIO_RLOOP )
			{
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128)
				{
					int a, b;
					a = ( pcontroller1[j] + 128 ) % 256;
					b = ( pcontroller2[j] + 128 ) % 256;
					value = ( ( a * dadt ) + ( b * ( 1.0f - dadt ) ) - 128.0f ) * ( 360.0f / 256.0f ) + pbonecontroller[j].start;
				}
				else
				{
					value = ( ( pcontroller1[i] * dadt + ( pcontroller2[i] ) * ( 1.0f - dadt ) ) ) * ( 360.0f / 256.0f ) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = ( pcontroller1[i] * dadt + pcontroller2[i] * ( 1.0f - dadt ) ) / 255.0f;
				if( value < 0.0f ) value = 0.0f;
				if( value > 1.0f ) value = 1.0f;
				value = ( 1.0f - value ) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			}
			// Con_DPrintf( "%d %d %f : %f\n", m_pCurrentEntity->curstate.controller[j], m_pCurrentEntity->latched.prevcontroller[j], value, dadt );
		}
		else
		{
			value = mouthopen / 64.0f;
			if( value > 1.0f ) value = 1.0f;				
			value = ( 1.0f - value ) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			// Con_DPrintf( "%d %f\n", mouthopen, value );
		}
		switch( pbonecontroller[j].type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = value * ( M_PI_F / 180.0f );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}


/*
====================
StudioCalcBoneQuaterion

====================
*/
void CStudioModelRenderer::StudioCalcBoneQuaterion( int frame, float s, mstudiobone_t *pbone, mstudioanim_t *panim, float *adj, float *q )
{
	int j, k;
	vec4_t q1, q2;
	vec3_t angle1, angle2;
	mstudioanimvalue_t *panimvalue;

	for( j = 0; j < 3; j++ )
	{
		if( panim->offset[j + 3] == 0 )
		{
			angle2[j] = angle1[j] = pbone->value[ j + 3]; // default;
		}
		else
		{
			panimvalue = (mstudioanimvalue_t *)( (byte *)panim + panim->offset[j + 3] );
			k = frame;
			// DEBUG
			if( panimvalue->num.total < panimvalue->num.valid )
				k = 0;
			while( panimvalue->num.total <= k )
			{
				k -= panimvalue->num.total;
				panimvalue += panimvalue->num.valid + 1;
				// DEBUG
				if( panimvalue->num.total < panimvalue->num.valid )
					k = 0;
			}
			// Bah, missing blend!
			if( panimvalue->num.valid > k )
			{
				angle1[j] = Unaligned( panimvalue[k + 1].value );

				if( panimvalue->num.valid > k + 1 )
				{
					angle2[j] = Unaligned( panimvalue[k + 2].value );
				}
				else
				{
					if( panimvalue->num.total > k + 1 )
						angle2[j] = angle1[j];
					else
						angle2[j] = Unaligned( panimvalue[panimvalue->num.valid + 2].value );
				}
			}
			else
			{
				angle1[j] = Unaligned( panimvalue[panimvalue->num.valid].value );
				if( panimvalue->num.total > k + 1 )
				{
					angle2[j] = angle1[j];
				}
				else
				{
					angle2[j] = Unaligned( panimvalue[panimvalue->num.valid + 2].value );
				}
			}
			angle1[j] = pbone->value[j+3] + angle1[j] * pbone->scale[j + 3];
			angle2[j] = pbone->value[j+3] + angle2[j] * pbone->scale[j + 3];
		}

		if( pbone->bonecontroller[j + 3] != -1 )
		{
			angle1[j] += adj[pbone->bonecontroller[j + 3]];
			angle2[j] += adj[pbone->bonecontroller[j + 3]];
		}
	}

	if( !VectorCompare( angle1, angle2 ) )
	{
		AngleQuaternion( angle1, q1 );
		AngleQuaternion( angle2, q2 );
		QuaternionSlerp( q1, q2, s, q );
	}
	else
	{
		AngleQuaternion( angle1, q );
	}
}

/*
====================
StudioCalcBonePosition

====================
*/
void CStudioModelRenderer::StudioCalcBonePosition( int frame, float s, mstudiobone_t *pbone, mstudioanim_t *panim, float *adj, float *pos )
{
	int j, k;
	mstudioanimvalue_t *panimvalue;

	for( j = 0; j < 3; j++ )
	{
		pos[j] = pbone->value[j]; // default;
		if( panim->offset[j] != 0 )
		{
			panimvalue = (mstudioanimvalue_t *)( (byte *)panim + panim->offset[j] );
			/*
			if( i == 0 && j == 0 )
				Con_DPrintf( "%d  %d:%d  %f\n", frame, panimvalue->num.valid, panimvalue->num.total, s );
			*/
			
			k = frame;
			// DEBUG
			if( panimvalue->num.total < panimvalue->num.valid )
				k = 0;
			// find span of values that includes the frame we want
			while( panimvalue->num.total <= k )
			{
				k -= panimvalue->num.total;
				panimvalue += panimvalue->num.valid + 1;
  				// DEBUG
				if( panimvalue->num.total < panimvalue->num.valid )
					k = 0;
			}
			// if we're inside the span
			if( panimvalue->num.valid > k )
			{
				// and there's more data in the span
				if( panimvalue->num.valid > k + 1 )
				{
					pos[j] += ( Unaligned( panimvalue[k + 1].value ) * ( 1.0f - s ) + s * Unaligned( panimvalue[k + 2].value ) ) * pbone->scale[j];
				}
				else
				{
					pos[j] += Unaligned( panimvalue[k + 1].value ) * pbone->scale[j];
				}
			}
			else
			{
				// are we at the end of the repeating values section and there's another section with data?
				if( panimvalue->num.total <= k + 1 )
				{
					pos[j] += ( Unaligned( panimvalue[panimvalue->num.valid].value ) * ( 1.0f - s ) + s * Unaligned( panimvalue[panimvalue->num.valid + 2].value ) ) * pbone->scale[j];
				}
				else
				{
					pos[j] += Unaligned( panimvalue[panimvalue->num.valid].value ) * pbone->scale[j];
				}
			}
		}
		if( pbone->bonecontroller[j] != -1 && adj )
		{
			pos[j] += adj[pbone->bonecontroller[j]];
		}
	}
}

/*
====================
StudioSlerpBones

====================
*/
void CStudioModelRenderer::StudioSlerpBones( vec4_t q1[], float pos1[][3], vec4_t q2[], float pos2[][3], float s )
{
	int i, tail;
	float s1;

	if( s < 0.0f )
		s = 0.0f;
	else if( s > 1.0f )
		s = 1.0f;

	s1 = 1.0f - s;

	tail = m_pStudioHeader->numbones & 3;

	for( i = 0; i < tail; i++ )
		QuaternionSlerp( q1[i], q2[i], s, q1[i] );

	for( ; i < m_pStudioHeader->numbones; i += 4 )
		QuaternionSlerpX4( q1 + i, q2 + i, s, q1 + i );

	for( i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		pos1[i][0] = pos1[i][0] * s1 + pos2[i][0] * s;
		pos1[i][1] = pos1[i][1] * s1 + pos2[i][1] * s;
		pos1[i][2] = pos1[i][2] * s1 + pos2[i][2] * s;
	}
}

/*
====================
StudioGetAnim

====================
*/
mstudioanim_t *CStudioModelRenderer::StudioGetAnim( model_t *m_pSubModel, mstudioseqdesc_t *pseqdesc )
{
	mstudioseqgroup_t *pseqgroup;
	cache_user_t *paSequences;

	pseqgroup = (mstudioseqgroup_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqgroupindex ) + pseqdesc->seqgroup;

	if( pseqdesc->seqgroup == 0 )
	{
		return (mstudioanim_t *)( (byte *)m_pStudioHeader + pseqdesc->animindex );
	}

	paSequences = (cache_user_t *)m_pSubModel->submodels;

	if( paSequences == NULL )
	{
		paSequences = (cache_user_t *)IEngineStudio.Mem_Calloc( 16, sizeof(cache_user_t) ); // UNDONE: leak!
		m_pSubModel->submodels = (dmodel_t *)paSequences;
	}

	if( !IEngineStudio.Cache_Check( (struct cache_user_s *)&( paSequences[pseqdesc->seqgroup] ) ) )
	{
		gEngfuncs.Con_DPrintf("loading %s\n", pseqgroup->name );
		IEngineStudio.LoadCacheFile( pseqgroup->name, (struct cache_user_s *)&paSequences[pseqdesc->seqgroup] );
	}

	return (mstudioanim_t *)( (byte *)paSequences[pseqdesc->seqgroup].data + pseqdesc->animindex );
}

/*
====================
StudioPlayerBlend

====================
*/
void CStudioModelRenderer::StudioPlayerBlend( mstudioseqdesc_t *pseqdesc, int *pBlend, float *pPitch )
{
	// calc up/down pointing
	*pBlend = ( *pPitch * 3 );
	if( *pBlend < pseqdesc->blendstart[0] )
	{
		*pPitch -= pseqdesc->blendstart[0] / 3.0f;
		*pBlend = 0;
	}
	else if( *pBlend > pseqdesc->blendend[0] )
	{
		*pPitch -= pseqdesc->blendend[0] / 3.0f;
		*pBlend = 255;
	}
	else
	{
		if( pseqdesc->blendend[0] - pseqdesc->blendstart[0] < 0.1f ) // catch qc error
			*pBlend = 127;
		else
			*pBlend = 255 * ( *pBlend - pseqdesc->blendstart[0] ) / ( pseqdesc->blendend[0] - pseqdesc->blendstart[0] );
		*pPitch = 0;
	}
}

/*
====================
StudioSetUpTransform

====================
*/
void CStudioModelRenderer::StudioSetUpTransform( int trivial_accept )
{
	int i;
	vec3_t angles;
	vec3_t modelpos;

	// tweek model origin
	//for( i = 0; i < 3; i++ )
	//	modelpos[i] = m_pCurrentEntity->origin[i];

	VectorCopy( m_pCurrentEntity->origin, modelpos );

// TODO: should really be stored with the entity instead of being reconstructed
// TODO: should use a look-up table
// TODO: could cache lazily, stored in the entity
	angles[ROLL] = m_pCurrentEntity->curstate.angles[ROLL];
	angles[PITCH] = m_pCurrentEntity->curstate.angles[PITCH];
	angles[YAW] = m_pCurrentEntity->curstate.angles[YAW];

	//Con_DPrintf( "Angles %4.2f prev %4.2f for %i\n", angles[PITCH], m_pCurrentEntity->index );
	//Con_DPrintf( "movetype %d %d\n", m_pCurrentEntity->movetype, m_pCurrentEntity->aiment );
	if( m_pCurrentEntity->curstate.movetype == MOVETYPE_STEP )
	{
		float f = 0;
		float d;

		// don't do it if the goalstarttime hasn't updated in a while.

		// NOTE:  Because we need to interpolate multiplayer characters, the interpolation time limit
		//  was increased to 1.0 s., which is 2x the max lag we are accounting for.

		if( ( m_clTime < m_pCurrentEntity->curstate.animtime + 1.0f ) &&
			( m_pCurrentEntity->curstate.animtime != m_pCurrentEntity->latched.prevanimtime ) )
		{
			f = ( m_clTime - m_pCurrentEntity->curstate.animtime ) / ( m_pCurrentEntity->curstate.animtime - m_pCurrentEntity->latched.prevanimtime );
			//Con_DPrintf( "%4.2f %.2f %.2f\n", f, m_pCurrentEntity->curstate.animtime, m_clTime );
		}

		if( m_fDoInterp )
		{
			// ugly hack to interpolate angle, position. current is reached 0.1 seconds after being set
			f = f - 1.0f;
		}
		else
		{
			f = 0.0f;
		}

		for( i = 0; i < 3; i++ )
		{
			modelpos[i] += ( m_pCurrentEntity->origin[i] - m_pCurrentEntity->latched.prevorigin[i] ) * f;
		}

		// NOTE:  Because multiplayer lag can be relatively large, we don't want to cap
		//  f at 1.5 anymore.
		//if( f > -1.0f && f < 1.5f ) {}
			//Con_DPrintf( "%.0f %.0f\n",m_pCurrentEntity->msg_angles[0][YAW], m_pCurrentEntity->msg_angles[1][YAW] );
		for( i = 0; i < 3; i++ )
		{
			float ang1, ang2;

			ang1 = m_pCurrentEntity->angles[i];
			ang2 = m_pCurrentEntity->latched.prevangles[i];

			d = ang1 - ang2;
			if( d > 180.0f )
			{
				d -= 360.0f;
			}
			else if( d < -180.0f )
			{
				d += 360.0f;
			}

			angles[i] += d * f;
		}
		//Con_DPrintf( "%.3f \n", f );
	}
	else if( m_pCurrentEntity->curstate.movetype != MOVETYPE_NONE ) 
	{
		VectorCopy( m_pCurrentEntity->angles, angles );
	}

	//Con_DPrintf( "%.0f %0.f %0.f\n", modelpos[0], modelpos[1], modelpos[2] );
	//Con_DPrintf( "%.0f %0.f %0.f\n", angles[0], angles[1], angles[2] );

	// Preserve the exact rendered origin for the later blob-shadow pass.
	VectorCopy( modelpos, g_BlobShadowStudioOrigin );
	g_BlobShadowStudioEntity = m_pCurrentEntity->index;

	angles[PITCH] = -angles[PITCH];
	AngleMatrix( angles, ( *m_protationmatrix ) );

	if( !IEngineStudio.IsHardware() )
	{
		static float viewmatrix[3][4];

		VectorCopy( m_vRight, viewmatrix[0] );
		VectorCopy( m_vUp, viewmatrix[1] );
		VectorInverse( viewmatrix[1] );
		VectorCopy( m_vNormal, viewmatrix[2] );

		(*m_protationmatrix)[0][3] = modelpos[0] - m_vRenderOrigin[0];
		(*m_protationmatrix)[1][3] = modelpos[1] - m_vRenderOrigin[1];
		(*m_protationmatrix)[2][3] = modelpos[2] - m_vRenderOrigin[2];

		ConcatTransforms( viewmatrix, (*m_protationmatrix), (*m_paliastransform) );

		// do the scaling up of x and y to screen coordinates as part of the transform
		// for the unclipped case (it would mess up clipping in the clipped case).
		// Also scale down z, so 1/z is scaled 31 bits for free, and scale down x and y
		// correspondingly so the projected x and y come out right
		// FIXME: make this work for clipped case too?
		if( trivial_accept )
		{
			for( i = 0; i < 4; i++ )
			{
				(*m_paliastransform)[0][i] *= m_fSoftwareXScale * ( 1.0f / ( ZISCALE * 0x10000 ) );
				(*m_paliastransform)[1][i] *= m_fSoftwareYScale * ( 1.0f / ( ZISCALE * 0x10000 ) );
				(*m_paliastransform)[2][i] *= 1.0f / ( ZISCALE * 0x10000 );
			}
		}
	}

	(*m_protationmatrix)[0][3] = modelpos[0];
	(*m_protationmatrix)[1][3] = modelpos[1];
	(*m_protationmatrix)[2][3] = modelpos[2];
}

/*
====================
StudioEstimateInterpolant

====================
*/
float CStudioModelRenderer::StudioEstimateInterpolant( void )
{
	float dadt = 1.0f;

	if( m_fDoInterp && ( m_pCurrentEntity->curstate.animtime >= m_pCurrentEntity->latched.prevanimtime + 0.01f ) )
	{
		dadt = ( m_clTime - m_pCurrentEntity->curstate.animtime ) / 0.1f;
		if( dadt > 2.0f )
		{
			dadt = 2.0f;
		}
	}
	return dadt;
}

/*
====================
StudioCalcRotations

====================
*/
void CStudioModelRenderer::StudioCalcRotations( float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	int i;
	int frame;
	mstudiobone_t *pbone;

	float s;
	float adj[MAXSTUDIOCONTROLLERS];
	float dadt;

	if( f > pseqdesc->numframes - 1 )
	{
		f = 0.0f;	// bah, fix this bug with changing sequences too fast
	}
	// BUG ( somewhere else ) but this code should validate this data.
	// This could cause a crash if the frame # is negative, so we'll go ahead
	//  and clamp it here
	else if( f < -0.01f )
	{
		f = -0.01f;
	}

	frame = (int)f;

	// Con_DPrintf( "%d %.4f %.4f %.4f %.4f %d\n", m_pCurrentEntity->curstate.sequence, m_clTime, m_pCurrentEntity->animtime, m_pCurrentEntity->frame, f, frame );

	// Con_DPrintf( "%f %f %f\n", m_pCurrentEntity->angles[ROLL], m_pCurrentEntity->angles[PITCH], m_pCurrentEntity->angles[YAW] );

	// Con_DPrintf( "frame %d %d\n", frame1, frame2 );

	dadt = StudioEstimateInterpolant();
	s = ( f - frame );

	// add in programtic controllers
	pbone = (mstudiobone_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	StudioCalcBoneAdj( dadt, adj, m_pCurrentEntity->curstate.controller, m_pCurrentEntity->latched.prevcontroller, m_pCurrentEntity->mouth.mouthopen );

	for( i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
	{
		StudioCalcBoneQuaterion( frame, s, pbone, panim, adj, q[i] );

		StudioCalcBonePosition( frame, s, pbone, panim, adj, pos[i] );
		// if( 0 && i == 0 )
		//	Con_DPrintf( "%d %d %d %d\n", m_pCurrentEntity->curstate.sequence, frame, j, k );
	}

	if( pseqdesc->motiontype & STUDIO_X )
	{
		pos[pseqdesc->motionbone][0] = 0.0f;
	}
	if( pseqdesc->motiontype & STUDIO_Y )
	{
		pos[pseqdesc->motionbone][1] = 0.0f;
	}
	if( pseqdesc->motiontype & STUDIO_Z )
	{
		pos[pseqdesc->motionbone][2] = 0.0f;
	}

	s = 0 * ( ( 1.0f - ( f - (int)( f ) ) ) / ( pseqdesc->numframes ) ) * m_pCurrentEntity->curstate.framerate;

	if( pseqdesc->motiontype & STUDIO_LX )
	{
		pos[pseqdesc->motionbone][0] += s * pseqdesc->linearmovement[0];
	}
	if( pseqdesc->motiontype & STUDIO_LY )
	{
		pos[pseqdesc->motionbone][1] += s * pseqdesc->linearmovement[1];
	}
	if( pseqdesc->motiontype & STUDIO_LZ )
	{
		pos[pseqdesc->motionbone][2] += s * pseqdesc->linearmovement[2];
	}
}

/*
====================
Studio_FxTransform

====================
*/
void CStudioModelRenderer::StudioFxTransform( cl_entity_t *ent, float transform[3][4] )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( gEngfuncs.pfnRandomLong( 0, 49 ) == 0 )
		{
			int axis = gEngfuncs.pfnRandomLong( 0, 1 );
			if( axis == 1 ) // Choose between x & z
				axis = 2;
			VectorScale( transform[axis], gEngfuncs.pfnRandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( gEngfuncs.pfnRandomLong( 0, 49 ) == 0 )
		{
			float offset;
			/*int axis = gEngfuncs.pfnRandomLong(0,1);
			if( axis == 1 ) // Choose between x & z
				axis = 2;*/
			offset = gEngfuncs.pfnRandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.pfnRandomLong( 0, 2 )][3] += offset;
		}
	break;
	case kRenderFxExplode:
		{
			float scale;

			scale = 1.0f + ( m_clTime - ent->curstate.animtime ) * 10.0f;
			if( scale > 2 )	// Don't blow up more than 200%
				scale = 2;
			transform[0][1] *= scale;
			transform[1][1] *= scale;
			transform[2][1] *= scale;
		}
	break;
	}
}

/*
====================
StudioEstimateFrame

====================
*/
float CStudioModelRenderer::StudioEstimateFrame( mstudioseqdesc_t *pseqdesc )
{
	double dfdt, f;

	if( m_fDoInterp )
	{
		if( m_clTime < m_pCurrentEntity->curstate.animtime )
		{
			dfdt = 0;
		}
		else
		{
			dfdt = ( m_clTime - m_pCurrentEntity->curstate.animtime ) * m_pCurrentEntity->curstate.framerate * pseqdesc->fps;
		}
	}
	else
	{
		dfdt = 0;
	}

	if( pseqdesc->numframes <= 1 )
	{
		f = 0.0;
	}
	else
	{
		f = ( m_pCurrentEntity->curstate.frame * ( pseqdesc->numframes - 1 ) ) / 256.0;
	}
 
	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
		{
			f -= (int)( f / ( pseqdesc->numframes - 1 ) ) * ( pseqdesc->numframes - 1 );
		}
		if( f < 0 )
		{
			f += ( pseqdesc->numframes - 1 );
		}
	}
	else 
	{
		if( f >= pseqdesc->numframes - 1.001 )
		{
			f = pseqdesc->numframes - 1.001;
		}
		if( f < 0.0 )
		{
			f = 0.0;
		}
	}
	return f;
}

/*
====================
StudioSetupBones

====================
*/
void CStudioModelRenderer::StudioSetupBones( void )
{
	int i, j;
	double f;

	mstudiobone_t *pbones;
	mstudioseqdesc_t *pseqdesc;
	mstudioanim_t *panim;

	static float pos[MAXSTUDIOBONES][3];
	static vec4_t q[MAXSTUDIOBONES];
	float bonematrix[3][4];

	static float pos2[MAXSTUDIOBONES][3];
	static vec4_t q2[MAXSTUDIOBONES];
	static float pos3[MAXSTUDIOBONES][3];
	static vec4_t q3[MAXSTUDIOBONES];
	static float pos4[MAXSTUDIOBONES][3];
	static vec4_t q4[MAXSTUDIOBONES];

	if( m_pCurrentEntity->curstate.sequence >=  m_pStudioHeader->numseq )
	{
		m_pCurrentEntity->curstate.sequence = 0;
	}

	pseqdesc = (mstudioseqdesc_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + m_pCurrentEntity->curstate.sequence;

	f = StudioEstimateFrame( pseqdesc );

	if( m_pCurrentEntity->latched.prevframe > f )
	{
		//Con_DPrintf( "%f %f\n", m_pCurrentEntity->prevframe, f );
	}

	panim = StudioGetAnim( m_pRenderModel, pseqdesc );
	StudioCalcRotations( pos, q, pseqdesc, panim, f );

	if( pseqdesc->numblends > 1 )
	{
		float s;
		float dadt;

		panim += m_pStudioHeader->numbones;
		StudioCalcRotations( pos2, q2, pseqdesc, panim, f );

		dadt = StudioEstimateInterpolant();
		s = ( m_pCurrentEntity->curstate.blending[0] * dadt + m_pCurrentEntity->latched.prevblending[0] * ( 1.0 - dadt ) ) / 255.0;

		StudioSlerpBones( q, pos, q2, pos2, s );

		if( pseqdesc->numblends == 4 )
		{
			panim += m_pStudioHeader->numbones;
			StudioCalcRotations( pos3, q3, pseqdesc, panim, f );

			panim += m_pStudioHeader->numbones;
			StudioCalcRotations( pos4, q4, pseqdesc, panim, f );

			s = ( m_pCurrentEntity->curstate.blending[0] * dadt + m_pCurrentEntity->latched.prevblending[0] * ( 1.0 - dadt ) ) / 255.0;
			StudioSlerpBones( q3, pos3, q4, pos4, s );

			s = ( m_pCurrentEntity->curstate.blending[1] * dadt + m_pCurrentEntity->latched.prevblending[1] * ( 1.0 - dadt ) ) / 255.0;
			StudioSlerpBones( q, pos, q3, pos3, s );
		}
	}
	
	if( m_fDoInterp && m_pCurrentEntity->latched.sequencetime &&
		( m_pCurrentEntity->latched.sequencetime + 0.2 > m_clTime ) && 
		( m_pCurrentEntity->latched.prevsequence < m_pStudioHeader->numseq ) )
	{
		// blend from last sequence
		static float pos1b[MAXSTUDIOBONES][3];
		static vec4_t q1b[MAXSTUDIOBONES];
		float s;

		pseqdesc = (mstudioseqdesc_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + m_pCurrentEntity->latched.prevsequence;
		panim = StudioGetAnim( m_pRenderModel, pseqdesc );
		// clip prevframe
		StudioCalcRotations( pos1b, q1b, pseqdesc, panim, m_pCurrentEntity->latched.prevframe );

		if( pseqdesc->numblends > 1 )
		{
			panim += m_pStudioHeader->numbones;
			StudioCalcRotations( pos2, q2, pseqdesc, panim, m_pCurrentEntity->latched.prevframe );

			s = (m_pCurrentEntity->latched.prevseqblending[0]) / 255.0;
			StudioSlerpBones( q1b, pos1b, q2, pos2, s );

			if( pseqdesc->numblends == 4 )
			{
				panim += m_pStudioHeader->numbones;
				StudioCalcRotations( pos3, q3, pseqdesc, panim, m_pCurrentEntity->latched.prevframe );

				panim += m_pStudioHeader->numbones;
				StudioCalcRotations( pos4, q4, pseqdesc, panim, m_pCurrentEntity->latched.prevframe );

				s = ( m_pCurrentEntity->latched.prevseqblending[0] ) / 255.0;
				StudioSlerpBones( q3, pos3, q4, pos4, s );

				s = ( m_pCurrentEntity->latched.prevseqblending[1] ) / 255.0;
				StudioSlerpBones( q1b, pos1b, q3, pos3, s );
			}
		}

		s = 1.0 - ( m_clTime - m_pCurrentEntity->latched.sequencetime ) / 0.2;
		StudioSlerpBones( q, pos, q1b, pos1b, s );
	}
	else
	{
		//Con_DPrintf("prevframe = %4.2f\n", f);
		m_pCurrentEntity->latched.prevframe = f;
	}

	pbones = (mstudiobone_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	// calc gait animation
	if( m_pPlayerInfo && m_pPlayerInfo->gaitsequence != 0 )
	{
		if( m_pPlayerInfo->gaitsequence >= m_pStudioHeader->numseq )
		{
			m_pPlayerInfo->gaitsequence = 0;
		}

		pseqdesc = (mstudioseqdesc_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + m_pPlayerInfo->gaitsequence;

		panim = StudioGetAnim( m_pRenderModel, pseqdesc );
		StudioCalcRotations( pos2, q2, pseqdesc, panim, m_pPlayerInfo->gaitframe );

		for( i = 0; i < m_pStudioHeader->numbones; i++ )
		{
			for( j = 0; j < LEGS_BONES_COUNT; j++ )
			{
				if( !strcmp( pbones[i].name, legs_bones[j] ) )
					break;
			}

			if( j == LEGS_BONES_COUNT )
				continue;	// not used for legs

			memcpy( pos[i], pos2[i], sizeof(pos[i]) );
			memcpy( q[i], q2[i], sizeof(q[i]) );
		}
	}


	// Match Source's ragdoll BuildTransformations path: simulated bones are
	// replaced as the hierarchy is traversed. Non-simulated bones continue
	// using the frozen death animation, relative to their already-simulated
	// parent when applicable. Applying all physics transforms after the loop
	// leaves children composed against the old walking-pose parent matrix.
	const int ragdollMarker = m_pCurrentEntity->curstate.iuser4;
	const int ragdollEntityIndex = m_pCurrentEntity->index;
	// The custom snapshot stream is authoritative. Entity-state user fields
	// are not guaranteed to survive every save/protocol path, so do not gate
	// the pose on curstate.iuser4. Ragdoll_GetBoneTransform verifies that this
	// entity has a complete, fresh snapshot before overriding any bone.
	const qboolean useRagdollPose =
		IEngineStudio.IsHardware() &&
		ragdollEntityIndex > 0;
	float ragdollTransform[3][4];

	if( ragdollEntityIndex > 0 )
		Ragdoll_DebugRenderGate(
			ragdollEntityIndex,
			ragdollMarker,
			IEngineStudio.IsHardware() ? 1 : 0
		);

	for( i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		if( useRagdollPose &&
			Ragdoll_GetBoneTransform( ragdollEntityIndex, i, ragdollTransform ) )
		{
			MatrixCopy( ragdollTransform, (*m_pbonetransform)[i] );
			MatrixCopy( ragdollTransform, (*m_plighttransform)[i] );
			continue;
		}

		QuaternionMatrix( q[i], bonematrix );

		bonematrix[0][3] = pos[i][0];
		bonematrix[1][3] = pos[i][1];
		bonematrix[2][3] = pos[i][2];

		if( pbones[i].parent == -1 )
		{
			if( IEngineStudio.IsHardware() )
			{
				ConcatTransforms( (*m_protationmatrix), bonematrix, (*m_pbonetransform)[i] );
				MatrixCopy( (*m_pbonetransform)[i], (*m_plighttransform)[i] );
			}
			else
			{
				ConcatTransforms( (*m_paliastransform), bonematrix, (*m_pbonetransform)[i] );
				ConcatTransforms( (*m_protationmatrix), bonematrix, (*m_plighttransform)[i] );
			}

			StudioFxTransform( m_pCurrentEntity, (*m_pbonetransform)[i] );
		}
		else
		{
			ConcatTransforms( (*m_pbonetransform)[pbones[i].parent], bonematrix, (*m_pbonetransform)[i] );
			ConcatTransforms( (*m_plighttransform)[pbones[i].parent], bonematrix, (*m_plighttransform)[i] );
		}
	}

}

/*
====================
StudioSaveBones

====================
*/
void CStudioModelRenderer::StudioSaveBones( void )
{
	int i;

	mstudiobone_t *pbones;
	pbones = (mstudiobone_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	m_nCachedBones = m_pStudioHeader->numbones;

	for( i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		strcpy( m_nCachedBoneNames[i], pbones[i].name );
		MatrixCopy( (*m_pbonetransform)[i], m_rgCachedBoneTransform[i] );
		MatrixCopy( (*m_plighttransform)[i], m_rgCachedLightTransform[i] );
	}
}

/*
====================
StudioMergeBones

====================
*/
void CStudioModelRenderer::StudioMergeBones( model_t *m_pSubModel )
{
	int i, j;
	double f;
	int do_hunt = true;

	mstudiobone_t *pbones;
	mstudioseqdesc_t *pseqdesc;
	mstudioanim_t *panim;

	static float pos[MAXSTUDIOBONES][3];
	float bonematrix[3][4];
	static vec4_t q[MAXSTUDIOBONES];

	if( m_pCurrentEntity->curstate.sequence >=  m_pStudioHeader->numseq )
	{
		m_pCurrentEntity->curstate.sequence = 0;
	}

	pseqdesc = (mstudioseqdesc_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + m_pCurrentEntity->curstate.sequence;

	f = StudioEstimateFrame( pseqdesc );

	if( m_pCurrentEntity->latched.prevframe > f )
	{
		//Con_DPrintf( "%f %f\n", m_pCurrentEntity->prevframe, f );
	}

	panim = StudioGetAnim( m_pSubModel, pseqdesc );
	StudioCalcRotations( pos, q, pseqdesc, panim, f );

	pbones = (mstudiobone_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->boneindex );

	for( i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		for( j = 0; j < m_nCachedBones; j++ )
		{
			if( stricmp( pbones[i].name, m_nCachedBoneNames[j] ) == 0 )
			{
				MatrixCopy( m_rgCachedBoneTransform[j], (*m_pbonetransform)[i] );
				MatrixCopy( m_rgCachedLightTransform[j], (*m_plighttransform)[i] );
				break;
			}
		}
		if( j >= m_nCachedBones )
		{
			QuaternionMatrix( q[i], bonematrix );

			bonematrix[0][3] = pos[i][0];
			bonematrix[1][3] = pos[i][1];
			bonematrix[2][3] = pos[i][2];

			if( pbones[i].parent == -1 )
			{
				if( IEngineStudio.IsHardware() )
				{
					ConcatTransforms( (*m_protationmatrix), bonematrix, (*m_pbonetransform)[i] );

					// MatrixCopy should be faster...
					//ConcatTransforms( (*m_protationmatrix), bonematrix, (*m_plighttransform)[i] );
					MatrixCopy( (*m_pbonetransform)[i], (*m_plighttransform)[i] );
				}
				else
				{
					ConcatTransforms( (*m_paliastransform), bonematrix, (*m_pbonetransform)[i] );
					ConcatTransforms( (*m_protationmatrix), bonematrix, (*m_plighttransform)[i] );
				}

				// Apply client-side effects to the transformation matrix
				StudioFxTransform( m_pCurrentEntity, (*m_pbonetransform)[i] );
			} 
			else 
			{
				ConcatTransforms( (*m_pbonetransform)[pbones[i].parent], bonematrix, (*m_pbonetransform)[i] );
				ConcatTransforms( (*m_plighttransform)[pbones[i].parent], bonematrix, (*m_plighttransform)[i] );
			}
		}
	}
}

/*
====================
StudioDrawModel

====================
*/
int CStudioModelRenderer::StudioDrawModel( int flags )
{
	alight_t lighting;
	vec3_t dir;

	m_pCurrentEntity = IEngineStudio.GetCurrentEntity();
	IEngineStudio.GetTimes( &m_nFrameCount, &m_clTime, &m_clOldTime );
	IEngineStudio.GetViewInfo( m_vRenderOrigin, m_vUp, m_vRight, m_vNormal );
	IEngineStudio.GetAliasScale( &m_fSoftwareXScale, &m_fSoftwareYScale );

	if( m_pCurrentEntity->curstate.renderfx == kRenderFxDeadPlayer )
	{
		entity_state_t deadplayer;

		int result;
		int save_interp;

		if( m_pCurrentEntity->curstate.renderamt <= 0 || m_pCurrentEntity->curstate.renderamt > gEngfuncs.GetMaxClients() )
			return 0;

		// get copy of player
		deadplayer = *( IEngineStudio.GetPlayerState( m_pCurrentEntity->curstate.renderamt - 1 ) ); //cl.frames[cl.parsecount & CL_UPDATE_MASK].playerstate[m_pCurrentEntity->curstate.renderamt - 1];

		// clear weapon, movement state
		deadplayer.number = m_pCurrentEntity->curstate.renderamt;
		deadplayer.weaponmodel = 0;
		deadplayer.gaitsequence = 0;

		deadplayer.movetype = MOVETYPE_NONE;
		VectorCopy( m_pCurrentEntity->curstate.angles, deadplayer.angles );
		VectorCopy( m_pCurrentEntity->curstate.origin, deadplayer.origin );

		save_interp = m_fDoInterp;
		m_fDoInterp = 0;
		
		// draw as though it were a player
		result = StudioDrawPlayer( flags, &deadplayer );
		
		m_fDoInterp = save_interp;
		return result;
	}

	m_pRenderModel = m_pCurrentEntity->model;
	m_pStudioHeader = (studiohdr_t *)IEngineStudio.Mod_Extradata( m_pRenderModel );
	IEngineStudio.StudioSetHeader( m_pStudioHeader );
	IEngineStudio.SetRenderModel( m_pRenderModel );

	StudioSetUpTransform( 0 );

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !IEngineStudio.StudioCheckBBox() )
			return 0;

		( *m_pModelsDrawn )++;
		( *m_pStudioModelCount )++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	if( m_pCurrentEntity->curstate.movetype == MOVETYPE_FOLLOW )
	{
		StudioMergeBones( m_pRenderModel );
	}
	else
	{
		StudioSetupBones();
	}
	StudioSaveBones();

	if( flags & STUDIO_EVENTS )
	{
		StudioCalcAttachments();
		IEngineStudio.StudioClientEvents();
		// copy attachments into global entity array
		if( m_pCurrentEntity->index > 0 )
		{
			cl_entity_t *ent = gEngfuncs.GetEntityByIndex( m_pCurrentEntity->index );

			memcpy( ent->attachment, m_pCurrentEntity->attachment, sizeof(vec3_t) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		lighting.plightvec = dir;
		IEngineStudio.StudioDynamicLight( m_pCurrentEntity, &lighting );

		IEngineStudio.StudioEntityLight( &lighting );

		// model and frame independant
		IEngineStudio.StudioSetupLighting( &lighting );

		// get remap colors
		m_nTopColor = m_pCurrentEntity->curstate.colormap & 0xFF;
		m_nBottomColor = ( m_pCurrentEntity->curstate.colormap & 0xFF00 ) >> 8;

		IEngineStudio.StudioSetRemapColors( m_nTopColor, m_nBottomColor );

		StudioRenderModel();
	}

	return 1;
}

/*
====================
StudioEstimateGait

====================
*/
void CStudioModelRenderer::StudioEstimateGait( entity_state_t *pplayer )
{
	float dt;
	vec3_t est_velocity;

	dt = ( m_clTime - m_clOldTime );
	if( dt < 0.0f )
		dt = 0.0f;
	else if( dt > 1.0f )
		dt = 1.0f;

	if( dt == 0 || m_pPlayerInfo->renderframe == m_nFrameCount )
	{
		m_flGaitMovement = 0;
		return;
	}

	// VectorAdd( pplayer->velocity, pplayer->prediction_error, est_velocity );
	if( m_fGaitEstimation )
	{
		VectorSubtract( m_pCurrentEntity->origin, m_pPlayerInfo->prevgaitorigin, est_velocity );
		VectorCopy( m_pCurrentEntity->origin, m_pPlayerInfo->prevgaitorigin );
		m_flGaitMovement = Length( est_velocity );
		if( dt <= 0 || m_flGaitMovement / dt < 5 )
		{
			m_flGaitMovement = 0;
			est_velocity[0] = 0;
			est_velocity[1] = 0;
		}
	}
	else
	{
		VectorCopy( pplayer->velocity, est_velocity );
		m_flGaitMovement = Length( est_velocity ) * dt;
	}

	if( est_velocity[1] == 0 && est_velocity[0] == 0 )
	{
		float flYawDiff = m_pCurrentEntity->angles[YAW] - m_pPlayerInfo->gaityaw;
		flYawDiff = flYawDiff - (int)( flYawDiff / 360.0f ) * 360.0f;
		if( flYawDiff > 180.0f )
			flYawDiff -= 360.0f;
		if( flYawDiff < -180.0f )
			flYawDiff += 360.0f;

		if( dt < 0.25f )
			flYawDiff *= dt * 4.0f;
		else
			flYawDiff *= dt;

		m_pPlayerInfo->gaityaw += flYawDiff;
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - (int)( m_pPlayerInfo->gaityaw / 360.0f ) * 360.0f;

		m_flGaitMovement = 0;
	}
	else
	{
		m_pPlayerInfo->gaityaw = ( atan2( est_velocity[1], est_velocity[0] ) * 180.0f / M_PI_F );
		if( m_pPlayerInfo->gaityaw > 180.0f )
			m_pPlayerInfo->gaityaw = 180.0f;
		if( m_pPlayerInfo->gaityaw < -180.0f )
			m_pPlayerInfo->gaityaw = -180.0f;
	}
}

/*
====================
StudioProcessGait

====================
*/
void CStudioModelRenderer::StudioProcessGait( entity_state_t *pplayer )
{
	mstudioseqdesc_t *pseqdesc;
	float dt;
	int iBlend;
	float flYaw;	 // view direction relative to movement

	if( m_pCurrentEntity->curstate.sequence >= m_pStudioHeader->numseq )
	{
		m_pCurrentEntity->curstate.sequence = 0;
	}

	pseqdesc = (mstudioseqdesc_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + m_pCurrentEntity->curstate.sequence;

	StudioPlayerBlend( pseqdesc, &iBlend, &m_pCurrentEntity->angles[PITCH] );

	m_pCurrentEntity->latched.prevangles[PITCH] = m_pCurrentEntity->angles[PITCH];
	m_pCurrentEntity->curstate.blending[0] = iBlend;
	m_pCurrentEntity->latched.prevblending[0] = m_pCurrentEntity->curstate.blending[0];
	m_pCurrentEntity->latched.prevseqblending[0] = m_pCurrentEntity->curstate.blending[0];

	// Con_DPrintf( "%f %d\n", m_pCurrentEntity->angles[PITCH], m_pCurrentEntity->blending[0] );

	dt = ( m_clTime - m_clOldTime );
	if( dt < 0.0f )
		dt = 0.0f;
	else if( dt > 1.0f )
		dt = 1.0f;

	StudioEstimateGait( pplayer );

	// Con_DPrintf( "%f %f\n", m_pCurrentEntity->angles[YAW], m_pPlayerInfo->gaityaw );

	// calc side to side turning
	flYaw = m_pCurrentEntity->angles[YAW] - m_pPlayerInfo->gaityaw;
	flYaw = flYaw - (int)( flYaw / 360.0f ) * 360.0f;
	if( flYaw < -180.0f )
		flYaw = flYaw + 360.0f;
	if( flYaw > 180.0f )
		flYaw = flYaw - 360.0f;

	if( flYaw > 120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw - 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw - 180.0f;
	}
	else if( flYaw < -120.0f )
	{
		m_pPlayerInfo->gaityaw = m_pPlayerInfo->gaityaw + 180.0f;
		m_flGaitMovement = -m_flGaitMovement;
		flYaw = flYaw + 180.0f;
	}

	// adjust torso
	m_pCurrentEntity->curstate.controller[0] = ( ( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	m_pCurrentEntity->curstate.controller[1] = ( ( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	m_pCurrentEntity->curstate.controller[2] = ( ( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	m_pCurrentEntity->curstate.controller[3] = ( ( flYaw / 4.0f ) + 30.0f ) / ( 60.0f / 255.0f );
	m_pCurrentEntity->latched.prevcontroller[0] = m_pCurrentEntity->curstate.controller[0];
	m_pCurrentEntity->latched.prevcontroller[1] = m_pCurrentEntity->curstate.controller[1];
	m_pCurrentEntity->latched.prevcontroller[2] = m_pCurrentEntity->curstate.controller[2];
	m_pCurrentEntity->latched.prevcontroller[3] = m_pCurrentEntity->curstate.controller[3];

	m_pCurrentEntity->angles[YAW] = m_pPlayerInfo->gaityaw;
	if( m_pCurrentEntity->angles[YAW] < -0.0f )
		m_pCurrentEntity->angles[YAW] += 360.0f;
	m_pCurrentEntity->latched.prevangles[YAW] = m_pCurrentEntity->angles[YAW];

	if( pplayer->gaitsequence >= m_pStudioHeader->numseq )
	{
		pplayer->gaitsequence = 0;
	}

	pseqdesc = (mstudioseqdesc_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->seqindex ) + pplayer->gaitsequence;

	// calc gait frame
	if( pseqdesc->linearmovement[0] > 0 )
	{
		m_pPlayerInfo->gaitframe += ( m_flGaitMovement / pseqdesc->linearmovement[0] ) * pseqdesc->numframes;
	}
	else
	{
		m_pPlayerInfo->gaitframe += pseqdesc->fps * dt;
	}

	// do modulo
	m_pPlayerInfo->gaitframe = m_pPlayerInfo->gaitframe - (int)(m_pPlayerInfo->gaitframe / pseqdesc->numframes) * pseqdesc->numframes;
	if( m_pPlayerInfo->gaitframe < 0 )
		m_pPlayerInfo->gaitframe += pseqdesc->numframes;
}

/*
====================
StudioDrawPlayer

====================
*/
int CStudioModelRenderer::StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	alight_t lighting;
	vec3_t dir;

	m_pCurrentEntity = IEngineStudio.GetCurrentEntity();
	IEngineStudio.GetTimes( &m_nFrameCount, &m_clTime, &m_clOldTime );
	IEngineStudio.GetViewInfo( m_vRenderOrigin, m_vUp, m_vRight, m_vNormal );
	IEngineStudio.GetAliasScale( &m_fSoftwareXScale, &m_fSoftwareYScale );

	// Con_DPrintf( "DrawPlayer %d\n", m_pCurrentEntity->blending[0] );

	// Con_DPrintf( "DrawPlayer %d %d (%d)\n", m_nFrameCount, pplayer->player_index, m_pCurrentEntity->curstate.sequence );

	// Con_DPrintf("Player %.2f %.2f %.2f\n", pplayer->velocity[0], pplayer->velocity[1], pplayer->velocity[2] );

	m_nPlayerIndex = pplayer->number - 1;

	if( m_nPlayerIndex < 0 || m_nPlayerIndex >= gEngfuncs.GetMaxClients() )
		return 0;

	m_pRenderModel = IEngineStudio.SetupPlayerModel( m_nPlayerIndex );
	if( m_pRenderModel == NULL )
		return 0;

	m_pStudioHeader = (studiohdr_t *)IEngineStudio.Mod_Extradata( m_pRenderModel );
	IEngineStudio.StudioSetHeader( m_pStudioHeader );
	IEngineStudio.SetRenderModel( m_pRenderModel );

	if( pplayer->gaitsequence )
	{
		vec3_t orig_angles;
		m_pPlayerInfo = IEngineStudio.PlayerInfo( m_nPlayerIndex );

		VectorCopy( m_pCurrentEntity->angles, orig_angles );

		StudioProcessGait( pplayer );

		m_pPlayerInfo->gaitsequence = pplayer->gaitsequence;
		m_pPlayerInfo = NULL;

		StudioSetUpTransform( 0 );
		VectorCopy( orig_angles, m_pCurrentEntity->angles );
	}
	else
	{
		m_pCurrentEntity->curstate.controller[0] = 127;
		m_pCurrentEntity->curstate.controller[1] = 127;
		m_pCurrentEntity->curstate.controller[2] = 127;
		m_pCurrentEntity->curstate.controller[3] = 127;
		m_pCurrentEntity->latched.prevcontroller[0] = m_pCurrentEntity->curstate.controller[0];
		m_pCurrentEntity->latched.prevcontroller[1] = m_pCurrentEntity->curstate.controller[1];
		m_pCurrentEntity->latched.prevcontroller[2] = m_pCurrentEntity->curstate.controller[2];
		m_pCurrentEntity->latched.prevcontroller[3] = m_pCurrentEntity->curstate.controller[3];

		m_pPlayerInfo = IEngineStudio.PlayerInfo( m_nPlayerIndex );
		m_pPlayerInfo->gaitsequence = 0;

		StudioSetUpTransform( 0 );
	}

	if( flags & STUDIO_RENDER )
	{
		// see if the bounding box lets us trivially reject, also sets
		if( !IEngineStudio.StudioCheckBBox () )
			return 0;

		( *m_pModelsDrawn )++;
		( *m_pStudioModelCount )++; // render data cache cookie

		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	m_pPlayerInfo = IEngineStudio.PlayerInfo( m_nPlayerIndex );
	StudioSetupBones();
	StudioSaveBones();
	m_pPlayerInfo->renderframe = m_nFrameCount;

	m_pPlayerInfo = NULL;

	if( flags & STUDIO_EVENTS )
	{
		StudioCalcAttachments();
		IEngineStudio.StudioClientEvents();
		// copy attachments into global entity array
		if( m_pCurrentEntity->index > 0 )
		{
			cl_entity_t *ent = gEngfuncs.GetEntityByIndex( m_pCurrentEntity->index );

			memcpy( ent->attachment, m_pCurrentEntity->attachment, sizeof(vec3_t) * 4 );
		}
	}

	if( flags & STUDIO_RENDER )
	{
		if( m_pCvarHiModels->value && m_pRenderModel != m_pCurrentEntity->model )
		{
			// show highest resolution multiplayer model
			m_pCurrentEntity->curstate.body = 255;
		}

		if( !( m_pCvarDeveloper->value == 0 && gEngfuncs.GetMaxClients() == 1 ) && ( m_pRenderModel == m_pCurrentEntity->model ) )
		{
			m_pCurrentEntity->curstate.body = 1; // force helmet
		}

		lighting.plightvec = dir;
		IEngineStudio.StudioDynamicLight( m_pCurrentEntity, &lighting );

		IEngineStudio.StudioEntityLight( &lighting );

		// model and frame independant
		IEngineStudio.StudioSetupLighting( &lighting );

		m_pPlayerInfo = IEngineStudio.PlayerInfo( m_nPlayerIndex );

		// get remap colors
		m_nTopColor = m_pPlayerInfo->topcolor;
		m_nBottomColor = m_pPlayerInfo->bottomcolor;
		if( m_nTopColor < 0 )
			m_nTopColor = 0;
		if( m_nTopColor > 360 )
			m_nTopColor = 360;
		if( m_nBottomColor < 0 )
			m_nBottomColor = 0;
		if( m_nBottomColor > 360 )
			m_nBottomColor = 360;

		IEngineStudio.StudioSetRemapColors( m_nTopColor, m_nBottomColor );

		StudioRenderModel();
		m_pPlayerInfo = NULL;

		if( pplayer->weaponmodel )
		{
			cl_entity_t saveent = *m_pCurrentEntity;

			model_t *pweaponmodel = IEngineStudio.GetModelByIndex( pplayer->weaponmodel );

			m_pStudioHeader = (studiohdr_t *)IEngineStudio.Mod_Extradata( pweaponmodel );
			IEngineStudio.StudioSetHeader( m_pStudioHeader );

			StudioMergeBones( pweaponmodel );

			IEngineStudio.StudioSetupLighting( &lighting );

			StudioRenderModel();

			StudioCalcAttachments();

			*m_pCurrentEntity = saveent;
		}
	}

	return 1;
}

/*
====================
StudioCalcAttachments

====================
*/
void CStudioModelRenderer::StudioCalcAttachments( void )
{
	int i;
	mstudioattachment_t *pattachment;

	if( m_pStudioHeader->numattachments > 4 )
	{
		gEngfuncs.Con_DPrintf( "Too many attachments on %s\n", m_pCurrentEntity->model->name );
		exit( -1 );
	}

	// calculate attachment points
	pattachment = (mstudioattachment_t *)( (byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex );
	for( i = 0; i < m_pStudioHeader->numattachments; i++ )
	{
		VectorTransform( pattachment[i].org, (*m_plighttransform)[pattachment[i].bone], m_pCurrentEntity->attachment[i] );
	}
}

/*
====================
StudioRenderModel

====================
*/
void CStudioModelRenderer::StudioRenderModel( void )
{
	IEngineStudio.SetChromeOrigin();
	IEngineStudio.SetForceFaceFlags( 0 );

	if( m_pCurrentEntity->curstate.renderfx == kRenderFxGlowShell )
	{
		m_pCurrentEntity->curstate.renderfx = kRenderFxNone;
		StudioRenderFinal();

		if( !IEngineStudio.IsHardware() )
		{
			gEngfuncs.pTriAPI->RenderMode( kRenderTransAdd );
		}

		IEngineStudio.SetForceFaceFlags( STUDIO_NF_CHROME );

		gEngfuncs.pTriAPI->SpriteTexture( m_pChromeSprite, 0 );
		m_pCurrentEntity->curstate.renderfx = kRenderFxGlowShell;

		StudioRenderFinal();
		if( !IEngineStudio.IsHardware() )
		{
			gEngfuncs.pTriAPI->RenderMode( kRenderNormal );
		}
	}
	else
	{
		StudioRenderFinal();
	}
}

/*
====================
StudioRenderFinal_Software

====================
*/
void CStudioModelRenderer::StudioRenderFinal_Software( void )
{
	int i;

	// Note, rendermode set here has effect in SW
	IEngineStudio.SetupRenderer( 0 ); 

	if( m_pCvarDrawEntities->value == 2 )
	{
		IEngineStudio.StudioDrawBones();
	}
	else if( m_pCvarDrawEntities->value == 3 )
	{
		IEngineStudio.StudioDrawHulls();
	}
	else
	{
		for( i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			IEngineStudio.StudioSetupModel( i, (void **)&m_pBodyPart, (void **)&m_pSubModel );
			IEngineStudio.StudioDrawPoints();
		}

	}

	if( m_pCvarDrawEntities->value == 4 )
	{
		gEngfuncs.pTriAPI->RenderMode( kRenderTransAdd );
		IEngineStudio.StudioDrawHulls();
		gEngfuncs.pTriAPI->RenderMode( kRenderNormal );
	}

	if( m_pCvarDrawEntities->value == 5 )
	{
		IEngineStudio.StudioDrawAbsBBox();
	}

	IEngineStudio.RestoreRenderer();

	// TriAPI vertices are world-space; draw only after the studio renderer
	// has restored the view transform and render state.
	if( m_pCvarDrawEntities->value != 2 && m_pCvarDrawEntities->value != 3 )
		StudioDrawBlobShadow(
			m_pCurrentEntity,
			g_BlobShadowStudioEntity == m_pCurrentEntity->index ?
				g_BlobShadowStudioOrigin : m_pCurrentEntity->origin
		);
}

/*
====================
StudioRenderFinal_Hardware

====================
*/
void CStudioModelRenderer::StudioRenderFinal_Hardware( void )
{
	int i;
	int rendermode;

	rendermode = IEngineStudio.GetForceFaceFlags() ? kRenderTransAdd : m_pCurrentEntity->curstate.rendermode;
	IEngineStudio.SetupRenderer( rendermode );

	if( m_pCvarDrawEntities->value == 2 )
	{
		IEngineStudio.StudioDrawBones();
	}
	else if( m_pCvarDrawEntities->value == 3 )
	{
		IEngineStudio.StudioDrawHulls();
	}
	else
	{
		for( i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			IEngineStudio.StudioSetupModel( i, (void **)&m_pBodyPart, (void **)&m_pSubModel );

			if( m_fDoInterp )
			{
				// interpolation messes up bounding boxes.
				m_pCurrentEntity->trivial_accept = 0; 
			}

			IEngineStudio.GL_SetRenderMode( rendermode );
			IEngineStudio.StudioDrawPoints();
			IEngineStudio.GL_StudioDrawShadow();
		}

	}

	if( m_pCvarDrawEntities->value == 4 )
	{
		gEngfuncs.pTriAPI->RenderMode( kRenderTransAdd );
		IEngineStudio.StudioDrawHulls();
		gEngfuncs.pTriAPI->RenderMode( kRenderNormal );
	}

	if( m_pCvarDrawEntities->value == 5 )
	{
		IEngineStudio.StudioDrawAbsBBox();
	}

	IEngineStudio.RestoreRenderer();

	// TriAPI vertices are world-space; draw only after the studio renderer
	// has restored the view transform and render state.
	if( m_pCvarDrawEntities->value != 2 && m_pCvarDrawEntities->value != 3 )
	{
		StudioDrawBlobShadow(
			m_pCurrentEntity,
			g_BlobShadowStudioEntity == m_pCurrentEntity->index ?
				g_BlobShadowStudioOrigin : m_pCurrentEntity->origin
		);
		StudioDrawRagdollImpactMarks( this );
	}
}

/*
====================
StudioRenderFinal

====================
*/
void CStudioModelRenderer::StudioRenderFinal( void )
{
	if( IEngineStudio.IsHardware() )
	{
		StudioRenderFinal_Hardware();
	}
	else
	{
		StudioRenderFinal_Software();
	}
}
