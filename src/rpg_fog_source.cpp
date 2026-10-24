#include "rpg_fog_source.h"

#include <obs-module.h>

#include <QByteArray>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

extern "C" {
#include <graphics/image-file.h>
#include <graphics/vec4.h>
}

// The mask is stored at a fraction of the canvas resolution: linear filtering softens
// the edges and it keeps texture uploads and saved data small.
static constexpr uint32_t kMaskDivisor = 2;
static constexpr size_t kMaxUndo = 30;

static const char *kEffectCode = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d fog_tex;
uniform float4 fog_color;
uniform float opacity;

sampler_state linear_clamp {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSFogColor(VertData v_in) : TARGET
{
	float a = image.Sample(linear_clamp, v_in.uv).r * fog_color.a * opacity;
	return float4(fog_color.rgb * a, a);
}

float4 PSFogImage(VertData v_in) : TARGET
{
	float4 t = fog_tex.Sample(linear_clamp, v_in.uv);
	float a = image.Sample(linear_clamp, v_in.uv).r * t.a * opacity;
	return float4(t.rgb * a, a);
}

float4 PSDrawOpacity(VertData v_in) : TARGET
{
	return image.Sample(linear_clamp, v_in.uv) * opacity;
}

technique FogColor
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSFogColor(v_in);
	}
}

technique FogImage
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSFogImage(v_in);
	}
}

technique DrawOpacity
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSDrawOpacity(v_in);
	}
}
)";

gs_effect_t *rpg_create_effect()
{
    char *errors = nullptr;
    gs_effect_t *effect = gs_effect_create(kEffectCode, "rpg_map_effects.effect", &errors);
    if (!effect)
        blog(LOG_ERROR, "[rpg-map-effects] Failed to compile effect: %s", errors ? errors : "(unknown)");
    bfree(errors);
    return effect;
}

static std::atomic<bool> g_gmPreviewRendering{false};

void rpg_set_gm_preview_rendering(bool active)
{
    g_gmPreviewRendering = active;
}

bool rpg_is_gm_preview_rendering()
{
    return g_gmPreviewRendering;
}

struct FogData {
    obs_source_t *source = nullptr;

    std::mutex mutex; // guards everything below except the graphics-only members
    uint32_t canvasW = 0;
    uint32_t canvasH = 0;
    uint32_t maskW = 0;
    uint32_t maskH = 0;
    std::vector<uint8_t> mask; // 255 = fog, 0 = revealed
    std::deque<std::vector<uint8_t>> undo;
    bool dirty = true;

    vec4 color = {};
    float opacity = 1.0f;
    float gmOpacity = 0.5f;
    gs_image_file_t image = {};
    bool hasImage = false;

    // Graphics thread only.
    gs_texture_t *tex = nullptr;
    uint32_t texW = 0;
    uint32_t texH = 0;
    gs_effect_t *effect = nullptr;
    bool effectFailed = false;
};

static FogData *FogFromSource(obs_source_t *source)
{
    if (!rpg_is_fog_source(source))
        return nullptr;
    return static_cast<FogData *>(obs_obj_get_data(source));
}

bool rpg_is_fog_source(obs_source_t *source)
{
    if (!source)
        return false;
    const char *id = obs_source_get_unversioned_id(source);
    return id && strcmp(id, RPG_FOG_SOURCE_ID) == 0;
}

static void GetCanvasSize(uint32_t &w, uint32_t &h)
{
    obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
        w = ovi.base_width;
        h = ovi.base_height;
    } else {
        w = 1920;
        h = 1080;
    }
}

/** Resize the mask to match the canvas, keeping existing contents (nearest neighbour). Lock held. */
static void FitMaskToCanvasLocked(FogData *f, uint32_t canvasW, uint32_t canvasH)
{
    const uint32_t newW = std::max(1u, (canvasW + kMaskDivisor - 1) / kMaskDivisor);
    const uint32_t newH = std::max(1u, (canvasH + kMaskDivisor - 1) / kMaskDivisor);
    f->canvasW = canvasW;
    f->canvasH = canvasH;
    if (newW == f->maskW && newH == f->maskH && f->mask.size() == size_t(newW) * newH)
        return;

    std::vector<uint8_t> resized(size_t(newW) * newH, 255);
    if (f->maskW > 0 && f->maskH > 0 && f->mask.size() == size_t(f->maskW) * f->maskH) {
        for (uint32_t y = 0; y < newH; ++y) {
            const uint32_t sy = std::min(f->maskH - 1, uint32_t(uint64_t(y) * f->maskH / newH));
            for (uint32_t x = 0; x < newW; ++x) {
                const uint32_t sx = std::min(f->maskW - 1, uint32_t(uint64_t(x) * f->maskW / newW));
                resized[size_t(y) * newW + x] = f->mask[size_t(sy) * f->maskW + sx];
            }
        }
    }
    f->mask.swap(resized);
    f->maskW = newW;
    f->maskH = newH;
    f->undo.clear();
    f->dirty = true;
}

