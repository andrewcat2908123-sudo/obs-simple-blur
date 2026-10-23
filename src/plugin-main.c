/*
 * obs-simple-blur
 *  - Video filter  "Blur (Simple)"  : add via Filters on any source/scene
 *  - Source        "Blur Layer"     : separate layer that renders another
 *                                     source/scene blurred
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-simple-blur", "en-US")

#define S_RADIUS "radius"
#define S_SOURCE "source"

#define MAX_RADIUS 100.0

/* ------------------------------------------------------------------ */
/* Shared blur core                                                    */
/* ------------------------------------------------------------------ */

/* Shader is embedded so the plugin does not depend on data/effects/ being installed */
static const char *blur_effect_source =
	"// Separable gaussian blur. Direction is set from C via texel_step:\n"
	"// (1/width, 0) for the horizontal pass, (0, 1/height) for the vertical one.\n"
	"\n"
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform float2 texel_step;\n"
	"uniform float radius; // in pixels\n"
	"\n"
	"sampler_state textureSampler {\n"
	"	Filter    = Linear;\n"
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
	"float4 PSBlur(VertData v_in) : TARGET\n"
	"{\n"
	"	// Work in premultiplied alpha so transparent edges don't turn dark\n"
	"	float4 c = image.Sample(textureSampler, v_in.uv);\n"
	"	float4 sum = float4(c.rgb * c.a, c.a);\n"
	"	float wsum = 1.0;\n"
	"\n"
	"	for (int i = 1; i <= 16; i++) {\n"
	"		float t = (float)i / 16.0;\n"
	"		float w = exp(-t * t * 4.5); // sigma ~= radius / 3\n"
	"		float2 o = texel_step * radius * t;\n"
	"\n"
	"		float4 a = image.Sample(textureSampler, v_in.uv + o);\n"
	"		float4 b = image.Sample(textureSampler, v_in.uv - o);\n"
	"\n"
	"		sum += float4(a.rgb * a.a, a.a) * w;\n"
	"		sum += float4(b.rgb * b.a, b.a) * w;\n"
	"		wsum += 2.0 * w;\n"
	"	}\n"
	"\n"
	"	float alpha = sum.a / wsum;\n"
	"	float3 rgb  = sum.rgb / max(sum.a, 0.00001);\n"
	"	return float4(rgb, alpha);\n"
	"}\n"
	"\n"
	"technique Draw\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader  = PSBlur(v_in);\n"
	"	}\n"
	"}\n"
	"\n";

struct blur_core {
	gs_effect_t *effect;
	gs_eparam_t *p_image;
	gs_eparam_t *p_step;
	gs_eparam_t *p_radius;
	gs_texrender_t *tex_a;
	gs_texrender_t *tex_b;
};

static bool core_init(struct blur_core *c)
{
	char *err = NULL;

	obs_enter_graphics();
	c->effect = gs_effect_create(blur_effect_source, "blur.effect", &err);
	if (c->effect) {
		c->p_image = gs_effect_get_param_by_name(c->effect, "image");
		c->p_step = gs_effect_get_param_by_name(c->effect, "texel_step");
		c->p_radius = gs_effect_get_param_by_name(c->effect, "radius");
	} else {
		blog(LOG_ERROR, "[obs-simple-blur] failed to compile effect: %s", err ? err : "unknown error");
	}
	c->tex_a = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	c->tex_b = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	bfree(err);
	return c->effect != NULL;
}

static void core_destroy(struct blur_core *c)
{
	obs_enter_graphics();
	if (c->effect)
		gs_effect_destroy(c->effect);
	if (c->tex_a)
		gs_texrender_destroy(c->tex_a);
	if (c->tex_b)
		gs_texrender_destroy(c->tex_b);
	obs_leave_graphics();
	memset(c, 0, sizeof(*c));
}

/* Set up effect params for one blur direction. */
static void core_set_pass(struct blur_core *c, float dx, float dy, float radius)
{
	struct vec2 step;
	vec2_set(&step, dx, dy);
	gs_effect_set_vec2(c->p_step, &step);
	gs_effect_set_float(c->p_radius, radius);
}

/* Draw `tex` blurred in one direction into the currently bound target. */
static void core_draw_pass(struct blur_core *c, gs_texture_t *tex, uint32_t w, uint32_t h, float dx, float dy,
			   float radius)
{
	gs_effect_set_texture(c->p_image, tex);
	core_set_pass(c, dx, dy, radius);
	while (gs_effect_loop(c->effect, "Draw"))
		gs_draw_sprite(tex, 0, w, h);
}

/* Blur `src` horizontally into `dst`. Returns dst's texture or NULL. */
static gs_texture_t *core_pass_to_texrender(struct blur_core *c, gs_texrender_t *dst, gs_texture_t *src, uint32_t w,
					    uint32_t h, float dx, float dy, float radius)
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
	core_draw_pass(c, src, w, h, dx, dy, radius);
	gs_blend_state_pop();

	gs_texrender_end(dst);
	return gs_texrender_get_texture(dst);
}

