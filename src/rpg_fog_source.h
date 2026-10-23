#pragma once

extern "C" {
#include <obs.h>
}

/** Source type id of the fog-of-war source (one instance per battlemap scene). */
#define RPG_FOG_SOURCE_ID "rpg_fog_of_war"

/** Register the fog-of-war source type. Call from obs_module_load. */
void rpg_fog_register_source();

/**
 * The GM preview (plugin window) sets this around its scene render so the fog draws
 * semi-transparent there while Program/stream output keeps the full fog opacity.
 * Graphics thread only.
 */
void rpg_set_gm_preview_rendering(bool active);
bool rpg_is_gm_preview_rendering();

/**
 * Create the plugin effect with techniques "FogColor", "FogImage" and "DrawOpacity"
 * (premultiplied texture * opacity). Graphics thread only; caller owns the result and
 * destroys it with gs_effect_destroy while graphics is still alive.
 */
gs_effect_t *rpg_create_effect();

/** True if the source is a fog-of-war source. */
bool rpg_is_fog_source(obs_source_t *source);

/* Mask editing. Coordinates are canvas (base resolution) pixels. reveal=true clears fog,
 * reveal=false paints fog back. Safe to call from the UI thread. */

/** Snapshot the mask for undo. Call once at the start of each stroke / edit. */
void rpg_fog_begin_edit(obs_source_t *fog);
/** Paint a round brush stroke from (x0,y0) to (x1,y1). */
void rpg_fog_stroke(obs_source_t *fog, float x0, float y0, float x1, float y1, float radius, bool reveal);
/** Paint an axis-aligned rectangle. */
void rpg_fog_rect(obs_source_t *fog, float x0, float y0, float x1, float y1, bool reveal);
/** Reveal the whole map (reveal=true) or cover it completely (reveal=false). */
void rpg_fog_fill(obs_source_t *fog, bool reveal);
/** Restore the mask from before the last edit. Returns false if there is nothing to undo. */
bool rpg_fog_undo(obs_source_t *fog);