static void LoadMaskFromSettings(FogData *f, obs_data_t *settings)
{
    const uint32_t w = (uint32_t)obs_data_get_int(settings, "mask_w");
    const uint32_t h = (uint32_t)obs_data_get_int(settings, "mask_h");
    const char *b64 = obs_data_get_string(settings, "mask_data");
    if (w == 0 || h == 0 || !b64 || !*b64)
        return;
    const QByteArray raw = qUncompress(QByteArray::fromBase64(QByteArray(b64)));
    if ((size_t)raw.size() != size_t(w) * h)
        return;
    f->maskW = w;
    f->maskH = h;
    f->mask.assign(raw.constData(), raw.constData() + raw.size());
}

static const char *fog_get_name(void *)
{
    return "RPG Fog of War";
}

static void fog_update(void *data, obs_data_t *settings)
{
    auto *f = static_cast<FogData *>(data);

    const uint32_t c = (uint32_t)obs_data_get_int(settings, "color");
    vec4 color;
    // OBS color properties are 0xAABBGGRR.
    vec4_set(&color, float(c & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f,
             float((c >> 16) & 0xff) / 255.0f, float((c >> 24) & 0xff) / 255.0f);
    const float opacity = float(obs_data_get_int(settings, "opacity")) / 100.0f;
    const float gmOpacity = float(obs_data_get_int(settings, "gm_opacity")) / 100.0f;

    // Decode the optional fog image outside any lock; texture upload needs graphics.
    const char *path = obs_data_get_string(settings, "image");
    gs_image_file_t newImage = {};
    bool hasNewImage = false;
    if (path && *path) {
        gs_image_file_init(&newImage, path);
        hasNewImage = newImage.loaded;
    }

    // Lock order everywhere: graphics, then f->mutex (the render callback already holds graphics).
    obs_enter_graphics();
    gs_image_file_t oldImage = {};
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        f->color = color;
        f->opacity = std::clamp(opacity, 0.0f, 1.0f);
        f->gmOpacity = std::clamp(gmOpacity, 0.0f, 1.0f);
        oldImage = f->image;
        f->image = newImage;
        f->hasImage = hasNewImage;
        if (hasNewImage)
            gs_image_file_init_texture(&f->image);
    }
    gs_image_file_free(&oldImage);
    if (!hasNewImage)
        gs_image_file_free(&newImage);
    obs_leave_graphics();
}

static void *fog_create(obs_data_t *settings, obs_source_t *source)
{
    auto *f = new FogData();
    f->source = source;
    LoadMaskFromSettings(f, settings);
    uint32_t w = 0, h = 0;
    GetCanvasSize(w, h);
    FitMaskToCanvasLocked(f, w, h);
    fog_update(f, settings);
    return f;
}

static void fog_destroy(void *data)
{
    auto *f = static_cast<FogData *>(data);
    obs_enter_graphics();
    if (f->tex)
        gs_texture_destroy(f->tex);
    if (f->effect)
        gs_effect_destroy(f->effect);
    gs_image_file_free(&f->image);
    obs_leave_graphics();
    delete f;
}

static void fog_save(void *data, obs_data_t *settings)
{
    auto *f = static_cast<FogData *>(data);
    QByteArray raw;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        w = f->maskW;
        h = f->maskH;
        raw = QByteArray(reinterpret_cast<const char *>(f->mask.data()), (qsizetype)f->mask.size());
    }
    obs_data_set_int(settings, "mask_w", w);
    obs_data_set_int(settings, "mask_h", h);
    obs_data_set_string(settings, "mask_data", qCompress(raw, 9).toBase64().constData());
}

static void fog_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "color", 0xFF141414);
    obs_data_set_default_int(settings, "opacity", 100);
    obs_data_set_default_int(settings, "gm_opacity", 45);
}

static bool fog_reveal_all_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    auto *f = static_cast<FogData *>(data);
    rpg_fog_begin_edit(f->source);
    rpg_fog_fill(f->source, true);
    return false;
}

