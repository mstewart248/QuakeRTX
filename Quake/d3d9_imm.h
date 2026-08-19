/*
	d3d9_imm.h -- immediate-mode shim for the D3D9 backend

	gl_draw.c draws the console, menus, HUD and loading screens entirely in
	GL 1.x immediate mode: glBegin/glTexCoord2f/glVertex2f/glEnd, with glOrtho
	and glViewport for the canvas. Rather than branch every one of those call
	sites, this header redirects the handful of entry points involved to QGL_*
	wrappers that dispatch to D3D9 when -d3d9 is active and to real GL
	otherwise.

	Streaming these vertices per frame is deliberate and safe: 2D geometry is
	regenerated every frame by definition, and Remix does not path trace the
	HUD. The static-buffer discipline that geometry hashing depends on applies
	to world surfaces, which get real static vertex buffers instead.

	Include this AFTER quakedef.h, and only in files that draw 2D.
*/

#ifndef _D3D9_IMM_H
#define _D3D9_IMM_H

void QGL_Begin (GLenum mode);
void QGL_End (void);
void QGL_Vertex2f (float x, float y);
void QGL_Vertex3f (float x, float y, float z);
void QGL_Vertex3fv (const float *v);
void QGL_TexCoord2f (float s, float t);
void QGL_Color4f (float r, float g, float b, float a);
void QGL_Color3f (float r, float g, float b);
void QGL_Color4fv (const float *v);
void QGL_Color4ubv (const unsigned char *v);
void QGL_Color3ubv (const unsigned char *v);
void QGL_Enable (GLenum cap);
void QGL_Disable (GLenum cap);
void QGL_BlendFunc (GLenum sfactor, GLenum dfactor);
void QGL_DepthMask (GLboolean flag);
void QGL_DepthFunc (GLenum func);
void QGL_ColorMask (GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void QGL_Color3fv (const float *v);
void QGL_TexEnvf (GLenum target, GLenum pname, GLfloat param);
void QGL_MatrixMode (GLenum mode);
void QGL_LoadIdentity (void);
void QGL_Ortho (double l, double r, double b, double t, double n, double f);
void QGL_Viewport (int x, int y, int w, int h);

//called by the backend when the 3D pass writes D3DTS_PROJECTION itself, so a
//later glViewport does not restore the 2D ortho over the top of it
void QGL_ProjectionOverridden (void);

#ifndef D3D9_IMM_IMPLEMENTATION

#define glBegin		QGL_Begin
#define glEnd		QGL_End
#define glVertex2f	QGL_Vertex2f
#define glVertex3f	QGL_Vertex3f
#define glVertex3fv	QGL_Vertex3fv
#define glTexCoord2f	QGL_TexCoord2f
#define glColor4f	QGL_Color4f
#define glColor3f	QGL_Color3f
#define glColor4fv	QGL_Color4fv
#define glColor4ubv	QGL_Color4ubv
#define glColor3ubv	QGL_Color3ubv
#define glEnable	QGL_Enable
#define glDisable	QGL_Disable
#define glBlendFunc	QGL_BlendFunc
#define glDepthMask	QGL_DepthMask
#define glDepthFunc	QGL_DepthFunc
#define glColorMask	QGL_ColorMask
#define glColor3fv	QGL_Color3fv
#define glTexEnvf	QGL_TexEnvf
#define glMatrixMode	QGL_MatrixMode
#define glLoadIdentity	QGL_LoadIdentity
#define glOrtho		QGL_Ortho
#define glViewport	QGL_Viewport

#endif	/* !D3D9_IMM_IMPLEMENTATION */

#endif	/* _D3D9_IMM_H */
