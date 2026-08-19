/*
	d3d9_imm.c -- immediate-mode shim implementation

	See d3d9_imm.h. This file defines D3D9_IMM_IMPLEMENTATION before including
	that header so it gets the prototypes without the gl* macros, leaving the
	real GL entry points callable for the non-D3D9 path.
*/

#include "quakedef.h"

#define D3D9_IMM_IMPLEMENTATION
#include "d3d9_imm.h"
#include "d3d9_backend.h"

#ifdef _WIN32

#include <windows.h>
#include <d3d9.h>

//provided by d3d9_backend.c
extern LPDIRECT3DDEVICE9 D3D9_GetDevice (void);

/*
	Batch size. Big enough that a full particle list -- 2048 quads, so 8192
	vertices -- goes out without splitting.
*/
#define D3D9_MAX_IMM_VERTS	8192

typedef struct
{
	float	x, y, z;
	D3DCOLOR colour;
	float	s, t;
} d3d9immvert_t;

#define D3D9_IMM_FVF	(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static d3d9immvert_t	imm_verts[D3D9_MAX_IMM_VERTS];

//quads expanded to a triangle list for submission: six vertices per four
static d3d9immvert_t	imm_quadverts[D3D9_MAX_IMM_VERTS / 4 * 6];
static int		imm_numverts = 0;
static GLenum		imm_mode = 0;
static qboolean		imm_active = false;

static float		imm_s = 0.0f, imm_t = 0.0f;
static D3DCOLOR		imm_colour = 0xffffffff;

//last mode passed to glTexEnvf. GL's own default is MODULATE, but
//GL_SetupState immediately sets REPLACE, so match the engine not the spec.
static GLenum		imm_texenv = GL_REPLACE;

//glEnable/glDisable(GL_TEXTURE_2D); decides whether colour and alpha come
//from the texture or straight from the vertex
static qboolean		imm_texturing = true;

//set when QGL_Flush turns the alpha test off for a blended draw
static qboolean		imm_suspendedatest = false;

//D3DRS_LIGHTING as it was before QGL_Flush turned it off for the draw
static DWORD		imm_savedlighting = FALSE;

//current viewport, kept so the projection can carry the half-texel correction
static int		imm_vp_w = 1, imm_vp_h = 1;

/*
	Last glOrtho, kept so the projection can be rebuilt when the viewport moves
	under it.

	GL_SetCanvas sets every canvas as glOrtho followed by glViewport, and the
	half-texel correction in QGL_ApplyOrtho is sized from the viewport, so
	applying the ortho as it arrives sizes that correction from the *previous*
	canvas. Within a steady frame that is a small constant error; across a
	resolution change or a fullscreen toggle the stale size belongs to a
	different resolution entirely and the whole 2D layer lands off-pixel.

	imm_ortho_active says the projection currently belongs to us. The 3D pass
	writes D3DTS_PROJECTION directly through D3D9_SetProjectionFromGL, which
	calls QGL_ProjectionOverridden to clear the flag, so a glViewport issued
	during 3D setup cannot stamp a stale 2D ortho over it.
*/
static double		imm_ortho[6];
static qboolean		imm_ortho_active = false;

