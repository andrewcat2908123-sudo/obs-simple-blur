/*
 * obs-simple-blur v2
 *  - Video filter "Blur (Advanced)": add via Filters on any source/scene
 *  - Source "Blur Layer": separate layer that renders another source/scene
 *    blurred, with crop, shape, tint and frosted-glass options
 *
 * Blur types: Gaussian, Box, Bokeh (disc), Motion, Radial (spin), Zoom, Pixelate
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <math.h>
#include <string.h>

OBS_DECLARE_MODULE()

static const char *blur_effect_source =
	"// obs-simple-blur v2 shader. This exact text is embedded into src/plugin-main.c at build time.\n"
	"// Intermediate textures hold PREMULTIPLIED alpha; the source image holds straight alpha.\n"
	"\n"
	"#define TAPS 24\n"
	"#define DISC_TAPS 64\n"
	"#define ARC_TAPS 24\n"
	"#define ZOOM_TAPS 32\n"
	"\n"
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform texture2d image_orig;\n"
	"\n"
	"uniform float2 blur_dir;\n"
	"uniform float2 radius_uv;\n"
	"uniform float in_premult;\n"
	"uniform float2 src_size;\n"
	"uniform float2 blur_center;\n"
	"uniform float blur_amount;\n"
	"uniform float block_px;\n"
	"uniform float bokeh_amt;\n"
	"\n"
	"uniform float2 out_size;\n"
	"uniform float2 uv_off;\n"
	"uniform float2 uv_scale;\n"
	"uniform int shape;\n"
	"uniform float corner_px;\n"
	"uniform float feather_px;\n"
	"uniform float mix_amt;\n"
	"uniform float opacity_amt;\n"
	"uniform float3 tint_color;\n"
	"uniform float tint_amt;\n"
	"uniform float bright_amt;\n"
	"uniform float sat_amt;\n"
	"uniform float grain_amt;\n"
	"\n"
	"sampler_state linearClamp {\n"
	"	Filter    = Linear;\n"
	"	AddressU  = Clamp;\n"
	"	AddressV  = Clamp;\n"
	"};\n"
	"\n"
	"sampler_state pointClamp {\n"
	"	Filter    = Point;\n"
	"	AddressU  = Clamp;\n"
	"	AddressV  = Clamp;\n"
	"};\n"
	"\n"
	"struct VertData {\n"
	"	float4 pos : POSITION;\n"
	"	float2 uv  : TEXCOORD0;\n"
	"};\n"
	"\n"
	"VertData VSDefault(VertData v_in)\n"
	"{\n"
	"	VertData vert_out;\n"
	"	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n"
	"	vert_out.uv  = v_in.uv;\n"
	"	return vert_out;\n"
	"}\n"
	"\n"
	"float4 fetch(float2 coord)\n"
	"{\n"
	"	float4 c = image.Sample(linearClamp, coord);\n"
	"	if (in_premult < 0.5)\n"
	"		c.rgb *= c.a;\n"
	"	return c;\n"
	"}\n"
	"\n"
	"float4 PSCopy(VertData v_in) : TARGET\n"
	"{\n"
	"	return image.Sample(linearClamp, v_in.uv);\n"
	"}\n"
	"\n"
	"// Gaussian, separable (direction in blur_dir)\n"
	"float4 PSGauss(VertData v_in) : TARGET\n"
	"{\n"
	"	float2 span = blur_dir * radius_uv;\n"
	"	float4 sum = fetch(v_in.uv);\n"
	"	float wsum = 1.0;\n"
	"	for (int i = 1; i <= TAPS; i++) {\n"
	"		float t = (float)i / (float)TAPS;\n"
	"		float w = exp(-t * t * 4.5);\n"
	"		float2 o = span * t;\n"
	"		sum += (fetch(v_in.uv + o) + fetch(v_in.uv - o)) * w;\n"
	"		wsum += 2.0 * w;\n"
	"	}\n"
	"	return sum / wsum;\n"
	"}\n"
	"\n"
	"// Box / motion blur: uniform weights along blur_dir\n"
	"float4 PSBox(VertData v_in) : TARGET\n"
	"{\n"
	"	float2 span = blur_dir * radius_uv;\n"
	"	float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
	"	for (int i = -TAPS; i <= TAPS; i++) {\n"
	"		float t = (float)i / (float)TAPS;\n"
	"		sum += fetch(v_in.uv + span * t);\n"
	"	}\n"
	"	return sum / (float)(2 * TAPS + 1);\n"
	"}\n"
	"\n"
	"// Bokeh: disc of samples on a golden-angle spiral, bright spots get extra weight\n"
	"float4 PSDisc(VertData v_in) : TARGET\n"
	"{\n"
	"	float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
	"	float wsum = 0.0;\n"
	"	for (int i = 0; i < DISC_TAPS; i++) {\n"
	"		float t = ((float)i + 0.5) / (float)DISC_TAPS;\n"
	"		float rr = sqrt(t);\n"
	"		float a = (float)i * 2.39996323;\n"
	"		float2 o = float2(cos(a), sin(a)) * rr * radius_uv;\n"
	"		float4 c = fetch(v_in.uv + o);\n"
	"		float lum = dot(c.rgb, float3(0.299, 0.587, 0.114));\n"
	"		float w = 1.0 + bokeh_amt * lum * lum * 8.0;\n"
	"		sum += c * w;\n"
	"		wsum += w;\n"
	"	}\n"
	"	return sum / wsum;\n"
	"}\n"
	"\n"
	"// Radial (spin) blur around blur_center, blur_amount = half-angle in radians\n"
	"float4 PSSpin(VertData v_in) : TARGET\n"
	"{\n"
	"	float2 p = (v_in.uv - blur_center) * src_size;\n"
	"	float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
	"	for (int i = -ARC_TAPS; i <= ARC_TAPS; i++) {\n"
	"		float a = blur_amount * (float)i / (float)ARC_TAPS;\n"
	"		float s = sin(a);\n"
	"		float c = cos(a);\n"
	"		float2 q = float2(c * p.x - s * p.y, s * p.x + c * p.y);\n"
	"		sum += fetch(blur_center + q / src_size);\n"
	"	}\n"
	"	return sum / (float)(2 * ARC_TAPS + 1);\n"
	"}\n"
	"\n"
	"// Zoom blur towards blur_center, blur_amount = 0..1\n"
	"float4 PSZoom(VertData v_in) : TARGET\n"
	"{\n"
	"	float2 d = v_in.uv - blur_center;\n"
	"	float4 sum = float4(0.0, 0.0, 0.0, 0.0);\n"
	"	for (int i = 0; i <= ZOOM_TAPS; i++) {\n"
	"		float t = (float)i / (float)ZOOM_TAPS;\n"
	"		sum += fetch(blur_center + d * (1.0 - blur_amount * t));\n"
	"	}\n"
	"	return sum / (float)(ZOOM_TAPS + 1);\n"
	"}\n"
	"\n"
	"// Pixelate / mosaic\n"
	"float4 PSPixel(VertData v_in) : TARGET\n"
	"{\n"
	"	float2 cell = float2(block_px, block_px) / src_size;\n"
	"	float2 suv = (floor(v_in.uv / cell) + float2(0.5, 0.5)) * cell;\n"
	"	float4 c = image.Sample(pointClamp, suv);\n"
	"	if (in_premult < 0.5)\n"
	"		c.rgb *= c.a;\n"
	"	return c;\n"
	"}\n"
	"\n"
	"// Final composite: crop, mix with original, colour, grain, shape mask\n"
	"float4 PSFinal(VertData v_in) : TARGET\n"
	"{\n"
	"	float2 suv = uv_off + v_in.uv * uv_scale;\n"
	"\n"
	"	float4 b = image.Sample(linearClamp, suv);\n"
	"	if (in_premult < 0.5)\n"
	"		b.rgb *= b.a;\n"
	"	float4 o = image_orig.Sample(linearClamp, suv);\n"
	"	o.rgb *= o.a;\n"
	"\n"
	"	float4 c = lerp(o, b, mix_amt);\n"
	"	float a = c.a;\n"
	"	float3 rgb = c.rgb / max(a, 0.00001);\n"
	"\n"
	"	rgb *= bright_amt;\n"
	"	float lum = dot(rgb, float3(0.299, 0.587, 0.114));\n"
	"	rgb = lerp(float3(lum, lum, lum), rgb, sat_amt);\n"
	"	rgb = lerp(rgb, tint_color, tint_amt);\n"
	"\n"
	"	float n = frac(sin(dot(v_in.uv * out_size, float2(12.9898, 78.233))) * 43758.5453) - 0.5;\n"
	"	rgb += n * grain_amt;\n"
	"\n"
	"	float2 p = (v_in.uv - float2(0.5, 0.5)) * out_size;\n"
	"	float2 hb = out_size * 0.5;\n"
	"	float d;\n"
	"	if (shape == 2) {\n"
	"		float2 e = p / hb;\n"
	"		d = (length(e) - 1.0) * min(hb.x, hb.y);\n"
	"	} else {\n"
	"		float r = 0.0;\n"
	"		if (shape == 1)\n"
	"			r = min(corner_px, min(hb.x, hb.y));\n"
	"		float2 q = abs(p) - hb + float2(r, r);\n"
	"		d = length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - r;\n"
	"	}\n"
	"	float mask = saturate((0.5 - d) / max(feather_px, 1.0));\n"
	"\n"
	"	return float4(saturate(rgb), saturate(a * mask * opacity_amt));\n"
	"}\n"
	"\n"
	"technique Copy\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSCopy(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Gauss\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSGauss(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Box\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSBox(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Disc\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSDisc(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Spin\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSSpin(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Zoom\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSZoom(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Pixel\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSPixel(v_in);\n"
	"	}\n"
	"}\n"
	"\n"
	"technique Final\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSFinal(v_in);\n"
	"	}\n"
	"}\n"
	"\n";


/* ------------------------------------------------------------------ */
/* Text (English / Russian, no external locale files needed)           */
/* ------------------------------------------------------------------ */

