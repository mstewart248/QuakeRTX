/*
	d3d9_backend.c -- Direct3D 9 fixed-function rendering backend

	See d3d9_backend.h for why this exists and the constraints RTX Remix
	imposes on it.

	Device creation notes:

	- We deliberately do NOT request D3DCREATE_PUREDEVICE. A pure device
	  refuses Get*State queries, and the fixed function state tracking this
	  backend needs is far easier to reason about when state is readable.
	- Multisampling is left off. Remix path traces the reconstructed scene and
	  does its own antialiasing; MSAA here would only cost fill rate.
	- Direct3DCreate9 resolves through the ordinary DLL search path, so when
	  Remix's d3d9.dll sits next to the executable we get Remix's runtime and
	  otherwise the system one. Nothing here needs to know which it got.
*/

#include "quakedef.h"

#ifdef _WIN32

#include <windows.h>
#include <d3d9.h>
#include <float.h>

#include "d3d9_backend.h"

//provided by the immediate-mode shim in d3d9_imm.c
extern void QGL_ProjectionOverridden (void);

static LPDIRECT3D9		d3d9_object = NULL;
static LPDIRECT3DDEVICE9	d3d9_device = NULL;
static D3DPRESENT_PARAMETERS	d3d9_presentparams;
static qboolean			d3d9_active = false;
static qboolean			d3d9_insidescene = false;
static qboolean			d3d9_devicelost = false;
static int			d3d9_maxtexturesize = 0;
static int			d3d9_maxanisotropy = 1;
static int			d3d9_maxactivelights = 0;
static qboolean			d3d9_index32able = false;
static int			d3d9_activelights = 0;

//size of the fixed-function blend matrix palette, from D3DCAPS9. Skeletal
//models with more bones than this cannot be drawn and are skipped.
#define D3D9_MAX_BONES		256
static int			d3d9_maxbones = 0;

//how many lights the runtime will actually let us enable at once, measured
//rather than taken from D3DCAPS9 -- see D3D9_ProbeLightCapacity
#define D3D9_LIGHT_PROBE_MAX	1024
static int			d3d9_lightcapacity = 0;

/*
===============
D3D9_Requested
===============
*/
qboolean D3D9_Requested (void)
{
	static int cached = -1;

	if (cached < 0)
		cached = COM_CheckParm ("-d3d9") ? 1 : 0;

	return cached ? true : false;
}

/*
===============
D3D9_Active
===============
*/
qboolean D3D9_Active (void)
{
	return d3d9_active;
}

/*
===============
D3D9_GetDevice -- for the immediate-mode shim in d3d9_imm.c
===============
*/
LPDIRECT3DDEVICE9 D3D9_GetDevice (void)
{
	return d3d9_device;
}

/*
===============
D3D9_SetupPresentParams
===============
*/
static void D3D9_SetupPresentParams (HWND hwnd, int width, int height, qboolean fullscreen)
{
	memset (&d3d9_presentparams, 0, sizeof(d3d9_presentparams));

	d3d9_presentparams.BackBufferWidth	= width;
	d3d9_presentparams.BackBufferHeight	= height;
	d3d9_presentparams.BackBufferCount	= 1;
	d3d9_presentparams.hDeviceWindow	= hwnd;
	d3d9_presentparams.Windowed		= fullscreen ? FALSE : TRUE;
	d3d9_presentparams.SwapEffect		= D3DSWAPEFFECT_DISCARD;

	//X8R8G8B8 is the safe universal choice; in windowed mode the format must
	//match the desktop, and D3DFMT_UNKNOWN asks the runtime to do just that.
	d3d9_presentparams.BackBufferFormat	= fullscreen ? D3DFMT_X8R8G8B8 : D3DFMT_UNKNOWN;

	d3d9_presentparams.EnableAutoDepthStencil	= TRUE;
	d3d9_presentparams.AutoDepthStencilFormat	= D3DFMT_D24S8;

	//no MSAA -- see file header
	d3d9_presentparams.MultiSampleType	= D3DMULTISAMPLE_NONE;
	d3d9_presentparams.MultiSampleQuality	= 0;

	d3d9_presentparams.PresentationInterval	= D3DPRESENT_INTERVAL_IMMEDIATE;
}

/*
===============
D3D9_ReportAdapter
===============
*/
static void D3D9_ReportAdapter (void)
{
	D3DADAPTER_IDENTIFIER9	ident;

	if (FAILED(IDirect3D9_GetAdapterIdentifier (d3d9_object, D3DADAPTER_DEFAULT, 0, &ident)))
		return;

	Con_Printf ("D3D9 adapter: %s\n", ident.Description);
	Con_Printf ("D3D9 driver : %s\n", ident.Driver);
}

static void D3D9_ProbeLightCapacity (void);
static void D3D9_ApplyDefaultState (void);
static void D3D9_InvalidateCachedState (void);

/*
===============
D3D9_ApplyDefaultState

Stands in for GL_SetupState, which never runs under -d3d9.

These are the global defaults the engine assumes exist: several draw paths
enable blending or alpha testing without ever setting the factors, because
GLQuake set them once at startup. Left at D3D9's own defaults (SRCBLEND=ONE,
DESTBLEND=ZERO) alpha is simply ignored, and blended content such as particles
renders as opaque black quads.

IDirect3DDevice9::Reset throws every render state back to those D3D9 defaults,
so this has to run again after each successful reset -- a resolution change or
a fullscreen toggle otherwise leaves the engine drawing with SRCBLEND=ONE,
DESTBLEND=ZERO and the alpha test wide open, which is what makes the HUD, menu
and console lose their transparency after vid_restart.
===============
*/
static void D3D9_ApplyDefaultState (void)
{
	if (!d3d9_device)
		return;

	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	//glAlphaFunc(GL_GREATER, 0.666) -- 0.666*255 = 170. Fences and grates
	//depend on this threshold; 127 lets through pixels GL would have cut.
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHATESTENABLE, TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHAREF, 170);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHAFUNC, D3DCMP_GREATER);

	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_SHADEMODE, D3DSHADE_FLAT);	//glShadeModel(GL_FLAT)
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_LIGHTING, FALSE);
}

/*
===============
D3D9_Init
===============
*/
qboolean D3D9_Init (void *hwnd, int width, int height, qboolean fullscreen)
{
	D3DCAPS9	caps;
	DWORD		behaviour;
	HRESULT		hr;

	if (d3d9_active)
		return true;

	d3d9_object = Direct3DCreate9 (D3D_SDK_VERSION);
	if (!d3d9_object)
	{
		Con_Warning ("D3D9: Direct3DCreate9 failed (no D3D9 runtime?)\n");
		return false;
	}

	D3D9_ReportAdapter ();

	//software vertex processing is a valid fallback, but every card that can
	//run Remix does hardware T&L, so this should always take the fast path.
	if (FAILED(IDirect3D9_GetDeviceCaps (d3d9_object, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps)))
	{
		behaviour = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
		d3d9_maxtexturesize = 1024;	//conservative floor if caps are unavailable
		//software vertex processing always has the full 256 world matrices
		d3d9_maxbones = D3D9_MAX_BONES;
	}
	else
	{
		behaviour = (caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)
				? D3DCREATE_HARDWARE_VERTEXPROCESSING
				: D3DCREATE_SOFTWARE_VERTEXPROCESSING;

		//the engine wants one square limit, so take the smaller dimension
		d3d9_maxtexturesize = (int) q_min(caps.MaxTextureWidth, caps.MaxTextureHeight);
		if (d3d9_maxtexturesize < 256)
			d3d9_maxtexturesize = 256;

		if (caps.RasterCaps & D3DPRASTERCAPS_ANISOTROPY)
			d3d9_maxanisotropy = (int)caps.MaxAnisotropy;
		if (d3d9_maxanisotropy < 1)
			d3d9_maxanisotropy = 1;

		d3d9_maxactivelights = (int)caps.MaxActiveLights;
		Con_Printf ("D3D9: MaxActiveLights = %d\n", d3d9_maxactivelights);

		//the world index buffer needs 32 bit indices: one map's worth of
		//surfaces runs well past 65535 vertices
		d3d9_index32able = (caps.MaxVertexIndex > 0xFFFF) ? true : false;

		/*
			Skeletal models need indexed vertex blending: one matrix per bone
			in the palette, four of them addressed per vertex. Both halves are
			separate caps and both have to hold up, so take the palette size
			only when the device can actually blend four matrices at once.

			Software vertex processing above ignores these -- the runtime
			implements the full 256 there regardless of what the card says.
		*/
		if (caps.MaxVertexBlendMatrices >= 4)
			d3d9_maxbones = (int)caps.MaxVertexBlendMatrixIndex + 1;
		else
			d3d9_maxbones = 0;
		if (d3d9_maxbones > D3D9_MAX_BONES)
			d3d9_maxbones = D3D9_MAX_BONES;

		Con_Printf ("D3D9: MaxVertexBlendMatrices = %d, MatrixIndex = %d (%d bones)\n",
				(int)caps.MaxVertexBlendMatrices,
				(int)caps.MaxVertexBlendMatrixIndex, d3d9_maxbones);
	}

	//the engine drives rendering from the main thread only, but SDL owns the
	//window and pumps messages, so let the runtime be defensive about focus.
	behaviour |= D3DCREATE_FPU_PRESERVE;

	D3D9_SetupPresentParams ((HWND)hwnd, width, height, fullscreen);

	hr = IDirect3D9_CreateDevice (d3d9_object, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
					(HWND)hwnd, behaviour, &d3d9_presentparams, &d3d9_device);
	if (FAILED(hr))
	{
		Con_Warning ("D3D9: CreateDevice failed (0x%08lx)\n", (unsigned long)hr);
		IDirect3D9_Release (d3d9_object);
		d3d9_object = NULL;
		return false;
	}

	/*
		Restore double precision on the x87 control word.

		Creating a D3D9 device switches the FPU to single precision unless
		D3DCREATE_FPU_PRESERVE is honoured, and not every driver honours it.
		That matters here far more than it looks: Sys_DoubleTime divides a large
		performance counter by its frequency, and with a 24-bit mantissa the
		result of consecutive calls rounds to the same value. host_frametime
		then comes out as zero, Host_FilterTime never lets another frame run,
		and the engine spins at 100% CPU having rendered exactly one frame.

		Setting it back explicitly costs nothing and removes the dependency on
		the driver respecting the creation flag.
	*/
	_controlfp_s (NULL, _PC_53, _MCW_PC);

	D3D9_ApplyDefaultState ();
	D3D9_InvalidateCachedState ();

	d3d9_active = true;
	d3d9_devicelost = false;
	d3d9_insidescene = false;

	D3D9_ProbeLightCapacity ();

	Con_Printf ("D3D9: %dx%d %s, fixed function\n", width, height, fullscreen ? "fullscreen" : "windowed");

	return true;
}

