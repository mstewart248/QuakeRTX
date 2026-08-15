/*
	d3d9_backend.h -- Direct3D 9 fixed-function rendering backend

	Built for NVIDIA RTX Remix, which reconstructs a scene from the fixed
	function pipeline data a game hands to D3D9. That imposes two hard rules
	on everything in this backend:

	  1. No shaders. Fixed function only -- Remix cannot interpret shader
	     output, so a single vertex/pixel shader anywhere makes those draws
	     invisible to it.
	  2. Static geometry must live in static buffers. Remix identifies meshes
	     by hashing their vertex data and uses that hash to anchor replacement
	     assets. Geometry streamed through D3DUSAGE_DYNAMIC buffers rehashes
	     every frame and cannot be replaced. World surfaces therefore go into
	     immutable buffers built once at map load.

	Enabled with -d3d9 on the command line; without it the engine uses its
	normal OpenGL path and none of this is touched.
*/

#ifndef _D3D9_BACKEND_H
#define _D3D9_BACKEND_H

//is -d3d9 on the command line? valid before video startup.
qboolean D3D9_Requested (void);

//is the device up? false whenever the GL path is in use.
qboolean D3D9_Active (void);

//creates the device on an existing window. hwnd is a HWND.
qboolean D3D9_Init (void *hwnd, int width, int height, qboolean fullscreen);
void D3D9_Shutdown (void);

//recreates the swapchain after a window resize or fullscreen toggle.
void D3D9_Resize (int width, int height, qboolean fullscreen);

//returns false if the device is lost and the frame should be skipped.
qboolean D3D9_BeginFrame (void);
void D3D9_EndFrame (void);

/*
	Textures.

	The engine hands us RGBA byte data (GL_RGBA/GL_UNSIGNED_BYTE order) and
	uploads each mip level explicitly. D3D9 needs the level count up front and
	stores pixels as BGRA, so D3D9_CreateTexture takes the count and the upload
	swizzles. Everything lives in D3DPOOL_MANAGED so a device reset does not
	require recreating it.

	Filter and address state is per sampler stage in D3D9, not per texture, so
	D3D9_SetTextureFilter records the settings and D3D9_BindTexture applies
	them when the texture is bound.
*/
void *D3D9_CreateTexture (int width, int height, int levels, qboolean alpha);
void D3D9_UploadLevel (void *tex, int level, int width, int height, const void *rgba);
void D3D9_SetTextureFilter (void *tex, qboolean mipmap, qboolean nearest, qboolean clamp, int anisotropy);
void D3D9_BindTexture (int stage, void *tex);
void D3D9_DestroyTexture (void *tex);

//how many textures the backend currently holds -- diagnostics only.
int D3D9_TextureCount (void);

//largest square texture the device supports, from D3DCAPS9. The engine needs
//this for gl_hardware_maxsize, which GL_Init would normally fill in from
//glGetIntegerv -- and which must not be left at zero, or TexMgr_SafeTextureSize
//clamps every texture down to nothing.
int D3D9_MaxTextureSize (void);

//largest anisotropy the device supports, from D3DCAPS9. Needed for the same
//reason as the size above: GL_CheckExtensions never runs under -d3d9, so
//gl_max_anisotropy would stay at zero and TexMgr_Anisotropy_f would clamp
//gl_texture_anisotropy down to it, leaving textures blurred at any oblique
//angle with no way to turn filtering back on.
int D3D9_MaxAnisotropy (void);

/*
	3D view setup. The engine already builds its own projection and view
	matrices in OpenGL column-major order; these take them as-is and handle the
	transpose and the [-1,1] to [0,1] depth remap.
*/
void D3D9_SetProjectionFromGL (const float *glmatrix);
void D3D9_SetViewFromGL (const float *glmatrix);
void D3D9_SetViewport (int x, int y, int w, int h);	//GL conventions: origin bottom-left
void D3D9_Set3DState (qboolean cull);

/*
	World geometry.

	All world surface vertices live in a single immutable D3DPOOL_MANAGED
	vertex buffer built once at map load, and each surface is drawn as a
	triangle fan from its fixed offset within it. Nothing rewrites the buffer
	per frame, which is what keeps Remix's geometry hashes stable and lets
	replacement assets stay anchored.

	Vertex layout matches the engine's: xyz, texcoord, lightmap coord.
*/
qboolean D3D9_CreateWorldBuffer (const void *data, int numverts);
void D3D9_DestroyWorldBuffer (void);

