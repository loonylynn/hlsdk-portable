// Minimal Xash3D FWGS render API v37 bridge.
//
// This deliberately avoids including the engine's full render_api.h, whose
// model/lightstyle declarations conflict with the stock Half-Life SDK.
// In render_api_t, R_DecalSetupVerts is the 22nd function-pointer entry
// (21 function pointers precede it). Function pointers have the same
// representation in the supported Xash Linux i386 client ABI.

#ifndef XASH_RENDER_API_H
#define XASH_RENDER_API_H

struct decal_s;
struct msurface_s;

typedef void (*xash_render_api_function_t)( void );

typedef struct xash_render_api_v37_s
{
	xash_render_api_function_t reserved_before_decal_setup[21];
	float *( *R_DecalSetupVerts )( struct decal_s *pDecal,
		struct msurface_s *surf, int texture, int *outCount );
} xash_render_api_v37_t;

#if defined( __cplusplus )
static_assert( sizeof( xash_render_api_function_t ) == sizeof( void * ),
	"Xash render API bridge requires pointer-sized function pointers" );
#endif

extern xash_render_api_v37_t *gXashRenderAPI;

#endif // XASH_RENDER_API_H