struct tr_entry {
	const char *key;
	const char *en;
	const char *ru;
};

static const struct tr_entry tr_table[] = {
	{"BlurFilter", "Blur (Advanced)", "\320\240\320\260\320\267\320\274\321\213\321\202\320\270\320\265 (Advanced)"},
	{"BlurLayer", "Blur Layer", "\320\241\320\273\320\276\320\271 \321\200\320\260\320\267\320\274\321\213\321\202\320\270\321\217"},
	{"Source", "Source to blur", "\320\230\321\201\321\202\320\276\321\207\320\275\320\270\320\272 \320\264\320\273\321\217 \321\200\320\260\320\267\320\274\321\213\321\202\320\270\321\217"},
	{"CropL", "Crop left (px)", "\320\236\320\261\321\200\320\265\320\267\320\272\320\260 \321\201\320\273\320\265\320\262\320\260 (px)"},
	{"CropR", "Crop right (px)", "\320\236\320\261\321\200\320\265\320\267\320\272\320\260 \321\201\320\277\321\200\320\260\320\262\320\260 (px)"},
	{"CropT", "Crop top (px)", "\320\236\320\261\321\200\320\265\320\267\320\272\320\260 \321\201\320\262\320\265\321\200\321\205\321\203 (px)"},
	{"CropB", "Crop bottom (px)", "\320\236\320\261\321\200\320\265\320\267\320\272\320\260 \321\201\320\275\320\270\320\267\321\203 (px)"},
	{"Type", "Blur type", "\320\242\320\270\320\277 \321\200\320\260\320\267\320\274\321\213\321\202\320\270\321\217"},
	{"T.Gauss", "Gaussian (smooth)", "\320\223\320\260\321\203\321\201\321\201\320\276\320\262\320\276 (\320\274\321\217\320\263\320\272\320\276\320\265)"},
	{"T.Box", "Box (flat)", "\320\237\321\200\321\217\320\274\320\276\321\203\320\263\320\276\320\273\321\214\320\275\320\276\320\265 (Box)"},
	{"T.Disc", "Bokeh (lens disc)", "\320\221\320\276\320\272\320\265 (\320\273\320\270\320\275\320\267\320\260)"},
	{"T.Motion", "Motion (directional)", "\320\224\320\262\320\270\320\266\320\265\320\275\320\270\320\265 (\320\275\320\260\320\277\321\200\320\260\320\262\320\273\320\265\320\275\320\275\320\276\320\265)"},
	{"T.Spin", "Radial (spin)", "\320\240\320\260\320\264\320\270\320\260\320\273\321\214\320\275\320\276\320\265 (\320\262\321\200\320\260\321\211\320\265\320\275\320\270\320\265)"},
	{"T.Zoom", "Zoom", "\320\227\321\203\320\274 (\320\276\321\202 \321\206\320\265\320\275\321\202\321\200\320\260)"},
	{"T.Pixel", "Pixelate (mosaic)", "\320\237\320\270\320\272\321\201\320\265\320\273\320\270\320\267\320\260\321\206\320\270\321\217 (\320\274\320\276\320\267\320\260\320\270\320\272\320\260)"},
	{"Radius", "Radius / distance (px)", "\320\240\320\260\320\264\320\270\321\203\321\201 / \320\264\320\270\321\201\321\202\320\260\320\275\321\206\320\270\321\217 (px)"},
	{"Passes", "Smoothness (passes)", "\320\223\320\273\320\260\320\264\320\272\320\276\321\201\321\202\321\214 (\320\277\321\200\320\276\321\205\320\276\320\264\321\213)"},
	{"Down", "Quality / speed", "\320\232\320\260\321\207\320\265\321\201\321\202\320\262\320\276 / \321\201\320\272\320\276\321\200\320\276\321\201\321\202\321\214"},
	{"D.1", "Best (1x)", "\320\233\321\203\321\207\321\210\320\265\320\265 (1x)"},
	{"D.2", "Fast (2x)", "\320\221\321\213\321\201\321\202\321\200\320\276 (2x)"},
	{"D.4", "Faster (4x)", "\320\221\321\213\321\201\321\202\321\200\320\265\320\265 (4x)"},
	{"D.8", "Fastest (8x)", "\320\234\320\260\320\272\321\201\320\270\320\274\321\203\320\274 \321\201\320\272\320\276\321\200\320\276\321\201\321\202\320\270 (8x)"},
	{"Angle", "Direction (deg)", "\320\235\320\260\320\277\321\200\320\260\320\262\320\273\320\265\320\275\320\270\320\265 (\320\263\321\200\320\260\320\264)"},
	{"Spin", "Spin amount (deg)", "\320\243\320\263\320\276\320\273 \320\262\321\200\320\260\321\211\320\265\320\275\320\270\321\217 (\320\263\321\200\320\260\320\264)"},
	{"Zoom", "Zoom strength (%)", "\320\241\320\270\320\273\320\260 \320\267\321\203\320\274\320\260 (%)"},
	{"CenterX", "Center X (%)", "\320\246\320\265\320\275\321\202\321\200 X (%)"},
	{"CenterY", "Center Y (%)", "\320\246\320\265\320\275\321\202\321\200 Y (%)"},
	{"Block", "Block size (px)", "\320\240\320\260\320\267\320\274\320\265\321\200 \320\261\320\273\320\276\320\272\320\260 (px)"},
	{"Bokeh", "Highlight boost (%)", "\320\243\321\201\320\270\320\273\320\265\320\275\320\270\320\265 \320\261\320\273\320\270\320\272\320\276\320\262 (%)"},
	{"Mix", "Blur amount (%)", "\320\224\320\276\320\273\321\217 \321\200\320\260\320\267\320\274\321\213\321\202\320\270\321\217 (%)"},
	{"Opacity", "Opacity (%)", "\320\235\320\265\320\277\321\200\320\276\320\267\321\200\320\260\321\207\320\275\320\276\321\201\321\202\321\214 (%)"},
	{"Bright", "Brightness (%)", "\320\257\321\200\320\272\320\276\321\201\321\202\321\214 (%)"},
	{"Sat", "Saturation (%)", "\320\235\320\260\321\201\321\213\321\211\320\265\320\275\320\275\320\276\321\201\321\202\321\214 (%)"},
	{"Tint", "Tint color", "\320\246\320\262\320\265\321\202 \320\277\320\276\320\264\320\272\321\200\320\260\321\201\320\272\320\270"},
	{"TintAmt", "Tint amount (%)", "\320\241\320\270\320\273\320\260 \320\277\320\276\320\264\320\272\321\200\320\260\321\201\320\272\320\270 (%)"},
	{"Grain", "Frost grain (%)", "\320\227\320\265\321\200\320\275\320\276 (\320\274\320\276\321\200\320\276\320\267\320\275\320\276\320\265 \321\201\321\202\320\265\320\272\320\273\320\276) (%)"},
	{"Shape", "Shape", "\320\244\320\276\321\200\320\274\320\260"},
	{"S.Rect", "Rectangle", "\320\237\321\200\321\217\320\274\320\276\321\203\320\263\320\276\320\273\321\214\320\275\320\270\320\272"},
	{"S.Round", "Rounded rectangle", "\320\241\320\272\321\200\321\203\320\263\320\273\321\221\320\275\320\275\321\213\320\271 \320\277\321\200\321\217\320\274\320\276\321\203\320\263\320\276\320\273\321\214\320\275\320\270\320\272"},
	{"S.Ellipse", "Ellipse", "\320\255\320\273\320\273\320\270\320\277\321\201"},
	{"Corner", "Corner radius (px)", "\320\240\320\260\320\264\320\270\321\203\321\201 \321\201\320\272\321\200\321\203\320\263\320\273\320\265\320\275\320\270\321\217 (px)"},
	{"Feather", "Edge softness (px)", "\320\234\321\217\320\263\320\272\320\276\321\201\321\202\321\214 \320\272\321\200\320\260\321\217 (px)"},
};

