/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
//gl_sky.c

#include "quakedef.h"
#include "d3d9_backend.h"
#include "d3d9_imm.h"

#define	MAX_CLIP_VERTS 64

float Fog_GetDensity(void);
float *Fog_GetColor(void);

extern	int	rs_skypolys; // for r_speeds readout
extern	int	rs_skypasses; // for r_speeds readout

float	skyflatcolor[3];
float	skymins[2][6], skymaxs[2][6];

qboolean skyroom_drawing;
qboolean skyroom_drawn;
qboolean skyroom_enabled;
vec4_t skyroom_origin;
vec4_t skyroom_orientation;

char	skybox_name[1024]; //name of current skybox, or "" if no skybox

gltexture_t	*skybox_textures[6];
gltexture_t	*solidskytexture, *alphaskytexture;

cvar_t r_fastsky = {"r_fastsky", "0", CVAR_NONE};
cvar_t r_sky_quality = {"r_sky_quality", "12", CVAR_NONE};
cvar_t r_skyalpha = {"r_skyalpha", "1", CVAR_NONE};
cvar_t r_skyfog = {"r_skyfog","0.5",CVAR_NONE};

/*
	Draw the sky as a real skybox under -d3d9.

	This is what makes RTX Remix classify the sky AS sky, which is the
	prerequisite for everything sky related on its side: the cubemap probe that
	feeds indirect lighting, HDRI replacement, and distant lights that are not
	blocked by the sky standing in front of them as ordinary geometry.

	Quake's own sky cannot be classified. It is thousands of little camera
	relative quads per frame, two blended layers deep, with the scroll folded
	into the texture coordinates -- so the vertex data rehashes every frame, the
	texture is shared between passes, and there is nothing stable for Remix to
	hang a category on. The classic trick of tagging it as a world space UI
	texture makes it visible again but it is still not sky to the path tracer.

	So instead the two cloud layers are resolved once per map into six real cube
	faces (Sky_BakeRemixSky), using exactly Quake's own sky projection, and
	drawn as six quads on a static camera centred cube. What is lost is the
	scrolling: the layers are baked at t=0 and stay put. That is a deliberate
	trade -- a path traced remaster replaces the sky with an HDRI anyway, and a
	sky that rehashes every frame cannot be replaced at all.

	0 restores the original per-frame cloud layers.
*/
cvar_t d3d9_skybox = {"d3d9_skybox", "1", CVAR_ARCHIVE};

//per face resolution of the baked cube. Also the ceiling on how much detail
//Remix's sky probe can pick up, so it is worth raising on a fast card.
cvar_t d3d9_skyresolution = {"d3d9_skyresolution", "256", CVAR_ARCHIVE};

int		skytexorder[6] = {0,2,1,3,4,5}; //for skybox

vec3_t	skyclip[6] = {
	{1,1,0},
	{1,-1,0},
	{0,-1,1},
	{0,1,1},
	{1,0,1},
	{-1,0,1}
};

int	st_to_vec[6][3] =
{
	{3,-1,2},
	{-3,1,2},
	{1,3,2},
	{-1,-3,2},
	{-2,-1,3},		// straight up
	{2,-1,-3}		// straight down
};

int	vec_to_st[6][3] =
{
	{-2,3,1},
	{2,3,-1},
	{1,3,2},
	{-1,3,-2},
	{-2,-1,3},
	{-2,1,-3}
};

float	skyfog; // ericw

//==============================================================================
//
//  BAKED SKY SOURCE DATA
//
//  The two cloud layers, kept as straight RGBA so the cube bake does not have
//  to care which format they arrived in. Owned here and freed on rebuild; the
//  texture manager only ever holds the baked results, never these.
//
//==============================================================================

static byte	*sky_layer_front = NULL;	//masked overlay, alpha 0 where it lets the back through
static byte	*sky_layer_back = NULL;		//opaque
static int	sky_layer_width = 0;
static int	sky_layer_height = 0;

static void Sky_FreeLayers (void)
{
	free (sky_layer_front);
	free (sky_layer_back);
	sky_layer_front = sky_layer_back = NULL;
	sky_layer_width = sky_layer_height = 0;
}

//defined with the rest of the Remix skybox further down
void Sky_InvalidateRemixSky (void);
static void Sky_FreeRemixCube (qboolean freetextures);

/*
=============
Sky_CaptureLayers

Takes the two layers as the loader has them and normalises both to RGBA.
Anything that is not indexed or plain RGBA (compressed replacement textures)
is declined, and the cube falls back to a flat sky-coloured box.
=============
*/
static void Sky_CaptureLayers (const byte *front, enum srcformat frontfmt,
				const byte *back, enum srcformat backfmt,
				int width, int height)
{
	int	i, count = width * height;

	Sky_FreeLayers ();
	Sky_InvalidateRemixSky ();

	if (count <= 0 || !front || !back)
		return;
	if ((frontfmt != SRC_INDEXED && frontfmt != SRC_RGBA) ||
	    (backfmt != SRC_INDEXED && backfmt != SRC_RGBA))
		return;

	sky_layer_front = (byte *) malloc ((size_t)count * 4);
	sky_layer_back = (byte *) malloc ((size_t)count * 4);
	if (!sky_layer_front || !sky_layer_back)
	{
		Sky_FreeLayers ();
		return;
	}

	for (i = 0; i < count; i++)
	{
		if (frontfmt == SRC_INDEXED)
		{
			const byte *p = (const byte *) &d_8to24table[front[i]];
			sky_layer_front[i*4+0] = p[0];
			sky_layer_front[i*4+1] = p[1];
			sky_layer_front[i*4+2] = p[2];
			/*
				Sky_LoadTexture has already folded the transparent index 0 onto
				255, because 255 is the only index the texture manager treats as
				transparent. Match that, not the raw 0.
			*/
			sky_layer_front[i*4+3] = (front[i] == 255) ? 0 : 255;
		}
		else
		{
			sky_layer_front[i*4+0] = front[i*4+0];
			sky_layer_front[i*4+1] = front[i*4+1];
			sky_layer_front[i*4+2] = front[i*4+2];
			sky_layer_front[i*4+3] = front[i*4+3];
		}

		if (backfmt == SRC_INDEXED)
		{
			const byte *p = (const byte *) &d_8to24table[back[i]];
			sky_layer_back[i*4+0] = p[0];
			sky_layer_back[i*4+1] = p[1];
			sky_layer_back[i*4+2] = p[2];
		}
		else
		{
			sky_layer_back[i*4+0] = back[i*4+0];
			sky_layer_back[i*4+1] = back[i*4+1];
			sky_layer_back[i*4+2] = back[i*4+2];
		}
		sky_layer_back[i*4+3] = 255;	//the back layer is never see-through
	}

	sky_layer_width = width;
	sky_layer_height = height;
}

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
=============
Sky_LoadTexture