/*
===============
QGL_Flush -- emits whatever has been accumulated
===============
*/
static void QGL_Flush (void)
{
	LPDIRECT3DDEVICE9	dev = D3D9_GetDevice ();
	int			i;

	if (!dev || !imm_numverts)
	{
		imm_numverts = 0;
		return;
	}

	IDirect3DDevice9_SetFVF (dev, D3D9_IMM_FVF);

	/*
		Reassert the whole stage 0 pipeline every flush, because the world and
		alias passes overwrite it between our draws.

		Alpha matters as much as colour here, and getting it wrong is subtle:
		glDisable(GL_TEXTURE_2D) points ALPHAARG1 at the vertex, and if that is
		never pointed back at the texture then alpha is always 255, the alpha
		test can never reject anything, and Quake's transparent palette index
		renders as an opaque pink box over the menu.

		Honour the requested texenv rather than forcing MODULATE: the status
		bar draws under GL_REPLACE, and modulating it against a leftover
		particle colour turns the whole HUD black.
	*/
	IDirect3DDevice9_SetTextureStageState (dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	IDirect3DDevice9_SetTextureStageState (dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

	if (!imm_texturing)
	{	//untextured: colour and alpha both come from the vertex
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	}
	else
	{
		DWORD op = (imm_texenv == GL_REPLACE) ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE;

		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_COLOROP, op);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

		//GL modulates alpha alongside colour, and REPLACE takes texture alpha
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_ALPHAOP, op);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		IDirect3DDevice9_SetTextureStageState (dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	}

	/*
		Immediate-mode content is pre-lit and carries its colour per vertex, but
		these vertices have no normals. With D3DRS_LIGHTING enabled -- which the
		light submission for Remix turns on -- D3D9 ignores the vertex diffuse
		and lights from the material instead, so every particle comes out black.
		Turn lighting off for the draw and put it back afterwards, so Remix
		still sees the lit state on the world and model passes.
	*/
	IDirect3DDevice9_GetRenderState (dev, D3DRS_LIGHTING, &imm_savedlighting);
	if (imm_savedlighting)
		IDirect3DDevice9_SetRenderState (dev, D3DRS_LIGHTING, FALSE);

	/*
		Alpha test and alpha blending together make no sense for soft-edged
		content: GL_SetupState leaves the 0.666 alpha test enabled globally,
		which hard-clips a particle's radial falloff into a solid block. When
		the caller has asked for blending it wants the gradient, so suspend the
		test for the duration of the draw.
	*/
	{
		DWORD blending = FALSE;
		IDirect3DDevice9_GetRenderState (dev, D3DRS_ALPHABLENDENABLE, &blending);
		if (blending)
			IDirect3DDevice9_SetRenderState (dev, D3DRS_ALPHATESTENABLE, FALSE);
		imm_suspendedatest = blending ? true : false;
	}

	if (imm_mode == GL_QUADS)
	{
		/*
			D3D9 has no quad primitive. Expand each group of four vertices into
			the two triangles a fan would have produced -- (0,1,2) and (0,2,3),
			same winding -- and submit the batch as one triangle list.

			Doing this per quad instead, which is what a fan per group of four
			amounts to, costs one draw call each. That is survivable for a few
			dozen glyphs and ruinous for particles: a full list is 2048 draw
			calls a frame, and under the Remix bridge every one of them is
			marshalled across to a separate 64 bit host process.
		*/
		int quads = imm_numverts / 4;
		int n = 0;

		for (i = 0; i < quads; i++)
		{
			const d3d9immvert_t *q = &imm_verts[i * 4];

			imm_quadverts[n++] = q[0];
			imm_quadverts[n++] = q[1];
			imm_quadverts[n++] = q[2];

			imm_quadverts[n++] = q[0];
			imm_quadverts[n++] = q[2];
			imm_quadverts[n++] = q[3];
		}

		if (quads)
			IDirect3DDevice9_DrawPrimitiveUP (dev, D3DPT_TRIANGLELIST, quads * 2,
								imm_quadverts, sizeof(d3d9immvert_t));
	}
	else if ((imm_mode == GL_TRIANGLE_FAN || imm_mode == GL_POLYGON) && imm_numverts >= 3)
	{
		//GL_POLYGON is a convex fan as far as Quake ever uses it. DrawGLPoly
		//emits every world/sky surface this way, so leaving it unhandled means
		//those draws silently vanish -- which is what made the sky solid black.
		IDirect3DDevice9_DrawPrimitiveUP (dev, D3DPT_TRIANGLEFAN, imm_numverts - 2, imm_verts, sizeof(d3d9immvert_t));
	}
	else if (imm_mode == GL_TRIANGLE_STRIP && imm_numverts >= 3)
		IDirect3DDevice9_DrawPrimitiveUP (dev, D3DPT_TRIANGLESTRIP, imm_numverts - 2, imm_verts, sizeof(d3d9immvert_t));
	else if (imm_mode == GL_TRIANGLES && imm_numverts >= 3)
		IDirect3DDevice9_DrawPrimitiveUP (dev, D3DPT_TRIANGLELIST, imm_numverts / 3, imm_verts, sizeof(d3d9immvert_t));

	if (imm_suspendedatest)
	{
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ALPHATESTENABLE, TRUE);
		imm_suspendedatest = false;
	}

	if (imm_savedlighting)
		IDirect3DDevice9_SetRenderState (dev, D3DRS_LIGHTING, TRUE);

	imm_numverts = 0;
}