static const char *tr(const char *key)
{
	const char *loc = obs_get_locale();
	const bool ru = loc && loc[0] == 'r' && loc[1] == 'u';
	for (size_t i = 0; i < sizeof(tr_table) / sizeof(tr_table[0]); i++) {
		if (strcmp(tr_table[i].key, key) == 0)
			return ru ? tr_table[i].ru : tr_table[i].en;
	}
	return key;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

#define S_SOURCE "source"
#define S_TYPE "blur_type"
#define S_RADIUS "radius"
#define S_PASSES "passes"
#define S_DOWNSAMPLE "downsample"
#define S_ANGLE "angle"
#define S_SPIN "spin"
#define S_ZOOM "zoom"
#define S_CENTER_X "center_x"
#define S_CENTER_Y "center_y"
#define S_BLOCK "block"
#define S_BOKEH "bokeh"
#define S_MIX "mix"
#define S_OPACITY "opacity"
#define S_BRIGHT "brightness"
#define S_SAT "saturation"
#define S_TINT "tint"
#define S_TINT_AMT "tint_amount"
#define S_GRAIN "grain"
#define S_SHAPE "shape"
#define S_CORNER "corner"
#define S_FEATHER "feather"
#define S_CROP_L "crop_left"
#define S_CROP_R "crop_right"
#define S_CROP_T "crop_top"
#define S_CROP_B "crop_bottom"

enum blur_type {
	BLUR_GAUSS = 0,
	BLUR_BOX,
	BLUR_DISC,
	BLUR_MOTION,
	BLUR_SPIN,
	BLUR_ZOOM,
	BLUR_PIXEL,
};

struct blur_params {
	int type;
	float radius;
	int passes;
	int downsample;
	float angle;
	float spin;
	float zoom;
	float cx, cy;
	float block;
	float bokeh;
	float mix;
	float opacity;
	float brightness;
	float saturation;
	float tint_amt;
	float grain;
	struct vec3 tint;
	int shape;
	float corner;
	float feather;
	uint32_t crop_l, crop_r, crop_t, crop_b;
};

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void read_params(obs_data_t *s, struct blur_params *p)
{
	const float deg = 3.14159265f / 180.0f;

	p->type = clampi((int)obs_data_get_int(s, S_TYPE), 0, BLUR_PIXEL);
	p->radius = (float)obs_data_get_double(s, S_RADIUS);
	p->passes = clampi((int)obs_data_get_int(s, S_PASSES), 1, 8);
	p->downsample = 1 << clampi((int)obs_data_get_int(s, S_DOWNSAMPLE), 0, 3);
	p->angle = (float)obs_data_get_double(s, S_ANGLE) * deg;
	p->spin = (float)obs_data_get_double(s, S_SPIN) * deg;
	p->zoom = (float)obs_data_get_double(s, S_ZOOM) / 100.0f;
	p->cx = (float)obs_data_get_double(s, S_CENTER_X) / 100.0f;
	p->cy = (float)obs_data_get_double(s, S_CENTER_Y) / 100.0f;
	p->block = (float)obs_data_get_int(s, S_BLOCK);
	p->bokeh = (float)obs_data_get_double(s, S_BOKEH) / 100.0f;
	p->mix = (float)obs_data_get_int(s, S_MIX) / 100.0f;
	p->opacity = (float)obs_data_get_int(s, S_OPACITY) / 100.0f;
	p->brightness = (float)obs_data_get_int(s, S_BRIGHT) / 100.0f;
	p->saturation = (float)obs_data_get_int(s, S_SAT) / 100.0f;
	p->tint_amt = (float)obs_data_get_int(s, S_TINT_AMT) / 100.0f;
	p->grain = (float)obs_data_get_int(s, S_GRAIN) / 100.0f * 0.25f;

	long long tc = obs_data_get_int(s, S_TINT);
	vec3_set(&p->tint, (float)(tc & 0xFF) / 255.0f, (float)((tc >> 8) & 0xFF) / 255.0f,
		 (float)((tc >> 16) & 0xFF) / 255.0f);

	p->shape = clampi((int)obs_data_get_int(s, S_SHAPE), 0, 2);
	p->corner = (float)obs_data_get_int(s, S_CORNER);
	p->feather = (float)obs_data_get_int(s, S_FEATHER);

	p->crop_l = (uint32_t)clampi((int)obs_data_get_int(s, S_CROP_L), 0, 16384);
	p->crop_r = (uint32_t)clampi((int)obs_data_get_int(s, S_CROP_R), 0, 16384);
	p->crop_t = (uint32_t)clampi((int)obs_data_get_int(s, S_CROP_T), 0, 16384);
	p->crop_b = (uint32_t)clampi((int)obs_data_get_int(s, S_CROP_B), 0, 16384);
}

static void common_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, S_TYPE, BLUR_GAUSS);
	obs_data_set_default_double(s, S_RADIUS, 24.0);
	obs_data_set_default_int(s, S_PASSES, 2);
	obs_data_set_default_int(s, S_DOWNSAMPLE, 0);
	obs_data_set_default_double(s, S_ANGLE, 0.0);
	obs_data_set_default_double(s, S_SPIN, 15.0);
	obs_data_set_default_double(s, S_ZOOM, 25.0);
	obs_data_set_default_double(s, S_CENTER_X, 50.0);
	obs_data_set_default_double(s, S_CENTER_Y, 50.0);
	obs_data_set_default_int(s, S_BLOCK, 16);
	obs_data_set_default_double(s, S_BOKEH, 40.0);
	obs_data_set_default_int(s, S_MIX, 100);
	obs_data_set_default_int(s, S_OPACITY, 100);
	obs_data_set_default_int(s, S_BRIGHT, 100);
	obs_data_set_default_int(s, S_SAT, 100);
	obs_data_set_default_int(s, S_TINT, 0xFFFFFF);
	obs_data_set_default_int(s, S_TINT_AMT, 0);
	obs_data_set_default_int(s, S_GRAIN, 0);
	obs_data_set_default_int(s, S_SHAPE, 0);
	obs_data_set_default_int(s, S_CORNER, 24);
	obs_data_set_default_int(s, S_FEATHER, 1);
}