A sky texture is 256*128, with the left side being a masked overlay
==============
*/
void Sky_LoadTexture (qmodel_t *mod, texture_t *mt, enum srcformat fmt, unsigned int srcwidth, unsigned int height)
{
	char		texturename[64];
	int			i, p, r, g, b, count;
	byte		*src;
	byte	*front_data;
	byte	*back_data;
	unsigned	*rgba;
	int rows, columns;
	int bb,bw,bh;
	int width = srcwidth/2;

	TexMgr_BlockSize(fmt, &bb, &bw, &bh);
	columns = (width+bw-1) / bw;
	rows = (height+bh-1) / bh;

	front_data = Hunk_AllocName (bb*columns*rows*2, "skytex");
	back_data = front_data+bb*columns*rows;

	src = (byte *)(mt+1);

// extract back layer and upload
	for (i=0 ; i<rows ; i++)
		memcpy(back_data+bb*i*columns, src+bb*(i*columns*2 + columns), columns*bb);

	q_snprintf(texturename, sizeof(texturename), "%s:%s_back", mod->name, mt->name);
	mt->gltexture = solidskytexture = TexMgr_LoadImage (mod, texturename, width, height, fmt, back_data, "", (src_offset_t)back_data, TEXPREF_NONE);

// extract front layer and upload
	for (i=0 ; i<rows ; i++)
		memcpy(front_data+bb*i*columns, src+bb*(i*columns*2), columns*bb);
	if (fmt == SRC_INDEXED)
	{	//the lame texmgr only knows one transparent index...
		for (i=0 ; i<width*height ; i++)
		{
			if (front_data[i] == 0)
				front_data[i] = 255;
		}
	}
	q_snprintf(texturename, sizeof(texturename), "%s:%s_front", mod->name, mt->name);
	mt->fullbright = alphaskytexture = TexMgr_LoadImage (mod, texturename, width, height, fmt, front_data, "", (src_offset_t)front_data, TEXPREF_ALPHA);

//keep a copy for the baked cube the D3D9 path draws; front_data's transparent
//index has already been folded onto 255 above
	Sky_CaptureLayers (front_data, fmt, back_data, fmt, width, height);

// calculate r_fastsky color based on average of all opaque foreground colors, if we can.
	r = g = b = count = 0;
	if (fmt == SRC_INDEXED)
	{
		for (i=0 ; i<width*height ; i++)
		{
			p = src[i];
			if (p != 0)
			{
				rgba = &d_8to24table[p];
				r += ((byte *)rgba)[0];
				g += ((byte *)rgba)[1];
				b += ((byte *)rgba)[2];
				count++;
			}
		}
	}

	//count is zero for any non-indexed source, and dividing by it leaves
	//skyflatcolor as NaN -- which then propagates into the flat sky fallback
	if (count)
	{
		skyflatcolor[0] = (float)r/(count*255);
		skyflatcolor[1] = (float)g/(count*255);
		skyflatcolor[2] = (float)b/(count*255);
	}
	else
	{
		skyflatcolor[0] = 0.15f;
		skyflatcolor[1] = 0.18f;
		skyflatcolor[2] = 0.25f;
	}
}

/*
=============
Sky_LoadTextureQ64

Quake64 sky textures are 32*64
==============
*/
void Sky_LoadTextureQ64 (qmodel_t *mod, texture_t *mt)
{
	char		texturename[64];
	unsigned	i, p, r, g, b, count, halfheight, *rgba;
	byte		*front, *back, *front_rgba;

	if (mt->width != 32 || mt->height != 64)
	{
		Con_DWarning ("Q64 sky texture %s is %d x %d, expected 32 x 64\n", mt->name, mt->width, mt->height);
		if (mt->width < 1 || mt->height < 2)
			return;
	}

	// pointers to both layer textures
	halfheight = mt->height / 2;
	front = (byte *)(mt+1);
	back = (byte *)(mt+1) + mt->width*halfheight;
	front_rgba = (byte *) Hunk_AllocName (4*mt->width*halfheight, "q64_skytex");

	// Normal indexed texture for the back layer
	q_snprintf(texturename, sizeof(texturename), "%s:%s_back", mod->name, mt->name);
	mt->gltexture = solidskytexture = TexMgr_LoadImage (mod, texturename, mt->width, halfheight, SRC_INDEXED, back, "", (src_offset_t)back, TEXPREF_NONE);

	// front layer, convert to RGBA and upload
	p = r = g = b = count = 0;

	for (i=mt->width*halfheight ; i!=0 ; i--)
	{
		rgba = &d_8to24table[*front++];

		// RGB
		front_rgba[p++] = ((byte*)rgba)[0];
		front_rgba[p++] = ((byte*)rgba)[1];
		front_rgba[p++] = ((byte*)rgba)[2];
		// Alpha
		front_rgba[p++] = 128; // this look ok to me!

		// Fast sky
		r += ((byte *)rgba)[0];
		g += ((byte *)rgba)[1];
		b += ((byte *)rgba)[2];
		count++;
	}

	q_snprintf(texturename, sizeof(texturename), "%s:%s_front", mod->name, mt->name);
	mt->fullbright = alphaskytexture = TexMgr_LoadImage (mod, texturename, mt->width, halfheight, SRC_RGBA, front_rgba, "", (src_offset_t)front_rgba, TEXPREF_ALPHA);

	//front_rgba carries alpha 128 throughout, so the bake blends the layers
	//half and half, which is what this path draws
	Sky_CaptureLayers (front_rgba, SRC_RGBA, back, SRC_INDEXED, mt->width, halfheight);

	// calculate r_fastsky color based on average of all opaque foreground colors
	skyflatcolor[0] = (float)r/(count*255);
	skyflatcolor[1] = (float)g/(count*255);
	skyflatcolor[2] = (float)b/(count*255);
}

/*
==================
Sky_LoadSkyBox
==================
*/
const char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
void Sky_LoadSkyBox (const char *name)
{
	int		i, mark, width, height;
	char	filename[MAX_OSPATH];
	byte	*data;
	qboolean nonefound = true;
	qboolean malloced;

	if (strcmp(skybox_name, name) == 0)
		return; //no change

	//purge old textures
	for (i=0; i<6; i++)
	{
		if (skybox_textures[i] && skybox_textures[i] != notexture)
			TexMgr_FreeTexture (skybox_textures[i]);
		skybox_textures[i] = NULL;
	}

	//turn off skybox if sky is set to ""
	if (name[0] == 0)
	{
		skybox_name[0] = 0;
		return;
	}

	//load textures
	for (i=0; i<6; i++)
	{
		enum srcformat fmt;
		mark = Hunk_LowMark ();
		q_snprintf (filename, sizeof(filename), "gfx/env/%s%s", name, suf[i]);
		data = Image_LoadImage (filename, &width, &height, &fmt, &malloced);
		if (data)
		{
			skybox_textures[i] = TexMgr_LoadImage (cl.worldmodel, filename, width, height, fmt, data, filename, 0, TEXPREF_NONE);
			nonefound = false;
		}
		else
		{
			Con_Printf ("Couldn't load %s\n", filename);
			skybox_textures[i] = notexture;
		}
		if (malloced)
			free(data);
		Hunk_FreeToLowMark (mark);
	}

	if (nonefound) // go back to scrolling sky if skybox is totally missing
	{
		for (i=0; i<6; i++)
		{
			if (skybox_textures[i] && skybox_textures[i] != notexture)
				TexMgr_FreeTexture (skybox_textures[i]);
			skybox_textures[i] = NULL;
		}
		skybox_name[0] = 0;
		Sky_InvalidateRemixSky ();
		return;
	}

	q_strlcpy(skybox_name, name, sizeof(skybox_name));
	Sky_InvalidateRemixSky ();
}