/*
===============
D3D9_Shutdown
===============
*/
void D3D9_Shutdown (void)
{
	if (d3d9_device)
	{
		if (d3d9_insidescene)
		{
			IDirect3DDevice9_EndScene (d3d9_device);
			d3d9_insidescene = false;
		}
		IDirect3DDevice9_Release (d3d9_device);
		d3d9_device = NULL;
	}

	if (d3d9_object)
	{
		IDirect3D9_Release (d3d9_object);
		d3d9_object = NULL;
	}

	d3d9_active = false;
}

/*
===============
D3D9_ResetDevice

Rebuilds the swapchain. Any D3DPOOL_DEFAULT resource would have to be
released before this and recreated after; the backend keeps its buffers in
D3DPOOL_MANAGED precisely so that reset stays this simple.
===============
*/
static qboolean D3D9_ResetDevice (void)
{
	HRESULT hr;

	if (!d3d9_device)
		return false;

	if (d3d9_insidescene)
	{
		IDirect3DDevice9_EndScene (d3d9_device);
		d3d9_insidescene = false;
	}

	hr = IDirect3DDevice9_Reset (d3d9_device, &d3d9_presentparams);
	if (FAILED(hr))
		return false;

	/*
		Reset does not preserve device state. Render states, texture stage and
		sampler states, transforms, lights and texture bindings all return to
		the runtime's defaults, and nothing else puts them back: GL_SetupState
		is a no-op under -d3d9, and the per-frame setup only touches the states
		it varies. Reassert the startup defaults and drop every cache that
		claims to know what the device already has.
	*/
	D3D9_ApplyDefaultState ();
	D3D9_InvalidateCachedState ();

	d3d9_devicelost = false;
	return true;
}

/*
===============
D3D9_Resize
===============
*/
void D3D9_Resize (int width, int height, qboolean fullscreen)
{
	if (!d3d9_active)
		return;

	D3D9_SetupPresentParams (d3d9_presentparams.hDeviceWindow, width, height, fullscreen);

	if (!D3D9_ResetDevice ())
	{
		//not fatal: the next BeginFrame retries via the lost-device path.
		d3d9_devicelost = true;
		Con_DPrintf ("D3D9: reset after resize failed, will retry\n");
	}
}