void QGL_Begin (GLenum mode)
{
	if (!D3D9_Active()) { glBegin (mode); return; }

	imm_mode = mode;
	imm_numverts = 0;
	imm_active = true;
}

void QGL_End (void)
{
	if (!D3D9_Active()) { glEnd (); return; }

	QGL_Flush ();
	imm_active = false;
}

void QGL_TexCoord2f (float s, float t)
{
	if (!D3D9_Active()) { glTexCoord2f (s, t); return; }

	imm_s = s;
	imm_t = t;
}

static void QGL_PushVertex (float x, float y, float z)
{
	d3d9immvert_t	*v;
	int		group;

	/*
		Split the batch when it fills, rather than dropping the tail.

		A busy scene puts far more than D3D9_MAX_IMM_VERTS through one
		glBegin/glEnd: 2048 particles as quads is 8192 vertices, so everything
		past the first 256 particles used to vanish, and a blood spray simply
		stopped halfway.

		Only the modes built from independent primitives can be cut, and only
		on a primitive boundary -- a fan or a strip shares vertices between
		triangles, so splitting one would lose the triangles spanning the cut.
		Those keep the old drop behaviour, which nothing reaches in practice:
		DrawGLPoly hands over one surface at a time, far short of the buffer.
	*/
	group = (imm_mode == GL_QUADS) ? 4 : (imm_mode == GL_TRIANGLES) ? 3 : 0;

	if (group && (imm_numverts % group) == 0 && imm_numverts + group > D3D9_MAX_IMM_VERTS)
		QGL_Flush ();	//draws what we have and leaves imm_numverts at 0

	if (imm_numverts >= D3D9_MAX_IMM_VERTS)
		return;		//drop rather than overrun

	v = &imm_verts[imm_numverts++];
	v->x = x;
	v->y = y;
	v->z = z;
	v->colour = imm_colour;
	v->s = imm_s;
	v->t = imm_t;
}

void QGL_Vertex2f (float x, float y)
{
	if (!D3D9_Active()) { glVertex2f (x, y); return; }
	QGL_PushVertex (x, y, 0.0f);
}

void QGL_Vertex3f (float x, float y, float z)
{
	if (!D3D9_Active()) { glVertex3f (x, y, z); return; }
	QGL_PushVertex (x, y, z);
}

void QGL_Vertex3fv (const float *v)
{
	if (!D3D9_Active()) { glVertex3fv (v); return; }
	QGL_PushVertex (v[0], v[1], v[2]);
}

void QGL_Color4f (float r, float g, float b, float a)
{
	if (!D3D9_Active()) { glColor4f (r, g, b, a); return; }

	imm_colour = D3DCOLOR_COLORVALUE (r, g, b, a);
}

void QGL_Color3f (float r, float g, float b)
{
	if (!D3D9_Active()) { glColor3f (r, g, b); return; }

	imm_colour = D3DCOLOR_COLORVALUE (r, g, b, 1.0f);
}

void QGL_Color4fv (const float *v)
{
	if (!D3D9_Active()) { glColor4fv (v); return; }

	imm_colour = D3DCOLOR_COLORVALUE (v[0], v[1], v[2], v[3]);
}

void QGL_Color4ubv (const unsigned char *v)
{
	if (!D3D9_Active()) { glColor4ubv (v); return; }

	imm_colour = D3DCOLOR_ARGB (v[3], v[0], v[1], v[2]);
}