/*
=================
Sky_ClearAll

Called on map unload/game change to avoid keeping pointers to freed data
=================
*/
void Sky_ClearAll (void)
{
	int i;

	skyroom_enabled = false;
	skybox_name[0] = 0;
	for (i=0; i<6; i++)
		skybox_textures[i] = NULL;
	solidskytexture = NULL;
	alphaskytexture = NULL;

	/*
		Mod_ClearAll has already run TexMgr_FreeTexturesForOwner over the world
		model by the time we get here, which took the baked cube faces with it.
		Drop the pointers rather than freeing them again.
	*/
	Sky_FreeRemixCube (false);
	Sky_FreeLayers ();
}

/*
=================
Sky_NewMap
=================
*/
void Sky_NewMap (void)
{
	char	key[128], value[4096];
	const char	*data;

	skyfog = r_skyfog.value;

	//
	// read worldspawn (this is so ugly, and shouldn't it be done on the server?)
	//
	data = cl.worldmodel->entities;
	if (!data)
		return; //FIXME: how could this possibly ever happen? -- if there's no
	// worldspawn then the sever wouldn't send the loadmap message to the client

	data = COM_Parse(data);
	if (!data) //should never happen
		return; // error
	if (com_token[0] != '{') //should never happen
		return; // error
	while (1)
	{
		data = COM_Parse(data);
		if (!data)
			return; // error
		if (com_token[0] == '}')
			break; // end of worldspawn
		if (com_token[0] == '_')
			q_strlcpy(key, com_token + 1, sizeof(key));
		else
			q_strlcpy(key, com_token, sizeof(key));
		while (key[0] && key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;
		data = COM_Parse(data);
		if (!data)
			return; // error
		q_strlcpy(value, com_token, sizeof(value));

		if (!strcmp("sky", key))
			Sky_LoadSkyBox(value);
		else if (!strcmp("skyroom", key))
		{	//"_skyroom" "X Y Z". ideally the gamecode would do this with an entity, but people want to use the vanilla gamecode from 1996 for some reason.
			const char *t = COM_Parse(value);
			skyroom_origin[0] = atof(com_token);
			t = COM_Parse(t);
			skyroom_origin[1] = atof(com_token);
			t = COM_Parse(t);
			skyroom_origin[2] = atof(com_token);
			t = COM_Parse(t);
			skyroom_origin[3] = atof(com_token);
			skyroom_enabled = true;

			t = COM_Parse(t);
			skyroom_orientation[3] = atof(com_token);
			t = COM_Parse(t);
			skyroom_orientation[0] = atof(com_token);
			t = COM_Parse(t);
			skyroom_orientation[1] = atof(com_token);
			t = COM_Parse(t);
			skyroom_orientation[2] = atof(com_token);
		}

		else if (!strcmp("skyfog", key))
			skyfog = atof(value);

#if 1 /* also accept non-standard keys */
		else if (!strcmp("skyname", key)) //half-life
			Sky_LoadSkyBox(value);
		else if (!strcmp("qlsky", key)) //quake lives
			Sky_LoadSkyBox(value);
#endif
	}
}

/*
=================
Sky_SkyCommand_f
=================
*/
void Sky_SkyCommand_f (void)
{
	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf("\"sky\" is \"%s\"\n", skybox_name);
		break;
	case 2:
		Sky_LoadSkyBox(Cmd_Argv(1));
		break;
	default:
		Con_Printf("usage: sky <skyname>\n");
	}
}

static void Sky_SkyRoomCommand_f (void)
{
	switch (Cmd_Argc())
	{
	case 1:
		if (skyroom_enabled)
			Con_Printf("\"skyroom\" is \"%f %f %f %f %f %f %f %f\"\n", skyroom_origin[0],skyroom_origin[1],skyroom_origin[2],skyroom_origin[3], skyroom_orientation[3],skyroom_orientation[0],skyroom_orientation[1],skyroom_orientation[2]);
		else
			Con_Printf("\"skyroom\" is \"\"\n");
		break;
	case 4:	//xyz
	case 5:	//xyz paralax
	case 6:	//+speed
	case 7:	//+axis_x
	case 8:	//+axis_y
	case 9:	//+axis_z
		skyroom_enabled = true;
		skyroom_origin[0] = atof(Cmd_Argv(1));
		skyroom_origin[1] = atof(Cmd_Argv(2));
		skyroom_origin[2] = atof(Cmd_Argv(3));
		skyroom_origin[3] = atof(Cmd_Argv(4));	//paralax

		skyroom_orientation[3] = atof(Cmd_Argv(5));	//speed
		skyroom_orientation[0] = atof(Cmd_Argv(6));
		skyroom_orientation[1] = atof(Cmd_Argv(7));
		skyroom_orientation[2] = atof(Cmd_Argv(8));
		break;
	case 2:	//x
		if (!*Cmd_Argv(1) || !q_strcasecmp(Cmd_Argv(1), "off"))
		{
			skyroom_enabled = false;
			break;
		}
		//fallthrough
	case 3:	//xy
	default:
		Con_Printf("usage: skyroom origin_x origin_y origin_z paralax_scale speed axis_x axis_y axis_z\n");
	}
}

/*
====================
R_SetSkyfog_f -- ericw
====================
*/
static void R_SetSkyfog_f (cvar_t *var)
{
// clear any skyfog setting from worldspawn
	skyfog = var->value;
}

/*
====================
Sky_RemixChanged_f -- rebake on the next frame
====================
*/
static void Sky_RemixChanged_f (cvar_t *var)
{
	(void) var;
	Sky_InvalidateRemixSky ();
}

