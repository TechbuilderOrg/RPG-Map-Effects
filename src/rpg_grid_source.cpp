#include "rpg_grid_source.h"
#include "rpg_fog_source.h"
#include "grid_layout.h"

#include <obs-module.h>

#include <algorithm>
#include <cstring>
#include <mutex>

struct GridData {
    obs_source_t *source = nullptr;

    std::mutex mutex; // guards the settings below
    int cell = 50;
    int lineWidth = 1;
    int offsetX = 0;
    int offsetY = 0;
    uint32_t colorAbgr = 0xFF606060;
    int opacity = 100;
    uint32_t canvasW = 0;
    uint32_t canvasH = 0;
    bool dirty = true;

    // Graphics thread only.
    gs_vertbuffer_t *vb = nullptr;
};

bool rpg_is_grid_source(obs_source_t *source)
{
    if (!source)
        return false;
    const char *id = obs_source_get_unversioned_id(source);
    return id && strcmp(id, RPG_GRID_SOURCE_ID) == 0;
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

static const char *grid_get_name(void *)
{
    return "RPG Map Grid";
}

static void grid_update(void *data, obs_data_t *settings)
{
    auto *g = static_cast<GridData *>(data);
    std::lock_guard<std::mutex> lock(g->mutex);
    g->cell = (int)std::clamp<long long>(obs_data_get_int(settings, "cell_size"), 10, 1000);
    g->lineWidth = (int)std::clamp<long long>(obs_data_get_int(settings, "line_width"), 1, 10);
    g->offsetX = (int)obs_data_get_int(settings, "offset_x");
    g->offsetY = (int)obs_data_get_int(settings, "offset_y");
    g->colorAbgr = (uint32_t)obs_data_get_int(settings, "color");
    g->opacity = (int)std::clamp<long long>(obs_data_get_int(settings, "opacity"), 0, 100);
    g->dirty = true;
}

static void *grid_create(obs_data_t *settings, obs_source_t *source)
{
    auto *g = new GridData();
    g->source = source;
    GetCanvasSize(g->canvasW, g->canvasH);
    grid_update(g, settings);
    return g;
}

static void grid_destroy(void *data)
{
    auto *g = static_cast<GridData *>(data);
    obs_enter_graphics();
    if (g->vb)
        gs_vertexbuffer_destroy(g->vb);
    obs_leave_graphics();
    delete g;
}

static void grid_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "cell_size", 50);
    obs_data_set_default_int(settings, "line_width", 1);
    obs_data_set_default_int(settings, "color", 0xFF606060);
    obs_data_set_default_int(settings, "opacity", 100);
}

static obs_properties_t *grid_properties(void *)
{
    obs_properties_t *props = obs_properties_create();
    obs_property_t *p = obs_properties_add_int(props, "cell_size", "Square size", 10, 1000, 1);
    obs_property_int_set_suffix(p, " px");
    p = obs_properties_add_int(props, "line_width", "Line width", 1, 10, 1);
    obs_property_int_set_suffix(p, " px");
    p = obs_properties_add_int(props, "offset_x", "Offset X", -1000, 1000, 1);
    obs_property_int_set_suffix(p, " px");
    p = obs_properties_add_int(props, "offset_y", "Offset Y", -1000, 1000, 1);
    obs_property_int_set_suffix(p, " px");
    obs_properties_add_color(props, "color", "Color");
    p = obs_properties_add_int_slider(props, "opacity", "Opacity", 0, 100, 1);
    obs_property_int_set_suffix(p, "%");
    return props;
}

static void grid_tick(void *data, float)
{
    auto *g = static_cast<GridData *>(data);
    uint32_t w = 0, h = 0;
    GetCanvasSize(w, h);
    std::lock_guard<std::mutex> lock(g->mutex);
    if (w != g->canvasW || h != g->canvasH) {
        g->canvasW = w;
        g->canvasH = h;
        g->dirty = true;
    }
}

static void grid_render(void *data, gs_effect_t *)
{
    // The GM preview draws its own pixel-snapped grid on top of the scene.
    if (rpg_is_gm_preview_rendering())
        return;

    auto *g = static_cast<GridData *>(data);
    uint32_t argb = 0;
    {
        std::lock_guard<std::mutex> lock(g->mutex);
        if (g->dirty) {
            if (g->vb) {
                gs_vertexbuffer_destroy(g->vb);
                g->vb = nullptr;
            }
            const GridLayout grid(g->cell, g->lineWidth, g->offsetX, g->offsetY, int(g->canvasW), int(g->canvasH));
            if (grid.valid()) {
                gs_render_start(true);
                ForEachGridRect(grid, 1.0f, [](float x0, float y0, float x1, float y1) {
                    gs_vertex2f(x0, y0);
                    gs_vertex2f(x1, y0);
                    gs_vertex2f(x0, y1);
                    gs_vertex2f(x1, y0);
                    gs_vertex2f(x1, y1);
                    gs_vertex2f(x0, y1);
                });
                g->vb = gs_render_save();
            }
            g->dirty = false;
        }
        const uint32_t c = g->colorAbgr;
        const uint32_t alpha = uint32_t(std::clamp(int((c >> 24) & 0xff) * g->opacity / 100, 0, 255));
        // OBS color properties are 0xAABBGGRR; the solid effect takes 0xAARRGGBB.
        argb = (alpha << 24) | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
    }
    if (!g->vb || (argb >> 24) == 0)
        return;

    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), argb);
    gs_load_vertexbuffer(g->vb);
    gs_load_indexbuffer(nullptr);
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(solid, "Solid"))
        gs_draw(GS_TRIS, 0, 0);
    gs_blend_state_pop();
    gs_load_vertexbuffer(nullptr);
}

static uint32_t grid_width(void *data)
{
    auto *g = static_cast<GridData *>(data);
    std::lock_guard<std::mutex> lock(g->mutex);
    return g->canvasW;
}

static uint32_t grid_height(void *data)
{
    auto *g = static_cast<GridData *>(data);
    std::lock_guard<std::mutex> lock(g->mutex);
    return g->canvasH;
}

void rpg_grid_register_source()
{
    obs_source_info info = {};
    info.id = RPG_GRID_SOURCE_ID;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = grid_get_name;
    info.create = grid_create;
    info.destroy = grid_destroy;
    info.update = grid_update;
    info.get_defaults = grid_defaults;
    info.get_properties = grid_properties;
    info.video_tick = grid_tick;
    info.video_render = grid_render;
    info.get_width = grid_width;
    info.get_height = grid_height;
    info.icon_type = OBS_ICON_TYPE_COLOR;
    obs_register_source(&info);
}