/* ------------------------------------------------------------------ */
/* Filter                                                              */
/* ------------------------------------------------------------------ */

struct blur_filter {
	obs_source_t *source;
	struct blur_core core;
	float radius;
};

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BlurFilter");
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct blur_filter *f = data;
	f->radius = (float)obs_data_get_double(settings, S_RADIUS);
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

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_RADIUS, 20.0);
}

static obs_properties_t *filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_float_slider(props, S_RADIUS, obs_module_text("Radius"), 0.0, MAX_RADIUS, 0.5);
	return props;
}

static void filter_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct blur_filter *f = data;

	obs_source_t *target = obs_filter_get_target(f->source);
	if (!target || !f->core.effect || f->radius < 0.5f) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	uint32_t w = obs_source_get_base_width(target);
	uint32_t h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	/* Pass 1: the filtered source -> horizontal blur -> tex_a */
	gs_texrender_reset(f->core.tex_a);
	if (!gs_texrender_begin(f->core.tex_a, w, h)) {
		obs_source_skip_video_filter(f->source);
		return;
	}

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	bool ok = false;
	if (obs_source_process_filter_begin(f->source, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		core_set_pass(&f->core, 1.0f / (float)w, 0.0f, f->radius);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_process_filter_tech_end(f->source, f->core.effect, w, h, "Draw");
		gs_blend_state_pop();
		ok = true;
	}
	gs_texrender_end(f->core.tex_a);

	gs_texture_t *tex = ok ? gs_texrender_get_texture(f->core.tex_a) : NULL;
	if (!tex)
		return;

	/* Pass 2: vertical blur straight to the screen */
	core_draw_pass(&f->core, tex, w, h, 0.0f, 1.0f / (float)h, f->radius);
}

static struct obs_source_info blur_filter_info = {
	.id = "simple_blur_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = filter_get_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.update = filter_update,
	.get_defaults = filter_defaults,
	.get_properties = filter_properties,
	.video_render = filter_render,
};

/* ------------------------------------------------------------------ */
/* Blur Layer (standalone source)                                      */
/* ------------------------------------------------------------------ */

struct blur_layer {
	obs_source_t *source;
	struct blur_core core;
	obs_weak_source_t *target;
	char *target_name;
	float radius;
	uint32_t width;
	uint32_t height;
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
	return obs_module_text("BlurLayer");
}

static void layer_update(void *data, obs_data_t *settings)
{
	struct blur_layer *l = data;
	const char *name = obs_data_get_string(settings, S_SOURCE);

	l->radius = (float)obs_data_get_double(settings, S_RADIUS);

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

static void layer_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_RADIUS, 20.0);
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

static obs_properties_t *layer_properties(void *data)
{
	struct blur_layer *l = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, S_SOURCE, obs_module_text("Source"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, "", "");

	struct enum_ctx ctx = {.list = list, .self = l->source};
	obs_enum_scenes(enum_cb, &ctx);
	obs_enum_sources(enum_cb, &ctx);

	obs_properties_add_float_slider(props, S_RADIUS, obs_module_text("Radius"), 0.0, MAX_RADIUS, 0.5);
	return props;
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
			l->width = obs_source_get_width(t);
			l->height = obs_source_get_height(t);
			obs_source_release(t);
		}
	}
	if (!l->target) {
		l->width = l->height = 0;
		layer_resolve_target(l);
	}
}

static uint32_t layer_get_width(void *data)
{
	struct blur_layer *l = data;
	return l->width ? l->width : 1920;
}

static uint32_t layer_get_height(void *data)
{
	struct blur_layer *l = data;
	return l->height ? l->height : 1080;
}

static void layer_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct blur_layer *l = data;

	if (l->rendering || !l->target || !l->core.effect || !l->width || !l->height)
		return;

	obs_source_t *t = obs_weak_source_get_source(l->target);
	if (!t)
		return;

	const uint32_t w = l->width;
	const uint32_t h = l->height;
	l->rendering = true;

	/* 1. Render the target source into tex_a */
	gs_texrender_reset(l->core.tex_a);
	if (gs_texrender_begin(l->core.tex_a, w, h)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_video_render(t);
		gs_texrender_end(l->core.tex_a);
	}

	gs_texture_t *a = gs_texrender_get_texture(l->core.tex_a);
	if (a) {
		/* 2. Horizontal blur: tex_a -> tex_b */
		gs_texture_t *b = core_pass_to_texrender(&l->core, l->core.tex_b, a, w, h, 1.0f / (float)w, 0.0f,
							 l->radius);
		/* 3. Vertical blur: tex_b -> screen */
		if (b)
			core_draw_pass(&l->core, b, w, h, 0.0f, 1.0f / (float)h, l->radius);
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
	blog(LOG_INFO, "[obs-simple-blur] loaded");
	return true;
}

void obs_module_unload(void)
{
}
