/*
	SDL_opengl_glext.h shim for the SDL 1.2 build.

	quakedef.h includes <SDL_opengl_glext.h> unconditionally, but that header
	only exists in SDL2 -- SDL 1.2 bundles the whole glext into SDL_opengl.h
	instead (which is why that header is ~380KB). So most of what the engine
	needs is already declared by the time we get here.

	The exception is ARB_buffer_storage and MapBufferRange: SDL 1.2.16's
	bundled glext predates them, so glquake.h's GL_MapBufferRangeFunc and
	GL_BufferStorageFunc declarations fail to parse without the typedefs
	below. Carrying the newer entries is exactly what a glext header is for.

	This lives in Windows/misc/include, which the project searches AFTER the
	SDL include directory. The SDL2 build therefore still picks up the genuine
	SDL2 header from Windows/SDL2/include and never sees this file.
*/

#ifndef QS_SDL12_GLEXT_SHIM_H
#define QS_SDL12_GLEXT_SHIM_H

#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

/* GL_ARB_map_buffer_range / GL 3.0 */
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT					0x0001
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT				0x0002
#endif
#ifndef GL_MAP_INVALIDATE_RANGE_BIT
#define GL_MAP_INVALIDATE_RANGE_BIT		0x0004
#endif
#ifndef GL_MAP_INVALIDATE_BUFFER_BIT
#define GL_MAP_INVALIDATE_BUFFER_BIT	0x0008
#endif
#ifndef GL_MAP_FLUSH_EXPLICIT_BIT
#define GL_MAP_FLUSH_EXPLICIT_BIT		0x0010
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
#define GL_MAP_UNSYNCHRONIZED_BIT		0x0020
#endif

/* GL_ARB_buffer_storage / GL 4.4 */
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT			0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT				0x0080
#endif
#ifndef GL_DYNAMIC_STORAGE_BIT
#define GL_DYNAMIC_STORAGE_BIT			0x0100
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT			0x0200
#endif

/* GL_EXT_texture_shared_exponent / GL 3.0 -- used for e5bgr9 lightmaps */
#ifndef GL_RGB9_E5
#define GL_RGB9_E5						0x8C3D
#endif
#ifndef GL_UNSIGNED_INT_5_9_9_9_REV
#define GL_UNSIGNED_INT_5_9_9_9_REV		0x8C3E
#endif
#ifndef GL_TEXTURE_SHARED_SIZE
#define GL_TEXTURE_SHARED_SIZE			0x8C3F
#endif

typedef void *(APIENTRYP PFNGLMAPBUFFERRANGEPROC) (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
typedef void (APIENTRYP PFNGLBUFFERSTORAGEPROC) (GLenum target, GLsizeiptr size, const void *data, GLbitfield flags);

#endif /* QS_SDL12_GLEXT_SHIM_H */