void QGL_Color3ubv (const unsigned char *v)
{
	if (!D3D9_Active()) { glColor3ubv (v); return; }

	imm_colour = D3DCOLOR_ARGB (255, v[0], v[1], v[2]);
}

void QGL_Enable (GLenum cap)
{
	LPDIRECT3DDEVICE9 dev;

	if (!D3D9_Active()) { glEnable (cap); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	switch (cap)
	{
	case GL_BLEND:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ALPHABLENDENABLE, TRUE);
		break;
	case GL_ALPHA_TEST:
		//threshold comes from GL_SetupState's glAlphaFunc(GL_GREATER, 0.666)
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ALPHATESTENABLE, TRUE);
		break;
	case GL_DEPTH_TEST:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ZENABLE, D3DZB_TRUE);
		break;
	case GL_CULL_FACE:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_CULLMODE, D3DCULL_CCW);
		break;
	case GL_TEXTURE_2D:
		imm_texturing = true;	//QGL_Flush rebuilds the stage from this
		break;
	default:
		break;
	}
}

void QGL_Disable (GLenum cap)
{
	LPDIRECT3DDEVICE9 dev;

	if (!D3D9_Active()) { glDisable (cap); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	switch (cap)
	{
	case GL_BLEND:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ALPHABLENDENABLE, FALSE);
		break;
	case GL_ALPHA_TEST:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ALPHATESTENABLE, FALSE);
		break;
	case GL_DEPTH_TEST:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ZENABLE, D3DZB_FALSE);
		break;
	case GL_CULL_FACE:
		IDirect3DDevice9_SetRenderState (dev, D3DRS_CULLMODE, D3DCULL_NONE);
		break;
	case GL_TEXTURE_2D:
		imm_texturing = false;	//QGL_Flush rebuilds the stage from this
		break;
	default:
		break;
	}
}