/*
===============
D3D9_BeginFrame
===============
*/
qboolean D3D9_BeginFrame (void)
{
	HRESULT hr;

	if (!d3d9_active || !d3d9_device)
		return false;

	//a lost device stays lost until the runtime says it can be reset, which
	//normally means the user has alt-tabbed back to us.
	if (d3d9_devicelost)
	{
		hr = IDirect3DDevice9_TestCooperativeLevel (d3d9_device);
		if (hr == D3DERR_DEVICELOST)
			return false;
		if (hr == D3DERR_DEVICENOTRESET && !D3D9_ResetDevice ())
			return false;
		d3d9_devicelost = false;
	}

	/*
		Reset to a full-screen viewport before clearing.

		Clear with a NULL rect clears the CURRENT VIEWPORT, not the whole render
		target. The last thing drawn in a frame is 2D, which leaves the viewport
		set to whatever canvas it used last -- the menu canvas is only 640x200 --
		so without this the next frame clears a small rectangle and everything
		outside it keeps undefined backbuffer contents.
	*/
	{
		D3DVIEWPORT9 vp;
		vp.X = 0;
		vp.Y = 0;
		vp.Width = d3d9_presentparams.BackBufferWidth;
		vp.Height = d3d9_presentparams.BackBufferHeight;
		vp.MinZ = 0.0f;
		vp.MaxZ = 1.0f;
		IDirect3DDevice9_SetViewport (d3d9_device, &vp);
	}

	IDirect3DDevice9_Clear (d3d9_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
				D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

	if (FAILED(IDirect3DDevice9_BeginScene (d3d9_device)))
		return false;

	d3d9_insidescene = true;
	return true;
}

/*
===============
D3D9_EndFrame
===============
*/
void D3D9_EndFrame (void)
{
	HRESULT hr;

	if (!d3d9_active || !d3d9_device)
		return;

	if (d3d9_insidescene)
	{
		IDirect3DDevice9_EndScene (d3d9_device);
		d3d9_insidescene = false;
	}

	/*
		Present needs the window to be pumping messages.

		Quake calls SCR_UpdateScreen from inside the map load path (the loading
		plaque), and during a load the engine is not servicing the message
		queue. Presenting there blocks in the driver and never returns -- the
		main thread stops while the driver's worker threads spin, which is
		exactly the "one frame then 100% CPU" hang seen during bringup.

		The GL path never had to care: SDL_GL_SwapWindow pumps internally.
	*/
	SDL_PumpEvents ();

	hr = IDirect3DDevice9_Present (d3d9_device, NULL, NULL, NULL, NULL);
	if (hr == D3DERR_DEVICELOST)
		d3d9_devicelost = true;
}

/*
=============================================================================

TEXTURES

=============================================================================
*/

typedef struct d3d9texture_s
{
	LPDIRECT3DTEXTURE9	tex;
	DWORD			minfilter;
	DWORD			magfilter;
	DWORD			mipfilter;
	DWORD			address;
	DWORD			maxaniso;
} d3d9texture_t;

static int	d3d9_texturecount = 0;

//cached per-stage binding so redundant SetTexture calls are skipped
#define D3D9_MAX_STAGES		8
static d3d9texture_t	*d3d9_boundtexture[D3D9_MAX_STAGES];

/*
===============
D3D9_TextureCount
===============
*/
int D3D9_TextureCount (void)
{
	return d3d9_texturecount;
}

/*
===============
D3D9_MaxTextureSize
===============
*/
int D3D9_MaxAnisotropy (void)
{
	return d3d9_maxanisotropy;
}

int D3D9_MaxTextureSize (void)
{
	return d3d9_maxtexturesize;
}

/*
===============
D3D9_CreateTexture
===============
*/
void *D3D9_CreateTexture (int width, int height, int levels, qboolean alpha)
{
	d3d9texture_t	*t;
	HRESULT		hr;

	if (!d3d9_device)
		return NULL;

	t = (d3d9texture_t *) calloc (1, sizeof(*t));
	if (!t)
		return NULL;

	//A8R8G8B8 for both cases: X8R8G8B8 would save nothing here (D3D9 pads it
	//to 32 bits anyway) and using one format keeps the upload path single.
	(void) alpha;

	hr = IDirect3DDevice9_CreateTexture (d3d9_device, width, height, levels, 0,
						D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t->tex, NULL);
	if (FAILED(hr))
	{
		Con_DPrintf ("D3D9: CreateTexture %dx%d (%d levels) failed (0x%08lx)\n",
				width, height, levels, (unsigned long)hr);
		free (t);
		return NULL;
	}

	//sane defaults until TexMgr_SetFilterModes says otherwise
	t->minfilter = D3DTEXF_LINEAR;
	t->magfilter = D3DTEXF_LINEAR;
	t->mipfilter = D3DTEXF_NONE;
	t->address   = D3DTADDRESS_WRAP;
	t->maxaniso  = 1;

	d3d9_texturecount++;
	return t;
}

/*
===============
D3D9_UploadLevel

Converts the engine's RGBA byte order to the BGRA that D3DFMT_A8R8G8B8 wants
while copying row by row, since the locked surface pitch need not match the
source stride.
===============
*/
void D3D9_UploadLevel (void *tex, int level, int width, int height, const void *rgba)
{
	d3d9texture_t	*t = (d3d9texture_t *)tex;
	D3DLOCKED_RECT	lock;
	const byte	*src;
	int		x, y;

	if (!t || !t->tex || !rgba)
		return;

	if (FAILED(IDirect3DTexture9_LockRect (t->tex, level, &lock, NULL, 0)))
	{
		Con_DPrintf ("D3D9: LockRect failed on level %d\n", level);
		return;
	}

	src = (const byte *)rgba;

	for (y = 0; y < height; y++)
	{
		byte *dst = (byte *)lock.pBits + (size_t)y * lock.Pitch;

		for (x = 0; x < width; x++, src += 4, dst += 4)
		{
			dst[0] = src[2];	//B
			dst[1] = src[1];	//G
			dst[2] = src[0];	//R
			dst[3] = src[3];	//A
		}
	}

	IDirect3DTexture9_UnlockRect (t->tex, level);
}

/*
===============
D3D9_TranslateFilter -- D3D9_FILTER_* to the D3DTEXF_* the sampler wants
===============
*/
static DWORD D3D9_TranslateFilter (int filter, DWORD fallback)
{
	switch (filter)
	{
	case D3D9_FILTER_POINT:		return D3DTEXF_POINT;
	case D3D9_FILTER_LINEAR:	return D3DTEXF_LINEAR;
	case D3D9_FILTER_NOMIP:		return D3DTEXF_NONE;
	default:			return fallback;
	}
}

/*
===============
D3D9_SetTextureFilter

minfilter/magfilter/mipfilter are D3D9_FILTER_* values, kept separate so the
caller can ask for any of the combinations gl_texturemode names -- bilinear
(linear min/mag, point mip) is a different thing from trilinear (linear
throughout), and a point mipfilter is a different thing again from
D3D9_FILTER_NOMIP, which ignores the mip chain entirely.
===============
*/
void D3D9_SetTextureFilter (void *tex, int minfilter, int magfilter, int mipfilter, qboolean clamp, int anisotropy)
{
	d3d9texture_t *t = (d3d9texture_t *)tex;

	if (!t)
		return;

	t->minfilter = D3D9_TranslateFilter (minfilter, D3DTEXF_LINEAR);
	t->magfilter = D3D9_TranslateFilter (magfilter, D3DTEXF_LINEAR);
	t->mipfilter = D3D9_TranslateFilter (mipfilter, D3DTEXF_NONE);
	t->address   = clamp ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP;
	t->maxaniso  = (anisotropy > 1) ? (DWORD)anisotropy : 1;

	//anisotropic minification only makes sense with a mip chain to walk, and
	//only when minification was going to blend texels in the first place --
	//asking for it on a point filter would undo the blockiness that was the
	//whole point of choosing one
	if (t->maxaniso > 1 && t->mipfilter != D3DTEXF_NONE && t->minfilter == D3DTEXF_LINEAR)
		t->minfilter = D3DTEXF_ANISOTROPIC;

	//force reapplication on the next bind
	{
		int i;
		for (i = 0; i < D3D9_MAX_STAGES; i++)
			if (d3d9_boundtexture[i] == t)
				d3d9_boundtexture[i] = NULL;
	}
}

/*
===============
D3D9_BindTexture
===============
*/
void D3D9_BindTexture (int stage, void *tex)
{
	d3d9texture_t *t = (d3d9texture_t *)tex;

	if (!d3d9_device || stage < 0 || stage >= D3D9_MAX_STAGES)
		return;


	if (d3d9_boundtexture[stage] == t)
		return;

	d3d9_boundtexture[stage] = t;

	if (!t)
	{
		IDirect3DDevice9_SetTexture (d3d9_device, stage, NULL);
		return;
	}

	IDirect3DDevice9_SetTexture (d3d9_device, stage, (IDirect3DBaseTexture9 *)t->tex);

	IDirect3DDevice9_SetSamplerState (d3d9_device, stage, D3DSAMP_MINFILTER, t->minfilter);
	IDirect3DDevice9_SetSamplerState (d3d9_device, stage, D3DSAMP_MAGFILTER, t->magfilter);
	IDirect3DDevice9_SetSamplerState (d3d9_device, stage, D3DSAMP_MIPFILTER, t->mipfilter);
	IDirect3DDevice9_SetSamplerState (d3d9_device, stage, D3DSAMP_ADDRESSU, t->address);
	IDirect3DDevice9_SetSamplerState (d3d9_device, stage, D3DSAMP_ADDRESSV, t->address);
	IDirect3DDevice9_SetSamplerState (d3d9_device, stage, D3DSAMP_MAXANISOTROPY, t->maxaniso);
}

/*
===============
D3D9_DestroyTexture
===============
*/
void D3D9_DestroyTexture (void *tex)
{
	d3d9texture_t	*t = (d3d9texture_t *)tex;
	int		i;

	if (!t)
		return;

	for (i = 0; i < D3D9_MAX_STAGES; i++)
	{
		if (d3d9_boundtexture[i] == t)
		{
			if (d3d9_device)
				IDirect3DDevice9_SetTexture (d3d9_device, i, NULL);
			d3d9_boundtexture[i] = NULL;
		}
	}

	if (t->tex)
		IDirect3DTexture9_Release (t->tex);

	free (t);
	d3d9_texturecount--;
}

/*
===============
D3D9_InvalidateCachedState

Drops the redundancy caches that shadow device state, for use after a reset
has silently cleared the device side of them.

The texture cache is the one that bites: Reset unbinds every stage, but
d3d9_boundtexture still names whatever was bound before, so D3D9_BindTexture
short-circuits and the next draw samples a stage with no texture in it. That
is enough on its own to turn the charset, HUD and menu into garbage after a
mode change. Clearing it also forces the sampler state to be reapplied, which
Reset dropped back to point filtering and wrap addressing.
===============
*/
static void D3D9_InvalidateCachedState (void)
{
	int i;

	for (i = 0; i < D3D9_MAX_STAGES; i++)
		d3d9_boundtexture[i] = NULL;

	//every light slot comes back disabled, so nothing needs turning off
	d3d9_activelights = 0;
}

/*
=============================================================================

3D VIEW SETUP

=============================================================================
*/

/*
===============
D3D9_TransposeGLMatrix

The engine builds its matrices in OpenGL's column-major order (they are handed
to glLoadMatrixf as-is). D3D9 expects row-major, so everything transposes.
===============
*/
static void D3D9_TransposeGLMatrix (const float *src, D3DMATRIX *dst)
{
	int r, c;

	/*
		GL stores element (row, col) at index col*4+row, and multiplies as
		v' = M*v (column vectors). D3D9 multiplies as v' = v*M (row vectors),
		so its matrix is the transpose of GL's logical one.

		Those two facts cancel: the transpose of "read column-major" is simply
		"read the floats in order". Indexing src[c*4+r] instead copies the
		logical matrix unchanged, which leaves the projection's w term in
		m[3][2] rather than m[2][3] and clips the entire scene away.
	*/
	for (r = 0; r < 4; r++)
		for (c = 0; c < 4; c++)
			dst->m[r][c] = src[r * 4 + c];
}

/*
===============
D3D9_SetProjectionFromGL
===============
*/
void D3D9_SetProjectionFromGL (const float *glmatrix)
{
	D3DMATRIX m;
	int i;

	if (!d3d9_device)
		return;

	//the 2D shim owns D3DTS_PROJECTION between canvases; tell it we are taking
	//it, so the next glViewport does not put the last ortho back
	QGL_ProjectionOverridden ();

	D3D9_TransposeGLMatrix (glmatrix, &m);

	/*
		GL clips z to [-1,1], D3D9 to [0,1]. Fold the remap (z' = (z+w)/2)
		into the projection rather than touching the depth range, so depth
		precision stays where D3D9 expects it. In row-vector convention that
		is a post-multiply by diag(1,1,0.5,1) with a 0.5 translate in z, which
		reduces to adjusting the third column.
	*/
	for (i = 0; i < 4; i++)
		m.m[i][2] = 0.5f * (m.m[i][2] + m.m[i][3]);

	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_PROJECTION, &m);
}

/*
===============
D3D9_SetViewFromGL
===============
*/
void D3D9_SetViewFromGL (const float *glmatrix)
{
	D3DMATRIX m, identity;

	if (!d3d9_device)
		return;

	D3D9_TransposeGLMatrix (glmatrix, &m);

	memset (&identity, 0, sizeof(identity));
	identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;

	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_VIEW, &m);
	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLD, &identity);
}

/*
===============
D3D9_SetViewport -- takes GL conventions (origin bottom-left)
===============
*/
void D3D9_SetViewport (int x, int y, int w, int h)
{
	D3DVIEWPORT9 vp;

	if (!d3d9_device)
		return;

	if (w < 1) w = 1;
	if (h < 1) h = 1;

	vp.X = (DWORD)q_max(0, x);
	vp.Y = (DWORD)q_max(0, (int)vid.height - y - h);
	vp.Width = (DWORD)w;
	vp.Height = (DWORD)h;
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;

	IDirect3DDevice9_SetViewport (d3d9_device, &vp);
}

/*
===============
D3D9_Set3DState
===============
*/
void D3D9_Set3DState (qboolean cull)
{
	if (!d3d9_device)
		return;

	//Quake winds its world polygons the opposite way to D3D9's default
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_CULLMODE, cull ? D3DCULL_CCW : D3DCULL_NONE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZENABLE, D3DZB_TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZWRITEENABLE, TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_LIGHTING, FALSE);
}