/*
=============
Sky_Init
=============
*/
void Sky_Init (void)
{
	int		i;

	Cvar_RegisterVariable (&r_fastsky);
	Cvar_RegisterVariable (&r_sky_quality);
	Cvar_RegisterVariable (&r_skyalpha);
	Cvar_RegisterVariable (&r_skyfog);
	Cvar_SetCallback (&r_skyfog, R_SetSkyfog_f);
	Cvar_RegisterVariable (&d3d9_skybox);
	Cvar_RegisterVariable (&d3d9_skyresolution);
	Cvar_SetCallback (&d3d9_skyresolution, Sky_RemixChanged_f);

	Cmd_AddCommand ("sky",Sky_SkyCommand_f);
	Cmd_AddCommand ("skyroom",Sky_SkyRoomCommand_f);

	skybox_name[0] = 0;
	for (i=0; i<6; i++)
		skybox_textures[i] = NULL;
}

//==============================================================================
//
//  REMIX SKYBOX
//
//  Six cube faces on a static camera centred cube, drawn through
//  D3D9_BeginSky so RTX Remix classifies them as sky. See the d3d9_skybox
//  comment at the top of this file for why the original sky cannot be.
//
//==============================================================================

#define SKY_CUBE_FACES	6

static gltexture_t	*sky_cube_tex[SKY_CUBE_FACES];
static byte		*sky_cube_pixels[SKY_CUBE_FACES];	//kept alive: TexMgr reloads from these
static qboolean		sky_cube_valid = false;
static qboolean		sky_cube_useenv = false;		//gfx/env skybox rather than baked layers
static qboolean		sky_cube_dirty = true;
static qboolean		sky_cube_vbready = false;

/*
=============
Sky_InvalidateRemixSky
=============
*/
void Sky_InvalidateRemixSky (void)
{
	sky_cube_dirty = true;
}

/*
=============
Sky_FreeRemixCube
=============
*/
static void Sky_FreeRemixCube (qboolean freetextures)
{
	int i;

	for (i = 0; i < SKY_CUBE_FACES; i++)
	{
		if (freetextures && sky_cube_tex[i])
			TexMgr_FreeTexture (sky_cube_tex[i]);
		sky_cube_tex[i] = NULL;

		free (sky_cube_pixels[i]);
		sky_cube_pixels[i] = NULL;
	}

	sky_cube_valid = false;
	sky_cube_useenv = false;
}

/*
=============
Sky_FaceDir

The direction Sky_SetBoxVert would produce for (s,t) on this face, with the
camera at the origin and a unit sized box. Everything downstream of this is
scale invariant, so the real size lives in the world matrix instead.
=============
*/
static void Sky_FaceDir (int axis, float s, float t, vec3_t out)
{
	float	b[3];
	int	j, k;

	b[0] = s;
	b[1] = t;
	b[2] = 1.0f;

	for (j = 0; j < 3; j++)
	{
		k = st_to_vec[axis][j];
		out[j] = (k < 0) ? -b[-k - 1] : b[k - 1];
	}
}

/*
=============
Sky_SampleLayer

Bilinear, wrapping, and weighted by alpha so the transparent texels of the
front layer do not drag their palette colour into the fringe.
=============
*/
static void Sky_SampleLayer (const byte *layer, float u, float v, float *out)
{
	int	w = sky_layer_width, h = sky_layer_height;
	int	x0, y0, x1, y1, i;
	float	fx, fy, ax, ay, wgt[4], suma, sum[3];
	const byte *tex[4];

	fx = u * w - 0.5f;
	fy = v * h - 0.5f;

	x0 = (int) floor (fx);
	y0 = (int) floor (fy);
	ax = fx - x0;
	ay = fy - y0;
	x1 = x0 + 1;
	y1 = y0 + 1;

	x0 = ((x0 % w) + w) % w;
	x1 = ((x1 % w) + w) % w;
	y0 = ((y0 % h) + h) % h;
	y1 = ((y1 % h) + h) % h;

	tex[0] = layer + ((size_t)y0 * w + x0) * 4;
	tex[1] = layer + ((size_t)y0 * w + x1) * 4;
	tex[2] = layer + ((size_t)y1 * w + x0) * 4;
	tex[3] = layer + ((size_t)y1 * w + x1) * 4;

	wgt[0] = (1.0f - ax) * (1.0f - ay);
	wgt[1] = ax * (1.0f - ay);
	wgt[2] = (1.0f - ax) * ay;
	wgt[3] = ax * ay;

	suma = 0.0f;
	sum[0] = sum[1] = sum[2] = 0.0f;

	for (i = 0; i < 4; i++)
	{
		float a = tex[i][3] * wgt[i];
		suma += a;
		sum[0] += tex[i][0] * a;
		sum[1] += tex[i][1] * a;
		sum[2] += tex[i][2] * a;
	}

	if (suma > 0.0f)
	{
		out[0] = sum[0] / suma;
		out[1] = sum[1] / suma;
		out[2] = sum[2] / suma;
	}
	else
		out[0] = out[1] = out[2] = 0.0f;

	//coverage, independent of the colour weighting above
	out[3] = tex[0][3]*wgt[0] + tex[1][3]*wgt[1] + tex[2][3]*wgt[2] + tex[3][3]*wgt[3];
}