static void layer_defaults(obs_data_t *s)
{
	common_defaults(s);
	obs_data_set_default_int(s, S_SHAPE, 1);
	obs_data_set_default_int(s, S_TINT_AMT, 8);
}

/* ------------------------------------------------------------------ */
/* Properties UI                                                       */
/* ------------------------------------------------------------------ */

static void set_vis(obs_properties_t *props, const char *name, bool vis)
{
	obs_property_t *p = obs_properties_get(props, name);
	if (p)
		obs_property_set_visible(p, vis);
}

static bool type_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	UNUSED_PARAMETER(prop);
	const int t = (int)obs_data_get_int(s, S_TYPE);

	set_vis(props, S_RADIUS, t == BLUR_GAUSS || t == BLUR_BOX || t == BLUR_DISC || t == BLUR_MOTION);
	set_vis(props, S_PASSES, t == BLUR_GAUSS || t == BLUR_BOX);
	set_vis(props, S_DOWNSAMPLE, t != BLUR_PIXEL);
	set_vis(props, S_ANGLE, t == BLUR_MOTION);
	set_vis(props, S_SPIN, t == BLUR_SPIN);
	set_vis(props, S_ZOOM, t == BLUR_ZOOM);
	set_vis(props, S_CENTER_X, t == BLUR_SPIN || t == BLUR_ZOOM);
	set_vis(props, S_CENTER_Y, t == BLUR_SPIN || t == BLUR_ZOOM);
	set_vis(props, S_BLOCK, t == BLUR_PIXEL);
	set_vis(props, S_BOKEH, t == BLUR_DISC);
	return true;
}