/*
=============================================================================

WORLD GEOMETRY

=============================================================================
*/

//xyz + texcoord + lightmap coord, matching the engine's VERTEXSIZE layout
#define D3D9_WORLD_FVF		(D3DFVF_XYZ | D3DFVF_TEX2)
#define D3D9_WORLD_STRIDE	(7 * sizeof(float))

static LPDIRECT3DVERTEXBUFFER9	d3d9_worldvb = NULL;
static int			d3d9_worldverts = 0;
static LPDIRECT3DINDEXBUFFER9	d3d9_worldib = NULL;
static int			d3d9_worldindices = 0;

/*
===============
D3D9_CreateWorldBuffer
===============
*/
qboolean D3D9_CreateWorldBuffer (const void *data, int numverts)
{
	void	*dst;
	UINT	bytes;

	D3D9_DestroyWorldBuffer ();

	if (!d3d9_device || !data || numverts <= 0)
		return false;

	bytes = (UINT)(numverts * D3D9_WORLD_STRIDE);

	//MANAGED with no DYNAMIC usage: written once, never locked again, and it
	//survives a device reset untouched.
	if (FAILED(IDirect3DDevice9_CreateVertexBuffer (d3d9_device, bytes, 0, D3D9_WORLD_FVF,
							D3DPOOL_MANAGED, &d3d9_worldvb, NULL)))
	{
		Con_Warning ("D3D9: world vertex buffer (%u bytes) failed\n", (unsigned)bytes);
		d3d9_worldvb = NULL;
		return false;
	}

	if (FAILED(IDirect3DVertexBuffer9_Lock (d3d9_worldvb, 0, bytes, &dst, 0)))
	{
		IDirect3DVertexBuffer9_Release (d3d9_worldvb);
		d3d9_worldvb = NULL;
		return false;
	}

	memcpy (dst, data, bytes);
	IDirect3DVertexBuffer9_Unlock (d3d9_worldvb);

	d3d9_worldverts = numverts;
	Con_Printf ("D3D9: world buffer %d verts (%u kb, static)\n", numverts, (unsigned)(bytes / 1024));

	return true;
}

/*
===============
D3D9_DestroyWorldBuffer
===============
*/
void D3D9_DestroyWorldBuffer (void)
{
	if (d3d9_worldvb)
	{
		IDirect3DVertexBuffer9_Release (d3d9_worldvb);
		d3d9_worldvb = NULL;
	}
	d3d9_worldverts = 0;

	D3D9_DestroyWorldIndexBuffer ();
}

/*
===============
D3D9_CreateWorldIndexBuffer

The static world, pre-triangulated and sorted into one run per texture, so a
whole map's worth of surfaces goes out as a handful of DrawIndexedPrimitive
calls instead of one DrawPrimitive per surface.

That is what makes submitting the entire level affordable, and submitting the
entire level is what a path tracer needs: geometry the game stops drawing
leaves Remix's BVH, and a reflection of a room you cannot see is a reflection
of nothing. It also fixes the hashes for good -- the same indices over the same
vertices every frame, forever, so a replacement asset anchored to a batch stays
anchored.
===============
*/
qboolean D3D9_CreateWorldIndexBuffer (const unsigned int *indices, int numindices)
{
	void	*dst;
	UINT	bytes;

	D3D9_DestroyWorldIndexBuffer ();

	if (!d3d9_device || !indices || numindices <= 0)
		return false;

	if (!d3d9_index32able)
	{
		Con_Warning ("D3D9: device has no 32 bit indices, world batching off\n");
		return false;
	}

	bytes = (UINT)((size_t)numindices * sizeof(unsigned int));

	if (FAILED(IDirect3DDevice9_CreateIndexBuffer (d3d9_device, bytes, 0, D3DFMT_INDEX32,
							D3DPOOL_MANAGED, &d3d9_worldib, NULL)))
	{
		Con_Warning ("D3D9: world index buffer (%u bytes) failed\n", (unsigned)bytes);
		d3d9_worldib = NULL;
		return false;
	}

	if (FAILED(IDirect3DIndexBuffer9_Lock (d3d9_worldib, 0, bytes, &dst, 0)))
	{
		IDirect3DIndexBuffer9_Release (d3d9_worldib);
		d3d9_worldib = NULL;
		return false;
	}

	memcpy (dst, indices, bytes);
	IDirect3DIndexBuffer9_Unlock (d3d9_worldib);

	d3d9_worldindices = numindices;
	return true;
}

/*
===============
D3D9_DestroyWorldIndexBuffer
===============
*/
void D3D9_DestroyWorldIndexBuffer (void)
{
	if (d3d9_worldib)
	{
		IDirect3DIndexBuffer9_Release (d3d9_worldib);
		d3d9_worldib = NULL;
	}
	d3d9_worldindices = 0;
}

/*
===============
D3D9_WorldBatchesReady
===============
*/
qboolean D3D9_WorldBatchesReady (void)
{
	return (d3d9_worldvb && d3d9_worldib) ? true : false;
}

/*
===============
D3D9_DrawWorldBatch

minvert/numverts describe the slice of the vertex buffer this run actually
touches. They could both be "the whole buffer" and D3D9 would not care, but
Remix hashes the vertices a draw references, so handing it the real range keeps
each batch's hash to its own geometry instead of the entire map.
===============
*/
void D3D9_DrawWorldBatch (int firstindex, int numindices, int minvert, int numverts)
{
	if (!d3d9_device || !d3d9_worldvb || !d3d9_worldib || numindices < 3)
		return;

	if (firstindex < 0 || firstindex + numindices > d3d9_worldindices)
		return;
	if (minvert < 0 || numverts <= 0 || minvert + numverts > d3d9_worldverts)
		return;

	IDirect3DDevice9_DrawIndexedPrimitive (d3d9_device, D3DPT_TRIANGLELIST,
						0,			//BaseVertexIndex
						(UINT)minvert,
						(UINT)numverts,
						(UINT)firstindex,
						(UINT)(numindices / 3));
}

/*
===============
D3D9_BeginWorld
===============
*/
void D3D9_BeginWorld (qboolean lightmaps, qboolean overbright)
{
	if (!d3d9_device || !d3d9_worldvb)
		return;

	IDirect3DDevice9_SetStreamSource (d3d9_device, 0, d3d9_worldvb, 0, (UINT)D3D9_WORLD_STRIDE);
	IDirect3DDevice9_SetFVF (d3d9_device, D3D9_WORLD_FVF);
	IDirect3DDevice9_SetIndices (d3d9_device, d3d9_worldib);	//NULL is fine

	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZENABLE, D3DZB_TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZWRITEENABLE, TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHABLENDENABLE, FALSE);
	//GL_SetupState uses glFrontFace(GL_CW) + cull back, so clockwise is the
	//front face; D3DCULL_CCW is the same rule stated the other way round.
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_CULLMODE, D3DCULL_CCW);

	//stage 0: the surface texture, straight through
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_TEXCOORDINDEX, 0);

	if (lightmaps)
	{	//stage 1: modulate by the lightmap. 2x is Quake's overbright.
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLOROP,
					overbright ? D3DTOP_MODULATE2X : D3DTOP_MODULATE);
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_TEXCOORDINDEX, 1);
	}
	else
	{	//albedo only -- Remix lights the scene itself
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		IDirect3DDevice9_SetTexture (d3d9_device, 1, NULL);
	}
}

/*
===============
D3D9_EndWorld
===============
*/
void D3D9_EndWorld (void)
{
	if (!d3d9_device)
		return;

	IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTexture (d3d9_device, 1, NULL);
}

/*
===============
D3D9_DrawWorldSurface

One surface, one fan, straight out of the static buffer. No per-frame vertex
writes anywhere in this path -- that is the whole point.
===============
*/
void D3D9_DrawWorldSurface (int firstvert, int numverts)
{
	if (!d3d9_device || !d3d9_worldvb || numverts < 3)
		return;

	if (firstvert < 0 || firstvert + numverts > d3d9_worldverts)
		return;

	IDirect3DDevice9_DrawPrimitive (d3d9_device, D3DPT_TRIANGLEFAN, (UINT)firstvert, (UINT)(numverts - 2));
}

/*
=============================================================================

SKY

See the header for why the sky gets its own buffer and its own viewport.

=============================================================================
*/

#define D3D9_SKY_FVF		(D3DFVF_XYZ | D3DFVF_TEX1)
#define D3D9_SKY_STRIDE		(5 * sizeof(float))

static LPDIRECT3DVERTEXBUFFER9	d3d9_skyvb = NULL;
static int			d3d9_skyverts = 0;
static D3DVIEWPORT9		d3d9_skysavedvp;
static DWORD			d3d9_skysavedz, d3d9_skysavedzw, d3d9_skysavedcull;
static qboolean			d3d9_insidesky = false;