static bool fog_cover_all_clicked(obs_properties_t *, obs_property_t *, void *data)
{
    auto *f = static_cast<FogData *>(data);
    rpg_fog_begin_edit(f->source);
    rpg_fog_fill(f->source, false);
    return false;
}

static obs_properties_t *fog_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_color_alpha(props, "color", "Fog color");
    obs_properties_add_path(props, "image", "Fog image (optional, replaces color)", OBS_PATH_FILE,
                            "Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)", nullptr);
    obs_property_t *p = obs_properties_add_int_slider(props, "opacity", "Opacity on output", 0, 100, 1);
    obs_property_int_set_suffix(p, "%");
    p = obs_properties_add_int_slider(props, "gm_opacity", "Opacity in RPG Map Effects window (GM view)", 0,
                                      100, 1);
    obs_property_int_set_suffix(p, "%");
    obs_properties_add_button(props, "reveal_all", "Reveal entire map", fog_reveal_all_clicked);
    obs_properties_add_button(props, "cover_all", "Cover entire map", fog_cover_all_clicked);
    return props;
}

static void fog_tick(void *data, float)
{
    auto *f = static_cast<FogData *>(data);
    uint32_t w = 0, h = 0;
    GetCanvasSize(w, h);
    std::lock_guard<std::mutex> lock(f->mutex);
    if (w != f->canvasW || h != f->canvasH)
        FitMaskToCanvasLocked(f, w, h);
}

static void fog_render(void *data, gs_effect_t *)
{
    auto *f = static_cast<FogData *>(data);

    if (!f->effect && !f->effectFailed) {
        f->effect = rpg_create_effect();
        f->effectFailed = !f->effect;
    }
    if (!f->effect)
        return;

    std::lock_guard<std::mutex> lock(f->mutex);
    if (f->maskW == 0 || f->maskH == 0)
        return;

    if (!f->tex || f->texW != f->maskW || f->texH != f->maskH) {
        if (f->tex)
            gs_texture_destroy(f->tex);
        f->tex = gs_texture_create(f->maskW, f->maskH, GS_R8, 1, nullptr, GS_DYNAMIC);
        f->texW = f->maskW;
        f->texH = f->maskH;
        f->dirty = true;
    }
    if (!f->tex)
        return;
    if (f->dirty) {
        gs_texture_set_image(f->tex, f->mask.data(), f->maskW, false);
        f->dirty = false;
    }

    const float opacity = rpg_is_gm_preview_rendering() ? f->gmOpacity : f->opacity;
    if (opacity <= 0.0f)
        return;

    const bool useImage = f->hasImage && f->image.texture;
    gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, "image"), f->tex);
    gs_effect_set_float(gs_effect_get_param_by_name(f->effect, "opacity"), opacity);
    gs_effect_set_vec4(gs_effect_get_param_by_name(f->effect, "fog_color"), &f->color);
    if (useImage)
        gs_effect_set_texture(gs_effect_get_param_by_name(f->effect, "fog_tex"), f->image.texture);

    // Shader outputs premultiplied alpha.
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(f->effect, useImage ? "FogImage" : "FogColor"))
        gs_draw_sprite(f->tex, 0, f->canvasW, f->canvasH);
    gs_blend_state_pop();
}

static uint32_t fog_width(void *data)
{
    auto *f = static_cast<FogData *>(data);
    std::lock_guard<std::mutex> lock(f->mutex);
    return f->canvasW;
}

static uint32_t fog_height(void *data)
{
    auto *f = static_cast<FogData *>(data);
    std::lock_guard<std::mutex> lock(f->mutex);
    return f->canvasH;
}

void rpg_fog_register_source()
{
    obs_source_info info = {};
    info.id = RPG_FOG_SOURCE_ID;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = fog_get_name;
    info.create = fog_create;
    info.destroy = fog_destroy;
    info.update = fog_update;
    info.save = fog_save;
    info.get_defaults = fog_defaults;
    info.get_properties = fog_properties;
    info.video_tick = fog_tick;
    info.video_render = fog_render;
    info.get_width = fog_width;
    info.get_height = fog_height;
    info.icon_type = OBS_ICON_TYPE_COLOR;
    obs_register_source(&info);
}

// --- Mask editing ---