void QGL_BlendFunc (GLenum sfactor, GLenum dfactor)
{
	LPDIRECT3DDEVICE9 dev;

	if (!D3D9_Active()) { glBlendFunc (sfactor, dfactor); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	IDirect3DDevice9_SetRenderState (dev, D3DRS_SRCBLEND,
		(sfactor == GL_ONE) ? D3DBLEND_ONE :
		(sfactor == GL_ZERO) ? D3DBLEND_ZERO :
		(sfactor == GL_DST_COLOR) ? D3DBLEND_DESTCOLOR : D3DBLEND_SRCALPHA);

	IDirect3DDevice9_SetRenderState (dev, D3DRS_DESTBLEND,
		(dfactor == GL_ONE) ? D3DBLEND_ONE :
		(dfactor == GL_ZERO) ? D3DBLEND_ZERO :
		(dfactor == GL_SRC_COLOR) ? D3DBLEND_SRCCOLOR :
		(dfactor == GL_ONE_MINUS_SRC_ALPHA) ? D3DBLEND_INVSRCALPHA : D3DBLEND_INVSRCALPHA);
}

void QGL_DepthMask (GLboolean flag)
{
	LPDIRECT3DDEVICE9 dev;

	if (!D3D9_Active()) { glDepthMask (flag); return; }

	dev = D3D9_GetDevice ();
	if (dev)
		IDirect3DDevice9_SetRenderState (dev, D3DRS_ZWRITEENABLE, flag ? TRUE : FALSE);
}

void QGL_DepthFunc (GLenum func)
{
	LPDIRECT3DDEVICE9 dev;

	if (!D3D9_Active()) { glDepthFunc (func); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	/*
		Map the whole set, not a few cases with a default. Sky_DrawSky uses
		GL_GEQUAL to lay the cloud layers exactly where the sky surfaces
		already wrote depth; falling through to LESSEQUAL inverts that test
		and the layers are rejected everywhere.
	*/
	IDirect3DDevice9_SetRenderState (dev, D3DRS_ZFUNC,
		(func == GL_NEVER)    ? D3DCMP_NEVER :
		(func == GL_LESS)     ? D3DCMP_LESS :
		(func == GL_EQUAL)    ? D3DCMP_EQUAL :
		(func == GL_LEQUAL)   ? D3DCMP_LESSEQUAL :
		(func == GL_GREATER)  ? D3DCMP_GREATER :
		(func == GL_NOTEQUAL) ? D3DCMP_NOTEQUAL :
		(func == GL_GEQUAL)   ? D3DCMP_GREATEREQUAL :
		(func == GL_ALWAYS)   ? D3DCMP_ALWAYS : D3DCMP_LESSEQUAL);
}

void QGL_ColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	LPDIRECT3DDEVICE9	dev;
	DWORD			mask = 0;

	if (!D3D9_Active()) { glColorMask (r, g, b, a); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	if (r) mask |= D3DCOLORWRITEENABLE_RED;
	if (g) mask |= D3DCOLORWRITEENABLE_GREEN;
	if (b) mask |= D3DCOLORWRITEENABLE_BLUE;
	if (a) mask |= D3DCOLORWRITEENABLE_ALPHA;

	IDirect3DDevice9_SetRenderState (dev, D3DRS_COLORWRITEENABLE, mask);
}

void QGL_Color3fv (const float *v)
{
	if (!D3D9_Active()) { glColor3fv (v); return; }

	imm_colour = D3DCOLOR_COLORVALUE (v[0], v[1], v[2], 1.0f);
}

void QGL_TexEnvf (GLenum target, GLenum pname, GLfloat param)
{
	LPDIRECT3DDEVICE9 dev;

	if (!D3D9_Active()) { glTexEnvf (target, pname, param); return; }

	dev = D3D9_GetDevice ();
	if (!dev || pname != GL_TEXTURE_ENV_MODE)
		return;

	//recorded rather than applied here: QGL_Flush reasserts it, since the
	//world pass overwrites stage 0 between our draws
	imm_texenv = (GLenum)param;
}

void QGL_MatrixMode (GLenum mode)
{
	if (!D3D9_Active()) { glMatrixMode (mode); return; }
	//D3D9 addresses each transform explicitly, so there is no current-matrix
	//mode to track. QGL_Ortho writes PROJECTION, QGL_LoadIdentity the rest.
}

void QGL_LoadIdentity (void)
{
	LPDIRECT3DDEVICE9	dev;
	D3DMATRIX		m;

	if (!D3D9_Active()) { glLoadIdentity (); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	memset (&m, 0, sizeof(m));
	m._11 = m._22 = m._33 = m._44 = 1.0f;

	IDirect3DDevice9_SetTransform (dev, D3DTS_WORLD, &m);
	IDirect3DDevice9_SetTransform (dev, D3DTS_VIEW, &m);
}

/*
===============
QGL_ApplyOrtho -- (re)builds D3DTS_PROJECTION from imm_ortho and the viewport
===============
*/
static void QGL_ApplyOrtho (void)
{
	LPDIRECT3DDEVICE9	dev;
	D3DMATRIX		m;
	double			l = imm_ortho[0], r = imm_ortho[1];
	double			b = imm_ortho[2], t = imm_ortho[3];
	double			n = imm_ortho[4], f = imm_ortho[5];
	float			halfx, halfy;

	dev = D3D9_GetDevice ();
	if (!dev || r == l || t == b || n == f)
		return;

	/*
		Right-handed off-center ortho in D3D's convention (z maps to [0,1]
		rather than GL's [-1,1]; harmless here since 2D runs with depth
		testing off).

		The half-pixel shift is the classic D3D9 2D correction: texel centres
		sit half a pixel off screen pixel centres, and without it HUD text and
		charset glyphs come out visibly soft. It is expressed in clip space,
		hence the division by the viewport size.
	*/
	halfx = (imm_vp_w > 0) ? (1.0f / (float)imm_vp_w) : 0.0f;
	halfy = (imm_vp_h > 0) ? (1.0f / (float)imm_vp_h) : 0.0f;

	memset (&m, 0, sizeof(m));
	m._11 = (float)( 2.0 / (r - l));
	m._22 = (float)( 2.0 / (t - b));
	m._33 = (float)( 1.0 / (n - f));
	m._41 = (float)((l + r) / (l - r)) - halfx;
	m._42 = (float)((t + b) / (b - t)) + halfy;
	m._43 = (float)( n / (n - f));
	m._44 = 1.0f;

	IDirect3DDevice9_SetTransform (dev, D3DTS_PROJECTION, &m);
}

void QGL_Ortho (double l, double r, double b, double t, double n, double f)
{
	if (!D3D9_Active()) { glOrtho (l, r, b, t, n, f); return; }

	imm_ortho[0] = l; imm_ortho[1] = r;
	imm_ortho[2] = b; imm_ortho[3] = t;
	imm_ortho[4] = n; imm_ortho[5] = f;
	imm_ortho_active = true;

	QGL_ApplyOrtho ();
}

/*
===============
QGL_ProjectionOverridden -- the 3D pass has taken D3DTS_PROJECTION off us
===============
*/
void QGL_ProjectionOverridden (void)
{
	imm_ortho_active = false;
}

void QGL_Viewport (int x, int y, int w, int h)
{
	LPDIRECT3DDEVICE9	dev;
	D3DVIEWPORT9		vp;

	if (!D3D9_Active()) { glViewport (x, y, w, h); return; }

	dev = D3D9_GetDevice ();
	if (!dev)
		return;

	if (w < 1) w = 1;
	if (h < 1) h = 1;

	if (imm_vp_w != w || imm_vp_h != h)
	{
		imm_vp_w = w;
		imm_vp_h = h;

		//the half-texel correction is sized from the viewport, and glOrtho
		//always arrives first, so the ortho has to be rebuilt here
		if (imm_ortho_active)
			QGL_ApplyOrtho ();
	}

	//GL measures the viewport from the bottom left, D3D9 from the top left
	vp.X = (DWORD)x;
	vp.Y = (DWORD)q_max(0, (int)vid.height - y - h);
	vp.Width = (DWORD)w;
	vp.Height = (DWORD)h;
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;

	IDirect3DDevice9_SetViewport (dev, &vp);
}

#else	/* !_WIN32 -- pass everything through to GL */

void QGL_Begin (GLenum mode) { glBegin (mode); }
void QGL_End (void) { glEnd (); }
void QGL_Vertex2f (float x, float y) { glVertex2f (x, y); }
void QGL_Vertex3f (float x, float y, float z) { glVertex3f (x, y, z); }
void QGL_Vertex3fv (const float *v) { glVertex3fv (v); }
void QGL_TexCoord2f (float s, float t) { glTexCoord2f (s, t); }
void QGL_Color4f (float r, float g, float b, float a) { glColor4f (r, g, b, a); }
void QGL_Color3f (float r, float g, float b) { glColor3f (r, g, b); }
void QGL_Color4fv (const float *v) { glColor4fv (v); }
void QGL_Color4ubv (const unsigned char *v) { glColor4ubv (v); }
void QGL_Color3ubv (const unsigned char *v) { glColor3ubv (v); }
void QGL_Enable (GLenum cap) { glEnable (cap); }
void QGL_Disable (GLenum cap) { glDisable (cap); }
void QGL_BlendFunc (GLenum s, GLenum d) { glBlendFunc (s, d); }
void QGL_DepthMask (GLboolean f) { glDepthMask (f); }
void QGL_DepthFunc (GLenum f) { glDepthFunc (f); }
void QGL_ColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a) { glColorMask (r, g, b, a); }
void QGL_Color3fv (const float *v) { glColor3fv (v); }
void QGL_TexEnvf (GLenum t, GLenum p, GLfloat v) { glTexEnvf (t, p, v); }
void QGL_MatrixMode (GLenum m) { glMatrixMode (m); }
void QGL_LoadIdentity (void) { glLoadIdentity (); }
void QGL_Ortho (double l, double r, double b, double t, double n, double f) { glOrtho (l, r, b, t, n, f); }
void QGL_Viewport (int x, int y, int w, int h) { glViewport (x, y, w, h); }
void QGL_ProjectionOverridden (void) { }

#endif	/* _WIN32 */