/*
===============
D3D9_CreateSkyBuffer
===============
*/
qboolean D3D9_CreateSkyBuffer (const void *data, int numverts)
{
	void	*dst;
	UINT	bytes;

	D3D9_DestroySkyBuffer ();

	if (!d3d9_device || !data || numverts <= 0)
		return false;

	bytes = (UINT)(numverts * D3D9_SKY_STRIDE);

	if (FAILED(IDirect3DDevice9_CreateVertexBuffer (d3d9_device, bytes, 0, D3D9_SKY_FVF,
							D3DPOOL_MANAGED, &d3d9_skyvb, NULL)))
	{
		Con_Warning ("D3D9: sky vertex buffer failed\n");
		d3d9_skyvb = NULL;
		return false;
	}

	if (FAILED(IDirect3DVertexBuffer9_Lock (d3d9_skyvb, 0, bytes, &dst, 0)))
	{
		IDirect3DVertexBuffer9_Release (d3d9_skyvb);
		d3d9_skyvb = NULL;
		return false;
	}

	memcpy (dst, data, bytes);
	IDirect3DVertexBuffer9_Unlock (d3d9_skyvb);

	d3d9_skyverts = numverts;
	return true;
}

/*
===============
D3D9_DestroySkyBuffer
===============
*/
void D3D9_DestroySkyBuffer (void)
{
	if (d3d9_skyvb)
	{
		IDirect3DVertexBuffer9_Release (d3d9_skyvb);
		d3d9_skyvb = NULL;
	}
	d3d9_skyverts = 0;
}

/*
===============
D3D9_BeginSky

origin is where the camera is; scale is the half-extent the unit cube in the
buffer is blown up to. Both go in the world matrix, never in the vertices.
===============
*/
void D3D9_BeginSky (const float *origin, float scale)
{
	D3DMATRIX	m;
	D3DVIEWPORT9	vp;

	if (!d3d9_device || !d3d9_skyvb || d3d9_insidesky)
		return;

	/*
		The whole point of this function.

		Remix's rtx.skyMinZThreshold rule reads the viewport minimum depth off
		each draw call and treats anything at or above the threshold (1.0 out
		of the box) as sky. Collapsing the range to [1,1] both satisfies that
		and puts the raster output exactly on the far plane, which is where a
		sky belongs anyway.
	*/
	IDirect3DDevice9_GetViewport (d3d9_device, &d3d9_skysavedvp);
	vp = d3d9_skysavedvp;
	vp.MinZ = 1.0f;
	vp.MaxZ = 1.0f;
	IDirect3DDevice9_SetViewport (d3d9_device, &vp);

	memset (&m, 0, sizeof(m));
	m._11 = m._22 = m._33 = scale;
	m._41 = origin[0];
	m._42 = origin[1];
	m._43 = origin[2];
	m._44 = 1.0f;
	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLD, &m);

	IDirect3DDevice9_SetStreamSource (d3d9_device, 0, d3d9_skyvb, 0, (UINT)D3D9_SKY_STRIDE);
	IDirect3DDevice9_SetFVF (d3d9_device, D3D9_SKY_FVF);

	//put back exactly what R_SetupGL left, rather than assuming its defaults
	IDirect3DDevice9_GetRenderState (d3d9_device, D3DRS_ZENABLE, &d3d9_skysavedz);
	IDirect3DDevice9_GetRenderState (d3d9_device, D3DRS_ZWRITEENABLE, &d3d9_skysavedzw);
	IDirect3DDevice9_GetRenderState (d3d9_device, D3DRS_CULLMODE, &d3d9_skysavedcull);

	//depth off both ways: nothing has been drawn yet this frame, and the sky
	//must not occlude anything that follows
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZENABLE, D3DZB_FALSE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZWRITEENABLE, FALSE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHATESTENABLE, FALSE);
	//we are inside the cube, so half its faces are backfacing by any rule
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_CULLMODE, D3DCULL_NONE);

	//D3DRS_LIGHTING is deliberately left as it is: the light state Remix reads
	//has to stay live, and stage 0 never looks at D3DTA_DIFFUSE anyway.
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_TEXCOORDINDEX, 0);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTexture (d3d9_device, 1, NULL);

	d3d9_insidesky = true;
}

/*
===============
D3D9_DrawSkyFace

face indexes groups of four verts in the sky buffer. The sampler state is
reasserted per face rather than left to D3D9_BindTexture, which would apply
the texture's own wrap mode -- and wrapping is exactly what produces a bright
seam down every cube edge, since the outer half texel of one face would blend
in the opposite edge of the same face.
===============
*/
void D3D9_DrawSkyFace (int face)
{
	if (!d3d9_device || !d3d9_skyvb || !d3d9_insidesky)
		return;

	if (face < 0 || (face + 1) * 4 > d3d9_skyverts)
		return;

	IDirect3DDevice9_SetSamplerState (d3d9_device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	IDirect3DDevice9_SetSamplerState (d3d9_device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	//the addressing above is the reason this block exists, but the filters have
	//to be restated alongside it or the sky would ignore gl_texturemode /
	//d3d9_texturefilter and stay smooth with filtering turned off. Take them
	//from the bound texture, which is where TexMgr_SetFilterModes put them.
	{
		const d3d9texture_t	*t = d3d9_boundtexture[0];
		DWORD			minf = t ? t->minfilter : D3DTEXF_LINEAR;
		DWORD			magf = t ? t->magfilter : D3DTEXF_LINEAR;

		//no mip chain is walked here, so anisotropy has nothing to sample along
		if (minf == D3DTEXF_ANISOTROPIC)
			minf = D3DTEXF_LINEAR;

		IDirect3DDevice9_SetSamplerState (d3d9_device, 0, D3DSAMP_MINFILTER, minf);
		IDirect3DDevice9_SetSamplerState (d3d9_device, 0, D3DSAMP_MAGFILTER, magf);
		IDirect3DDevice9_SetSamplerState (d3d9_device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	}

	IDirect3DDevice9_DrawPrimitive (d3d9_device, D3DPT_TRIANGLEFAN, (UINT)(face * 4), 2);
}

/*
===============
D3D9_EndSky
===============
*/
void D3D9_EndSky (void)
{
	D3DMATRIX m;

	if (!d3d9_device || !d3d9_insidesky)
		return;

	d3d9_insidesky = false;

	IDirect3DDevice9_SetViewport (d3d9_device, &d3d9_skysavedvp);

	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZENABLE, d3d9_skysavedz);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZWRITEENABLE, d3d9_skysavedzw);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_CULLMODE, d3d9_skysavedcull);

	memset (&m, 0, sizeof(m));
	m._11 = m._22 = m._33 = m._44 = 1.0f;
	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLD, &m);

	//the next bind has to reapply the texture's own sampler state over ours
	{
		int i;
		for (i = 0; i < D3D9_MAX_STAGES; i++)
			d3d9_boundtexture[i] = NULL;
	}
}

/*
=============================================================================

LIGHTS

RTX Remix reads the fixed-function light state and converts each enabled
D3D9 light into a path-traced light with its own hash, which is what makes
them selectable and replaceable in the Toolkit. So Quake's lights have to be
submitted as real D3DLIGHT9s even though nothing in our own rasterisation
uses them -- we still draw flat albedo.

D3DRS_LIGHTING has to be on for the lights to be part of the captured state.
That would normally tint our vertices, but every texture stage here selects
the texture directly and never touches D3DTA_DIFFUSE, so the rasterised
output is unaffected.

=============================================================================
*/

/*
===============
D3D9_SubmitLights

Takes positions, radii and colours and installs them as D3D9 point lights.
Index order is kept stable by the caller so a given map light keeps the same
slot frame to frame -- Remix hashes on the light, and a light that hops
between slots is harder to pin down in the Toolkit.
===============
*/
void D3D9_SubmitLights (const float *origins, const float *radii, const float *colours, int count)
{
	D3DLIGHT9	l;
	int		i, maxlights;

	if (!d3d9_device)
		return;

	maxlights = D3D9_MaxActiveLights ();
	if (maxlights <= 0)
		maxlights = 8;
	if (count > maxlights)
		count = maxlights;

	for (i = 0; i < count; i++)
	{
		memset (&l, 0, sizeof(l));

		l.Type = D3DLIGHT_POINT;

		l.Diffuse.r = colours[i*3+0];
		l.Diffuse.g = colours[i*3+1];
		l.Diffuse.b = colours[i*3+2];
		l.Diffuse.a = 1.0f;
		l.Specular = l.Diffuse;

		l.Position.x = origins[i*3+0];
		l.Position.y = origins[i*3+1];
		l.Position.z = origins[i*3+2];

		//Quake's "light" key is a radius in map units, not an intensity
		l.Range = radii[i];

		//inverse-square falloff; Remix mostly cares about position/colour/range
		l.Attenuation0 = 0.0f;
		l.Attenuation1 = 0.0f;
		l.Attenuation2 = 1.0f;

		IDirect3DDevice9_SetLight (d3d9_device, (DWORD)i, &l);
		IDirect3DDevice9_LightEnable (d3d9_device, (DWORD)i, TRUE);
	}

	//turn off any slot we used last frame but not this one
	for (i = count; i < d3d9_activelights; i++)
		IDirect3DDevice9_LightEnable (d3d9_device, (DWORD)i, FALSE);

	d3d9_activelights = count;

	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_LIGHTING, count ? TRUE : FALSE);
}

/*
===============
D3D9_MaxActiveLights
===============
*/
int D3D9_MaxActiveLights (void)
{
	return d3d9_lightcapacity > 0 ? d3d9_lightcapacity : d3d9_maxactivelights;
}