void rpg_fog_begin_edit(obs_source_t *fog)
{
    FogData *f = FogFromSource(fog);
    if (!f)
        return;
    std::lock_guard<std::mutex> lock(f->mutex);
    f->undo.push_back(f->mask);
    while (f->undo.size() > kMaxUndo)
        f->undo.pop_front();
}

void rpg_fog_stroke(obs_source_t *fog, float x0, float y0, float x1, float y1, float radius, bool reveal)
{
    FogData *f = FogFromSource(fog);
    if (!f || radius <= 0.0f)
        return;
    std::lock_guard<std::mutex> lock(f->mutex);
    if (f->maskW == 0 || f->maskH == 0 || f->canvasW == 0 || f->canvasH == 0)
        return;

    // Work in mask pixel space.
    const float sx = float(f->maskW) / float(f->canvasW);
    const float sy = float(f->maskH) / float(f->canvasH);
    const float ax = x0 * sx, ay = y0 * sy;
    const float bx = x1 * sx, by = y1 * sy;
    const float r = radius * sx;
    const float feather = std::max(1.0f, r * 0.2f); // soft edge for a fog-like look
    const float abx = bx - ax, aby = by - ay;
    const float len2 = abx * abx + aby * aby;

    const int minX = std::max(0, int(std::floor(std::min(ax, bx) - r)));
    const int maxX = std::min(int(f->maskW) - 1, int(std::ceil(std::max(ax, bx) + r)));
    const int minY = std::max(0, int(std::floor(std::min(ay, by) - r)));
    const int maxY = std::min(int(f->maskH) - 1, int(std::ceil(std::max(ay, by) + r)));
    if (minX > maxX || minY > maxY)
        return;

    for (int y = minY; y <= maxY; ++y) {
        uint8_t *row = f->mask.data() + size_t(y) * f->maskW;
        const float py = float(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x) {
            const float px = float(x) + 0.5f;
            float t = 0.0f;
            if (len2 > 1e-6f)
                t = std::clamp(((px - ax) * abx + (py - ay) * aby) / len2, 0.0f, 1.0f);
            const float dx = px - (ax + abx * t);
            const float dy = py - (ay + aby * t);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d >= r)
                continue;
            // coverage: 1 in the core, falling to 0 at the brush edge
            const float coverage = std::clamp((r - d) / feather, 0.0f, 1.0f);
            const uint8_t amount = uint8_t(std::lround(coverage * 255.0f));
            if (reveal)
                row[x] = std::min<uint8_t>(row[x], uint8_t(255 - amount));
            else
                row[x] = std::max<uint8_t>(row[x], amount);
        }
    }
    f->dirty = true;
}

void rpg_fog_rect(obs_source_t *fog, float x0, float y0, float x1, float y1, bool reveal)
{
    FogData *f = FogFromSource(fog);
    if (!f)
        return;
    std::lock_guard<std::mutex> lock(f->mutex);
    if (f->maskW == 0 || f->maskH == 0 || f->canvasW == 0 || f->canvasH == 0)
        return;
    const float sx = float(f->maskW) / float(f->canvasW);
    const float sy = float(f->maskH) / float(f->canvasH);
    const int minX = std::max(0, int(std::lround(std::min(x0, x1) * sx)));
    const int maxX = std::min(int(f->maskW), int(std::lround(std::max(x0, x1) * sx)));
    const int minY = std::max(0, int(std::lround(std::min(y0, y1) * sy)));
    const int maxY = std::min(int(f->maskH), int(std::lround(std::max(y0, y1) * sy)));
    if (minX >= maxX || minY >= maxY)
        return;
    const uint8_t value = reveal ? 0 : 255;
    for (int y = minY; y < maxY; ++y)
        std::memset(f->mask.data() + size_t(y) * f->maskW + minX, value, size_t(maxX - minX));
    f->dirty = true;
}

void rpg_fog_fill(obs_source_t *fog, bool reveal)
{
    FogData *f = FogFromSource(fog);
    if (!f)
        return;
    std::lock_guard<std::mutex> lock(f->mutex);
    std::fill(f->mask.begin(), f->mask.end(), reveal ? 0 : 255);
    f->dirty = true;
}

bool rpg_fog_undo(obs_source_t *fog)
{
    FogData *f = FogFromSource(fog);
    if (!f)
        return false;
    std::lock_guard<std::mutex> lock(f->mutex);
    if (f->undo.empty())
        return false;
    if (f->undo.back().size() == f->mask.size())
        f->mask.swap(f->undo.back());
    f->undo.pop_back();
    f->dirty = true;
    return true;
}
