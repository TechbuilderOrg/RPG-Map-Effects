#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QAction>
#include <QApplication>
#include <QObject>
#include <QWidget>

#include "rpg_window.hpp"
#include "rpg_fog_source.h"
#include "rpg_grid_source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("rpg-map-effects", "en-US")

static RPGWindow *g_window = nullptr;
static QAction *g_action = nullptr;

static obs_hotkey_id g_triggerAllHotkey = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_triggerNextHotkey = OBS_INVALID_HOTKEY_ID;
static const char *kTriggerAllSaveKey = "rpg_map_effects_trigger_all_hotkey";
static const char *kTriggerNextSaveKey = "rpg_map_effects_trigger_next_hotkey";

/** Hotkeys fire on the OBS hotkey thread; hop to the Qt thread before touching the window. */
static void on_trigger_hotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
    if (!pressed)
        return;
    const bool all = data != nullptr;
    QMetaObject::invokeMethod(
        qApp,
        [all]() {
            if (!g_window)
                return;
            if (all)
                g_window->triggerAllStaged();
            else
                g_window->triggerNextStaged();
        },
        Qt::QueuedConnection);
}

/** Persist hotkey bindings in the scene collection (frontend hotkeys are not saved by OBS). */
static void on_frontend_save(obs_data_t *save_data, bool saving, void *)
{
    const std::pair<obs_hotkey_id, const char *> hotkeys[] = {
        {g_triggerAllHotkey, kTriggerAllSaveKey},
        {g_triggerNextHotkey, kTriggerNextSaveKey},
    };
    for (const auto &[id, key] : hotkeys) {
        if (id == OBS_INVALID_HOTKEY_ID)
            continue;
        if (saving) {
            obs_data_array_t *arr = obs_hotkey_save(id);
            obs_data_set_array(save_data, key, arr);
            obs_data_array_release(arr);
        } else {
            obs_data_array_t *arr = obs_data_get_array(save_data, key);
            obs_hotkey_load(id, arr);
            obs_data_array_release(arr);
        }
    }
}

static void open_window()
{
    if (!g_window) {
        g_window = new RPGWindow();
        g_window->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    g_window->show();
    g_window->raise();
    g_window->activateWindow();
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (event == OBS_FRONTEND_EVENT_EXIT) {
        if (g_window)
            g_window->releaseOBSResources();
        return;
    }
    if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
        if (g_window)
            g_window->releaseSceneCollectionRefs();
        return;
    }
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
        return;

    if (!g_action) {
        g_action = (QAction *)obs_frontend_add_tools_menu_qaction("RPG Map Effects");
        QObject::connect(g_action, &QAction::triggered, []() { open_window(); });
        blog(LOG_INFO, "[rpg-map-effects] Tools menu action added");
    }
}

bool obs_module_load(void)
{
    rpg_fog_register_source();
    rpg_grid_register_source();

    g_triggerAllHotkey = obs_hotkey_register_frontend("rpg_map_effects.trigger_all_staged",
                                                      "RPG Map Effects: Trigger all staged FX",
                                                      on_trigger_hotkey, (void *)1);
    g_triggerNextHotkey = obs_hotkey_register_frontend("rpg_map_effects.trigger_next_staged",
                                                       "RPG Map Effects: Trigger next staged FX",
                                                       on_trigger_hotkey, nullptr);
    obs_frontend_add_save_callback(on_frontend_save, nullptr);

    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    blog(LOG_INFO, "[rpg-map-effects] loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    obs_frontend_remove_save_callback(on_frontend_save, nullptr);

    if (g_window) {
        g_window->releaseOBSResources();
        g_window->close();
        delete g_window;
        g_window = nullptr;
    }
    g_action = nullptr;

    blog(LOG_INFO, "[rpg-map-effects] unloaded");
}