//sets stream source, FVF and the fixed-function texture stages for world
//drawing. lightmaps==false gives albedo only, which is what RTX Remix wants
//since it supplies lighting itself and baked lightmaps would double-light.
void D3D9_BeginWorld (qboolean lightmaps, qboolean overbright);
void D3D9_EndWorld (void);
void D3D9_DrawWorldSurface (int firstvert, int numverts);

/*
	World batches.

	One DrawPrimitive per surface is how a rasteriser does it, and it is what
	made submitting anything beyond the view frustum unaffordable here -- under
	Remix every call is marshalled across a process boundary. Pre-triangulating
	the static world into one index run per texture collapses a map's worth of
	surfaces into a few dozen draws, which is what lets the whole level be
	submitted every frame instead of just the part the player is looking at.

	Indices are 32 bit: the world vertex buffer runs well past 65535. Creation
	fails gracefully if the device cannot do that, and the caller falls back to
	D3D9_DrawWorldSurface.
*/
qboolean D3D9_CreateWorldIndexBuffer (const unsigned int *indices, int numindices);
void D3D9_DestroyWorldIndexBuffer (void);
qboolean D3D9_WorldBatchesReady (void);
void D3D9_DrawWorldBatch (int firstindex, int numindices, int minvert, int numverts);

/*
	Sky.

	Remix only path traces sky lighting -- distant lights, the sky probe that
	feeds indirect illumination, HDRI replacement -- for draw calls it has
	classified as sky. It offers several ways to make that classification, and
	the one this backend uses is rtx.skyMinZThreshold: a draw call whose
	VIEWPORT has a minimum depth at or above the threshold (1.0 by default) is
	sky. That is entirely under our control and needs nothing in rtx.conf, no
	texture or geometry hashes to chase, and it cannot accidentally swallow
	world geometry the way the camera heuristics can in a game like Quake where
	sky and world share one camera.

	The other rules the backend is built to satisfy, since Remix's own sky
	handling depends on them:

	  - The sky is a cube centred on the camera, so the cubemap probe Remix
	    bakes by re-rendering these draws is complete in every direction.
	  - Its vertices are a fixed unit cube in a static buffer; the camera
	    position and the size go in the world matrix. Nothing rewrites vertex
	    data, so the geometry hash is stable and rtx.skyBoxGeometries stays
	    usable as a second way to tag it.
	  - Depth test and depth write are both off, which is also what
	    rtx.skyAutoDetect's CameraPositionAndDepthFlags mode looks for.

	The buffer holds two texcoord conventions, 4 verts per face: faces 0-5 have
	v increasing with the face's t axis (what gl_sky.c's baked cube wants) and
	faces 6-11 have it flipped (what a gfx/env skybox wants).
*/
qboolean D3D9_CreateSkyBuffer (const void *data, int numverts);	//xyz + st
void D3D9_DestroySkyBuffer (void);
void D3D9_BeginSky (const float *origin, float scale);
void D3D9_DrawSkyFace (int face);
void D3D9_EndSky (void);

/*
	Lights.

	Remix converts each enabled fixed-function D3D9 light into a path-traced
	light with its own hash, which is what makes it selectable and replaceable
	in the Toolkit. Quake's lights therefore have to be submitted as real
	D3DLIGHT9s even though our own rasterisation stays flat albedo.

	Arrays are packed: origins and colours are 3 floats each, radii 1.
	Keep the ordering stable between frames so a given light keeps its slot.
*/
void D3D9_SubmitLights (const float *origins, const float *radii, const float *colours, int count);

//how many LightEnable calls succeed. Larger than the number that actually
//reach the renderer -- use D3D9_CapsActiveLights for that.
int D3D9_MaxActiveLights (void);
int D3D9_CapsActiveLights (void);