/*
===============
D3D9_CapsActiveLights

What D3DCAPS9 claims, which turns out to be the number that actually matters.
D3D9_ProbeLightCapacity below measures how many LightEnable calls succeed, and
that figure is much larger and completely misleading -- the calls succeed and
the lights past the caps limit are simply not there.
===============
*/
int D3D9_CapsActiveLights (void)
{
	return d3d9_maxactivelights;
}

/*
===============
D3D9_ProbeLightCapacity

D3DCAPS9.MaxActiveLights is what the fixed-function vertex pipeline can light
a vertex with, and it is 8 almost everywhere. That number has nothing to do
with how many lights the runtime will hold, and under Remix it is the held
count that matters -- our own rasterisation never uses the lights at all, they
exist purely for Remix to convert into path traced lights.

So ask the device directly: enable lights until it refuses. Done once at
startup, before any scene, and everything is disabled again afterwards.
===============
*/
static void D3D9_ProbeLightCapacity (void)
{
	D3DLIGHT9	l;
	int		i;

	if (!d3d9_device)
		return;

	memset (&l, 0, sizeof(l));
	l.Type = D3DLIGHT_POINT;
	l.Diffuse.r = l.Diffuse.g = l.Diffuse.b = l.Diffuse.a = 1.0f;
	l.Range = 1.0f;
	l.Attenuation0 = 1.0f;

	for (i = 0; i < D3D9_LIGHT_PROBE_MAX; i++)
	{
		if (FAILED(IDirect3DDevice9_SetLight (d3d9_device, (DWORD)i, &l)))
			break;
		if (FAILED(IDirect3DDevice9_LightEnable (d3d9_device, (DWORD)i, TRUE)))
			break;
	}

	d3d9_lightcapacity = i;

	while (i-- > 0)
		IDirect3DDevice9_LightEnable (d3d9_device, (DWORD)i, FALSE);

	Con_Printf ("D3D9: %d simultaneous lights usable (caps reported %d)\n",
			d3d9_lightcapacity, d3d9_maxactivelights);
}

/*
=============================================================================

ALIAS MODELS

=============================================================================
*/

/*
	Two alias vertex layouts: xyz + st, and xyz + normal + st.

	Without normals Remix has to derive them, and what it derives is per
	triangle -- which is why a .mdl lit by a real light looks faceted rather
	than shaded. Quake has had a normal per vertex since 1996 (the
	lightnormalindex into r_avertexnormals) and md5 carries real ones, so the
	data is there; it was only ever the vertex format dropping it.

	d3d9_modelnormals picks. Note the layout is part of what Remix hashes, so
	switching it changes every alias geometry hash.
*/
#define D3D9_ALIAS_FVF		(D3DFVF_XYZ | D3DFVF_TEX1)
#define D3D9_ALIAS_STRIDE	(5 * sizeof(float))
#define D3D9_ALIASN_FVF		(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
#define D3D9_ALIASN_STRIDE	(8 * sizeof(float))

//xyz + 3 blend weights + 4 packed bone indices + st. XYZB4 with the last beta
//declared as UBYTE4 is D3D9's way of spelling "three weights and an index
//dword"; the fourth weight is 1 minus the other three.
#define D3D9_SKEL_FVF		(D3DFVF_XYZB4 | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_TEX1)
#define D3D9_SKEL_STRIDE	(9 * sizeof(float))

typedef struct d3d9skel_s
{
	LPDIRECT3DVERTEXBUFFER9	vb;
	LPDIRECT3DINDEXBUFFER9	ib;
	int			numverts;
	int			numindices;
	int			numbones;
} d3d9skel_t;

/*
	Poses are packed into buffers of at most this many vertices.

	A pose is selected with BaseVertexIndex, and while D3D9 is perfectly happy
	for that to run past 65535 against a 16 bit index buffer -- rasterisation
	is correct either way -- Remix's geometry extraction is not: past that
	point models blink out and reappear in the wrong place. A .mdl never gets
	near the limit (a few hundred vertices a frame), but a rerelease zombie is
	831 vertices across 198 poses and crosses it a third of the way through
	its animation.

	So the poses are split across however many buffers it takes to keep every
	BaseVertexIndex inside 16 bits. Each pose still occupies one fixed range
	of one immutable buffer, so hashes are as stable as before.
*/
#define D3D9_ALIAS_MAXVERTS	65536

typedef struct d3d9alias_s
{
	LPDIRECT3DVERTEXBUFFER9	*vb;		//numchunks of them
	LPDIRECT3DINDEXBUFFER9	ib;		//shared: every pose has the same topology
	int			numchunks;
	int			posesperchunk;
	int			numverts;	//per pose
	int			numposes;
	int			numindices;
	DWORD			fvf;		//layout this one was built with
	UINT			stride;
} d3d9alias_t;

static void D3D9_SetAliasFixedFunction (void);

/*
===============
D3D9_CreateAliasBuffer

floatverts holds numposes * numverts vertices, each float x,y,z,s,t.
===============
*/
qboolean D3D9_CreateAliasBuffer (void **handle, const void *floatverts, int numverts, int numposes,
					const unsigned short *indices, int numindices, qboolean withnormals)
{
	d3d9alias_t	*a;
	void		*dst;
	UINT		vbytes, ibytes;
	int		chunk;

	if (handle)
		D3D9_DestroyAliasBuffer (handle);

	if (!d3d9_device || !handle || !floatverts || numverts <= 0 || numposes <= 0 || numindices <= 0)
		return false;

	a = (d3d9alias_t *) calloc (1, sizeof(*a));
	if (!a)
		return false;

	a->numverts = numverts;
	a->numposes = numposes;
	a->numindices = numindices;
	a->fvf    = withnormals ? D3D9_ALIASN_FVF : D3D9_ALIAS_FVF;
	a->stride = withnormals ? D3D9_ALIASN_STRIDE : D3D9_ALIAS_STRIDE;

	//as many whole poses as fit under the 16 bit vertex ceiling
	a->posesperchunk = D3D9_ALIAS_MAXVERTS / numverts;
	if (a->posesperchunk < 1)
		a->posesperchunk = 1;	//a single pose bigger than the ceiling: nothing to split
	if (a->posesperchunk > numposes)
		a->posesperchunk = numposes;
	a->numchunks = (numposes + a->posesperchunk - 1) / a->posesperchunk;

	a->vb = (LPDIRECT3DVERTEXBUFFER9 *) calloc (a->numchunks, sizeof(*a->vb));
	if (!a->vb)
	{
		free (a);
		return false;
	}

	ibytes = (UINT)(numindices * sizeof(unsigned short));
	if (FAILED(IDirect3DDevice9_CreateIndexBuffer (d3d9_device, ibytes, 0, D3DFMT_INDEX16,
							D3DPOOL_MANAGED, &a->ib, NULL)))
	{
		free (a->vb);
		free (a);
		return false;
	}

	if (SUCCEEDED(IDirect3DIndexBuffer9_Lock (a->ib, 0, ibytes, &dst, 0)))
	{
		memcpy (dst, indices, ibytes);
		IDirect3DIndexBuffer9_Unlock (a->ib);
	}

	for (chunk = 0; chunk < a->numchunks; chunk++)
	{
		int firstpose = chunk * a->posesperchunk;
		int poses = numposes - firstpose;
		if (poses > a->posesperchunk)
			poses = a->posesperchunk;

		vbytes = (UINT)((size_t)numverts * poses * a->stride);

		if (FAILED(IDirect3DDevice9_CreateVertexBuffer (d3d9_device, vbytes, 0, a->fvf,
								D3DPOOL_MANAGED, &a->vb[chunk], NULL)))
		{
			D3D9_DestroyAliasBuffer ((void **)&a);
			return false;
		}

		if (SUCCEEDED(IDirect3DVertexBuffer9_Lock (a->vb[chunk], 0, vbytes, &dst, 0)))
		{
			memcpy (dst, (const byte *)floatverts + (size_t)firstpose * numverts * a->stride, vbytes);
			IDirect3DVertexBuffer9_Unlock (a->vb[chunk]);
		}
	}

	if (a->numchunks > 1)
		Con_DPrintf ("D3D9: %d poses of %d verts split across %d buffers\n",
				numposes, numverts, a->numchunks);

	*handle = a;
	return true;
}

/*
===============
D3D9_DestroyAliasBuffer
===============
*/
void D3D9_DestroyAliasBuffer (void **handle)
{
	d3d9alias_t *a;

	if (!handle || !*handle)
		return;

	a = (d3d9alias_t *)*handle;

	if (a->vb)
	{
		int chunk;
		for (chunk = 0; chunk < a->numchunks; chunk++)
			if (a->vb[chunk])
				IDirect3DVertexBuffer9_Release (a->vb[chunk]);
		free (a->vb);
	}
	if (a->ib) IDirect3DIndexBuffer9_Release (a->ib);

	free (a);
	*handle = NULL;
}