static bool shape_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	UNUSED_PARAMETER(prop);
	set_vis(props, S_CORNER, obs_data_get_int(s, S_SHAPE) == 1);
	return true;
}

struct enum_ctx {
	obs_property_t *list;
	obs_source_t *self;
};

static bool enum_cb(void *param, obs_source_t *src)
{
	struct enum_ctx *ctx = param;
	if (src == ctx->self)
		return true;
	if (!(obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO))
		return true;
	const char *name = obs_source_get_name(src);
	if (name && *name)
		obs_property_list_add_string(ctx->list, name, name);
	return true;
}

static obs_properties_t *build_properties(obs_source_t *self, bool layer)
{
	obs_properties_t *props = obs_properties_create();

	if (layer) {
		obs_property_t *list = obs_properties_add_list(props, S_SOURCE, tr("Source"), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(list, "", "");
		struct enum_ctx ctx = {.list = list, .self = self};
		obs_enum_scenes(enum_cb, &ctx);
		obs_enum_sources(enum_cb, &ctx);

		obs_properties_add_int(props, S_CROP_L, tr("CropL"), 0, 16384, 1);
		obs_properties_add_int(props, S_CROP_R, tr("CropR"), 0, 16384, 1);
		obs_properties_add_int(props, S_CROP_T, tr("CropT"), 0, 16384, 1);
		obs_properties_add_int(props, S_CROP_B, tr("CropB"), 0, 16384, 1);
	}

	obs_property_t *type = obs_properties_add_list(props, S_TYPE, tr("Type"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, tr("T.Gauss"), BLUR_GAUSS);
	obs_property_list_add_int(type, tr("T.Box"), BLUR_BOX);
	obs_property_list_add_int(type, tr("T.Disc"), BLUR_DISC);
	obs_property_list_add_int(type, tr("T.Motion"), BLUR_MOTION);
	obs_property_list_add_int(type, tr("T.Spin"), BLUR_SPIN);
	obs_property_list_add_int(type, tr("T.Zoom"), BLUR_ZOOM);
	obs_property_list_add_int(type, tr("T.Pixel"), BLUR_PIXEL);
	obs_property_set_modified_callback(type, type_modified);

	obs_properties_add_float_slider(props, S_RADIUS, tr("Radius"), 0.0, 300.0, 0.5);
	obs_properties_add_int_slider(props, S_PASSES, tr("Passes"), 1, 8, 1);
	obs_properties_add_float_slider(props, S_ANGLE, tr("Angle"), 0.0, 360.0, 1.0);
	obs_properties_add_float_slider(props, S_SPIN, tr("Spin"), 0.0, 90.0, 0.5);
	obs_properties_add_float_slider(props, S_ZOOM, tr("Zoom"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, S_CENTER_X, tr("CenterX"), 0.0, 100.0, 0.5);
	obs_properties_add_float_slider(props, S_CENTER_Y, tr("CenterY"), 0.0, 100.0, 0.5);
	obs_properties_add_int_slider(props, S_BLOCK, tr("Block"), 2, 200, 1);
	obs_properties_add_float_slider(props, S_BOKEH, tr("Bokeh"), 0.0, 100.0, 1.0);

	obs_property_t *down = obs_properties_add_list(props, S_DOWNSAMPLE, tr("Down"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(down, tr("D.1"), 0);
	obs_property_list_add_int(down, tr("D.2"), 1);
	obs_property_list_add_int(down, tr("D.4"), 2);
	obs_property_list_add_int(down, tr("D.8"), 3);

	obs_properties_add_int_slider(props, S_MIX, tr("Mix"), 0, 100, 1);
	obs_properties_add_int_slider(props, S_OPACITY, tr("Opacity"), 0, 100, 1);
	obs_properties_add_int_slider(props, S_BRIGHT, tr("Bright"), 0, 200, 1);
	obs_properties_add_int_slider(props, S_SAT, tr("Sat"), 0, 300, 1);
	obs_properties_add_color(props, S_TINT, tr("Tint"));
	obs_properties_add_int_slider(props, S_TINT_AMT, tr("TintAmt"), 0, 100, 1);
	obs_properties_add_int_slider(props, S_GRAIN, tr("Grain"), 0, 100, 1);

	obs_property_t *shape = obs_properties_add_list(props, S_SHAPE, tr("Shape"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(shape, tr("S.Rect"), 0);
	obs_property_list_add_int(shape, tr("S.Round"), 1);
	obs_property_list_add_int(shape, tr("S.Ellipse"), 2);
	obs_property_set_modified_callback(shape, shape_modified);
	obs_properties_add_int_slider(props, S_CORNER, tr("Corner"), 0, 1000, 1);
	obs_properties_add_int_slider(props, S_FEATHER, tr("Feather"), 1, 200, 1);

	/* apply initial visibility from current settings */
	obs_data_t *cur = self ? obs_source_get_settings(self) : NULL;
	if (cur) {
		type_modified(props, NULL, cur);
		shape_modified(props, NULL, cur);
		obs_data_release(cur);
	}
	return props;
}

/* ------------------------------------------------------------------ */
/* Blur core                                                           */
/* ------------------------------------------------------------------ */

struct blur_core {
	gs_effect_t *effect;
	gs_eparam_t *p_image, *p_orig, *p_dir, *p_radius_uv, *p_premult, *p_size, *p_center, *p_amount, *p_block,
		*p_bokeh, *p_out_size, *p_uv_off, *p_uv_scale, *p_shape, *p_corner, *p_feather, *p_mix, *p_opacity,
		*p_tint, *p_tint_amt, *p_bright, *p_sat, *p_grain;
	gs_texrender_t *tex_src;
	gs_texrender_t *tex_a;
	gs_texrender_t *tex_b;
};

static bool core_init(struct blur_core *c)
{
	char *err = NULL;
	bool ok = true;

	obs_enter_graphics();
	c->effect = gs_effect_create(blur_effect_source, "blur.effect", &err);
	if (!c->effect) {
		blog(LOG_ERROR, "[obs-simple-blur] failed to compile effect: %s", err ? err : "unknown error");
		ok = false;
	} else {
#define GET_PARAM(field, name)                                                                         \
	do {                                                                                           \
		c->field = gs_effect_get_param_by_name(c->effect, name);                               \
		if (!c->field) {                                                                       \
			blog(LOG_ERROR, "[obs-simple-blur] shader parameter '%s' not found", name);    \
			ok = false;                                                                    \
		}                                                                                      \
	} while (0)
		GET_PARAM(p_image, "image");
		GET_PARAM(p_orig, "image_orig");
		GET_PARAM(p_dir, "blur_dir");
		GET_PARAM(p_radius_uv, "radius_uv");
		GET_PARAM(p_premult, "in_premult");
		GET_PARAM(p_size, "src_size");
		GET_PARAM(p_center, "blur_center");
		GET_PARAM(p_amount, "blur_amount");
		GET_PARAM(p_block, "block_px");
		GET_PARAM(p_bokeh, "bokeh_amt");
		GET_PARAM(p_out_size, "out_size");
		GET_PARAM(p_uv_off, "uv_off");
		GET_PARAM(p_uv_scale, "uv_scale");
		GET_PARAM(p_shape, "shape");
		GET_PARAM(p_corner, "corner_px");
		GET_PARAM(p_feather, "feather_px");
		GET_PARAM(p_mix, "mix_amt");
		GET_PARAM(p_opacity, "opacity_amt");
		GET_PARAM(p_tint, "tint_color");
		GET_PARAM(p_tint_amt, "tint_amt");
		GET_PARAM(p_bright, "bright_amt");
		GET_PARAM(p_sat, "sat_amt");
		GET_PARAM(p_grain, "grain_amt");
#undef GET_PARAM
	}

	if (ok) {
		c->tex_src = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		c->tex_a = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		c->tex_b = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	}
	obs_leave_graphics();

	bfree(err);
	return ok;
}

static void core_destroy(struct blur_core *c)
{
	obs_enter_graphics();
	if (c->effect)
		gs_effect_destroy(c->effect);
	if (c->tex_src)
		gs_texrender_destroy(c->tex_src);
	if (c->tex_a)
		gs_texrender_destroy(c->tex_a);
	if (c->tex_b)
		gs_texrender_destroy(c->tex_b);
	obs_leave_graphics();
	memset(c, 0, sizeof(*c));
}

static void set_vec2(gs_eparam_t *param, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(param, &v);
}

/* Render `src` through technique `tech` into `dst` (size w x h). */
static gs_texture_t *render_pass(struct blur_core *c, gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech,
				 gs_texture_t *src)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return NULL;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_set_texture(c->p_image, src);
	while (gs_effect_loop(c->effect, tech))
		gs_draw_sprite(src, 0, w, h);
	gs_blend_state_pop();

	gs_texrender_end(dst);
	return gs_texrender_get_texture(dst);
}

/*
 * Blur stage. `src` is the straight-alpha source image (w x h).
 * Returns the texture holding the blurred result and sets *premult to 1 if that
 * texture holds premultiplied alpha (0 if it is simply `src`).
 */
static gs_texture_t *blur_stage(struct blur_core *c, const struct blur_params *p, gs_texture_t *src, uint32_t w,
				uint32_t h, float *premult)
{
	const float fw = (float)w;
	const float fh = (float)h;
	const bool pixel = p->type == BLUR_PIXEL;
	const uint32_t ds = pixel ? 1 : (uint32_t)p->downsample;
	uint32_t bw = w / ds;
	uint32_t bh = h / ds;
	if (bw < 1)
		bw = 1;
	if (bh < 1)
		bh = 1;

	*premult = 0.0f;

	set_vec2(c->p_size, fw, fh);
	set_vec2(c->p_center, p->cx, p->cy);
	gs_effect_set_float(c->p_block, p->block);
	gs_effect_set_float(c->p_bokeh, p->bokeh);
	gs_effect_set_float(c->p_amount, 0.0f);
	gs_effect_set_float(c->p_premult, 0.0f);
	set_vec2(c->p_dir, 1.0f, 0.0f);
	set_vec2(c->p_radius_uv, 0.0f, 0.0f);

	gs_texture_t *out = NULL;

	switch (p->type) {
	case BLUR_GAUSS:
	case BLUR_BOX: {
		if (p->radius < 0.5f)
			return src;
		const char *tech = p->type == BLUR_GAUSS ? "Gauss" : "Box";
		const float r = p->radius / sqrtf((float)p->passes);
		set_vec2(c->p_radius_uv, r / fw, r / fh);

		gs_texture_t *cur = src;
		float pm = 0.0f;
		for (int i = 0; i < p->passes; i++) {
			gs_effect_set_float(c->p_premult, pm);
			set_vec2(c->p_dir, 1.0f, 0.0f);
			gs_texture_t *t1 = render_pass(c, c->tex_a, bw, bh, tech, cur);
			if (!t1)
				return src;

			gs_effect_set_float(c->p_premult, 1.0f);
			set_vec2(c->p_dir, 0.0f, 1.0f);
			gs_texture_t *t2 = render_pass(c, c->tex_b, bw, bh, tech, t1);
			if (!t2)
				return src;

			cur = t2;
			pm = 1.0f;
		}
		*premult = 1.0f;
		return cur;
	}
	case BLUR_DISC:
		if (p->radius < 0.5f)
			return src;
		set_vec2(c->p_radius_uv, p->radius / fw, p->radius / fh);
		out = render_pass(c, c->tex_b, bw, bh, "Disc", src);
		break;
	case BLUR_MOTION:
		if (p->radius < 0.5f)
			return src;
		set_vec2(c->p_radius_uv, p->radius / fw, p->radius / fh);
		set_vec2(c->p_dir, cosf(p->angle), sinf(p->angle));
		out = render_pass(c, c->tex_b, bw, bh, "Box", src);
		break;
	case BLUR_SPIN:
		if (p->spin < 0.001f)
			return src;
		gs_effect_set_float(c->p_amount, p->spin);
		out = render_pass(c, c->tex_b, bw, bh, "Spin", src);
		break;
	case BLUR_ZOOM:
		if (p->zoom < 0.001f)
			return src;
		gs_effect_set_float(c->p_amount, p->zoom);
		out = render_pass(c, c->tex_b, bw, bh, "Zoom", src);
		break;
	case BLUR_PIXEL:
		if (p->block < 2.0f)
			return src;
		out = render_pass(c, c->tex_b, w, h, "Pixel", src);
		break;
	default:
		return src;
	}

	if (!out)
		return src;
	*premult = 1.0f;
	return out;
}

/* Crop rectangle inside a w x h source (w, h >= 1). */
static void crop_rect(const struct blur_params *p, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y, uint32_t *ow,
		      uint32_t *oh)
{
	uint32_t l = p->crop_l >= w ? w - 1 : p->crop_l;
	uint32_t r = p->crop_r;
	if (l + r >= w)
		r = w - 1 - l;
	uint32_t t = p->crop_t >= h ? h - 1 : p->crop_t;
	uint32_t b = p->crop_b;
	if (t + b >= h)
		b = h - 1 - t;

	*x = l;
	*y = t;
	*ow = w - l - r;
	*oh = h - t - b;
}

/* Final composite into the currently bound render target. */
static void blur_final(struct blur_core *c, const struct blur_params *p, gs_texture_t *blurred, float premult,
		       gs_texture_t *src, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0, uint32_t ow, uint32_t oh)
{
	const float fw = (float)w;
	const float fh = (float)h;

	gs_effect_set_float(c->p_premult, premult);
	set_vec2(c->p_out_size, (float)ow, (float)oh);
	set_vec2(c->p_uv_off, (float)x0 / fw, (float)y0 / fh);
	set_vec2(c->p_uv_scale, (float)ow / fw, (float)oh / fh);
	gs_effect_set_int(c->p_shape, p->shape);
	gs_effect_set_float(c->p_corner, p->corner);
	gs_effect_set_float(c->p_feather, p->feather);
	gs_effect_set_float(c->p_mix, p->mix);
	gs_effect_set_float(c->p_opacity, p->opacity);
	gs_effect_set_vec3(c->p_tint, &p->tint);
	gs_effect_set_float(c->p_tint_amt, p->tint_amt);
	gs_effect_set_float(c->p_bright, p->brightness);
	gs_effect_set_float(c->p_sat, p->saturation);
	gs_effect_set_float(c->p_grain, p->grain);
	gs_effect_set_texture(c->p_image, blurred);
	gs_effect_set_texture(c->p_orig, src);

	while (gs_effect_loop(c->effect, "Final"))
		gs_draw_sprite(blurred, 0, ow, oh);
}

/* ------------------------------------------------------------------ */
/* Filter                                                              */
/* ------------------------------------------------------------------ */

struct blur_filter {
	obs_source_t *source;
	struct blur_core core;
	struct blur_params p;
};

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return tr("BlurFilter");
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct blur_filter *f = data;
	read_params(settings, &f->p);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct blur_filter *f = bzalloc(sizeof(*f));
	f->source = source;
	if (!core_init(&f->core)) {
		core_destroy(&f->core);
		bfree(f);
		return NULL;
	}
	filter_update(f, settings);
	return f;
}

static void filter_destroy(void *data)
{
	struct blur_filter *f = data;
	core_destroy(&f->core);
	bfree(f);
}

static obs_properties_t *filter_properties(void *data)
{
	struct blur_filter *f = data;
	return build_properties(f->source, false);
}

static void filter_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct blur_filter *f = data;
	struct blur_core *c = &f->core;

	obs_source_t *target = obs_filter_get_target(f->source);
	if (!target || !c->effect) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	/* 1. Capture the filtered source into tex_src (straight alpha) */
	gs_texrender_reset(c->tex_src);
	if (!gs_texrender_begin(c->tex_src, w, h)) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	bool ok = false;
	if (obs_source_process_filter_begin(f->source, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_process_filter_tech_end(f->source, c->effect, w, h, "Copy");
		gs_blend_state_pop();
		ok = true;
	}
	gs_texrender_end(c->tex_src);

	gs_texture_t *src = ok ? gs_texrender_get_texture(c->tex_src) : NULL;
	if (!src)
		return;

	/* 2. Blur and 3. composite to the screen */
	float premult = 0.0f;
	gs_texture_t *blurred = blur_stage(c, &f->p, src, w, h, &premult);
	blur_final(c, &f->p, blurred, premult, src, w, h, 0, 0, w, h);
}

static struct obs_source_info blur_filter_info = {
	.id = "simple_blur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.update = filter_update,
	.get_defaults = common_defaults,
	.get_properties = filter_properties,
	.video_render = filter_render,
};

/* ------------------------------------------------------------------ */
/* Blur Layer (standalone source)                                      */
/* ------------------------------------------------------------------ */

struct blur_layer {
	obs_source_t *source;
	struct blur_core core;
	struct blur_params p;
	obs_weak_source_t *target;
	char *target_name;
	uint32_t src_w;
	uint32_t src_h;
	bool rendering; /* recursion guard */
};

static void layer_release_target(struct blur_layer *l)
{
	if (!l->target)
		return;
	obs_source_t *t = obs_weak_source_get_source(l->target);
	if (t) {
		obs_source_dec_active(t);
		obs_source_dec_showing(t);
		obs_source_release(t);
	}
	obs_weak_source_release(l->target);
	l->target = NULL;
}

static void layer_resolve_target(struct blur_layer *l)
{
	if (l->target || !l->target_name || !*l->target_name)
		return;
	obs_source_t *t = obs_get_source_by_name(l->target_name);
	if (!t)
		return;
	if (t != l->source) {
		l->target = obs_source_get_weak_source(t);
		obs_source_inc_showing(t);
		obs_source_inc_active(t);
	}
	obs_source_release(t);
}

static const char *layer_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return tr("BlurLayer");
}

static void layer_update(void *data, obs_data_t *settings)
{
	struct blur_layer *l = data;
	const char *name = obs_data_get_string(settings, S_SOURCE);

	read_params(settings, &l->p);

	if (!l->target_name || strcmp(l->target_name, name) != 0) {
		layer_release_target(l);
		bfree(l->target_name);
		l->target_name = bstrdup(name);
		layer_resolve_target(l);
	}
}

static void *layer_create(obs_data_t *settings, obs_source_t *source)
{
	struct blur_layer *l = bzalloc(sizeof(*l));
	l->source = source;
	if (!core_init(&l->core)) {
		core_destroy(&l->core);
		bfree(l);
		return NULL;
	}
	layer_update(l, settings);
	return l;
}

static void layer_destroy(void *data)
{
	struct blur_layer *l = data;
	layer_release_target(l);
	bfree(l->target_name);
	core_destroy(&l->core);
	bfree(l);
}

static obs_properties_t *layer_properties(void *data)
{
	struct blur_layer *l = data;
	return build_properties(l->source, true);
}

static void layer_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct blur_layer *l = data;

	/* The target may be loaded after us, or may have been removed */
	if (l->target) {
		obs_source_t *t = obs_weak_source_get_source(l->target);
		if (!t) {
			obs_weak_source_release(l->target);
			l->target = NULL;
		} else {
			l->src_w = obs_source_get_width(t);
			l->src_h = obs_source_get_height(t);
			obs_source_release(t);
		}
	}
	if (!l->target) {
		l->src_w = l->src_h = 0;
		layer_resolve_target(l);
	}
}

static uint32_t layer_get_width(void *data)
{
	struct blur_layer *l = data;
	if (!l->src_w || !l->src_h)
		return 1920;
	uint32_t x, y, ow, oh;
	crop_rect(&l->p, l->src_w, l->src_h, &x, &y, &ow, &oh);
	return ow;
}

static uint32_t layer_get_height(void *data)
{
	struct blur_layer *l = data;
	if (!l->src_w || !l->src_h)
		return 1080;
	uint32_t x, y, ow, oh;
	crop_rect(&l->p, l->src_w, l->src_h, &x, &y, &ow, &oh);
	return oh;
}

static void layer_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct blur_layer *l = data;
	struct blur_core *c = &l->core;

	if (l->rendering || !l->target || !c->effect || !l->src_w || !l->src_h)
		return;

	obs_source_t *t = obs_weak_source_get_source(l->target);
	if (!t)
		return;

	const uint32_t w = l->src_w;
	const uint32_t h = l->src_h;
	l->rendering = true;

	/* 1. Render the target source into tex_src */
	gs_texrender_reset(c->tex_src);
	if (gs_texrender_begin(c->tex_src, w, h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_video_render(t);
		gs_texrender_end(c->tex_src);
	}

	gs_texture_t *src = gs_texrender_get_texture(c->tex_src);
	if (src) {
		/* 2. Blur, 3. crop/shape/colour to the screen */
		float premult = 0.0f;
		gs_texture_t *blurred = blur_stage(c, &l->p, src, w, h, &premult);

		uint32_t x0, y0, ow, oh;
		crop_rect(&l->p, w, h, &x0, &y0, &ow, &oh);
		blur_final(c, &l->p, blurred, premult, src, w, h, x0, y0, ow, oh);
	}

	l->rendering = false;
	obs_source_release(t);
}

static struct obs_source_info blur_layer_info = {
	.id = "simple_blur_layer",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = layer_get_name,
	.create = layer_create,
	.destroy = layer_destroy,
	.update = layer_update,
	.get_defaults = layer_defaults,
	.get_properties = layer_properties,
	.video_tick = layer_tick,
	.video_render = layer_render,
	.get_width = layer_get_width,
	.get_height = layer_get_height,
};

/* ------------------------------------------------------------------ */

bool obs_module_load(void)
{
	obs_register_source(&blur_filter_info);
	obs_register_source(&blur_layer_info);
	blog(LOG_INFO, "[obs-simple-blur] v2 loaded");
	return true;
}

void obs_module_unload(void)
{
}
