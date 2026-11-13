// Minimal Xash3D FWGS render API v37 bridge.
//
// The engine's full render_api.h conflicts with some stock HL SDK types, so
// this file mirrors only the entries needed by the client shadow renderer.
// Function-pointer slots are kept in their exact v37 order.

#ifndef XASH_RENDER_API_H
#define XASH_RENDER_API_H

#include <stdint.h>
#include <stddef.h>

struct msurface_s;

typedef void (*xash_render_api_function_t)( void );
typedef intptr_t (*xash_render_get_parm_fn)( int parm, int arg );
typedef int (*xash_render_find_texture_fn)( const char *name );
typedef int (*xash_render_create_texture_fn)(
	const char *name, int width, int height, const void *buffer, int flags );
typedef float *( *xash_render_decal_setup_verts_fn )(
	void *pDecal, struct msurface_s *surf, int texture, int *outCount );

typedef struct xash_render_api_v37_s
{
	// Slot 1.
	xash_render_get_parm_fn RenderGetParm;

	// Slots 2-12.
	xash_render_api_function_t reserved_02_to_12[11];

	// Slot 13.
	xash_render_find_texture_fn GL_FindTexture;

	// Slots 14-16.
	xash_render_api_function_t reserved_14_to_16[3];

	// Slot 17.
	xash_render_create_texture_fn GL_CreateTexture;

	// Slots 18-21.
	xash_render_api_function_t reserved_18_to_21[4];

	// Slot 22.
	xash_render_decal_setup_verts_fn R_DecalSetupVerts;
} xash_render_api_v37_t;

// Xash3D FWGS decal_t layout in the version-37 renderer ABI.
typedef struct xash_decal_v37_s
{
	struct xash_decal_v37_s *pnext;
	struct msurface_s *psurface;
	float dx;
	float dy;
	float scale;
	short texture;
	short flags;
	short entityIndex;
	float position[3];
	void *polys;
	intptr_t reserved[4];
} xash_decal_v37_t;

#define XASH_RENDER_PARM_TEX_WIDTH  1
#define XASH_RENDER_PARM_TEX_HEIGHT 2

#if defined( __cplusplus )
static_assert( sizeof( xash_render_api_function_t ) == sizeof( void * ),
	"Xash render API bridge requires pointer-sized function pointers" );
static_assert( offsetof( xash_render_api_v37_t, GL_FindTexture ) == 12 * sizeof( void * ),
	"Unexpected Xash render API v37 GL_FindTexture slot" );
static_assert( offsetof( xash_render_api_v37_t, GL_CreateTexture ) == 16 * sizeof( void * ),
	"Unexpected Xash render API v37 GL_CreateTexture slot" );
static_assert( offsetof( xash_render_api_v37_t, R_DecalSetupVerts ) == 21 * sizeof( void * ),
	"Unexpected Xash render API v37 R_DecalSetupVerts slot" );
static_assert( sizeof( xash_decal_v37_t ) == ( sizeof( void * ) == 4 ? 60 : 88 ),
	"Unexpected Xash render API v37 decal layout" );
#endif

extern xash_render_api_v37_t *gXashRenderAPI;

#endif // XASH_RENDER_API_H