/*
===============
D3D9_DrawAlias

Selects a pose with BaseVertexIndex rather than a byte offset on the stream
source. Both are legal D3D9, but the stream-offset form is unusual and RTX
Remix's geometry extraction did not pick these draws up at all -- models were
visible in the raster output and absent from the path traced scene. Offsetting
by vertex index is what games normally do, and what Remix expects.

Which buffer holds the pose is worked out here too, since BaseVertexIndex has
to stay inside 16 bits for Remix to read the draw correctly -- see
D3D9_ALIAS_MAXVERTS.

The vertex data itself is still never touched after load, so hashes stay stable.
===============
*/
void D3D9_DrawAlias (void *handle, int pose, int numverts, int numindices)
{
	d3d9alias_t	*a = (d3d9alias_t *)handle;
	int		chunk, base;

	(void) numverts;
	(void) numindices;

	if (!d3d9_device || !a || !a->vb || !a->ib)
		return;

	if (pose < 0 || pose >= a->numposes)
		pose = 0;

	chunk = pose / a->posesperchunk;
	base  = (pose % a->posesperchunk) * a->numverts;

	if (chunk >= a->numchunks || !a->vb[chunk])
		return;

	IDirect3DDevice9_SetFVF (d3d9_device, a->fvf);
	IDirect3DDevice9_SetStreamSource (d3d9_device, 0, a->vb[chunk], 0, a->stride);
	IDirect3DDevice9_SetIndices (d3d9_device, a->ib);

	D3D9_SetAliasFixedFunction ();

	//pose selected by vertex index, not stream byte offset -- see above
	IDirect3DDevice9_DrawIndexedPrimitive (d3d9_device, D3DPT_TRIANGLELIST,
						base,			//BaseVertexIndex
						0,			//MinVertexIndex
						(UINT)a->numverts,
						0, (UINT)(a->numindices / 3));
}

/*
===============
D3D9_SetAliasFixedFunction

The state both model paths want: one texture straight through, opaque, depth
tested. Lighting is Remix's job, so nothing here does any.
===============
*/
static void D3D9_SetAliasFixedFunction (void)
{
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTextureStageState (d3d9_device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTexture (d3d9_device, 1, NULL);

	//models are opaque solid geometry; do not inherit blend/depth state left
	//behind by the 2D or particle passes
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZENABLE, D3DZB_TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_ZWRITEENABLE, TRUE);
}

/*
===============
D3D9_DrawAliasLerped
===============
*/
void D3D9_DrawAliasLerped (const void *verts, int numverts,
				const unsigned short *indices, int numindices, qboolean withnormals)
{
	if (!d3d9_device || !verts || !indices || numverts <= 0 || numindices < 3)
		return;

	IDirect3DDevice9_SetFVF (d3d9_device, withnormals ? D3D9_ALIASN_FVF : D3D9_ALIAS_FVF);
	D3D9_SetAliasFixedFunction ();

	IDirect3DDevice9_DrawIndexedPrimitiveUP (d3d9_device, D3DPT_TRIANGLELIST,
						0, (UINT)numverts,
						(UINT)(numindices / 3),
						indices, D3DFMT_INDEX16,
						verts, withnormals ? (UINT)D3D9_ALIASN_STRIDE : (UINT)D3D9_ALIAS_STRIDE);

	//DrawIndexedPrimitiveUP leaves stream 0 and the indices unbound; the next
	//buffered draw sets both, but do not leave a stale binding behind
	IDirect3DDevice9_SetStreamSource (d3d9_device, 0, NULL, 0, 0);
	IDirect3DDevice9_SetIndices (d3d9_device, NULL);
}

/*
===============
D3D9_SetAliasTransform

Quake's convention: translate to origin, then yaw, pitch, roll. Pitch is
negated because Quake measures it downwards.
===============
*/
//the last entity transform handed to the device. Skeletal draws need it as a
//value rather than as device state, because vertex blending replaces
//D3DTS_WORLD with the bone palette and every bone has to carry it instead.
static D3DMATRIX	d3d9_entitymatrix =
{{{
	1,0,0,0,
	0,1,0,0,
	0,0,1,0,
	0,0,0,1
}}};

static void D3D9_BuildEntityMatrix (D3DMATRIX *out, const float *origin, const float *angles)
{
	float	sy, cy, sp, cp, sr, cr;
	float	yaw, pitch, roll;

	yaw   = angles[1] * (float)(M_PI / 180.0);
	pitch = -angles[0] * (float)(M_PI / 180.0);
	roll  = angles[2] * (float)(M_PI / 180.0);

	sy = sinf(yaw);   cy = cosf(yaw);
	sp = sinf(pitch); cp = cosf(pitch);
	sr = sinf(roll);  cr = cosf(roll);

	//R = Rz(yaw) * Ry(pitch) * Rx(roll), written transposed for D3D row vectors
	out->_11 = cy * cp;
	out->_12 = sy * cp;
	out->_13 = -sp;
	out->_14 = 0.0f;

	out->_21 = cy * sp * sr - sy * cr;
	out->_22 = sy * sp * sr + cy * cr;
	out->_23 = cp * sr;
	out->_24 = 0.0f;

	out->_31 = cy * sp * cr + sy * sr;
	out->_32 = sy * sp * cr - cy * sr;
	out->_33 = cp * cr;
	out->_34 = 0.0f;

	out->_41 = origin[0];
	out->_42 = origin[1];
	out->_43 = origin[2];
	out->_44 = 1.0f;
}

static void D3D9_MultMatrix (D3DMATRIX *out, const D3DMATRIX *a, const D3DMATRIX *b)
{
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			out->m[i][j] = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j]
				     + a->m[i][2]*b->m[2][j] + a->m[i][3]*b->m[3][j];
}

/*
===============
D3D9_SetViewmodelTransform

Places the viewmodel in real world space instead of the trick GL uses.

GL draws the weapon by throwing away the camera from the modelview, leaving
only Quake's axis swizzle, so the gun sits in view space. Remix reconstructs
world geometry from the world and view matrices, so a draw whose view matrix
has no camera in it lands at a meaningless world position -- the gun ends up
somewhere else in the level entirely.

Working it through with D3D's row vectors: we want v*W*V == v*E*S, and the
view matrix is V = T(-org) * R(angles)^-1 * S, so V^-1 = S^-1 * R * T and

    W = E * S * V^-1 = E * R(viewangles) * T(vieworg)

which is just the gun's local offset carried out to where the camera is.
===============
*/
void D3D9_SetViewmodelTransform (const float *entorigin, const float *entangles,
					const float *vieworg, const float *viewangles)
{
	D3DMATRIX	ent, cam, m;
	float		camangles[3];

	if (!d3d9_device)
		return;

	/*
		Note the negated pitch on the camera.

		D3D9_BuildEntityMatrix follows Quake's ENTITY convention, which flips
		pitch -- the same quirk r_brush.c compensates for with its "stupid
		quake bug" line. r_refdef.viewangles is in the CAMERA convention used
		by AngleVectors, where forward is (cos*cos, cos*sin, -sin(pitch)).
		Feeding view angles straight into the entity builder therefore tilts
		the gun the wrong way: it pitches up as you look down.
	*/
	camangles[0] = -viewangles[0];
	camangles[1] = viewangles[1];
	camangles[2] = viewangles[2];

	D3D9_BuildEntityMatrix (&ent, entorigin, entangles);
	D3D9_BuildEntityMatrix (&cam, vieworg, camangles);
	D3D9_MultMatrix (&m, &ent, &cam);

	d3d9_entitymatrix = m;
	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLD, &m);
}

void D3D9_SetAliasTransform (const float *origin, const float *angles)
{
	D3DMATRIX	m;

	if (!d3d9_device)
		return;

	D3D9_BuildEntityMatrix (&m, origin, angles);

	d3d9_entitymatrix = m;
	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLD, &m);
}

/*
===============
D3D9_ClearAliasTransform
===============
*/
void D3D9_ClearAliasTransform (void)
{
	D3DMATRIX m;

	if (!d3d9_device)
		return;

	memset (&m, 0, sizeof(m));
	m._11 = m._22 = m._33 = m._44 = 1.0f;

	d3d9_entitymatrix = m;
	IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLD, &m);
}

/*
===============
D3D9_MaxBones
===============
*/
int D3D9_MaxBones (void)
{
	extern cvar_t d3d9_gpuskinning;

	if (!d3d9_gpuskinning.value)
		return 0;	//everything gets skinned at load instead

	return d3d9_maxbones;
}

/*
===============
D3D9_CreateSkeletalBuffer

verts is numverts records of position(3f), weight(3f), packed bone indices
(1 dword), texcoord(2f) -- the bind pose, written once and never touched
again.
===============
*/
qboolean D3D9_CreateSkeletalBuffer (void **handle, const void *verts, int numverts,
					const unsigned short *indices, int numindices, int numbones)
{
	d3d9skel_t	*s;
	void		*dst;
	UINT		vbytes, ibytes;

	if (handle)
		D3D9_DestroySkeletalBuffer (handle);

	if (!d3d9_device || !handle || !verts || numverts <= 0 || numindices <= 0 || numbones <= 0)
		return false;

	if (numbones > d3d9_maxbones)
	{
		//nothing sane to fall back to: without the palette the bind pose is
		//all we could draw, and a frozen monster is worse than a loud failure
		Con_Warning ("D3D9: skeletal model needs %d bones, device palette holds %d\n",
				numbones, d3d9_maxbones);
		return false;
	}

	s = (d3d9skel_t *) calloc (1, sizeof(*s));
	if (!s)
		return false;

	s->numverts = numverts;
	s->numindices = numindices;
	s->numbones = numbones;

	vbytes = (UINT)((size_t)numverts * D3D9_SKEL_STRIDE);
	ibytes = (UINT)(numindices * sizeof(unsigned short));

	if (FAILED(IDirect3DDevice9_CreateVertexBuffer (d3d9_device, vbytes, 0, D3D9_SKEL_FVF,
							D3DPOOL_MANAGED, &s->vb, NULL)) ||
	    FAILED(IDirect3DDevice9_CreateIndexBuffer (d3d9_device, ibytes, 0, D3DFMT_INDEX16,
							D3DPOOL_MANAGED, &s->ib, NULL)))
	{
		if (s->vb) IDirect3DVertexBuffer9_Release (s->vb);
		free (s);
		return false;
	}

	if (SUCCEEDED(IDirect3DVertexBuffer9_Lock (s->vb, 0, vbytes, &dst, 0)))
	{
		memcpy (dst, verts, vbytes);
		IDirect3DVertexBuffer9_Unlock (s->vb);
	}

	if (SUCCEEDED(IDirect3DIndexBuffer9_Lock (s->ib, 0, ibytes, &dst, 0)))
	{
		memcpy (dst, indices, ibytes);
		IDirect3DIndexBuffer9_Unlock (s->ib);
	}

	*handle = s;
	return true;
}