/*
	Alias models (.mdl).

	Every keyframe pose is expanded to float positions once at load time and
	stored in a single immutable vertex buffer; drawing a pose just offsets the
	stream source into it. Nothing is interpolated on the CPU and nothing is
	rewritten per frame, so each pose presents identical vertex data every time
	it appears and Remix can hash and replace it.

	Only whole keyframes live here, so animation out of these buffers steps at
	Quake's 10Hz. d3d9_lerpmodels 1 (the default) draws the in-between frames
	instead, through D3D9_DrawAliasLerped below, and gives up the stable hash
	to do it; 0 keeps everything on these buffers. Either way this is where a
	model that is not currently between two poses gets drawn.
*/
qboolean D3D9_CreateAliasBuffer (void **handle, const void *floatverts, int numverts, int numposes,
					const unsigned short *indices, int numindices, qboolean withnormals);
void D3D9_DestroyAliasBuffer (void **handle);
void D3D9_DrawAlias (void *handle, int pose, int numverts, int numindices);

/*
	Skeletal alias models (.md5mesh -- the 2021 rerelease models).

	Pose expansion is not an option here: a rerelease monster carries over a
	hundred frames, and unlike a .mdl the animation is a bone hierarchy rather
	than a vertex snapshot, so there is nothing to expand without skinning on
	the CPU first -- which would rewrite vertex data every frame and destroy
	the hashes the whole backend is built around.

	Fixed function vertex blending solves it exactly: the vertex buffer holds
	the bind pose once and never changes, and the animation arrives as a
	palette of world matrices through D3DTS_WORLDMATRIX(i). Remix hashes the
	unskinned buffer and runs the skinning itself on the GPU, so one hash
	covers every frame of every animation -- a replacement asset authored
	against the model stays anchored no matter what the monster is doing.

	Vertices are position, three blend weights, four packed bone indices and
	one texcoord (D3DFVF_XYZB4 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_TEX1). The
	fourth weight is implied, which is why the loader's four-influence limit
	costs nothing here.

	The catch is the size of the palette. Fixed function keeps it small -- the
	Remix runtime reports nine matrices, where a rerelease shambler has
	thirty-six -- so most models never get here at all and are skinned pose by
	pose at load instead, ending up in the ordinary alias buffer above. Check
	D3D9_MaxBones before creating anything.

	Bones come in as Quake's bonepose_t: 12 floats, row major, already
	multiplied by the inverse bind pose. The entity transform set by
	D3D9_SetAliasTransform is folded into each one, because with blending
	enabled the palette replaces D3DTS_WORLD entirely.
*/
qboolean D3D9_CreateSkeletalBuffer (void **handle, const void *verts, int numverts,
					const unsigned short *indices, int numindices, int numbones);
void D3D9_DestroySkeletalBuffer (void **handle);
void D3D9_DrawSkeletal (void *handle, const float *bonematrices, int numbones);

//how many matrices the device's blend palette holds. Zero means the device
//cannot do indexed vertex blending at all and skeletal models must be skipped.
int D3D9_MaxBones (void);

/*
	Interpolated models.

	Quake's animations are 10 keyframes a second and the engine's whole job
	between them is to interpolate -- .mdl by blending two poses, md5 by
	blending the bones. Drawing a keyframe straight, as the buffers above do,
	throws that away and the result steps visibly.

	So this takes the vertices the engine has already interpolated and draws
	them directly, without a buffer to keep them in. That is the one thing the
	rest of this backend refuses to do, and the tradeoff is exactly the one in
	the file header: geometry that changes every frame hashes differently every
	frame, so a Remix replacement keyed on a mesh hash will not stick to it.
	Replacements keyed on an asset hash that excludes positions still will --
	see rtx.geometryAssetHashRuleString -- and that is the usual setup for an
	animated game anyway.

	d3d9_lerpmodels picks between the two.

	Drawn from user memory rather than a dynamic buffer on purpose: a
	D3DPOOL_DEFAULT resource would have to be released and rebuilt around
	device resets, which nothing else here needs, and the immediate mode path
	already submits this way.
*/
void D3D9_DrawAliasLerped (const void *verts, int numverts,
				const unsigned short *indices, int numindices, qboolean withnormals);

//entity placement, built from Quake's origin/angles convention
void D3D9_SetAliasTransform (const float *origin, const float *angles);
void D3D9_ClearAliasTransform (void);

//The weapon model. GL fakes this by dropping the camera out of the modelview,
//which leaves Remix unable to work out where the geometry actually is. This
//places it in real world space in front of the camera instead.
void D3D9_SetViewmodelTransform (const float *entorigin, const float *entangles,
					const float *vieworg, const float *viewangles);

#endif	/* _D3D9_BACKEND_H */