/*
=============
Sky_BakeFace

Resolves Quake's two cloud layers into one cube face. Per texel this is the
same projection Sky_GetTexCoord does per vertex, with the scroll at zero --
which means the result is if anything more correct than the original, since
the original only evaluates it on an r_sky_quality grid and interpolates.
=============
*/
static void Sky_BakeFace (int axis, byte *out, int n)
{
	int	x, y;
	vec3_t	dir;
	float	front[4], back[4], s, t, len, a;

	for (y = 0; y < n; y++)
	{
		t = ((y + 0.5f) / n) * 2.0f - 1.0f;

		for (x = 0; x < n; x++)
		{
			byte *dst = out + ((size_t)y * n + x) * 4;

			s = ((x + 0.5f) / n) * 2.0f - 1.0f;

			Sky_FaceDir (axis, s, t, dir);

			if (!sky_layer_front || !sky_layer_back)
			{	//no usable source layers: a flat box in the fast sky colour
				dst[0] = (byte) CLAMP(0, (int)(skyflatcolor[0] * 255), 255);
				dst[1] = (byte) CLAMP(0, (int)(skyflatcolor[1] * 255), 255);
				dst[2] = (byte) CLAMP(0, (int)(skyflatcolor[2] * 255), 255);
				dst[3] = 255;
				continue;
			}

			dir[2] *= 3;	// flatten the sphere
			len = (float) sqrt (dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
			len = (len > 0.0f) ? 6*63/len : 0.0f;

			Sky_SampleLayer (sky_layer_back, dir[0]*len*(1.0f/128), dir[1]*len*(1.0f/128), back);
			Sky_SampleLayer (sky_layer_front, dir[0]*len*(1.0f/128), dir[1]*len*(1.0f/128), front);

			a = front[3] * (1.0f/255.0f);

			dst[0] = (byte) CLAMP(0, (int)(front[0]*a + back[0]*(1.0f-a) + 0.5f), 255);
			dst[1] = (byte) CLAMP(0, (int)(front[1]*a + back[1]*(1.0f-a) + 0.5f), 255);
			dst[2] = (byte) CLAMP(0, (int)(front[2]*a + back[2]*(1.0f-a) + 0.5f), 255);
			dst[3] = 255;
		}
	}
}

/*
=============
Sky_BuildCubeBuffer

48 vertices: the unit cube twice over. Faces 0-5 carry the texcoords the baked
faces want, faces 6-11 the vertically flipped ones a gfx/env skybox wants
(Sky_EmitSkyBoxVertex does that flip inline). Written once and never touched
again, which is what keeps the geometry hash stable for Remix.
=============
*/
static qboolean Sky_BuildCubeBuffer (void)
{
	static const float corner[4][2] = {{-1,-1}, {-1,1}, {1,1}, {1,-1}};
	float	verts[2 * SKY_CUBE_FACES * 4 * 5];
	int	flip, axis, i, o = 0;

	if (sky_cube_vbready)
		return true;

	for (flip = 0; flip < 2; flip++)
	{
		for (axis = 0; axis < SKY_CUBE_FACES; axis++)
		{
			for (i = 0; i < 4; i++)
			{
				float	s = corner[i][0], t = corner[i][1];
				vec3_t	dir;

				Sky_FaceDir (axis, s, t, dir);

				verts[o++] = dir[0];
				verts[o++] = dir[1];
				verts[o++] = dir[2];
				verts[o++] = (s + 1.0f) * 0.5f;
				verts[o++] = flip ? 1.0f - (t + 1.0f) * 0.5f : (t + 1.0f) * 0.5f;
			}
		}
	}

	sky_cube_vbready = D3D9_CreateSkyBuffer (verts, 2 * SKY_CUBE_FACES * 4) ? true : false;
	return sky_cube_vbready;
}

/*
=============
Sky_BuildRemixSky
=============
*/
static qboolean Sky_BuildRemixSky (void)
{
	char	name[64];
	int	i, n;

	sky_cube_dirty = false;
	Sky_FreeRemixCube (true);

	if (!Sky_BuildCubeBuffer ())
		return false;

	//a real skybox is already six cube faces; nothing to bake
	if (skybox_name[0])
	{
		for (i = 0; i < SKY_CUBE_FACES; i++)
			if (!skybox_textures[i])
				break;
		if (i == SKY_CUBE_FACES)
		{
			sky_cube_useenv = true;
			sky_cube_valid = true;
			return true;
		}
	}

	/*
		Round down to a power of two. The texture manager rescales anything
		else on upload, which would silently change the pixels Remix hashes --
		and a hash that does not match what you tagged is a sky that is not a
		sky.
	*/
	{
		int want = CLAMP(32, (int)d3d9_skyresolution.value, 1024);
		for (n = 32; (n << 1) <= want; n <<= 1)
			;
	}

	for (i = 0; i < SKY_CUBE_FACES; i++)
	{
		sky_cube_pixels[i] = (byte *) malloc ((size_t)n * n * 4);
		if (!sky_cube_pixels[i])
		{
			Sky_FreeRemixCube (true);
			return false;
		}

		Sky_BakeFace (i, sky_cube_pixels[i], n);

		/*
			Named per map so two maps with different skies never collide in the
			texture manager. Remix hashes the pixels rather than the name, so a
			shared sky such as sky1 still lands on one hash across maps and one
			entry in rtx.skyBoxTextures covers all of them.
		*/
		q_snprintf (name, sizeof(name), "skyremix_%d:%s", i,
				cl.worldmodel ? cl.worldmodel->name : "");

		sky_cube_tex[i] = TexMgr_LoadImage (cl.worldmodel, name, n, n, SRC_RGBA,
					sky_cube_pixels[i], "", (src_offset_t)sky_cube_pixels[i],
					TEXPREF_NOPICMIP | TEXPREF_OVERWRITE);

		if (!sky_cube_tex[i])
		{
			Sky_FreeRemixCube (true);
			return false;
		}
	}

	sky_cube_valid = true;
	Con_DPrintf ("D3D9: baked %dx%d sky cube\n", n, n);
	return true;
}

/*
=============
Sky_DrawRemixSky

Returns false if the caller should fall back to the original sky.
=============
*/
static qboolean Sky_DrawRemixSky (void)
{
	int	i;
	float	scale;

	if (!D3D9_Active() || !d3d9_skybox.value)
		return false;

	//no sky texture and no skybox means this map has no sky surfaces at all
	if (!skybox_name[0] && !solidskytexture)
		return false;

	if (sky_cube_dirty && !Sky_BuildRemixSky ())
		return false;

	if (!sky_cube_valid)
		return false;

	/*
		Half the far clip out to the corners. Depth testing is off for these
		draws so the distance changes nothing visually, but the cube still has
		to sit inside the frustum or its corners get clipped away.
	*/
	scale = gl_farclip.value * 0.5f / 1.7320508f;
	if (scale < 64.0f)
		scale = 64.0f;

	D3D9_BeginSky (r_origin, scale);

	for (i = 0; i < SKY_CUBE_FACES; i++)
	{
		if (sky_cube_useenv)
		{
			GL_Bind (skybox_textures[skytexorder[i]]);
			D3D9_DrawSkyFace (SKY_CUBE_FACES + i);
		}
		else
		{
			GL_Bind (sky_cube_tex[i]);
			D3D9_DrawSkyFace (i);
		}

		rs_skypolys++;
		rs_skypasses++;
	}

	D3D9_EndSky ();
	return true;
}

//==============================================================================
//
//  PROCESS SKY SURFS
//
//==============================================================================

/*
=================
Sky_ProjectPoly

update sky bounds
=================
*/
void Sky_ProjectPoly (int nump, vec3_t vecs)
{
	int		i,j;
	vec3_t	v, av;
	float	s, t, dv;
	int		axis;
	float	*vp;

	// decide which face it maps to
	VectorCopy (vec3_origin, v);
	for (i=0, vp=vecs ; i<nump ; i++, vp+=3)
	{
		VectorAdd (vp, v, v);
	}
	av[0] = fabs(v[0]);
	av[1] = fabs(v[1]);
	av[2] = fabs(v[2]);
	if (av[0] > av[1] && av[0] > av[2])
	{
		if (v[0] < 0)
			axis = 1;
		else
			axis = 0;
	}
	else if (av[1] > av[2] && av[1] > av[0])
	{
		if (v[1] < 0)
			axis = 3;
		else
			axis = 2;
	}
	else
	{
		if (v[2] < 0)
			axis = 5;
		else
			axis = 4;
	}

	// project new texture coords
	for (i=0 ; i<nump ; i++, vecs+=3)
	{
		j = vec_to_st[axis][2];
		if (j > 0)
			dv = vecs[j - 1];
		else
			dv = -vecs[-j - 1];

		j = vec_to_st[axis][0];
		if (j < 0)
			s = -vecs[-j -1] / dv;
		else
			s = vecs[j-1] / dv;
		j = vec_to_st[axis][1];
		if (j < 0)
			t = -vecs[-j -1] / dv;
		else
			t = vecs[j-1] / dv;

		if (s < skymins[0][axis])
			skymins[0][axis] = s;
		if (t < skymins[1][axis])
			skymins[1][axis] = t;
		if (s > skymaxs[0][axis])
			skymaxs[0][axis] = s;
		if (t > skymaxs[1][axis])
			skymaxs[1][axis] = t;
	}
}

/*
=================
Sky_ClipPoly
=================
*/
void Sky_ClipPoly (int nump, vec3_t vecs, int stage)
{
	float	*norm;
	float	*v;
	qboolean	front, back;
	float	d, e;
	float	dists[MAX_CLIP_VERTS];
	int		sides[MAX_CLIP_VERTS];
	vec3_t	newv[2][MAX_CLIP_VERTS];
	int		newc[2];
	int		i, j;

	if (nump > MAX_CLIP_VERTS-2)
		Sys_Error ("Sky_ClipPoly: MAX_CLIP_VERTS");
	if (stage == 6) // fully clipped
	{
		Sky_ProjectPoly (nump, vecs);
		return;
	}

	front = back = false;
	norm = skyclip[stage];
	for (i=0, v = vecs ; i<nump ; i++, v+=3)
	{
		d = DotProduct (v, norm);
		if (d > ON_EPSILON)
		{
			front = true;
			sides[i] = SIDE_FRONT;
		}
		else if (d < ON_EPSILON)
		{
			back = true;
			sides[i] = SIDE_BACK;
		}
		else
			sides[i] = SIDE_ON;
		dists[i] = d;
	}

	if (!front || !back)
	{	// not clipped
		Sky_ClipPoly (nump, vecs, stage+1);
		return;
	}

	// clip it
	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy (vecs, (vecs+(i*3)) );
	newc[0] = newc[1] = 0;

	for (i=0, v = vecs ; i<nump ; i++, v+=3)
	{
		switch (sides[i])
		{
		case SIDE_FRONT:
			VectorCopy (v, newv[0][newc[0]]);
			newc[0]++;
			break;
		case SIDE_BACK:
			VectorCopy (v, newv[1][newc[1]]);
			newc[1]++;
			break;
		case SIDE_ON:
			VectorCopy (v, newv[0][newc[0]]);
			newc[0]++;
			VectorCopy (v, newv[1][newc[1]]);
			newc[1]++;
			break;
		}

		if (sides[i] == SIDE_ON || sides[i+1] == SIDE_ON || sides[i+1] == sides[i])
			continue;

		d = dists[i] / (dists[i] - dists[i+1]);
		for (j=0 ; j<3 ; j++)
		{
			e = v[j] + d*(v[j+3] - v[j]);
			newv[0][newc[0]][j] = e;
			newv[1][newc[1]][j] = e;
		}
		newc[0]++;
		newc[1]++;
	}

	// continue
	Sky_ClipPoly (newc[0], newv[0][0], stage+1);
	Sky_ClipPoly (newc[1], newv[1][0], stage+1);
}

/*
================
Sky_ProcessPoly
================
*/
void Sky_ProcessPoly (glpoly_t	*p)
{
	int			i;
	vec3_t		verts[MAX_CLIP_VERTS];

	//draw it
	DrawGLPoly(p);
	rs_brushpasses++;

	//update sky bounds
	if (!r_fastsky.value)
	{
		for (i=0 ; i<p->numverts ; i++)
			VectorSubtract (p->verts[i], r_origin, verts[i]);
		Sky_ClipPoly (p->numverts, verts[0], 0);
	}
}

/*
================
Sky_ProcessTextureChains -- handles sky polys in world model
================
*/
void Sky_ProcessTextureChains (void)
{
	int			i;
	msurface_t	*s;
	texture_t	*t;

	if (!r_drawworld_cheatsafe)
		return;

	for (i=0 ; i<cl.worldmodel->numtextures ; i++)
	{
		t = cl.worldmodel->textures[i];

		if (!t || !t->texturechains[chain_world] || !(t->texturechains[chain_world]->flags & SURF_DRAWSKY))
			continue;

		for (s = t->texturechains[chain_world]; s; s = s->texturechain)
			Sky_ProcessPoly (s->polys);
	}
}

/*
================
Sky_ProcessEntities -- handles sky polys on brush models
================
*/
void Sky_ProcessEntities (void)
{
	entity_t	*e;
	msurface_t	*s;
	glpoly_t	*p;
	int			i,j,k,mark;
	float		dot;
	qboolean	rotated;
	vec3_t		temp, forward, right, up;

	if (!r_drawentities.value)
		return;

	for (i=0 ; i<cl_numvisedicts ; i++)
	{
		e = cl_visedicts[i];

		if (!e->model || e->model->needload || e->model->type != mod_brush)
			continue;

		if (e->model->submodelof == cl.worldmodel &&
			skipsubmodels &&
			skipsubmodels[e->model->submodelidx>>3]&(1u<<(e->model->submodelidx&7)))
			return;	//its in the scenecache that we're drawing. don't draw it twice (and certainly not the slow way).

		if (R_CullModelForEntity(e))
			continue;

		if (e->alpha == ENTALPHA_ZERO)
			continue;

		VectorSubtract (r_refdef.vieworg, e->origin, modelorg);
		if (e->angles[0] || e->angles[1] || e->angles[2])
		{
			rotated = true;
			AngleVectors (e->angles, forward, right, up);
			VectorCopy (modelorg, temp);
			modelorg[0] = DotProduct (temp, forward);
			modelorg[1] = -DotProduct (temp, right);
			modelorg[2] = DotProduct (temp, up);
		}
		else
			rotated = false;

		s = &e->model->surfaces[e->model->firstmodelsurface];

		for (j=0 ; j<e->model->nummodelsurfaces ; j++, s++)
		{
			if (s->flags & SURF_DRAWSKY)
			{
				dot = DotProduct (modelorg, s->plane->normal) - s->plane->dist;
				if (((s->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
					(!(s->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
				{
					//copy the polygon and translate manually, since Sky_ProcessPoly needs it to be in world space
					mark = Hunk_LowMark();
					p = (glpoly_t *) Hunk_Alloc (sizeof(*s->polys)); //FIXME: don't allocate for each poly
					p->numverts = s->polys->numverts;
					for (k=0; k<p->numverts; k++)
					{
						if (rotated)
						{
							p->verts[k][0] = e->origin[0] + s->polys->verts[k][0] * forward[0]
														  - s->polys->verts[k][1] * right[0]
														  + s->polys->verts[k][2] * up[0];
							p->verts[k][1] = e->origin[1] + s->polys->verts[k][0] * forward[1]
														  - s->polys->verts[k][1] * right[1]
														  + s->polys->verts[k][2] * up[1];
							p->verts[k][2] = e->origin[2] + s->polys->verts[k][0] * forward[2]
														  - s->polys->verts[k][1] * right[2]
														  + s->polys->verts[k][2] * up[2];
						}
						else
							VectorAdd(s->polys->verts[k], e->origin, p->verts[k]);
					}
					Sky_ProcessPoly (p);
					Hunk_FreeToLowMark (mark);
				}
			}
		}
	}
}

//==============================================================================
//
//  RENDER SKYBOX
//
//==============================================================================

/*
==============
Sky_EmitSkyBoxVertex
==============
*/
void Sky_EmitSkyBoxVertex (float s, float t, int axis)
{
	vec3_t		v, b;
	int			j, k;
	float		w, h;

	b[0] = s * gl_farclip.value / sqrt(3.0);
	b[1] = t * gl_farclip.value / sqrt(3.0);
	b[2] = gl_farclip.value / sqrt(3.0);

	for (j=0 ; j<3 ; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
		v[j] += r_origin[j];
	}

	// convert from range [-1,1] to [0,1]
	s = (s+1)*0.5;
	t = (t+1)*0.5;

	// avoid bilerp seam
	w = skybox_textures[skytexorder[axis]]->width;
	h = skybox_textures[skytexorder[axis]]->height;
	s = s * (w-1)/w + 0.5/w;
	t = t * (h-1)/h + 0.5/h;

	t = 1.0 - t;
	glTexCoord2f (s, t);
	glVertex3fv (v);
}

/*
==============
Sky_DrawSkyBox

FIXME: eliminate cracks by adding an extra vert on tjuncs
==============
*/
void Sky_DrawSkyBox (void)
{
	int i;

	for (i=0 ; i<6 ; i++)
	{
		if (skymins[0][i] >= skymaxs[0][i] || skymins[1][i] >= skymaxs[1][i])
			continue;

		GL_Bind (skybox_textures[skytexorder[i]]);

#if 1 /* FIXME: this is to avoid tjunctions until i can do it the right way */
		skymins[0][i] = -1;
		skymins[1][i] = -1;
		skymaxs[0][i] = 1;
		skymaxs[1][i] = 1;
#endif
		glBegin (GL_QUADS);
		Sky_EmitSkyBoxVertex (skymins[0][i], skymins[1][i], i);
		Sky_EmitSkyBoxVertex (skymins[0][i], skymaxs[1][i], i);
		Sky_EmitSkyBoxVertex (skymaxs[0][i], skymaxs[1][i], i);
		Sky_EmitSkyBoxVertex (skymaxs[0][i], skymins[1][i], i);
		glEnd ();

		rs_skypolys++;
		rs_skypasses++;

		if (Fog_GetDensity() > 0 && skyfog > 0)
		{
			float *c;

			c = Fog_GetColor();
			glEnable (GL_BLEND);
			glDisable (GL_TEXTURE_2D);
			glColor4f (c[0],c[1],c[2], CLAMP(0.0f,skyfog,1.0f));

			glBegin (GL_QUADS);
			Sky_EmitSkyBoxVertex (skymins[0][i], skymins[1][i], i);
			Sky_EmitSkyBoxVertex (skymins[0][i], skymaxs[1][i], i);
			Sky_EmitSkyBoxVertex (skymaxs[0][i], skymaxs[1][i], i);
			Sky_EmitSkyBoxVertex (skymaxs[0][i], skymins[1][i], i);
			glEnd ();

			glColor3f (1, 1, 1);
			glEnable (GL_TEXTURE_2D);
			glDisable (GL_BLEND);

			rs_skypasses++;
		}
	}
}

//==============================================================================
//
//  RENDER CLOUDS
//
//==============================================================================

/*
==============
Sky_SetBoxVert
==============
*/
void Sky_SetBoxVert (float s, float t, int axis, vec3_t v)
{
	vec3_t		b;
	int			j, k;

	b[0] = s * gl_farclip.value / sqrt(3.0);
	b[1] = t * gl_farclip.value / sqrt(3.0);
	b[2] = gl_farclip.value / sqrt(3.0);

	for (j=0 ; j<3 ; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
		v[j] += r_origin[j];
	}
}

/*
=============
Sky_GetTexCoord
=============
*/
void Sky_GetTexCoord (vec3_t v, float speed, float *s, float *t)
{
	vec3_t	dir;
	float	length, scroll;

	VectorSubtract (v, r_origin, dir);
	dir[2] *= 3;	// flatten the sphere

	length = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
	length = sqrt (length);
	length = 6*63/length;

	scroll = cl.time*speed;
	scroll -= (int)scroll & ~127;

	*s = (scroll + dir[0] * length) * (1.0/128);
	*t = (scroll + dir[1] * length) * (1.0/128);
}

/*
===============
Sky_DrawFaceQuad
===============
*/
void Sky_DrawFaceQuad (glpoly_t *p)
{
	float	s, t;
	float	*v;
	int		i;

	if (gl_mtexable && r_skyalpha.value >= 1.0)
	{
		GL_Bind (solidskytexture);
		GL_EnableMultitexture();
		GL_Bind (alphaskytexture);
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);

		glBegin (GL_QUADS);
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
		{
			Sky_GetTexCoord (v, 8, &s, &t);
			GL_MTexCoord2fFunc (GL_TEXTURE0_ARB, s, t);
			Sky_GetTexCoord (v, 16, &s, &t);
			GL_MTexCoord2fFunc (GL_TEXTURE1_ARB, s, t);
			glVertex3fv (v);
		}
		glEnd ();

		GL_DisableMultitexture();

		rs_skypolys++;
		rs_skypasses++;
	}
	else
	{
		GL_Bind (solidskytexture);

		if (r_skyalpha.value < 1.0)
			glColor3f (1, 1, 1);

		glBegin (GL_QUADS);
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
		{
			Sky_GetTexCoord (v, 8, &s, &t);
			glTexCoord2f (s, t);
			glVertex3fv (v);
		}
		glEnd ();

		GL_Bind (alphaskytexture);
		glEnable (GL_BLEND);

		if (r_skyalpha.value < 1.0)
			glColor4f (1, 1, 1, r_skyalpha.value);

		glBegin (GL_QUADS);
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
		{
			Sky_GetTexCoord (v, 16, &s, &t);
			glTexCoord2f (s, t);
			glVertex3fv (v);
		}
		glEnd ();

		glDisable (GL_BLEND);

		rs_skypolys++;
		rs_skypasses += 2;
	}

	if (Fog_GetDensity() > 0 && skyfog > 0)
	{
		float *c;

		c = Fog_GetColor();
		glEnable (GL_BLEND);
		glDisable (GL_TEXTURE_2D);
		glColor4f (c[0],c[1],c[2], CLAMP(0.0f,skyfog,1.0f));

		glBegin (GL_QUADS);
		for (i=0, v=p->verts[0] ; i<4 ; i++, v+=VERTEXSIZE)
			glVertex3fv (v);
		glEnd ();

		glColor3f (1, 1, 1);
		glEnable (GL_TEXTURE_2D);
		glDisable (GL_BLEND);

		rs_skypasses++;
	}
}

/*
==============
Sky_DrawFace
==============
*/

void Sky_DrawFace (int axis)
{
	glpoly_t	*p;
	vec3_t		verts[4];
	int			i, j, start;
	float		di,qi,dj,qj;
	vec3_t		up, right, temp, temp2;

	Sky_SetBoxVert(-1.0, -1.0, axis, verts[0]);
	Sky_SetBoxVert(-1.0,  1.0, axis, verts[1]);
	Sky_SetBoxVert(1.0,   1.0, axis, verts[2]);
	Sky_SetBoxVert(1.0,  -1.0, axis, verts[3]);

	start = Hunk_LowMark ();
	p = (glpoly_t *) Hunk_Alloc(sizeof(glpoly_t));

	VectorSubtract(verts[2],verts[3],up);
	VectorSubtract(verts[2],verts[1],right);

	di = q_max((int)r_sky_quality.value, 1);
	qi = 1.0 / di;
	dj = (axis < 4) ? di*2 : di; //subdivide vertically more than horizontally on skybox sides
	qj = 1.0 / dj;

	for (i=0; i<di; i++)
	{
		for (j=0; j<dj; j++)
		{
			if (i*qi < skymins[0][axis]/2+0.5 - qi || i*qi > skymaxs[0][axis]/2+0.5 ||
				j*qj < skymins[1][axis]/2+0.5 - qj || j*qj > skymaxs[1][axis]/2+0.5)
				continue;

			//if (i&1 ^ j&1) continue; //checkerboard test
			VectorScale (right, qi*i, temp);
			VectorScale (up, qj*j, temp2);
			VectorAdd(temp,temp2,temp);
			VectorAdd(verts[0],temp,p->verts[0]);

			VectorScale (up, qj, temp);
			VectorAdd (p->verts[0],temp,p->verts[1]);

			VectorScale (right, qi, temp);
			VectorAdd (p->verts[1],temp,p->verts[2]);

			VectorAdd (p->verts[0],temp,p->verts[3]);

			Sky_DrawFaceQuad (p);
		}
	}
	Hunk_FreeToLowMark (start);
}

/*
==============
Sky_DrawSkyLayers

draws the old-style scrolling cloud layers
==============
*/
void Sky_DrawSkyLayers (void)
{
	int i;

	if (r_skyalpha.value < 1.0)
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

	for (i=0 ; i<6 ; i++)
		if (skymins[0][i] < skymaxs[0][i] && skymins[1][i] < skymaxs[1][i])
			Sky_DrawFace (i);

	if (r_skyalpha.value < 1.0)
		glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
}

/*
==============
Sky_DrawSky

called once per frame before drawing anything else
==============
*/
void Sky_DrawSky (void)
{
	int i;

	//in these special render modes, the sky faces are handled in the normal world/brush renderer
	if (r_drawflat_cheatsafe || r_lightmap_cheatsafe)
		return;

	{	//same when the D3D9 world pass is drawing textured sky for Remix
		extern cvar_t d3d9_skytextured;
		if (D3D9_Active() && d3d9_skytextured.value)
			return;
	}

	/*
		The D3D9 skybox replaces everything below: it is drawn first, with depth
		test and depth write off, so the world simply paints over it. The sky
		brush surfaces themselves are not drawn at all under -d3d9 (r_world.c
		skips them and r_brush.c's DrawGLPoly is a no-op), which is what leaves
		the box showing through every sky opening.
	*/
	if (!skyroom_drawn && Sky_DrawRemixSky ())
		return;

	if (skyroom_drawn)
	{	//Spike: We already drew a skyroom underneath. If we draw an actual sky now then we'll have wasted all that effort.
		//however, if we fiddle with stuff, we can make sure that other surfaces don't draw over it either.

		int			i;
		msurface_t	*s;
		texture_t	*t;

		glColorMask(false,false,false,false);
		glDisable (GL_TEXTURE_2D);
		for (i=0 ; i<cl.worldmodel->numtextures ; i++)
		{
			t = cl.worldmodel->textures[i];

			if (!t || !t->texturechains[chain_world] || !(t->texturechains[chain_world]->flags & SURF_DRAWSKY))
				continue;

			for (s = t->texturechains[chain_world]; s; s = s->texturechain)
			{
				DrawGLPoly(s->polys);
				rs_brushpasses++;
				Sky_ProcessPoly (s->polys);
			}
		}
		glEnable (GL_TEXTURE_2D);
		glColorMask(true,true,true,true);
		return;
	}

	//
	// reset sky bounds
	//
	for (i=0 ; i<6 ; i++)
	{
		skymins[0][i] = skymins[1][i] = FLT_MAX;
		skymaxs[0][i] = skymaxs[1][i] = -FLT_MAX;
	}

	//
	// process world and bmodels: draw flat-shaded sky surfs, and update skybounds
	//
	Fog_DisableGFog ();
	glDisable (GL_TEXTURE_2D);
	if (Fog_GetDensity() > 0)
		glColor3fv (Fog_GetColor());
	else
		glColor3fv (skyflatcolor);
#ifndef SDL_THREADS_DISABLED
	if (skybox_name[0] && !r_fastsky.value && RSceneCache_DrawSkySurfDepth())
	{	//we have no surfaces to process... fill all sides. its probably still faster.
		for (i=0 ; i<6 ; i++)
		{
			skymins[0][i] = skymins[1][i] = -FLT_MAX;
			skymaxs[0][i] = skymaxs[1][i] = FLT_MAX;
		}
	}
	else
#endif
		Sky_ProcessTextureChains ();
	Sky_ProcessEntities ();
	glColor3f (1, 1, 1);
	glEnable (GL_TEXTURE_2D);

	//
	// render slow sky: cloud layers or skybox
	//
	if (!r_fastsky.value && !(Fog_GetDensity() > 0 && skyfog >= 1))
	{
		glDepthFunc(GL_GEQUAL);
		glDepthMask(0);

		if (skybox_name[0])
			Sky_DrawSkyBox ();
		else
			Sky_DrawSkyLayers ();

		glDepthMask(1);
		glDepthFunc(GL_LEQUAL);
	}

	Fog_EnableGFog ();
}