/*
===============
D3D9_DestroySkeletalBuffer
===============
*/
void D3D9_DestroySkeletalBuffer (void **handle)
{
	d3d9skel_t *s;

	if (!handle || !*handle)
		return;

	s = (d3d9skel_t *)*handle;

	if (s->vb) IDirect3DVertexBuffer9_Release (s->vb);
	if (s->ib) IDirect3DIndexBuffer9_Release (s->ib);

	free (s);
	*handle = NULL;
}

/*
===============
D3D9_BoneToWorldMatrix

Quake's bonepose_t is a 3x4 laid out for column vectors (out = M * v), which is
the transpose of what D3D9 wants for its row vectors, with the translation
moving from the last column to the last row. The entity transform is then
concatenated on the right so the bone lands in world space.
===============
*/
static void D3D9_BoneToWorldMatrix (D3DMATRIX *out, const float *bone)
{
	D3DMATRIX	b;

	b._11 = bone[0];  b._21 = bone[1];  b._31 = bone[2];  b._41 = bone[3];
	b._12 = bone[4];  b._22 = bone[5];  b._32 = bone[6];  b._42 = bone[7];
	b._13 = bone[8];  b._23 = bone[9];  b._33 = bone[10]; b._43 = bone[11];
	b._14 = 0.0f;     b._24 = 0.0f;     b._34 = 0.0f;     b._44 = 1.0f;

	D3D9_MultMatrix (out, &b, &d3d9_entitymatrix);
}

/*
===============
D3D9_DrawSkeletal

bonematrices is numbones bonepose_t -- 12 floats each, already lerped between
keyframes and multiplied by the inverse bind pose by the engine.

The lerping is free here in a way it is not for a .mdl: it happens to the
matrices, never to the vertices, so the buffer Remix hashes is the same one it
saw last frame. r_lerpmodels can stay on for these models.
===============
*/
void D3D9_DrawSkeletal (void *handle, const float *bonematrices, int numbones)
{
	d3d9skel_t	*s = (d3d9skel_t *)handle;
	D3DMATRIX	m;
	int		i;

	if (!d3d9_device || !s || !s->vb || !s->ib || !bonematrices)
		return;

	if (numbones > s->numbones)
		numbones = s->numbones;
	if (numbones <= 0)
		return;

	for (i = 0; i < numbones; i++)
	{
		D3D9_BoneToWorldMatrix (&m, bonematrices + i*12);
		IDirect3DDevice9_SetTransform (d3d9_device, D3DTS_WORLDMATRIX(i), &m);
	}

	IDirect3DDevice9_SetFVF (d3d9_device, D3D9_SKEL_FVF);
	IDirect3DDevice9_SetStreamSource (d3d9_device, 0, s->vb, 0, (UINT)D3D9_SKEL_STRIDE);
	IDirect3DDevice9_SetIndices (d3d9_device, s->ib);

	//four matrices per vertex: three explicit weights and one implied
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS);

	//same flat albedo the rigid path uses -- lighting comes from Remix
	D3D9_SetAliasFixedFunction ();

	IDirect3DDevice9_DrawIndexedPrimitive (d3d9_device, D3DPT_TRIANGLELIST,
						0,			//BaseVertexIndex
						0,			//MinVertexIndex
						(UINT)s->numverts,
						0, (UINT)(s->numindices / 3));

	//blending is device state, not a property of the draw: leaving it on
	//would skin the world, the sky and every .mdl behind it
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
	IDirect3DDevice9_SetRenderState (d3d9_device, D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
}


#else	/* !_WIN32 */

#include "d3d9_backend.h"

qboolean D3D9_Requested (void) { return false; }
qboolean D3D9_Active (void) { return false; }
qboolean D3D9_Init (void *hwnd, int width, int height, qboolean fullscreen) { (void)hwnd; (void)width; (void)height; (void)fullscreen; return false; }
void D3D9_Shutdown (void) {}
void D3D9_Resize (int width, int height, qboolean fullscreen) { (void)width; (void)height; (void)fullscreen; }
qboolean D3D9_BeginFrame (void) { return false; }
void D3D9_EndFrame (void) {}
void *D3D9_CreateTexture (int width, int height, int levels, qboolean alpha) { (void)width; (void)height; (void)levels; (void)alpha; return NULL; }
void D3D9_UploadLevel (void *tex, int level, int width, int height, const void *rgba) { (void)tex; (void)level; (void)width; (void)height; (void)rgba; }
void D3D9_SetTextureFilter (void *tex, int minfilter, int magfilter, int mipfilter, qboolean clamp, int anisotropy) { (void)tex; (void)minfilter; (void)magfilter; (void)mipfilter; (void)clamp; (void)anisotropy; }
void D3D9_BindTexture (int stage, void *tex) { (void)stage; (void)tex; }
void D3D9_DestroyTexture (void *tex) { (void)tex; }
int D3D9_TextureCount (void) { return 0; }
int D3D9_MaxTextureSize (void) { return 0; }
int D3D9_MaxAnisotropy (void) { return 1; }
qboolean D3D9_CreateWorldBuffer (const void *data, int numverts) { (void)data; (void)numverts; return false; }
void D3D9_DestroyWorldBuffer (void) {}
void D3D9_BeginWorld (qboolean lightmaps, qboolean overbright) { (void)lightmaps; (void)overbright; }
void D3D9_EndWorld (void) {}
void D3D9_DrawWorldSurface (int firstvert, int numverts) { (void)firstvert; (void)numverts; }
qboolean D3D9_CreateWorldIndexBuffer (const unsigned int *i, int n) { (void)i; (void)n; return false; }
void D3D9_DestroyWorldIndexBuffer (void) {}
qboolean D3D9_WorldBatchesReady (void) { return false; }
void D3D9_DrawWorldBatch (int fi, int ni, int mv, int nv) { (void)fi; (void)ni; (void)mv; (void)nv; }
void D3D9_SetProjectionFromGL (const float *m) { (void)m; }
void D3D9_SetViewFromGL (const float *m) { (void)m; }
void D3D9_SetViewport (int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void D3D9_Set3DState (qboolean cull) { (void)cull; }
qboolean D3D9_CreateAliasBuffer (void **h, const void *v, int nv, int np, const unsigned short *i, int ni, qboolean n) { (void)h; (void)v; (void)nv; (void)np; (void)i; (void)ni; (void)n; return false; }
void D3D9_DestroyAliasBuffer (void **h) { (void)h; }
void D3D9_DrawAlias (void *h, int pose, int nv, int ni) { (void)h; (void)pose; (void)nv; (void)ni; }
qboolean D3D9_CreateSkeletalBuffer (void **h, const void *v, int nv, const unsigned short *i, int ni, int nb) { (void)h; (void)v; (void)nv; (void)i; (void)ni; (void)nb; return false; }
void D3D9_DestroySkeletalBuffer (void **h) { (void)h; }
void D3D9_DrawSkeletal (void *h, const float *b, int nb) { (void)h; (void)b; (void)nb; }
void D3D9_DrawAliasLerped (const void *v, int nv, const unsigned short *i, int ni, qboolean n) { (void)v; (void)nv; (void)i; (void)ni; (void)n; }
int D3D9_MaxBones (void) { return 0; }
void D3D9_SetAliasTransform (const float *o, const float *a) { (void)o; (void)a; }
void D3D9_SetViewmodelTransform (const float *eo, const float *ea, const float *vo, const float *va) { (void)eo; (void)ea; (void)vo; (void)va; }
void D3D9_ClearAliasTransform (void) {}
void D3D9_SubmitLights (const float *o, const float *r, const float *c, int n) { (void)o; (void)r; (void)c; (void)n; }
int D3D9_MaxActiveLights (void) { return 0; }
int D3D9_CapsActiveLights (void) { return 0; }
qboolean D3D9_CreateSkyBuffer (const void *data, int numverts) { (void)data; (void)numverts; return false; }
void D3D9_DestroySkyBuffer (void) {}
void D3D9_BeginSky (const float *origin, float scale) { (void)origin; (void)scale; }
void D3D9_DrawSkyFace (int face) { (void)face; }
void D3D9_EndSky (void) {}

#endif	/* _WIN32 */
