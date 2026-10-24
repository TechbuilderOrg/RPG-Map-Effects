#pragma once

extern "C" {
#include <obs.h>
}

/** Source type id of the on-output grid. */
#define RPG_GRID_SOURCE_ID "rpg_map_grid"

/**
 * Register the grid source type. Call from obs_module_load.
 *
 * The grid is drawn as geometry at canvas resolution instead of a pre-rendered image: an image
 * scaled down to a preview (a 4K canvas in a laptop-sized window) drops or merges thin lines, so
 * squares stop looking square. The GM preview skips this source and draws its own pixel-snapped
 * copy of the same grid (OBSDisplayWidget), see rpg_is_gm_preview_rendering().
 *
 * Settings: cell_size, line_width, offset_x, offset_y (canvas px), color (0xAABBGGRR, OBS color
 * property format), opacity (0-100).
 */
void rpg_grid_register_source();

bool rpg_is_grid_source(obs_source_t *source);
