#include "rpg_window.hpp"
#include "obs_display_widget.hpp"
#include "cursor_assets.h"
#include "rpg_fog_source.h"
#include "rpg_grid_source.h"

#include <cstdlib>
#include <cstring>
#include <obs-module.h>
#include <obs.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFrame>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QImage>
#include <QPainter>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QStyledItemDelegate>
#include <QPushButton>
#include <QRadioButton>
#include <QShortcut>
#include <QScrollArea>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWidget>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <algorithm>
#include <cmath>

extern "C" {
#include <obs-frontend-api.h>
}

extern "C" {
#include <graphics/vec2.h>
}

// Compile-time stamp for this translation unit only
static const char *kCompileStamp = __DATE__ " " __TIME__;

// Anchor symbol so dladdr / GetModuleHandleEx can locate the plugin binary on disk
static void rpg_me_anchor_symbol() {}

static QString GetPluginBinaryPath()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&rpg_me_anchor_symbol), &module))
        return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    return len ? QString::fromWCharArray(path, int(len)) : QString();
#else
    Dl_info info {};
    if (dladdr((void *)&rpg_me_anchor_symbol, &info) && info.dli_fname)
        return QString::fromUtf8(info.dli_fname);
    return {};
#endif
}

static QString GetPluginMTimeString()
{
    const QString path = GetPluginBinaryPath();
    if (path.isEmpty())
        return "unknown";

    QFileInfo fi(path);
    if (!fi.exists())
        return "missing";

    return fi.lastModified().toString("yyyy-MM-dd HH:mm:ss");
}

static bool IsFxTemplateSceneName(const QString &name)
{
    // Accept FX:, fx:, Fx:, fX: etc.
    return name.startsWith("fx:", Qt::CaseInsensitive);
}

static bool IsBattlemapSceneName(const QString &name)
{
    // Battlemap scenes are prefixed with "Map:" (case-insensitive).
    return name.startsWith(QStringLiteral("Map:"), Qt::CaseInsensitive);
}

static QString GetDefaultsConfigPath()
{
    char *path = obs_module_config_path("rpg-map-effects-defaults.json");
    QString qpath = path ? QString::fromUtf8(path) : QString();
    bfree(path);
    return qpath;
}

static QIcon iconFromColor(const QColor &c)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // Palette shape: rounded rect (paint well) + small circle (thumb)
    p.setBrush(c);
    p.setPen(QColor(100, 100, 100));
    p.drawRoundedRect(2, 2, 14, 14, 3, 3);
    p.setBrush(Qt::lightGray);
    p.drawEllipse(10, 10, 5, 5);
    p.end();
    return QIcon(pm);
}

static QIcon iconTarget(int size = 20)
{
    if (size <= 0)
        size = 20;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const float s = size / 20.0f;
    p.setPen(QPen(Qt::black, qMax(1, (int)(2 * s))));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(4 * s, 4 * s, 12 * s, 12 * s));
    p.drawEllipse(QRectF(7 * s, 7 * s, 6 * s, 6 * s));
    p.setBrush(Qt::black);
    p.drawEllipse(QRectF(9 * s, 9 * s, 2 * s, 2 * s));
    p.end();
    return QIcon(pm);
}

static const int kToolButtonSize = 24;

static QIcon iconFromCursorAsset(const CursorAsset &asset)
{
    if (!asset.data || asset.size <= 0)
        return QIcon();

    QPixmap pm;
    pm.loadFromData(reinterpret_cast<const uchar *>(asset.data),
                    static_cast<uint>(asset.size), "PNG");
    if (pm.isNull())
        return QIcon();

    const int iconSize = 32;
    QPixmap scaled =
        pm.scaled(iconSize, iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return QIcon(scaled);
}

static uint32_t argbFromColor(const QColor &c)
{
    return (uint32_t(c.alpha()) << 24) | (uint32_t(c.red()) << 16) |
           (uint32_t(c.green()) << 8) | uint32_t(c.blue());
}

// Role for storing locked state in FX list items
static constexpr int kFxItemLockedRole = Qt::UserRole;
// Role for storing staged (hidden until triggered) state in FX list items
static constexpr int kFxItemStagedRole = Qt::UserRole + 1;

// Delegate that draws lock/unlock icon on the right of each FX list item
class FxListDelegate : public QStyledItemDelegate {
public:
    explicit FxListDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);

        const bool locked = index.data(kFxItemLockedRole).toBool();
        // Use QString for emoji; QChar only supports BMP (<=0xFFFF), lock emoji are U+1F512/U+1F513
        const QString icon = locked ? QStringLiteral("\U0001F512") : QStringLiteral("\U0001F513");
        const int margin = 4;
        const QRect rect = option.rect.adjusted(margin, 0, -margin, 0);

        painter->save();
        QPalette::ColorRole textRole = (option.state & QStyle::State_Selected)
            ? QPalette::HighlightedText : QPalette::WindowText;
        painter->setPen(option.palette.color(textRole));
        QFont f = option.font;
        f.setPixelSize(qMin(rect.height() - 4, 14));
        painter->setFont(f);
        const QRect textRect = painter->boundingRect(rect, Qt::AlignRight | Qt::AlignVCenter, icon);
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, icon);

        if (index.data(kFxItemStagedRole).toBool()) {
            const QString tag = QStringLiteral("STAGED");
            QFont tf = option.font;
            tf.setBold(true);
            tf.setPixelSize(qMin(rect.height() - 6, 10));
            painter->setFont(tf);
            const QRect tagArea = rect.adjusted(0, 0, -(textRect.width() + 6), 0);
            QRect tagRect = painter->boundingRect(tagArea, Qt::AlignRight | Qt::AlignVCenter, tag);
            tagRect.adjust(-3, -1, 3, 1);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(255, 160, 0));
            painter->drawRoundedRect(tagRect, 3, 3);
            painter->setPen(Qt::black);
            painter->drawText(tagRect, Qt::AlignCenter, tag);
        }
        painter->restore();
    }
};

static QString DefaultLabelFromTemplateName(const QString &templateName)
{
    QString label = templateName.trimmed();
    int colon = label.indexOf(':');
    if (colon >= 0)
        label = label.mid(colon + 1).trimmed();
    return label;
}

struct TemplateDefaults {
    int fadeMs = 600;
    int lifetimeSec = 0;
    bool sequence = true;
};

static TemplateDefaults LoadTemplateDefaults(const QString &templateUuid)
{
    TemplateDefaults def;
    const QString path = GetDefaultsConfigPath();
    if (path.isEmpty())
        return def;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return def;

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return def;

    QJsonObject root = doc.object();
    QJsonObject tmpl = root.value(templateUuid).toObject();
    if (tmpl.isEmpty())
        return def;

    def.fadeMs = tmpl.value("fadeMs").toInt(def.fadeMs);
    def.lifetimeSec = tmpl.value("lifetimeSec").toInt(def.lifetimeSec);
    def.sequence = tmpl.value("sequence").toBool(def.sequence);
    return def;
}

static void SaveTemplateDefaults(const QString &templateUuid, int fadeMs, int lifetimeSec,
                                 bool sequence)
{
    const QString path = GetDefaultsConfigPath();
    if (path.isEmpty())
        return;

    QJsonObject root;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (doc.isObject())
            root = doc.object();
    }

    QJsonObject tmpl;
    tmpl.insert("fadeMs", fadeMs);
    tmpl.insert("lifetimeSec", lifetimeSec);
    tmpl.insert("sequence", sequence);
    root.insert(templateUuid, tmpl);

    QFile out(path);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
}

static void AddBattlemapScenesToCombo(QComboBox *combo)
{
    combo->clear();

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);

    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t *src = scenes.sources.array[i];
        const char *name = obs_source_get_name(src);
        const QString qname = QString::fromUtf8(name);
        if (!IsFxTemplateSceneName(qname) && IsBattlemapSceneName(qname))
            combo->addItem(qname);
    }

    obs_frontend_source_list_free(&scenes);
}

/** Return scene source by name, or empty if not found / not a scene. Caller can use obs_scene_from_source() when needed. */
static OBSSourceAutoRelease GetSceneSourceByName(const QString &sceneName)
{
    if (sceneName.isEmpty())
        return OBSSourceAutoRelease();
    obs_source_t *src = obs_get_source_by_name(sceneName.toUtf8().constData());
    if (!src || !obs_source_is_scene(src)) {
        if (src)
            obs_source_release(src);
        return OBSSourceAutoRelease();
    }
    return OBSSourceAutoRelease(src);
}

static void AddFxTemplateScenesToCombo(QComboBox *combo)
{
    combo->clear();

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);

    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t *src = scenes.sources.array[i];
        const char *name = obs_source_get_name(src);
        const QString qname = QString::fromUtf8(name);
        if (IsFxTemplateSceneName(qname))
            combo->addItem(qname);
    }

    obs_frontend_source_list_free(&scenes);
}

static int ParseDelayMsFromItemName(const char *name)
{
    if (!name)
        return 0;

    // Convention: "[250] Smoke" means show after 250ms.
    // If no prefix, delay = 0.
    const char *p = name;
    if (*p != '[')
        return 0;
    p++;

    int value = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        digits++;
        p++;
        if (digits > 9)
            return 0;
    }

    if (digits == 0 || *p != ']')
        return 0;

    return value;
}

struct ChildItemSchedule {
    int delayMs = 0;
    int64_t itemId = 0;
};

static std::vector<ChildItemSchedule> BuildChildSchedule(obs_scene_t *scene)
{
    std::vector<ChildItemSchedule> out;
    if (!scene)
        return out;

    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
            auto *vec = static_cast<std::vector<ChildItemSchedule> *>(param);
            if (!item)
                return true;

            obs_source_t *src = obs_sceneitem_get_source(item);
            const char *nm = src ? obs_source_get_name(src) : nullptr;
            if (nm) {
                QString name = QString::fromUtf8(nm);
                // Never sequence FX label items; they start hidden and are shown via context menus.
                if (name.startsWith("FX Label "))
                    return true;
            }

            ChildItemSchedule s;
            s.delayMs = ParseDelayMsFromItemName(nm);
            s.itemId = obs_sceneitem_get_id(item);
            vec->push_back(s);
            return true;
        },
        &out);

    std::sort(out.begin(), out.end(),
              [](const ChildItemSchedule &a, const ChildItemSchedule &b) {
                  if (a.delayMs != b.delayMs)
                      return a.delayMs < b.delayMs;
                  return a.itemId < b.itemId;
              });
    return out;
}

/** Callback for obs_scene_atomic_update: clear hide transition on each item then set invisible. Used for sequencing so we don't trigger the template's hide (fade-out) transitions on the initial hide. */
static void SetAllSceneItemsHiddenForSequenceHelper(void * /*param*/, obs_scene_t *scene)
{
    if (!scene)
        return;
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *) {
            obs_source_t *src = obs_sceneitem_get_source(item);
            const char *nm = src ? obs_source_get_name(src) : nullptr;
            // Labels are never sequenced; keep whatever visibility the user gave them.
            if (nm && QString::fromUtf8(nm).startsWith("FX Label "))
                return true;
            obs_sceneitem_set_transition(item, false, nullptr); // clear hide transition → instant hide
            obs_sceneitem_set_visible(item, false);
            return true;
        },
        nullptr);
}

/** Set all scene items hidden on the graphics thread for sequencing: clear hide transitions first so the initial hide is instant (no fade out), then set visible false. Show transitions are left intact so [time] fade-in still works. */
static void SetAllSceneItemsHiddenForSequenceOnGraphicsThread(obs_scene_t *scene)
{
    if (!scene)
        return;
    obs_enter_graphics();
    obs_scene_atomic_update(scene, SetAllSceneItemsHiddenForSequenceHelper, nullptr);
    obs_leave_graphics();
}

static void RestartMediaForSceneItem(obs_sceneitem_t *item)
{
    if (!item)
        return;
    obs_source_t *src = obs_sceneitem_get_source(item);
    if (!src)
        return;
    obs_source_media_restart(src);
}

static const char *kGridSourceName = "RPG Map Grid";
static const char *kCursorSourceName = "RPG Map Cursor";

/** Find or add grid in the given scene and set visibility. Uses atomic update like the frontend. */
struct GridSceneItemCtx {
    obs_source_t *gridSource = nullptr;
    obs_sceneitem_t *item = nullptr;
};

static bool findGridItemInScene(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
    auto *ctx = static_cast<GridSceneItemCtx *>(param);
    if (!ctx->gridSource)
        return true;
    obs_source_t *src = obs_sceneitem_get_source(item);
    if (src != ctx->gridSource)
        return true;
    ctx->item = item;
    obs_sceneitem_addref(item);
    return false;
}

// --- Cursor helpers ---
/** Find the scene item that uses the cursor source (for add/update). */
struct CursorItemCtx {
    obs_source_t *cursorSource = nullptr;
    obs_sceneitem_t *item = nullptr;
};

static bool findCursorItemInScene(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
    auto *ctx = static_cast<CursorItemCtx *>(param);
    if (!ctx->cursorSource)
        return true;
    if (obs_sceneitem_get_source(item) != ctx->cursorSource)
        return true;
    ctx->item = item;
    obs_sceneitem_addref(item);
    return false;
}

struct AddGridToSceneData {
    obs_source_t *gridSource = nullptr;
    bool visible = false;
};

static void RaiseOverlaysInSceneLocked(obs_scene_t *scene);

/** The grid used to be an image_source with this name; hide it so it doesn't double the grid source. */
static void HideLegacyGridImageLocked(obs_scene_t *scene)
{
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *) {
            obs_source_t *src = obs_sceneitem_get_source(item);
            const char *id = src ? obs_source_get_unversioned_id(src) : nullptr;
            const char *nm = src ? obs_source_get_name(src) : nullptr;
            if (id && nm && strcmp(id, "image_source") == 0 && strcmp(nm, kGridSourceName) == 0)
                obs_sceneitem_set_visible(item, false);
            return true;
        },
        nullptr);
}

static void AddOrShowGridInSceneHelper(void *param, obs_scene_t *scene)
{
    auto *data = static_cast<AddGridToSceneData *>(param);
    if (!data || !data->gridSource || !scene)
        return;

    GridSceneItemCtx ctx;
    ctx.gridSource = data->gridSource;
    obs_scene_enum_items(scene, findGridItemInScene, &ctx);

    if (data->visible)
        HideLegacyGridImageLocked(scene);

    if (ctx.item) {
        obs_sceneitem_set_visible(ctx.item, data->visible);
        if (data->visible) {
            obs_sceneitem_set_order(ctx.item, OBS_ORDER_MOVE_TOP);
            RaiseOverlaysInSceneLocked(scene);
        }
        obs_sceneitem_release(ctx.item);
        return;
    }

    obs_sceneitem_t *item = obs_scene_add(scene, data->gridSource);
    if (item) {
        // The grid is drawn in canvas coordinates; keep it from being dragged around.
        obs_sceneitem_set_locked(item, true);
        obs_sceneitem_set_visible(item, data->visible);
        obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
        RaiseOverlaysInSceneLocked(scene);
    }
}

struct CursorInSceneData {
    obs_source_t *cursorSource = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    bool visible = true;
    bool createIfMissing = true;
    float boundsSize = 48.0f;
    bool updateVisibility = true;
};

static void AddOrUpdateCursorInSceneHelper(void *param, obs_scene_t *scene)
{
    auto *data = static_cast<CursorInSceneData *>(param);
    if (!data || !data->cursorSource || !scene)
        return;

    CursorItemCtx ctx;
    ctx.cursorSource = data->cursorSource;
    obs_scene_enum_items(scene, findCursorItemInScene, &ctx);

    if (ctx.item) {
        struct vec2 pos;
        vec2_set(&pos, data->x, data->y);
        obs_sceneitem_set_pos(ctx.item, &pos);

        struct vec2 bounds;
        vec2_set(&bounds, data->boundsSize, data->boundsSize);
        obs_sceneitem_set_bounds_type(ctx.item, OBS_BOUNDS_SCALE_INNER);
        obs_sceneitem_set_bounds(ctx.item, &bounds);

        if (data->updateVisibility) {
            obs_sceneitem_set_visible(ctx.item, data->visible);
            if (data->visible)
                obs_sceneitem_set_order(ctx.item, OBS_ORDER_MOVE_TOP);
        }
        obs_sceneitem_release(ctx.item);
        return;
    }

    if (!data->visible || !data->createIfMissing)
        return;

    obs_sceneitem_t *item = obs_scene_add(scene, data->cursorSource);
    if (item) {
        struct vec2 pos;
        vec2_set(&pos, data->x, data->y);
        obs_sceneitem_set_pos(item, &pos);
        obs_sceneitem_set_alignment(item, OBS_ALIGN_CENTER);

        struct vec2 bounds;
        vec2_set(&bounds, data->boundsSize, data->boundsSize);
        obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
        obs_sceneitem_set_bounds(item, &bounds);

        obs_sceneitem_set_visible(item, true);
        obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
        obs_sceneitem_release(item);
    }
}

static void HideCursorInAllScenes(obs_source_t *cursorSource)
{
    if (!cursorSource)
        return;

    obs_frontend_source_list list = {};
    obs_frontend_get_scenes(&list);
    for (size_t i = 0; i < list.sources.num; i++) {
        obs_source_t *sceneSrc = list.sources.array[i];
        if (!sceneSrc || !obs_source_is_scene(sceneSrc))
            continue;
        const char *nm = obs_source_get_name(sceneSrc);
        const QString qname = QString::fromUtf8(nm ? nm : "");
        if (!IsBattlemapSceneName(qname))
            continue;
        obs_scene_t *scene = obs_scene_from_source(sceneSrc);
        if (!scene)
            continue;
        CursorInSceneData data;
        data.cursorSource = cursorSource;
        data.visible = false;
        data.createIfMissing = false;
        obs_enter_graphics();
        obs_scene_atomic_update(scene, AddOrUpdateCursorInSceneHelper, &data);
        obs_leave_graphics();
    }
    obs_frontend_source_list_free(&list);
}

/** Add grid to user's battlemap scene and set visibility. Uses obs_scene_atomic_update like OBS frontend. */
static void addGridToSceneAndSetVisible(obs_scene_t *scene, obs_source_t *gridSource, bool visible)
{
    if (!scene || !gridSource)
        return;

    AddGridToSceneData data;
    data.gridSource = gridSource;
    data.visible = visible;

    obs_enter_graphics();
    obs_scene_atomic_update(scene, AddOrShowGridInSceneHelper, &data);
    obs_leave_graphics();
}

/** Move the fog (and then the cursor) above everything else so new FX stay under the fog. Scene lock held. */
static void RaiseOverlaysInSceneLocked(obs_scene_t *scene)
{
    struct Ctx {
        obs_sceneitem_t *fog = nullptr;
        obs_sceneitem_t *cursor = nullptr;
    } ctx;
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
            auto *ctx = static_cast<Ctx *>(param);
            obs_source_t *src = obs_sceneitem_get_source(item);
            if (rpg_is_fog_source(src)) {
                ctx->fog = item;
            } else {
                const char *nm = src ? obs_source_get_name(src) : nullptr;
                if (nm && strcmp(nm, kCursorSourceName) == 0)
                    ctx->cursor = item;
            }
            return true;
        },
        &ctx);
    if (ctx.fog)
        obs_sceneitem_set_order(ctx.fog, OBS_ORDER_MOVE_TOP);
    if (ctx.cursor)
        obs_sceneitem_set_order(ctx.cursor, OBS_ORDER_MOVE_TOP);
}

struct AddFxItemData {
    obs_source_t *fxSource = nullptr;
    float x = 0.0f;
    float y = 0.0f;
    bool visible = true;
    obs_sceneitem_t *outItem = nullptr;
};

/** Add the FX to the map with transform and visibility set in one atomic update, so output never
 *  renders a frame of it at the wrong place or (when staged) visible at all. */
static void AddFxItemHelper(void *param, obs_scene_t *scene)
{
    auto *data = static_cast<AddFxItemData *>(param);
    obs_sceneitem_t *fxItem = obs_scene_add(scene, data->fxSource);
    if (!fxItem)
        return;
    obs_sceneitem_set_visible(fxItem, data->visible);

    vec2 pos;
    vec2_set(&pos, data->x, data->y);
    obs_sceneitem_set_alignment(fxItem, OBS_ALIGN_CENTER);
    obs_sceneitem_set_pos(fxItem, &pos);

    // Spawn in template's natural orientation (OBS 0° = right). Use Rotate to face up or other directions.
    obs_sceneitem_set_rot(fxItem, 0.0f);

    // Default: additive blend over the battlemap.
    obs_sceneitem_set_blending_mode(fxItem, OBS_BLEND_ADDITIVE);

    RaiseOverlaysInSceneLocked(scene);
    data->outItem = fxItem;
}

/** Staged reveal using [ms] prefixes + per-item show transitions. Children must already be hidden. */
static void ScheduleFxSequence(const QString &fxUuidQ)
{
    OBSSourceAutoRelease fxSource(obs_get_source_by_uuid(fxUuidQ.toUtf8().constData()));
    obs_scene_t *fxScene = fxSource.Get() ? obs_scene_from_source(fxSource.Get()) : nullptr;
    if (!fxScene)
        return;

    const std::vector<ChildItemSchedule> sched = BuildChildSchedule(fxScene);
    for (const auto &entry : sched) {
        QTimer::singleShot(entry.delayMs, qApp, [fxUuidQ, entry]() {
            OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(fxUuidQ.toUtf8().constData()));
            if (!fxSrc.Get())
                return;
            obs_scene_t *scene = obs_scene_from_source(fxSrc.Get());
            if (!scene)
                return;
            obs_sceneitem_t *it = obs_scene_find_sceneitem_by_id(scene, entry.itemId);
            if (!it)
                return;
            RestartMediaForSceneItem(it);
            obs_sceneitem_set_visible(it, true);
        });
    }
}

/** Restart all media inside an FX scene (used when a staged FX without sequencing is triggered). */
static void RestartAllMediaInFxScene(obs_scene_t *fxScene)
{
    if (!fxScene)
        return;
    obs_scene_enum_items(
        fxScene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *) {
            RestartMediaForSceneItem(item);
            return true;
        },
        nullptr);
}

/** Spawn an FX instance. When staged, the FX is added hidden on output and its sequence is not
 *  started; RPGWindow::triggerFx() reveals it later. */
static void SpawnFxAtClick(const QString &templateSceneName, obs_source_t *mapSceneSource, float x, float y,
                           bool sequenceByDelay, bool staged, int defaultFadeMs,
                           const QString &labelText, uint32_t labelColorArgb,
                           QString &outMapSceneUuid, QString &outEffectUuid,
                           int64_t &outSceneItemId)
{
    outMapSceneUuid.clear();
    outEffectUuid.clear();
    outSceneItemId = 0;

    if (templateSceneName.isEmpty() || !mapSceneSource)
        return;

    obs_scene_t *mapScene = obs_scene_from_source(mapSceneSource);
    if (!mapScene)
        return;

    obs_source_t *templateSource = obs_get_source_by_name(templateSceneName.toUtf8().constData());
    if (!templateSource)
        return;

    if (!obs_source_is_scene(templateSource)) {
        obs_source_release(templateSource);
        return;
    }

    const QString uniqueName =
        QString("FX Spawn %1 %2")
            .arg(templateSceneName)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));

    obs_source_t *dup = obs_source_duplicate(templateSource, uniqueName.toUtf8().constData(), true);
    obs_source_release(templateSource);
    if (!dup)
        return;

    const char *mapUuid = obs_source_get_uuid(mapSceneSource);
    const char *fxUuid = obs_source_get_uuid(dup);
    if (mapUuid)
        outMapSceneUuid = QString::fromUtf8(mapUuid);
    if (fxUuid)
        outEffectUuid = QString::fromUtf8(fxUuid);

    // If sequencing, hide all children on the graphics thread: clear hide transitions first so we
    // don't trigger the template's fade-out (instant hide), then set invisible. Show transitions
    // stay so [time] fade-in still works. Doing this on the graphics thread avoids a "full on" frame.
    // Staged FX keep their children visible so the GM preview shows the full effect; the sequence
    // runs when the FX is triggered.
    obs_scene_t *fxSceneForChildren = obs_scene_from_source(dup);
    if (sequenceByDelay && !staged && fxSceneForChildren)
        SetAllSceneItemsHiddenForSequenceOnGraphicsThread(fxSceneForChildren);

        // Add a text label inside the FX scene so it moves with the effect.
        // The label is always created (using template name or custom text) but starts hidden;
        // visibility is controlled later via context menus.
        if (!labelText.isEmpty() && fxSceneForChildren) {
            obs_data_t *settings = obs_data_create();
            obs_data_set_string(settings, "text", labelText.toUtf8().constData());

            // Smaller, simple font tuned for map labels
            obs_data_t *font = obs_data_create();
            obs_data_set_string(font, "face", "Arial");
            obs_data_set_string(font, "style", "Regular");
            obs_data_set_int(font, "size", 32); // reasonably small/legible
            obs_data_set_int(font, "flags", 0);
            obs_data_set_obj(settings, "font", font);
            obs_data_release(font);

            // No outline/shadow by default for cleaner labels
            obs_data_set_bool(settings, "outline", false);
            obs_data_set_bool(settings, "drop_shadow", false);

            if (labelColorArgb != 0) {
                obs_data_set_int(settings, "color1", (int64_t)labelColorArgb);
                obs_data_set_int(settings, "color2", (int64_t)labelColorArgb);
            }

            QByteArray nameUtf8 =
                QString("FX Label %1").arg(labelText).toUtf8();
            obs_source_t *labelSrc =
                obs_source_create("text_ft2_source_v2", nameUtf8.constData(), settings, nullptr);
            if (!labelSrc) {
                // Fallback for older builds/plugins
                labelSrc =
                    obs_source_create("text_ft2_source", nameUtf8.constData(), settings, nullptr);
            }
            obs_data_release(settings);

            if (labelSrc) {
                obs_sceneitem_t *labelItem = obs_scene_add(fxSceneForChildren, labelSrc);
                if (labelItem) {
                    obs_video_info ovi = {};
                    if (obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
                        const float cx = float(ovi.base_width) * 0.5f;
                        const float cy = float(ovi.base_height) * 0.5f;
                        vec2 lpos;
                        vec2_set(&lpos, cx, cy);
                        obs_sceneitem_set_alignment(labelItem, OBS_ALIGN_CENTER);
                        obs_sceneitem_set_pos(labelItem, &lpos);
                    } else {
                        vec2 lpos;
                        vec2_set(&lpos, 0.0f, 0.0f);
                        obs_sceneitem_set_alignment(labelItem, OBS_ALIGN_CENTER);
                        obs_sceneitem_set_pos(labelItem, &lpos);
                    }

                    obs_sceneitem_set_visible(labelItem, false);
                    obs_sceneitem_set_order(labelItem, OBS_ORDER_MOVE_TOP);
                }
                obs_source_release(labelSrc);
            }
        }

    AddFxItemData addData;
    addData.fxSource = dup;
    addData.x = x;
    addData.y = y;
    addData.visible = !staged;
    obs_enter_graphics();
    obs_scene_atomic_update(mapScene, AddFxItemHelper, &addData);
    obs_leave_graphics();
    obs_sceneitem_t *fxItem = addData.outItem;

    // The scene now owns it; drop our strong ref.
    obs_source_release(dup);

    if (!fxItem)
        return;

    outSceneItemId = obs_sceneitem_get_id(fxItem);

    // Configure an automatic fade-out hide transition on the FX scene item.
    if (defaultFadeMs > 0) {
        // Use OBS's built-in fade transition type if available.
        const char *fadeId = nullptr;
        size_t idx = 0;
        while (obs_enum_transition_types(idx++, &fadeId)) {
            if (fadeId && strcmp(fadeId, "fade_transition") == 0)
                break;
            fadeId = nullptr;
        }

        if (fadeId) {
            obs_data_t *settings = obs_data_create();
            obs_source_t *fadeTr =
                obs_source_create_private(fadeId, "RPG FX Fade Transition", settings);
            obs_data_release(settings);

            if (fadeTr) {
                // false = hide transition
                obs_sceneitem_set_transition(fxItem, false, fadeTr);
                obs_sceneitem_set_transition_duration(fxItem, false,
                                                      (uint32_t)defaultFadeMs);
                obs_source_release(fadeTr);
            }
        }
    }

    if (sequenceByDelay && !staged)
        ScheduleFxSequence(outEffectUuid);
}

// --- FX label helpers (DefaultLabelFromTemplateName above used for spawn placeholder) ---
struct FxLabelUpdateCtx {
    QString text;
    uint32_t colorArgb = 0;
};

static void UpdateFxLabelSourcesByEffectUuid(const QString &effectUuid, const QString &labelText,
                                             uint32_t colorArgb)
{
    if (effectUuid.isEmpty())
        return;

    OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(effectUuid.toUtf8().constData()));
    if (!fxSrc.Get())
        return;
    obs_scene_t *scene = obs_scene_from_source(fxSrc.Get());
    if (!scene)
        return;

    FxLabelUpdateCtx ctx{labelText, colorArgb};
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
            auto *ctx = static_cast<FxLabelUpdateCtx *>(param);
            obs_source_t *src = obs_sceneitem_get_source(item);
            if (!src)
                return true;
            const char *nm = obs_source_get_name(src);
            if (!nm)
                return true;
            QString name = QString::fromUtf8(nm);
            if (!name.startsWith("FX Label "))
                return true;
            obs_data_t *settings = obs_source_get_settings(src);
            if (!settings)
                return true;
            obs_data_set_string(settings, "text", ctx->text.toUtf8().constData());
            if (ctx->colorArgb != 0) {
                obs_data_set_int(settings, "color1", (int64_t)ctx->colorArgb);
                obs_data_set_int(settings, "color2", (int64_t)ctx->colorArgb);
            }
            obs_source_update(src, settings);
            obs_data_release(settings);
            return true;
        },
        &ctx);
}

static void SetFxLabelsVisibleByEffectUuid(const QString &effectUuid, bool visible)
{
    if (effectUuid.isEmpty())
        return;

    OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(effectUuid.toUtf8().constData()));
    if (!fxSrc.Get())
        return;
    obs_scene_t *scene = obs_scene_from_source(fxSrc.Get());
    if (!scene)
        return;

    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
            const bool visible = *static_cast<bool *>(param);
            obs_source_t *src = obs_sceneitem_get_source(item);
            if (!src)
                return true;
            const char *nm = obs_source_get_name(src);
            if (!nm)
                return true;
            QString name = QString::fromUtf8(nm);
            if (!name.startsWith("FX Label "))
                return true;
            obs_sceneitem_set_visible(item, visible);
            return true;
        },
        (void *)&visible);
}

static void ClearLastFx(QString mapSceneUuid, QString fxUuid, int64_t sceneItemId)
{
    if (mapSceneUuid.isEmpty() || fxUuid.isEmpty() || sceneItemId == 0)
        return;

    OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(mapSceneUuid.toUtf8().constData()));
    if (mapSrc.Get()) {
        obs_scene_t *scene = obs_scene_from_source(mapSrc.Get());
        if (scene) {
            obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, sceneItemId);
            if (item)
                obs_sceneitem_remove(item);
        }
    }

    OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(fxUuid.toUtf8().constData()));
    if (fxSrc.Get())
        obs_source_remove(fxSrc.Get());
}

void RPGWindow::fadeOutAndRemoveInstance(const FxInstance &inst, int fadeMs)
{
    if (inst.mapSceneUuid.isEmpty() || inst.effectUuid.isEmpty() || inst.sceneItemId == 0)
        return;

    releaseStagedShowing(inst.effectUuid);

    // Start hide (triggers any configured hide transitions).
    {
        OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(inst.mapSceneUuid.toUtf8().constData()));
        if (mapSrc.Get()) {
            obs_scene_t *scene = obs_scene_from_source(mapSrc.Get());
            if (scene) {
                obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, inst.sceneItemId);
                if (item)
                    obs_sceneitem_set_visible(item, false);
            }
        }
    }

    releaseFxLockAndClearArrow();

    // After fadeMs, actually remove the scene item and FX source.
    QTimer::singleShot(fadeMs, qApp, [inst]() {
        ClearLastFx(inst.mapSceneUuid, inst.effectUuid, inst.sceneItemId);
    });
}

int RPGWindow::findNearestFxInstanceIndex(float sceneX, float sceneY) const
{
    if (activeFx.empty())
        return -1;

    const float maxRadius = 200.0f; // pixels in canvas space
    const float maxDist2 = maxRadius * maxRadius;
    int bestIndex = -1;
    float bestDist2 = maxDist2;

    for (size_t i = 0; i < activeFx.size(); ++i) {
        const FxInstance &inst = activeFx[i];
        obs_sceneitem_t *item = getSceneItemForInstance(inst);
        if (!item)
            continue;

        vec2 pos;
        obs_sceneitem_get_pos(item, &pos);
        OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(inst.mapSceneUuid.toUtf8().constData()));
        if (!mapSrc.Get())
            continue;
        float cx = 0.0f, cy = 0.0f;
        if (!scenePosToCanvas(mapSrc.Get(), pos.x, pos.y, cx, cy))
            continue;

        const float dx = cx - sceneX;
        const float dy = cy - sceneY;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestDist2) {
            bestDist2 = d2;
            bestIndex = static_cast<int>(i);
        }
    }

    return bestIndex;
}

QString RPGWindow::getMapSceneUuidFromName(const QString &sceneName) const
{
    OBSSourceAutoRelease src = GetSceneSourceByName(sceneName);
    if (!src.Get())
        return {};
    const char *uuid = obs_source_get_uuid(src.Get());
    return uuid ? QString::fromUtf8(uuid) : QString();
}

void RPGWindow::refreshFxListForCurrentMap()
{
    activeFx.clear();
    if (fxList)
        fxList->clear();
    auto it = fxByMapSceneUuid_.find(currentMapSceneUuid_);
    if (currentMapSceneUuid_.isEmpty() || it == fxByMapSceneUuid_.end()) {
        updateStagedOverlay();
        return;
    }
    for (const FxInstance &inst : it.value()) {
        activeFx.push_back(inst);
        if (fxList)
            fxList->addItem(makeFxListItem(inst));
    }
    updateStagedOverlay();
}

QListWidgetItem *RPGWindow::makeFxListItem(const FxInstance &inst) const
{
    const QString lockedPrefix = QStringLiteral("[Locked] ");
    QString listText = inst.label.isEmpty() ? inst.templateName : inst.label;
    if (listText.startsWith(lockedPrefix))
        listText = listText.mid(lockedPrefix.size()).trimmed();
    auto *item = new QListWidgetItem(listText);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    item->setData(kFxItemLockedRole, inst.locked);
    item->setData(kFxItemStagedRole, inst.staged);
    return item;
}

bool RPGWindow::sameFx(const FxInstance &a, const FxInstance &b)
{
    return a.effectUuid == b.effectUuid && a.sceneItemId == b.sceneItemId;
}

void RPGWindow::removeFxFromPerMapStore(const FxInstance &inst)
{
    auto it = fxByMapSceneUuid_.find(inst.mapSceneUuid);
    if (it == fxByMapSceneUuid_.end())
        return;
    std::vector<FxInstance> &vec = it.value();
    for (size_t i = 0; i < vec.size(); ++i) {
        if (sameFx(vec[i], inst)) {
            vec.erase(vec.begin() + (long)i);
            break;
        }
    }
}

void RPGWindow::syncStoredLabel(const FxInstance &inst, const QString &newLabel)
{
    auto it = fxByMapSceneUuid_.find(inst.mapSceneUuid);
    if (it == fxByMapSceneUuid_.end())
        return;
    for (FxInstance &stored : it.value()) {
        if (sameFx(stored, inst)) {
            stored.label = newLabel;
            break;
        }
    }
}

void RPGWindow::syncStoredLocked(const FxInstance &inst, bool locked)
{
    auto it = fxByMapSceneUuid_.find(inst.mapSceneUuid);
    if (it == fxByMapSceneUuid_.end())
        return;
    for (FxInstance &stored : it.value()) {
        if (sameFx(stored, inst)) {
            stored.locked = locked;
            break;
        }
    }
}

void RPGWindow::releaseFxLockAndClearArrow()
{
    fxLockMode_ = FxLockMode::None;
    if (display)
        display->clearDirectionArrow();
}

obs_sceneitem_t *RPGWindow::getSceneItemForInstance(const FxInstance &inst) const
{
    if (inst.mapSceneUuid.isEmpty() || inst.sceneItemId == 0)
        return nullptr;
    OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(inst.mapSceneUuid.toUtf8().constData()));
    if (!mapSrc.Get())
        return nullptr;
    obs_scene_t *scene = obs_scene_from_source(mapSrc.Get());
    if (!scene)
        return nullptr;
    return obs_scene_find_sceneitem_by_id(scene, inst.sceneItemId);
}

bool RPGWindow::canvasToScene(obs_source_t *mapSource, float canvasX, float canvasY,
                              float &outSceneX, float &outSceneY) const
{
    obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
        return false;
    const uint32_t sceneW = obs_source_get_width(mapSource);
    const uint32_t sceneH = obs_source_get_height(mapSource);
    if (sceneW == 0 || sceneH == 0)
        return false;
    outSceneX = canvasX * float(sceneW) / float(ovi.base_width);
    outSceneY = canvasY * float(sceneH) / float(ovi.base_height);
    return true;
}

bool RPGWindow::scenePosToCanvas(obs_source_t *mapSource, float posX, float posY,
                                float &outCX, float &outCY) const
{
    obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
        return false;
    const uint32_t baseW = ovi.base_width;
    const uint32_t baseH = ovi.base_height;
    const uint32_t sceneW = obs_source_get_width(mapSource);
    const uint32_t sceneH = obs_source_get_height(mapSource);
    if (sceneW == 0 || sceneH == 0)
        return false;
    outCX = posX * float(baseW) / float(sceneW);
    outCY = posY * float(baseH) / float(sceneH);
    return true;
}

void RPGWindow::clearFxAtRow(int row, int fadeMs)
{
    if (row < 0 || row >= (int)activeFx.size() || !fxList)
        return;
    const FxInstance &inst = activeFx[static_cast<size_t>(row)];
    fadeOutAndRemoveInstance(inst, fadeMs);
    removeFxFromPerMapStore(inst);
    activeFx.erase(activeFx.begin() + row);
    delete fxList->takeItem(row);
    releaseFxLockAndClearArrow();
    updateStagedOverlay();
}

// --- Staged FX ---

void RPGWindow::releaseStagedShowing(const QString &effectUuid)
{
    if (!stagedShowingUuids_.remove(effectUuid))
        return;
    OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(effectUuid.toUtf8().constData()));
    if (fxSrc.Get())
        obs_source_dec_showing(fxSrc.Get());
}

void RPGWindow::scheduleLifetime(const FxInstance &inst, int lifetimeSec, int fadeMs)
{
    if (lifetimeSec <= 0)
        return;
    QTimer::singleShot(lifetimeSec * 1000, this, [this, inst, fadeMs]() {
        auto it = fxByMapSceneUuid_.find(inst.mapSceneUuid);
        if (it == fxByMapSceneUuid_.end())
            return;
        std::vector<FxInstance> &vec = it.value();
        for (size_t i = 0; i < vec.size(); ++i) {
            if (sameFx(vec[i], inst)) {
                fadeOutAndRemoveInstance(vec[i], fadeMs);
                removeFxFromPerMapStore(inst);
                if (inst.mapSceneUuid == currentMapSceneUuid_) {
                    for (size_t j = 0; j < activeFx.size(); ++j) {
                        if (sameFx(activeFx[j], inst)) {
                            activeFx.erase(activeFx.begin() + (long)j);
                            if (fxList)
                                delete fxList->takeItem((int)j);
                            break;
                        }
                    }
                }
                break;
            }
        }
    });
}

void RPGWindow::triggerFx(const FxInstance &instIn)
{
    if (!instIn.staged)
        return;
    const FxInstance inst = instIn; // copy: the stored entries are updated below
    obs_sceneitem_t *item = getSceneItemForInstance(inst);
    if (!item)
        return;

    OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(inst.effectUuid.toUtf8().constData()));
    obs_scene_t *fxScene = fxSrc.Get() ? obs_scene_from_source(fxSrc.Get()) : nullptr;
    if (inst.sequence && fxScene)
        SetAllSceneItemsHiddenForSequenceOnGraphicsThread(fxScene);
    else
        RestartAllMediaInFxScene(fxScene);

    obs_sceneitem_set_visible(item, true);
    if (inst.sequence)
        ScheduleFxSequence(inst.effectUuid);
    releaseStagedShowing(inst.effectUuid);

    auto mapIt = fxByMapSceneUuid_.find(inst.mapSceneUuid);
    if (mapIt != fxByMapSceneUuid_.end()) {
        for (FxInstance &stored : mapIt.value())
            if (sameFx(stored, inst))
                stored.staged = false;
    }
    for (size_t i = 0; i < activeFx.size(); ++i) {
        if (sameFx(activeFx[i], inst)) {
            activeFx[i].staged = false;
            if (fxList && (int)i < fxList->count())
                fxList->item((int)i)->setData(kFxItemStagedRole, false);
        }
    }

    scheduleLifetime(inst, inst.lifetimeSec, inst.fadeMs);
    updateStagedOverlay();
}

void RPGWindow::triggerAllStaged()
{
    std::vector<FxInstance> staged;
    for (const FxInstance &inst : activeFx)
        if (inst.staged)
            staged.push_back(inst);
    for (const FxInstance &inst : staged)
        triggerFx(inst);
}

void RPGWindow::triggerNextStaged()
{
    for (const FxInstance &inst : activeFx) {
        if (inst.staged) {
            triggerFx(inst);
            return;
        }
    }
}

void RPGWindow::updateStagedOverlay()
{
    std::vector<int64_t> ids;
    for (const FxInstance &inst : activeFx)
        if (inst.staged)
            ids.push_back(inst.sceneItemId);
    if (display)
        display->setStagedItemIds(ids);
    if (triggerAllBtn_) {
        triggerAllBtn_->setText(ids.empty() ? QStringLiteral("Trigger all staged")
                                            : QStringLiteral("Trigger all staged (%1)").arg(ids.size()));
        triggerAllBtn_->setEnabled(!ids.empty());
    }
    if (triggerSelectedBtn_ && fxList) {
        const int row = fxList->currentRow();
        triggerSelectedBtn_->setEnabled(row >= 0 && row < (int)activeFx.size() &&
                                        activeFx[static_cast<size_t>(row)].staged);
    }
}

// --- Fog of war ---

/** Fog item in the scene (addref'd; caller releases), or nullptr. */
static obs_sceneitem_t *FindFogItemInScene(obs_scene_t *scene)
{
    obs_sceneitem_t *found = nullptr;
    obs_scene_enum_items(
        scene,
        [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
            if (!rpg_is_fog_source(obs_sceneitem_get_source(item)))
                return true;
            obs_sceneitem_addref(item);
            *static_cast<obs_sceneitem_t **>(param) = item;
            return false;
        },
        &found);
    return found;
}

static void AddFogItemHelper(void *param, obs_scene_t *scene)
{
    auto *fog = static_cast<obs_source_t *>(param);
    obs_sceneitem_t *item = obs_scene_add(scene, fog);
    if (!item)
        return;
    vec2 pos;
    vec2_set(&pos, 0.0f, 0.0f);
    obs_sceneitem_set_pos(item, &pos);
    obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
    // Brush coordinates assume the fog covers the canvas 1:1, so keep it from being dragged around.
    obs_sceneitem_set_locked(item, true);
    obs_sceneitem_set_visible(item, true);
    RaiseOverlaysInSceneLocked(scene);
}

OBSSource RPGWindow::fogSourceForCurrentMap(bool create)
{
    if (!sceneCombo_)
        return {};
    const QString mapName = sceneCombo_->currentText();
    OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(mapName);
    obs_scene_t *scene = sceneSrc.Get() ? obs_scene_from_source(sceneSrc.Get()) : nullptr;
    if (!scene)
        return {};

    if (obs_sceneitem_t *item = FindFogItemInScene(scene)) {
        OBSSource fog = obs_sceneitem_get_source(item);
        obs_sceneitem_release(item);
        return fog;
    }
    if (!create)
        return {};

    // Reuse a fog source left over from this map (e.g. removed from the scene), else make one.
    const QString baseName = QStringLiteral("RPG Fog: %1").arg(mapName);
    QString name = baseName;
    OBSSourceAutoRelease fog;
    for (int n = 2;; ++n) {
        OBSSourceAutoRelease existing(obs_get_source_by_name(name.toUtf8().constData()));
        if (!existing.Get()) {
            fog = OBSSourceAutoRelease(obs_source_create(RPG_FOG_SOURCE_ID, name.toUtf8().constData(), nullptr, nullptr));
            break;
        }
        if (rpg_is_fog_source(existing.Get())) {
            fog = std::move(existing);
            break;
        }
        name = QStringLiteral("%1 (%2)").arg(baseName).arg(n);
    }
    if (!fog.Get())
        return {};

    obs_enter_graphics();
    obs_scene_atomic_update(scene, AddFogItemHelper, fog.Get());
    obs_leave_graphics();
    return OBSSource(fog.Get());
}

void RPGWindow::setFogEnabledForCurrentMap(bool enabled)
{
    if (enabled) {
        currentFog_ = fogSourceForCurrentMap(true);
    }
    OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneCombo_ ? sceneCombo_->currentText() : QString());
    obs_scene_t *scene = sceneSrc.Get() ? obs_scene_from_source(sceneSrc.Get()) : nullptr;
    if (scene) {
        if (obs_sceneitem_t *item = FindFogItemInScene(scene)) {
            obs_sceneitem_set_visible(item, enabled);
            obs_sceneitem_release(item);
        }
    }
    if (!enabled)
        setFogTool(FogTool::None);
    syncFogUiForCurrentMap();
}

void RPGWindow::syncFogUiForCurrentMap()
{
    currentFog_ = fogSourceForCurrentMap(false);

    bool visible = false;
    OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneCombo_ ? sceneCombo_->currentText() : QString());
    obs_scene_t *scene = sceneSrc.Get() ? obs_scene_from_source(sceneSrc.Get()) : nullptr;
    if (scene) {
        if (obs_sceneitem_t *item = FindFogItemInScene(scene)) {
            visible = obs_sceneitem_visible(item);
            obs_sceneitem_release(item);
        }
    }
    if (fogEnableCheck_) {
        const QSignalBlocker block(fogEnableCheck_);
        fogEnableCheck_->setChecked(visible);
    }
    if (currentFog_.Get()) {
        OBSDataAutoRelease settings = obs_source_get_settings(currentFog_.Get());
        if (fogGmOpacitySpin_) {
            const QSignalBlocker block(fogGmOpacitySpin_);
            fogGmOpacitySpin_->setValue((int)obs_data_get_int(settings, "gm_opacity"));
        }
        if (fogColorBtn_) {
            const uint32_t c = (uint32_t)obs_data_get_int(settings, "color");
            fogColorBtn_->setIcon(iconFromColor(QColor(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff)));
        }
    }
    if (!visible && fogTool_ != FogTool::None)
        setFogTool(FogTool::None);
}

void RPGWindow::setFogTool(FogTool tool)
{
    if (tool != FogTool::None) {
        // Painting needs fog on this map: turn it on rather than silently doing nothing.
        if (!currentFog_.Get() || (fogEnableCheck_ && !fogEnableCheck_->isChecked()))
            setFogEnabledForCurrentMap(true);
        if (!currentFog_.Get())
            tool = FogTool::None;
        // Fog painting and click-to-spawn both use left click; the last one chosen wins.
        if (spawnMode_ && spawnBtn_) {
            spawnMode_ = false;
            spawnBtn_->setChecked(false);
            spawnLabel_->setText("Click to spawn");
        }
    }
    fogTool_ = tool;
    fogStrokeActive_ = false;
    if (fogBrushBtn_)
        fogBrushBtn_->setChecked(tool == FogTool::Brush);
    if (fogRectBtn_)
        fogRectBtn_->setChecked(tool == FogTool::Rect);
    if (display) {
        if (tool != FogTool::Brush)
            display->setBrushOverlay(false, 0, 0, 0);
        display->setRectOverlay(false, 0, 0, 0, 0);
    }
}

bool RPGWindow::fogRevealMode() const
{
    const bool reveal = !fogRevealRadio_ || fogRevealRadio_->isChecked();
    // Holding Alt temporarily inverts the mode (quick touch-ups without switching radio buttons).
    return (QApplication::keyboardModifiers() & Qt::AltModifier) ? !reveal : reveal;
}

void RPGWindow::setFogSetting(const char *name, long long value)
{
    if (!currentFog_.Get())
        return;
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_int(settings, name, value);
    obs_source_update(currentFog_.Get(), settings);
}

RPGWindow::RPGWindow()
{
    setWindowTitle("RPG Map Effects");
    resize(1200, 720);

    // Compact style: smaller font and controls so everything fits
    setStyleSheet(
        "QWidget { font-size: 11px; } "
        "QToolBar { spacing: 2px; padding: 1px 2px; } "
        "QComboBox, QSpinBox, QLineEdit { min-height: 22px; max-height: 22px; } "
        "QComboBox { min-width: 120px; } "
        "QToolButton { min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px; padding: 0; border: none; } "
        "QCheckBox { spacing: 4px; } "
        "QGroupBox { font-weight: bold; margin-top: 4px; padding-top: 4px; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 6px; padding: 0 4px; } "
    );

    // Central layout: map display on the left, FX controls on the right.
    auto *central = new QWidget(this);
    auto *hLayout = new QHBoxLayout(central);
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);

    display = new OBSDisplayWidget(central);
    display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    hLayout->addWidget(display, /*stretch*/ 3);

    auto *fxPanel = new QWidget(central);
    auto *fxLayout = new QVBoxLayout(fxPanel);
    fxLayout->setContentsMargins(4, 4, 4, 4);
    fxLayout->setSpacing(3);
    fxLayout->setAlignment(Qt::AlignTop);
    hLayout->addWidget(fxPanel, /*stretch*/ 1);

    setCentralWidget(central);

    auto *tb = addToolBar("Controls");
    tb->setMovable(false);
    tb->setIconSize(QSize(16, 16));

    tb->addWidget(new QLabel("Scene:", this));

    sceneCombo_ = new QComboBox(this);
    sceneCombo_->setMinimumWidth(160);
    sceneCombo_->setMaximumWidth(220);
    tb->addWidget(sceneCombo_);

    AddBattlemapScenesToCombo(sceneCombo_);

    // On open/refresh: hide cursor in all battlemap scenes so preview starts clean.
    {
        obs_source_t *cursor = obs_get_source_by_name(kCursorSourceName);
        if (cursor) {
            HideCursorInAllScenes(cursor);
            obs_source_release(cursor);
        }
    }

    QObject::connect(sceneCombo_, &QComboBox::currentTextChanged, this,
                     [this](const QString &sceneName) {
                         display->setSceneByName(sceneName);
                         currentMapSceneUuid_ = getMapSceneUuidFromName(sceneName);
                         refreshFxListForCurrentMap();
                         releaseFxLockAndClearArrow();
                         fogStrokeActive_ = false;
                         syncFogUiForCurrentMap();
                         OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneName);
                         if (sceneSrc.Get())
                             obs_frontend_set_current_scene(sceneSrc.Get());
                     });

    tb->addSeparator();

    auto *gridCheck = new QCheckBox("Show grid", this);
    gridCheck->setChecked(false);
    tb->addWidget(gridCheck);

    auto *gridCellSpin = gridCellSpin_ = new QSpinBox(this);
    gridCellSpin->setRange(10, 500);
    gridCellSpin->setSingleStep(10);
    gridCellSpin->setValue(50);
    gridCellSpin->setSuffix(" px");
    tb->addWidget(gridCellSpin);

    auto *gridColorBtn = new QToolButton(this);
    gridColorBtn->setIcon(iconFromColor(display->gridColor()));
    gridColorBtn->setToolTip("Grid color");
    gridColorBtn->setFixedSize(kToolButtonSize, kToolButtonSize);
    gridColorBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tb->addWidget(gridColorBtn);

    auto *gridWidthSpin = gridWidthSpin_ = new QSpinBox(this);
    gridWidthSpin->setRange(1, 10);
    gridWidthSpin->setValue(1);
    gridWidthSpin->setSuffix(" px");
    tb->addWidget(new QLabel("Line:", this));
    tb->addWidget(gridWidthSpin);

    auto *gridOpacitySpin = gridOpacitySpin_ = new QSpinBox(this);
    gridOpacitySpin->setRange(5, 100);
    gridOpacitySpin->setSingleStep(5);
    gridOpacitySpin->setValue(100);
    gridOpacitySpin->setSuffix("%");
    gridOpacitySpin->setToolTip(QStringLiteral("Grid line opacity (preview and output)"));
    tb->addWidget(new QLabel("Opacity:", this));
    tb->addWidget(gridOpacitySpin);

    // Alignment: offset the grid so its squares line up with the squares printed on the map.
    auto *gridAlignBtn = new QPushButton(QStringLiteral("Align"), this);
    gridAlignBtn->setToolTip(QStringLiteral("Line the grid up with the squares on your map"));
    tb->addWidget(gridAlignBtn);
    gridAlignPopup_ = new QFrame(this, Qt::Popup);
    gridAlignPopup_->setFrameStyle(QFrame::StyledPanel);
    {
        auto *alignLayout = new QGridLayout(gridAlignPopup_);
        alignLayout->setContentsMargins(8, 8, 8, 8);
        gridOffsetXSpin_ = new QSpinBox(gridAlignPopup_);
        gridOffsetYSpin_ = new QSpinBox(gridAlignPopup_);
        for (QSpinBox *spin : {gridOffsetXSpin_, gridOffsetYSpin_}) {
            spin->setRange(0, 499);
            spin->setSuffix(" px");
        }
        auto *clickAlignBtn = new QPushButton(QStringLiteral("Click a square corner on the map"), gridAlignPopup_);
        clickAlignBtn->setToolTip(QStringLiteral("Then click the corner of any square on your map; the grid snaps to it"));
        auto *resetAlignBtn = new QPushButton(QStringLiteral("Reset"), gridAlignPopup_);
        alignLayout->addWidget(new QLabel(QStringLiteral("Offset X:"), gridAlignPopup_), 0, 0);
        alignLayout->addWidget(gridOffsetXSpin_, 0, 1);
        alignLayout->addWidget(new QLabel(QStringLiteral("Offset Y:"), gridAlignPopup_), 1, 0);
        alignLayout->addWidget(gridOffsetYSpin_, 1, 1);
        alignLayout->addWidget(clickAlignBtn, 2, 0, 1, 2);
        alignLayout->addWidget(resetAlignBtn, 3, 0, 1, 2);
        QObject::connect(clickAlignBtn, &QAbstractButton::clicked, this, [this]() {
            gridAlignMode_ = true;
            gridAlignPopup_->close();
            statusBar()->showMessage(QStringLiteral("Click the corner of a square on the map to align the grid"), 8000);
        });
        QObject::connect(resetAlignBtn, &QAbstractButton::clicked, this, [this]() {
            gridOffsetXSpin_->setValue(0);
            gridOffsetYSpin_->setValue(0);
        });
    }
    QObject::connect(gridAlignBtn, &QAbstractButton::clicked, this, [this, gridAlignBtn]() {
        gridAlignPopup_->adjustSize();
        gridAlignPopup_->move(gridAlignBtn->mapToGlobal(QPoint(0, gridAlignBtn->height())));
        gridAlignPopup_->show();
    });

    auto *snapCheck = new QCheckBox("Snap to grid", this);
    snapCheck->setChecked(false);
    tb->addWidget(snapCheck);

    auto *gridOnOutputCheck = new QCheckBox("Grid on output", this);
    gridOnOutputCheck->setChecked(false);
    gridOnOutputCheck->setToolTip(QStringLiteral("Adds the grid to your selected battlemap scene. Check to show on output, uncheck to hide. After switching battlemaps, uncheck then check again to move the grid to the new scene."));
    tb->addWidget(gridOnOutputCheck);

    tb->addSeparator();

    // Status bar for live mouse / last click coordinates + version info.
    auto *mouseLabel = new QLabel(this);
    mouseLabel->setText("Mouse: —");
    auto *clickLabel = new QLabel(this);
    clickLabel->setText("Click: —");

    auto *verLabel = new QLabel(this);
    statusBar()->addPermanentWidget(mouseLabel, 1);
    statusBar()->addPermanentWidget(clickLabel, 1);
    statusBar()->addPermanentWidget(verLabel, 0);

    auto updateGridOverlay = [this, gridCheck, gridCellSpin, gridWidthSpin, gridOnOutputCheck]() {
        // The on-output grid source skips this preview; the preview draws the same grid itself,
        // snapped to its own pixels so it stays even when a large canvas is scaled down.
        display->setGridOverlay(gridCheck->isChecked() || gridOnOutputCheck->isChecked(), gridCellSpin->value());
        display->setGridLineWidth(gridWidthSpin->value());
        display->setGridOpacity(gridOpacitySpin_->value());
        display->setGridOffset(gridOffsetXSpin_->value(), gridOffsetYSpin_->value());
    };
    QObject::connect(gridCheck, &QCheckBox::toggled, this, [updateGridOverlay](bool) { updateGridOverlay(); });
    QObject::connect(gridOnOutputCheck, &QCheckBox::toggled, this, [updateGridOverlay](bool) { updateGridOverlay(); });
    for (QSpinBox *spin : {gridCellSpin, gridWidthSpin, gridOpacitySpin, gridOffsetXSpin_, gridOffsetYSpin_}) {
        QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, updateGridOverlay](int) {
            updateGridOverlay();
            saveGridSettings();
        });
    }

    auto syncGridOutput = [this, gridOnOutputCheck]() {
        if (!gridOnOutputCheck->isChecked())
            return;
        ensureGridSource();
        if (gridSource_.Get()) {
            updateGridSourceSettings(gridSource_.Get());
            syncGridOutputToScene(sceneCombo_->currentText(), true);
        }
    };

    QObject::connect(gridOnOutputCheck, &QCheckBox::toggled, this,
                     [this](bool checked) {
                         const QString sceneName = sceneCombo_->currentText();
                         QTimer::singleShot(0, this, [this, checked, sceneName]() {
                             if (checked) {
                                 ensureGridSource();
                                 if (gridSource_.Get()) {
                                     updateGridSourceSettings(gridSource_.Get());
                                     syncGridOutputToScene(sceneName, true);
                                     blog(LOG_INFO, "[rpg-map-effects] Grid on output: added to scene \"%s\".", sceneName.toUtf8().constData());
                                 }
                             } else {
                                 syncGridOutputToScene(sceneName, false);
                             }
                         });
                     });

    // Do NOT sync grid when switching battlemaps: it can crash when scenes share sources
    // or use move transitions (graphics thread can hit detach_sceneitem with invalid state).
    // Grid is only added/updated when the user checks "Show grid on output" or changes grid settings.
    // To get the grid on a different battlemap, uncheck then check "Show grid on output" after switching.

    for (QSpinBox *spin : {gridCellSpin, gridWidthSpin, gridOpacitySpin, gridOffsetXSpin_, gridOffsetYSpin_})
        QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this,
                         [syncGridOutput](int) { syncGridOutput(); });
    QObject::connect(gridColorBtn, &QAbstractButton::clicked, this, [this, gridColorBtn, syncGridOutput]() {
        const QColor current = display->gridColor();
        QColor chosen = QColorDialog::getColor(current, this, "Choose grid color");
        if (chosen.isValid()) {
            display->setGridColor(chosen);
            gridColorBtn->setIcon(iconFromColor(chosen));
            saveGridSettings();
            syncGridOutput();
        }
    });

    // Restore the last grid settings (signals blocked: nothing is on output yet).
    {
        for (QSpinBox *spin : {gridCellSpin, gridWidthSpin, gridOpacitySpin, gridOffsetXSpin_, gridOffsetYSpin_})
            spin->blockSignals(true);
        loadGridSettings();
        for (QSpinBox *spin : {gridCellSpin, gridWidthSpin, gridOpacitySpin, gridOffsetXSpin_, gridOffsetYSpin_})
            spin->blockSignals(false);
        gridColorBtn->setIcon(iconFromColor(display->gridColor()));
        updateGridOverlay();
    }

    QObject::connect(display, &OBSDisplayWidget::sceneMouseMoved, this,
                     [this, mouseLabel, gridCheck, gridCellSpin, snapCheck](float x, float y,
                                                                 float nx,
                                                                 float ny,
                                                                 bool inside,
                                                                 bool leftButtonDown) {
                         Q_UNUSED(leftButtonDown);
                         // Live mouse tracking in status bar (similar to click/grid readout).
                         {
                             QString text;
                             if (!inside) {
                                 text = QStringLiteral("Mouse: (outside map)");
                             } else {
                                 // Derive normalized coords if not provided (fallback), then cell if grid enabled.
                                 obs_video_info ovi = {};
                                 float nnx = nx;
                                 float nny = ny;
                                 if ((!std::isfinite(nnx) || !std::isfinite(nny)) &&
                                     obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0) {
                                     nnx = x / float(ovi.base_width);
                                     nny = y / float(ovi.base_height);
                                 }
                                 text = QString("Mouse: x=%1 y=%2  |  n=(%3, %4)")
                                            .arg(x, 0, 'f', 1)
                                            .arg(y, 0, 'f', 1)
                                            .arg(nnx, 0, 'f', 4)
                                            .arg(nny, 0, 'f', 4);
                                 if (gridCheck->isChecked() && gridCellSpin->value() > 0) {
                                     const GridLayout grid = gridLayout();
                                     text += QString("  |  cell (%1, %2)").arg(grid.colAt(x)).arg(grid.rowAt(y));
                                 }
                             }
                             mouseLabel->setText(text);
                         }
                        // Cursor follow: when cursor is shown in the current scene, lock it to mouse.
                        if (inside && cursorSource_.Get() && sceneCombo_) {
                            OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneCombo_->currentText());
                            if (sceneSrc.Get()) {
                                obs_scene_t *scene = obs_scene_from_source(sceneSrc.Get());
                                if (scene) {
                                    CursorInSceneData data;
                                    data.cursorSource = cursorSource_.Get();
                                    data.x = x;
                                    data.y = y;
                                    data.createIfMissing = false; // Only move if it already exists
                                    data.updateVisibility = false; // Do not change visible/hidden state
                                    obs_enter_graphics();
                                    obs_scene_atomic_update(scene, AddOrUpdateCursorInSceneHelper, &data);
                                    obs_leave_graphics();
                                }
                            }
                        }
                        // Fog tools: brush outline follows the mouse; dragging paints / sizes the rectangle.
                        if (fogTool_ != FogTool::None && display) {
                            const bool painting = fogStrokeActive_ && leftButtonDown;
                            if (fogTool_ == FogTool::Brush) {
                                const float r = fogBrushSpin_ ? float(fogBrushSpin_->value()) : 80.0f;
                                display->setBrushOverlay(inside, x, y, r);
                                if (painting && inside && currentFog_.Get()) {
                                    rpg_fog_stroke(currentFog_.Get(), fogLastX_, fogLastY_, x, y, r, fogRevealMode());
                                    fogLastX_ = x;
                                    fogLastY_ = y;
                                }
                            } else if (fogTool_ == FogTool::Rect && painting && inside) {
                                float x0 = std::min(fogRectX0_, x), x1 = std::max(fogRectX0_, x);
                                float y0 = std::min(fogRectY0_, y), y1 = std::max(fogRectY0_, y);
                                if (snapCheck->isChecked() && gridLayout().valid()) {
                                    const GridLayout grid = gridLayout();
                                    grid.snapSpanX(x0, x1);
                                    grid.snapSpanY(y0, y1);
                                }
                                fogRectX1_ = x1;
                                fogRectY1_ = y1;
                                fogLastX_ = x0;
                                fogLastY_ = y0;
                                display->setRectOverlay(true, x0, y0, x1, y1);
                            }
                        }
                        // Move lock: FX center follows cursor (item has OBS_ALIGN_CENTER).
                         if (fxLockMode_ == FxLockMode::Move && display && fxList && inside) {
                             const int row = fxList->currentRow();
                             if (row >= 0 && row < (int)activeFx.size()) {
                                 const FxInstance &inst = activeFx[static_cast<size_t>(row)];
                                 obs_sceneitem_t *item = getSceneItemForInstance(inst);
                                 if (item) {
                                     OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(inst.mapSceneUuid.toUtf8().constData()));
                                     if (mapSrc.Get()) {
                                         float moveX = x, moveY = y;
                                         if (snapCheck->isChecked() && gridLayout().valid()) {
                                             const GridLayout grid = gridLayout();
                                             moveX = grid.snapX(x);
                                             moveY = grid.snapY(y);
                                         }
                                         float sceneX = 0.0f, sceneY = 0.0f;
                                         if (canvasToScene(mapSrc.Get(), moveX, moveY, sceneX, sceneY)) {
                                             struct vec2 pos;
                                             vec2_set(&pos, sceneX, sceneY);
                                             obs_sceneitem_set_pos(item, &pos);
                                         }
                                     }
                                 }
                             }
                             return;
                         }

                         // Resize: from context menu (Resize) only. Distance from FX center = scale.
                         if (fxLockMode_ == FxLockMode::Resize && display && fxList && inside) {
                             const int row = fxList->currentRow();
                             if (row >= 0 && row < (int)activeFx.size()) {
                                 const FxInstance &inst = activeFx[static_cast<size_t>(row)];
                                 obs_sceneitem_t *item = getSceneItemForInstance(inst);
                                 if (item) {
                                     OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(inst.mapSceneUuid.toUtf8().constData()));
                                     if (mapSrc.Get()) {
                                         struct vec2 pos;
                                         obs_sceneitem_get_pos(item, &pos);
                                         float cx = 0.0f, cy = 0.0f;
                                         if (scenePosToCanvas(mapSrc.Get(), pos.x, pos.y, cx, cy)) {
                                             const float dx = x - cx;
                                             const float dy = y - cy;
                                             const float dist = std::sqrt(dx * dx + dy * dy);
                                             const float ratio = (resizeStartDistance_ > 1.0f)
                                                 ? (dist / resizeStartDistance_) : 1.0f;
                                             float s = resizeStartScale_ * ratio;
                                             s = (s < 0.1f) ? 0.1f : ((s > 5.0f) ? 5.0f : s);
                                             struct vec2 scale;
                                             vec2_set(&scale, s, s);
                                             obs_sceneitem_set_scale(item, &scale);
                                         }
                                     }
                                 }
                             }
                             return;
                         }

                         // Rotate: from context menu (Rotate) only
                         const bool inRotateMode = (fxLockMode_ == FxLockMode::Rotate);
                         if (!display || !inRotateMode || !fxList) {
                             if (display)
                                 display->clearDirectionArrow();
                             return;
                         }

                         // Rotate mode + FX selected: show arrow and rotate FX toward cursor.
                         const int row = fxList->currentRow();
                         if (row < 0 || row >= (int)activeFx.size()) {
                             display->clearDirectionArrow();
                             return;
                         }

                         const FxInstance &inst = activeFx[static_cast<size_t>(row)];
                         obs_sceneitem_t *item = getSceneItemForInstance(inst);
                         if (!item) {
                             display->clearDirectionArrow();
                             return;
                         }
                         struct vec2 pos;
                         obs_sceneitem_get_pos(item, &pos);
                         OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(inst.mapSceneUuid.toUtf8().constData()));
                         if (!mapSrc.Get()) {
                             display->clearDirectionArrow();
                             return;
                         }
                         float cx = 0.0f, cy = 0.0f;
                         if (!scenePosToCanvas(mapSrc.Get(), pos.x, pos.y, cx, cy)) {
                             display->clearDirectionArrow();
                             return;
                         }

                         display->setDirectionArrow(cx, cy, x, y);
                         const float dx = x - cx;
                         const float dy = y - cy;
                         const float angleRad = std::atan2(dy, dx);
                         const float angleDeg = angleRad * (180.0f / 3.14159265f);
                         // Align with direction arrow: atan2 gives 0°=right, -90°=up. Template default (0°) is "up", so store rotation as angleDeg+90 so cursor up → 0°.
                         obs_sceneitem_set_rot(item, angleDeg + 90.0f);
                     });

    // Left-click release: exit move/rotate lock.
    QObject::connect(display, &OBSDisplayWidget::sceneLeftReleased, this, [this]() {
                         if (fxLockMode_ != FxLockMode::None)
                             releaseFxLockAndClearArrow();
                         if (fogStrokeActive_) {
                             fogStrokeActive_ = false;
                             if (fogTool_ == FogTool::Rect && currentFog_.Get()) {
                                 // fogLast* holds the snapped top-left corner, fogRect*1 the bottom-right.
                                 rpg_fog_rect(currentFog_.Get(), fogLastX_, fogLastY_, fogRectX1_, fogRectY1_,
                                              fogRevealMode());
                             }
                             if (display)
                                 display->setRectOverlay(false, 0, 0, 0, 0);
                         }
                     });

    tb->addWidget(new QLabel("Cursor:", this));
    cursorBtn_ = new QToolButton(this);
    cursorBtn_->setFixedSize(36, 36);
    cursorBtn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    cursorBtn_->setToolTip(QStringLiteral("Cursor style for Show cursor (right-click on map)."));
    cursorBtn_->setStyleSheet("QToolButton { background-color: #d6d6d6; }");
    const CursorAsset *initAsset =
        get_cursor_asset_by_name(selectedCursorFilename_.toUtf8().constData());
    if (initAsset)
        cursorBtn_->setIcon(iconFromCursorAsset(*initAsset));
    QObject::connect(cursorBtn_, &QToolButton::clicked, this, [this]() {
        const int nCursors = get_cursor_asset_count();
        const CursorAsset *assets = get_cursor_assets();
        if (!assets || nCursors == 0)
            return;
        if (!cursorPopup_) {
            cursorPopup_ = new QFrame(this, Qt::Popup);
            cursorPopup_->setFrameStyle(QFrame::StyledPanel);
            cursorPopup_->setStyleSheet("QToolButton { background-color: #d6d6d6; }");
            QGridLayout *grid = new QGridLayout(cursorPopup_);
            const int cols = 2;
            const int iconSz = 32;
            for (int i = 0; i < nCursors; i++) {
                QString name = QString::fromUtf8(assets[i].filename);
                QIcon icon = iconFromCursorAsset(assets[i]);
                QToolButton *btn = new QToolButton(cursorPopup_);
                btn->setFixedSize(iconSz, iconSz);
                btn->setIcon(icon);
                btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
                btn->setToolTip(QFileInfo(name).baseName());
                connect(btn, &QToolButton::clicked, this, [this, name]() {
                    selectedCursorFilename_ = name;
                    const CursorAsset *a = get_cursor_asset_by_name(name.toUtf8().constData());
                    if (a)
                        cursorBtn_->setIcon(iconFromCursorAsset(*a));
                    updateCursorSourceImage();
                    cursorPopup_->close();
                });
                grid->addWidget(btn, i / cols, i % cols);
            }
        }
        cursorPopup_->adjustSize();
        cursorPopup_->move(cursorBtn_->mapToGlobal(QPoint(0, cursorBtn_->height())));
        cursorPopup_->show();
    });
    tb->addWidget(cursorBtn_);

    auto *toolbarSpacer = new QWidget(this);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(toolbarSpacer);
    auto *refreshBtn = new QToolButton(this);
    refreshBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refreshBtn->setToolTip("Refresh scenes");
    refreshBtn->setFixedSize(kToolButtonSize, kToolButtonSize);
    refreshBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *refreshCol = new QWidget(this);
    auto *refreshColLayout = new QVBoxLayout(refreshCol);
    refreshColLayout->setContentsMargins(0, 0, 0, 0);
    refreshColLayout->setSpacing(2);
    refreshColLayout->addWidget(refreshBtn, 0, Qt::AlignHCenter);
    auto *refreshLbl = new QLabel("Refresh", refreshCol);
    refreshLbl->setAlignment(Qt::AlignCenter);
    refreshLbl->setStyleSheet("font-size: 9px;");
    refreshColLayout->addWidget(refreshLbl);
    tb->addWidget(refreshCol);

    tb->addSeparator();

    // Build FX side panel UI (compact).
    fxPanel->setMaximumWidth(280);
    fxLayout->addWidget(new QLabel("Effects", this));

    auto *fxCombo = new QComboBox(this);
    fxCombo->setMinimumWidth(140);
    fxLayout->addWidget(new QLabel("Template:", this));
    fxLayout->addWidget(fxCombo);

    auto *sequenceBox = new QCheckBox("Sequence [ms] with [123] prefixes", this);
    sequenceBox->setChecked(true);
    fxLayout->addWidget(sequenceBox);

    auto *stageBox = new QCheckBox("Stage hidden (place now, trigger later)", this);
    stageBox->setChecked(false);
    stageBox->setToolTip(QStringLiteral(
        "Spawned FX stay hidden from players. They show as translucent ghosts here so you can "
        "move, rotate and resize them, then use Trigger to reveal them on output."));
    fxLayout->addWidget(stageBox);

    auto *labelRowWidget = new QWidget(this);
    auto *labelRowLayout = new QHBoxLayout(labelRowWidget);
    labelRowLayout->setContentsMargins(0, 0, 0, 0);
    labelRowLayout->setSpacing(4);

    labelEdit = new QLineEdit(labelRowWidget);
    labelEdit->setPlaceholderText(QStringLiteral("Optional label (defaults to template name)"));
    labelRowLayout->addWidget(labelEdit, 1);

    fxLayout->addWidget(labelRowWidget);

    static const int kSpawnButtonSize = 48;
    auto *spawnBtn = new QToolButton(this);
    spawnBtn->setObjectName("spawnAtClickButton");
    spawnBtn->setIcon(iconTarget(kSpawnButtonSize - 8));
    spawnBtn->setToolTip("Click to choose spawn location on map");
    spawnBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    spawnBtn->setFixedSize(kSpawnButtonSize, kSpawnButtonSize);
    spawnBtn->setMinimumSize(kSpawnButtonSize, kSpawnButtonSize);
    spawnBtn->setIconSize(QSize(kSpawnButtonSize - 8, kSpawnButtonSize - 8));
    spawnBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    spawnBtn->setStyleSheet(
        QString("QToolButton#spawnAtClickButton { min-width: %1px; min-height: %1px; max-width: %1px; max-height: %1px; background-color: #4caf50; }"
                " QToolButton#spawnAtClickButton:checked { background-color: #c62828; }")
            .arg(kSpawnButtonSize));
    spawnBtn->setCheckable(true);
    spawnBtn_ = spawnBtn;
    auto *clearAllBtn = new QToolButton(this);
    clearAllBtn->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    clearAllBtn->setToolTip("Clear all effects");
    clearAllBtn->setFixedSize(kToolButtonSize, kToolButtonSize);
    clearAllBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto *timersRow = new QWidget(this);
    auto *timersLayout = new QHBoxLayout(timersRow);
    timersLayout->setContentsMargins(0, 0, 0, 0);
    timersLayout->setSpacing(8);
    timersLayout->setAlignment(Qt::AlignTop);
    auto *fadeSpin = new QSpinBox(this);
    fadeSpin->setRange(0, 60000);
    fadeSpin->setSingleStep(100);
    fadeSpin->setValue(600);
    fadeSpin->setMaximumWidth(72);
    fadeSpin->setAlignment(Qt::AlignRight);
    auto *fadeLabel = new QLabel("Fade (ms)", this);
    fadeLabel->setAlignment(Qt::AlignCenter);
    auto *fadeCol = new QWidget(this);
    auto *fadeColLayout = new QVBoxLayout(fadeCol);
    fadeColLayout->setContentsMargins(0, 0, 0, 0);
    fadeColLayout->setSpacing(0);
    fadeColLayout->addWidget(fadeSpin);
    fadeColLayout->addWidget(fadeLabel);
    timersLayout->addWidget(fadeCol);
    auto *lifetimeSpin = new QSpinBox(this);
    lifetimeSpin->setRange(0, 3600);
    lifetimeSpin->setSingleStep(5);
    lifetimeSpin->setValue(0);
    lifetimeSpin->setMaximumWidth(72);
    lifetimeSpin->setAlignment(Qt::AlignRight);
    auto *lifetimeLabel = new QLabel("Lifetime (s)", this);
    lifetimeLabel->setAlignment(Qt::AlignCenter);
    auto *lifetimeCol = new QWidget(this);
    auto *lifetimeColLayout = new QVBoxLayout(lifetimeCol);
    lifetimeColLayout->setContentsMargins(0, 0, 0, 0);
    lifetimeColLayout->setSpacing(0);
    lifetimeColLayout->addWidget(lifetimeSpin);
    lifetimeColLayout->addWidget(lifetimeLabel);
    timersLayout->addWidget(lifetimeCol);
    auto *spawnCol = new QWidget(this);
    auto *spawnColLayout = new QVBoxLayout(spawnCol);
    spawnColLayout->setContentsMargins(0, 0, 0, 0);
    spawnColLayout->setSpacing(2);
    spawnColLayout->addWidget(spawnBtn, 0, Qt::AlignHCenter);
    auto *spawnLbl = new QLabel("Click to spawn", spawnCol);
    spawnLbl->setAlignment(Qt::AlignCenter);
    spawnLabel_ = spawnLbl;
    spawnColLayout->addWidget(spawnLbl);
    timersLayout->addWidget(spawnCol);
    fxLayout->addWidget(timersRow);

    auto applyTemplateDefaults = [this, fxCombo, sequenceBox, fadeSpin, lifetimeSpin]() {
        const QString name = fxCombo->currentText();
        if (name.isEmpty())
            return;
        obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
        if (!src)
            return;
        const char *uuid = obs_source_get_uuid(src);
        obs_source_release(src);
        if (!uuid)
            return;
        TemplateDefaults def = LoadTemplateDefaults(QString::fromUtf8(uuid));
        sequenceBox->setChecked(def.sequence);
        fadeSpin->setValue(def.fadeMs);
        lifetimeSpin->setValue(def.lifetimeSec);
        if (labelEdit) {
            labelEdit->clear();
            labelEdit->setPlaceholderText(DefaultLabelFromTemplateName(name));
        }
    };

    QObject::connect(fxCombo, &QComboBox::currentTextChanged, this, applyTemplateDefaults);

    fxList = new QListWidget(this);
    fxList->setSelectionMode(QAbstractItemView::SingleSelection);
    fxList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked);
    fxList->setMinimumHeight(100);
    fxList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    fxList->setContextMenuPolicy(Qt::CustomContextMenu);
    fxList->setItemDelegate(new FxListDelegate(this));
    auto *spawnListSep = new QFrame(this);
    spawnListSep->setFrameShape(QFrame::HLine);
    spawnListSep->setFrameShadow(QFrame::Sunken);
    fxLayout->addWidget(spawnListSep);
    auto *activeRow = new QWidget(this);
    auto *activeRowLayout = new QHBoxLayout(activeRow);
    activeRowLayout->setContentsMargins(0, 0, 0, 0);
    activeRowLayout->setSpacing(4);
    activeRowLayout->addWidget(new QLabel("Label color:", this));
    auto *labelColorBtn = new QToolButton(this);
    labelColorBtn->setIcon(iconFromColor(labelColor_));
    labelColorBtn->setToolTip("Set label color for selected FX (or next spawn)");
    labelColorBtn->setFixedSize(kToolButtonSize, kToolButtonSize);
    labelColorBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    activeRowLayout->addWidget(labelColorBtn);
    activeRowLayout->addStretch(1);
    activeRowLayout->addWidget(clearAllBtn);
    fxLayout->addWidget(activeRow);

    QObject::connect(labelColorBtn, &QAbstractButton::clicked, this, [this, labelColorBtn]() {
        QColor chosen = QColorDialog::getColor(labelColor_, this, "Choose label color");
        if (!chosen.isValid())
            return;
        labelColor_ = chosen;
        labelColorBtn->setIcon(iconFromColor(chosen));
        const int row = fxList->currentRow();
        if (row >= 0 && row < (int)activeFx.size()) {
            const FxInstance &inst = activeFx[static_cast<size_t>(row)];
            UpdateFxLabelSourcesByEffectUuid(inst.effectUuid, inst.label, argbFromColor(chosen));
        }
    });

    fxLayout->addWidget(fxList, /*stretch*/ 1);

    auto *triggerRow = new QWidget(this);
    auto *triggerRowLayout = new QHBoxLayout(triggerRow);
    triggerRowLayout->setContentsMargins(0, 0, 0, 0);
    triggerRowLayout->setSpacing(4);
    triggerSelectedBtn_ = new QPushButton(QStringLiteral("Trigger selected"), this);
    triggerSelectedBtn_->setToolTip(QStringLiteral("Reveal the selected staged FX on output"));
    triggerAllBtn_ = new QPushButton(QStringLiteral("Trigger all staged"), this);
    triggerAllBtn_->setToolTip(QStringLiteral(
        "Reveal every staged FX on this map at once (also available as an OBS hotkey: "
        "Settings → Hotkeys → \"RPG Map Effects: Trigger all staged FX\")"));
    triggerAllBtn_->setStyleSheet("QPushButton:enabled { background-color: #e09000; color: black; font-weight: bold; }");
    triggerRowLayout->addWidget(triggerSelectedBtn_);
    triggerRowLayout->addWidget(triggerAllBtn_, 1);
    fxLayout->addWidget(triggerRow);

    QObject::connect(triggerSelectedBtn_, &QAbstractButton::clicked, this, [this]() {
        const int row = fxList->currentRow();
        if (row >= 0 && row < (int)activeFx.size())
            triggerFx(activeFx[static_cast<size_t>(row)]);
    });
    QObject::connect(triggerAllBtn_, &QAbstractButton::clicked, this, [this]() { triggerAllStaged(); });

    // --- Fog of war controls ---
    auto *fogGroup = new QGroupBox(QStringLiteral("Fog of War"), this);
    auto *fogLayout = new QGridLayout(fogGroup);
    fogLayout->setContentsMargins(4, 8, 4, 4);
    fogLayout->setHorizontalSpacing(4);
    fogLayout->setVerticalSpacing(3);

    fogEnableCheck_ = new QCheckBox(QStringLiteral("Fog on this map"), fogGroup);
    fogEnableCheck_->setToolTip(QStringLiteral(
        "Adds a fog layer to this battlemap. Players see solid fog; this window shows it "
        "see-through so you can still read the map."));
    fogColorBtn_ = new QToolButton(fogGroup);
    fogColorBtn_->setIcon(iconFromColor(QColor(20, 20, 20)));
    fogColorBtn_->setToolTip(QStringLiteral("Fog color (an image can be set in the fog source's properties)"));
    fogColorBtn_->setFixedSize(kToolButtonSize, kToolButtonSize);
    fogColorBtn_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    fogGmOpacitySpin_ = new QSpinBox(fogGroup);
    fogGmOpacitySpin_->setRange(0, 100);
    fogGmOpacitySpin_->setValue(45);
    fogGmOpacitySpin_->setSuffix(QStringLiteral("%"));
    fogGmOpacitySpin_->setToolTip(QStringLiteral("Fog opacity in this window only (output always uses the full fog)"));
    auto *fogTopRow = new QHBoxLayout();
    fogTopRow->setSpacing(4);
    fogTopRow->addWidget(fogEnableCheck_, 1);
    fogTopRow->addWidget(fogColorBtn_);
    fogTopRow->addWidget(new QLabel(QStringLiteral("GM view:"), fogGroup));
    fogTopRow->addWidget(fogGmOpacitySpin_);
    fogLayout->addLayout(fogTopRow, 0, 0, 1, 3);

    fogBrushBtn_ = new QPushButton(QStringLiteral("Brush"), fogGroup);
    fogBrushBtn_->setCheckable(true);
    fogBrushBtn_->setToolTip(QStringLiteral("Paint on the map to reveal or hide fog. [ and ] change the size."));
    fogRectBtn_ = new QPushButton(QStringLiteral("Rectangle"), fogGroup);
    fogRectBtn_->setCheckable(true);
    fogRectBtn_->setToolTip(QStringLiteral("Drag a rectangle (snaps to the grid when Snap to grid is on)"));
    fogRevealRadio_ = new QRadioButton(QStringLiteral("Reveal"), fogGroup);
    fogRevealRadio_->setChecked(true);
    auto *fogHideRadio = new QRadioButton(QStringLiteral("Hide"), fogGroup);
    fogHideRadio->setToolTip(QStringLiteral("Paint fog back over the map (hold Alt to switch temporarily)"));
    fogLayout->addWidget(fogBrushBtn_, 1, 0);
    fogLayout->addWidget(fogRectBtn_, 1, 1);

    fogBrushSpin_ = new QSpinBox(fogGroup);
    fogBrushSpin_->setRange(5, 1000);
    fogBrushSpin_->setSingleStep(10);
    fogBrushSpin_->setValue(80);
    fogBrushSpin_->setSuffix(QStringLiteral(" px"));
    fogBrushSpin_->setToolTip(QStringLiteral("Brush radius"));
    auto *fogUndoBtn = new QPushButton(QStringLiteral("Undo"), fogGroup);
    fogUndoBtn->setToolTip(QStringLiteral("Undo the last fog edit (Ctrl+Z / Cmd+Z)"));
    auto *fogRevealAllBtn = new QPushButton(QStringLiteral("Clear all"), fogGroup);
    fogRevealAllBtn->setToolTip(QStringLiteral("Remove all fog from this map"));
    auto *fogCoverAllBtn = new QPushButton(QStringLiteral("Cover all"), fogGroup);
    fogCoverAllBtn->setToolTip(QStringLiteral("Cover the whole map in fog again"));
    fogLayout->addWidget(fogBrushSpin_, 1, 2);
    fogLayout->addWidget(fogRevealRadio_, 2, 0);
    fogLayout->addWidget(fogHideRadio, 2, 1);
    fogLayout->addWidget(fogUndoBtn, 2, 2);
    fogLayout->addWidget(fogRevealAllBtn, 3, 0);
    fogLayout->addWidget(fogCoverAllBtn, 3, 1);
    fxLayout->addWidget(fogGroup);

    QObject::connect(fogEnableCheck_, &QCheckBox::toggled, this,
                     [this](bool checked) { setFogEnabledForCurrentMap(checked); });
    QObject::connect(fogBrushBtn_, &QAbstractButton::clicked, this, [this](bool checked) {
        setFogTool(checked ? FogTool::Brush : FogTool::None);
    });
    QObject::connect(fogRectBtn_, &QAbstractButton::clicked, this, [this](bool checked) {
        setFogTool(checked ? FogTool::Rect : FogTool::None);
    });
    QObject::connect(fogGmOpacitySpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
                     [this](int value) { setFogSetting("gm_opacity", value); });
    QObject::connect(fogColorBtn_, &QAbstractButton::clicked, this, [this]() {
        if (!currentFog_.Get())
            setFogEnabledForCurrentMap(true);
        if (!currentFog_.Get())
            return;
        OBSDataAutoRelease settings = obs_source_get_settings(currentFog_.Get());
        const uint32_t c = (uint32_t)obs_data_get_int(settings, "color");
        const QColor current(c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, (c >> 24) & 0xff);
        const QColor chosen =
            QColorDialog::getColor(current, this, QStringLiteral("Choose fog color"), QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid())
            return;
        // OBS color properties are 0xAABBGGRR.
        const uint32_t abgr = (uint32_t(chosen.alpha()) << 24) | (uint32_t(chosen.blue()) << 16) |
                              (uint32_t(chosen.green()) << 8) | uint32_t(chosen.red());
        setFogSetting("color", (long long)abgr);
        fogColorBtn_->setIcon(iconFromColor(chosen));
    });
    auto fogUndo = [this]() {
        if (currentFog_.Get())
            rpg_fog_undo(currentFog_.Get());
    };
    QObject::connect(fogUndoBtn, &QAbstractButton::clicked, this, fogUndo);
    QObject::connect(fogRevealAllBtn, &QAbstractButton::clicked, this, [this]() {
        if (!currentFog_.Get())
            return;
        rpg_fog_begin_edit(currentFog_.Get());
        rpg_fog_fill(currentFog_.Get(), true);
    });
    QObject::connect(fogCoverAllBtn, &QAbstractButton::clicked, this, [this]() {
        if (!currentFog_.Get())
            setFogEnabledForCurrentMap(true);
        if (!currentFog_.Get())
            return;
        rpg_fog_begin_edit(currentFog_.Get());
        rpg_fog_fill(currentFog_.Get(), false);
    });

    auto *undoShortcut = new QShortcut(QKeySequence::Undo, this);
    QObject::connect(undoShortcut, &QShortcut::activated, this, fogUndo);
    auto *brushSmaller = new QShortcut(QKeySequence(Qt::Key_BracketLeft), this);
    QObject::connect(brushSmaller, &QShortcut::activated, this,
                     [this]() { fogBrushSpin_->setValue(fogBrushSpin_->value() - 10); });
    auto *brushBigger = new QShortcut(QKeySequence(Qt::Key_BracketRight), this);
    QObject::connect(brushBigger, &QShortcut::activated, this,
                     [this]() { fogBrushSpin_->setValue(fogBrushSpin_->value() + 10); });

    QObject::connect(fxList, &QListWidget::customContextMenuRequested, this,
                     [this, fxCombo, fadeSpin](const QPoint &pos) {
                         const int row = fxList->row(fxList->itemAt(pos));
                         if (row < 0 || row >= (int)activeFx.size())
                             return;
                         fxList->setCurrentRow(row);
                         FxInstance &inst = activeFx[static_cast<size_t>(row)];

                         // Determine label visibility state for this FX instance.
                         bool hasLabel = false;
                         bool labelVisible = false;
                         {
                             if (!inst.effectUuid.isEmpty()) {
                                 OBSSourceAutoRelease fxSrc(
                                     obs_get_source_by_uuid(inst.effectUuid.toUtf8().constData()));
                                 if (fxSrc.Get()) {
                                     obs_scene_t *scene = obs_scene_from_source(fxSrc.Get());
                                     if (scene) {
                                         struct LabelVisCtx {
                                             bool hasLabel = false;
                                             bool anyVisible = false;
                                         } ctx;
                                         obs_scene_enum_items(
                                             scene,
                                             [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
                                                 auto *ctx = static_cast<LabelVisCtx *>(param);
                                                 obs_source_t *src = obs_sceneitem_get_source(item);
                                                 if (!src)
                                                     return true;
                                                 const char *nm = obs_source_get_name(src);
                                                 if (!nm)
                                                     return true;
                                                 QString name = QString::fromUtf8(nm);
                                                 if (!name.startsWith("FX Label "))
                                                     return true;
                                                 ctx->hasLabel = true;
                                                 if (obs_sceneitem_visible(item))
                                                     ctx->anyVisible = true;
                                                 return true;
                                             },
                                             &ctx);
                                         hasLabel = ctx.hasLabel;
                                         labelVisible = ctx.anyVisible;
                                     }
                                 }
                             }
                         }

                         QMenu menu(this);
                         QAction *triggerAct = nullptr;
                         if (inst.staged) {
                             triggerAct = menu.addAction(QStringLiteral("Trigger now (show to players)"));
                             menu.addSeparator();
                         }
                         auto *resetSizeAct = menu.addAction(QStringLiteral("Reset size"));
                         menu.addSeparator();
                         QAction *lockAct = nullptr;
                         QAction *unlockAct = nullptr;
                         if (inst.locked)
                             unlockAct = menu.addAction(QStringLiteral("Unlock"));
                         else
                             lockAct = menu.addAction(QStringLiteral("Lock"));
                         auto *clearAct = menu.addAction(QStringLiteral("Clear selected"));
                         clearAct->setEnabled(!inst.locked);
                         menu.addSeparator();
                         QAction *showLabelAct = nullptr;
                         QAction *hideLabelAct = nullptr;
                         if (hasLabel && !labelVisible)
                             showLabelAct = menu.addAction(QStringLiteral("Show label"));
                         if (hasLabel && labelVisible)
                             hideLabelAct = menu.addAction(QStringLiteral("Hide label"));

                         QAction *chosen = menu.exec(fxList->mapToGlobal(pos));
                         if (chosen && chosen == triggerAct) {
                             triggerFx(inst);
                         } else if (chosen == resetSizeAct) {
                             obs_sceneitem_t *item = getSceneItemForInstance(inst);
                             if (item) {
                                 struct vec2 scale;
                                 vec2_set(&scale, 1.0f, 1.0f);
                                 obs_sceneitem_set_scale(item, &scale);
                             }
                         } else if (chosen == lockAct) {
                             inst.locked = true;
                             syncStoredLocked(inst, true);
                             if (QListWidgetItem *item = fxList->item(row))
                                 item->setData(kFxItemLockedRole, true);
                         } else if (chosen == unlockAct) {
                             inst.locked = false;
                             syncStoredLocked(inst, false);
                             if (QListWidgetItem *item = fxList->item(row))
                                 item->setData(kFxItemLockedRole, false);
                         } else if (chosen == clearAct) {
                             clearFxAtRow(row, fadeSpin ? fadeSpin->value() : 600);
                         } else if (chosen == showLabelAct || chosen == hideLabelAct) {
                             const bool makeVisible = (chosen == showLabelAct);
                             SetFxLabelsVisibleByEffectUuid(inst.effectUuid, makeVisible);
                         }
                     });

    // Right-click on map: over FX → Move / Rotate / Clear / label options; anywhere → cursor options.
    QObject::connect(display, &OBSDisplayWidget::sceneRightClicked, this,
                     [this, fadeSpin](float sceneX, float sceneY, float, float, bool inside) {
                         if (!inside)
                             return;
                         const int idx = findNearestFxInstanceIndex(sceneX, sceneY);
                         const bool overFx = (idx >= 0 && fxList && idx < fxList->count());
                         FxInstance *ctxInst = nullptr;
                         if (overFx) {
                             fxList->setCurrentRow(idx);
                             ctxInst = &activeFx[static_cast<size_t>(idx)];
                         }

                        // Determine cursor visibility state for current battlemap scene.
                        bool cursorVisibleInScene = false;
                        bool cursorExistsInScene = false;
                        if (cursorSource_.Get() && sceneCombo_) {
                            OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneCombo_->currentText());
                            if (sceneSrc.Get()) {
                                obs_scene_t *scene = obs_scene_from_source(sceneSrc.Get());
                                if (scene) {
                                    CursorItemCtx ctx;
                                         ctx.cursorSource = cursorSource_.Get();
                                         obs_scene_enum_items(scene, findCursorItemInScene, &ctx);
                                         if (ctx.item) {
                                             cursorExistsInScene = true;
                                             cursorVisibleInScene = obs_sceneitem_visible(ctx.item);
                                            obs_sceneitem_release(ctx.item);
                                    }
                                }
                            }
                        }

                        QMenu menu(this);
                         QAction *moveAct = nullptr;
                         QAction *rotateAct = nullptr;
                         QAction *resizeAct = nullptr;
                         QAction *resetSizeAct = nullptr;
                         QAction *lockAct = nullptr;
                         QAction *unlockAct = nullptr;
                         QAction *clearAct = nullptr;
                         QAction *showLabelAct = nullptr;
                         QAction *hideLabelAct = nullptr;
                         QAction *triggerAct = nullptr;
                         if (overFx) {
                             if (ctxInst && ctxInst->staged) {
                                 triggerAct = menu.addAction(QStringLiteral("Trigger now (show to players)"));
                                 menu.addSeparator();
                             }
                             moveAct = menu.addAction(QStringLiteral("Move"));
                             rotateAct = menu.addAction(QStringLiteral("Rotate"));
                             resizeAct = menu.addAction(QStringLiteral("Resize"));
                             resetSizeAct = menu.addAction(QStringLiteral("Reset size"));
                             menu.addSeparator();
                             if (ctxInst) {
                                 if (ctxInst->locked)
                                     unlockAct = menu.addAction(QStringLiteral("Unlock"));
                                 else
                                     lockAct = menu.addAction(QStringLiteral("Lock"));
                             }
                             clearAct = menu.addAction(QStringLiteral("Clear"));
                             if (ctxInst)
                                 clearAct->setEnabled(!ctxInst->locked);
                             menu.addSeparator();
                             // Label show/hide for this FX instance.
                             if (ctxInst) {
                                 bool hasLabelFx = false;
                                 bool labelVisibleFx = false;
                                 if (!ctxInst->effectUuid.isEmpty()) {
                                     OBSSourceAutoRelease fxSrc(
                                         obs_get_source_by_uuid(ctxInst->effectUuid.toUtf8().constData()));
                                     if (fxSrc.Get()) {
                                         obs_scene_t *scene = obs_scene_from_source(fxSrc.Get());
                                         if (scene) {
                                             struct LabelVisCtxFx {
                                                 bool hasLabel = false;
                                                 bool anyVisible = false;
                                             } lv;
                                             obs_scene_enum_items(
                                                 scene,
                                                 [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
                                                     auto *lv = static_cast<LabelVisCtxFx *>(param);
                                                     obs_source_t *src = obs_sceneitem_get_source(item);
                                                     if (!src)
                                                         return true;
                                                     const char *nm = obs_source_get_name(src);
                                                     if (!nm)
                                                         return true;
                                                     QString name = QString::fromUtf8(nm);
                                                     if (!name.startsWith("FX Label "))
                                                         return true;
                                                     lv->hasLabel = true;
                                                     if (obs_sceneitem_visible(item))
                                                         lv->anyVisible = true;
                                                     return true;
                                                 },
                                                 &lv);
                                             hasLabelFx = lv.hasLabel;
                                             labelVisibleFx = lv.anyVisible;
                                         }
                                     }
                                 }
                                 if (hasLabelFx && !labelVisibleFx)
                                     showLabelAct = menu.addAction(QStringLiteral("Show label"));
                                 if (hasLabelFx && labelVisibleFx)
                                     hideLabelAct = menu.addAction(QStringLiteral("Hide label"));
                             }
                             menu.addSeparator();
                         }
                         static constexpr float kCursorDisplaySize = 48.0f;
                         QAction *showCursorAct = nullptr;
                         QAction *hideCursorAct = nullptr;
                         if (!cursorVisibleInScene)
                             showCursorAct = menu.addAction(QStringLiteral("Show cursor"));
                         if (cursorExistsInScene && cursorVisibleInScene)
                             hideCursorAct = menu.addAction(QStringLiteral("Hide cursor"));
                         QAction *chosen = menu.exec(QCursor::pos());
                        if (chosen == showCursorAct) {
                            if (ensureCursorSource().Get()) {
                                OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneCombo_->currentText());
                                if (sceneSrc.Get()) {
                                    obs_scene_t *scene = obs_scene_from_source(sceneSrc.Get());
                                    if (scene) {
                                        const uint32_t sceneW = obs_source_get_width(sceneSrc.Get());
                                         const uint32_t sceneH = obs_source_get_height(sceneSrc.Get());
                                         const float centerX = (sceneW > 0) ? float(sceneW) * 0.5f : 0.0f;
                                         const float centerY = (sceneH > 0) ? float(sceneH) * 0.5f : 0.0f;
                                         CursorInSceneData data;
                                         data.cursorSource = cursorSource_.Get();
                                         data.x = centerX;
                                         data.y = centerY;
                                         data.visible = true;
                                         data.createIfMissing = true;
                                         data.boundsSize = kCursorDisplaySize;
                                         obs_enter_graphics();
                                         obs_scene_atomic_update(scene, AddOrUpdateCursorInSceneHelper, &data);
                                        obs_leave_graphics();
                                    }
                                }
                            }
                        } else if (chosen == hideCursorAct) {
                            if (cursorSource_.Get()) {
                                OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneCombo_->currentText());
                                if (sceneSrc.Get()) {
                                    obs_scene_t *scene = obs_scene_from_source(sceneSrc.Get());
                                    if (scene) {
                                        CursorInSceneData data;
                                        data.cursorSource = cursorSource_.Get();
                                        data.visible = false;
                                        data.createIfMissing = false;
                                        data.boundsSize = kCursorDisplaySize;
                                        obs_enter_graphics();
                                        obs_scene_atomic_update(scene, AddOrUpdateCursorInSceneHelper, &data);
                                        obs_leave_graphics();
                                    }
                                }
                            }
                        } else if (overFx) {
                             const int row = idx;
                             if (chosen && chosen == triggerAct && ctxInst) {
                                 triggerFx(*ctxInst);
                             } else if (chosen == clearAct) {
                                 clearFxAtRow(row, fadeSpin ? fadeSpin->value() : 600);
                             } else if (chosen == moveAct) {
                                 fxLockMode_ = FxLockMode::Move;
                                 if (display)
                                     display->clearDirectionArrow();
                             } else if (chosen == rotateAct) {
                                 fxLockMode_ = FxLockMode::Rotate;
                             } else if (chosen == lockAct && ctxInst) {
                                 ctxInst->locked = true;
                                 syncStoredLocked(*ctxInst, true);
                                 if (fxList && row >= 0 && row < fxList->count())
                                     fxList->item(row)->setData(kFxItemLockedRole, true);
                             } else if (chosen == unlockAct && ctxInst) {
                                 ctxInst->locked = false;
                                 syncStoredLocked(*ctxInst, false);
                                 if (fxList && row >= 0 && row < fxList->count())
                                     fxList->item(row)->setData(kFxItemLockedRole, false);
                             } else if (chosen == resizeAct) {
                                 fxLockMode_ = FxLockMode::Resize;
                                 if (display)
                                     display->clearDirectionArrow();
                                 // Capture initial scale and distance at right-click for immediate feedback.
                                 obs_sceneitem_t *item = getSceneItemForInstance(*ctxInst);
                                 if (item) {
                                     struct vec2 pos, scale;
                                     obs_sceneitem_get_pos(item, &pos);
                                     obs_sceneitem_get_scale(item, &scale);
                                     resizeStartScale_ = (scale.x + scale.y) * 0.5f;
                                     OBSSourceAutoRelease mapSrc(obs_get_source_by_uuid(ctxInst->mapSceneUuid.toUtf8().constData()));
                                     if (mapSrc.Get()) {
                                         float cx = 0.0f, cy = 0.0f;
                                         if (scenePosToCanvas(mapSrc.Get(), pos.x, pos.y, cx, cy)) {
                                             const float dx = sceneX - cx;
                                             const float dy = sceneY - cy;
                                             resizeStartDistance_ = (std::sqrt(dx * dx + dy * dy) > 5.0f)
                                                 ? std::sqrt(dx * dx + dy * dy) : 100.0f;
                                         }
                                     }
                                 }
                             } else if (chosen == resetSizeAct && ctxInst) {
                                 obs_sceneitem_t *item = getSceneItemForInstance(*ctxInst);
                                 if (item) {
                                     struct vec2 scale;
                                     vec2_set(&scale, 1.0f, 1.0f);
                                     obs_sceneitem_set_scale(item, &scale);
                                 }
                             } else if (chosen == showLabelAct || chosen == hideLabelAct) {
                                 const bool makeVisible = (chosen == showLabelAct);
                                 if (ctxInst)
                                     SetFxLabelsVisibleByEffectUuid(ctxInst->effectUuid, makeVisible);
                             }
                         }
                     });

    QObject::connect(fxList, &QListWidget::currentRowChanged, this, [this](int row) {
        updateStagedOverlay();
        if (row < 0 || row >= (int)activeFx.size()) {
            releaseFxLockAndClearArrow();
            return;
        }

        // If in rotate mode and selection changed, clear arrow until mouse moves again.
        if (fxLockMode_ == FxLockMode::Rotate && display)
            display->clearDirectionArrow();
    });

    QObject::connect(fxList, &QListWidget::itemChanged, this,
                     [this](QListWidgetItem *item) {
        if (!item)
            return;
        const int row = fxList->row(item);
        if (row < 0 || row >= (int)activeFx.size())
            return;

        FxInstance &inst = activeFx[static_cast<size_t>(row)];
        QString newLabel = item->text().trimmed();
        // Strip [Locked] prefix so we never store it; it's display-only.
        const QString lockedPrefix = QStringLiteral("[Locked] ");
        if (newLabel.startsWith(lockedPrefix))
            newLabel = newLabel.mid(lockedPrefix.size()).trimmed();
        inst.label = newLabel;
        syncStoredLabel(inst, newLabel);

        // Update text and color of any FX Label source inside this FX scene instance.
        const uint32_t labelArgb = argbFromColor(labelColor_);
        UpdateFxLabelSourcesByEffectUuid(inst.effectUuid, newLabel, labelArgb);
    });

    auto refreshLists = [this, fxCombo, applyTemplateDefaults]() {
        const QString prevScene = sceneCombo_->currentText();
        const QString prevFx = fxCombo->currentText();

        AddBattlemapScenesToCombo(sceneCombo_);
        AddFxTemplateScenesToCombo(fxCombo);

        if (!prevScene.isEmpty()) {
            const int idx = sceneCombo_->findText(prevScene);
            if (idx >= 0)
                sceneCombo_->setCurrentIndex(idx);
        }
        if (!prevFx.isEmpty()) {
            const int idx = fxCombo->findText(prevFx);
            if (idx >= 0)
                fxCombo->setCurrentIndex(idx);
        }
        applyTemplateDefaults();
    };

    refreshLists();

    // Ensure grid image source exists for output overlay.
    QTimer::singleShot(0, this, [this]() { ensureGridSource(); });

    QObject::connect(refreshBtn, &QAbstractButton::clicked, this, [refreshLists]() { refreshLists(); });

    auto doSpawn = [this, fxCombo, sequenceBox, stageBox, fadeSpin, lifetimeSpin, gridCellSpin, snapCheck](
                       float spawnX, float spawnY) {
        OBSSourceAutoRelease mapSrc(display->getSceneSourceRef());
        if (!mapSrc.Get())
            return;

        if (snapCheck->isChecked() && gridLayout().valid()) {
            const GridLayout grid = gridLayout();
            spawnX = grid.snapX(spawnX);
            spawnY = grid.snapY(spawnY);
        }

        QString mapUuid;
        QString fxUuid;
        int64_t sceneItemId = 0;

        const bool seq = sequenceBox->isChecked();
        const bool staged = stageBox->isChecked();

        QString labelText = DefaultLabelFromTemplateName(fxCombo->currentText());
        if (labelEdit) {
            const QString custom = labelEdit->text().trimmed();
            if (!custom.isEmpty())
                labelText = custom;
        }

        obs_source_t *tmplSrc = obs_get_source_by_name(fxCombo->currentText().toUtf8().constData());
        if (tmplSrc) {
            const char *uuid = obs_source_get_uuid(tmplSrc);
            if (uuid) {
                SaveTemplateDefaults(QString::fromUtf8(uuid), fadeSpin->value(), lifetimeSpin->value(),
                                     seq);
            }
            obs_source_release(tmplSrc);
        }

        const uint32_t labelArgb = argbFromColor(labelColor_);

        SpawnFxAtClick(fxCombo->currentText(), mapSrc.Get(), spawnX, spawnY, seq, staged,
                       fadeSpin->value(), labelText, labelArgb,
                       mapUuid, fxUuid, sceneItemId);

        if (!mapUuid.isEmpty() && !fxUuid.isEmpty() && sceneItemId != 0) {
            FxInstance inst;
            inst.mapSceneUuid = mapUuid;
            inst.effectUuid = fxUuid;
            inst.sceneItemId = sceneItemId;
            inst.templateName = fxCombo->currentText();
            inst.label = labelText;
            inst.staged = staged;
            inst.sequence = seq;
            inst.lifetimeSec = lifetimeSpin->value();
            inst.fadeMs = fadeSpin->value();
            if (staged) {
                // Keep the hidden FX "showing" so animated/media content plays in the GM preview.
                OBSSourceAutoRelease fxSrc(obs_get_source_by_uuid(fxUuid.toUtf8().constData()));
                if (fxSrc.Get()) {
                    obs_source_inc_showing(fxSrc.Get());
                    stagedShowingUuids_.insert(fxUuid);
                }
            }
            fxByMapSceneUuid_[mapUuid].push_back(inst);
            if (mapUuid == currentMapSceneUuid_) {
                activeFx.push_back(inst);
                fxList->addItem(makeFxListItem(inst));
                fxList->setCurrentRow(fxList->count() - 1);
            }

            // Staged FX start their lifetime when triggered.
            if (!staged)
                scheduleLifetime(inst, inst.lifetimeSec, inst.fadeMs);
            updateStagedOverlay();
        }

        if (labelEdit)
            labelEdit->clear();
    };

    QObject::connect(spawnBtn, &QAbstractButton::clicked, this, [this]() {
        if (spawnMode_) {
            spawnMode_ = false;
            spawnBtn_->setChecked(false);
            spawnLabel_->setText("Click to spawn");
        } else {
            setFogTool(FogTool::None);
            spawnMode_ = true;
            spawnBtn_->setChecked(true);
            spawnLabel_->setText("Click spawn location");
        }
    });

    QObject::connect(display, &OBSDisplayWidget::sceneClicked, this,
                     [this, clickLabel, gridCheck, gridCellSpin, doSpawn](float x, float y, float nx,
                                                                         float ny, bool inside) {
                         if (display)
                             display->clearDirectionArrow();

                         lastClickX = x;
                         lastClickY = y;
                         lastClickInside = inside;

                         if (gridAlignMode_) {
                             gridAlignMode_ = false;
                             if (inside) {
                                 const int cell = gridCellSpin_->value();
                                 gridOffsetXSpin_->setValue(GridLayout::wrap(int(std::lround(x)), cell));
                                 gridOffsetYSpin_->setValue(GridLayout::wrap(int(std::lround(y)), cell));
                                 statusBar()->showMessage(QStringLiteral("Grid aligned"), 3000);
                             }
                             return;
                         }

                         if (!spawnMode_ && fogTool_ != FogTool::None) {
                             if (inside && currentFog_.Get()) {
                                 rpg_fog_begin_edit(currentFog_.Get());
                                 fogStrokeActive_ = true;
                                 fogLastX_ = x;
                                 fogLastY_ = y;
                                 fogRectX0_ = x;
                                 fogRectY0_ = y;
                                 fogRectX1_ = x;
                                 fogRectY1_ = y;
                                 if (fogTool_ == FogTool::Brush) {
                                     const float r = float(fogBrushSpin_->value());
                                     rpg_fog_stroke(currentFog_.Get(), x, y, x, y, r, fogRevealMode());
                                 }
                             }
                             return;
                         }

                         if (spawnMode_ && inside) {
                             doSpawn(x, y);
                             spawnMode_ = false;
                             spawnBtn_->setChecked(false);
                             spawnLabel_->setText("Click to spawn");
                             return;
                         }

                         if (!inside) {
                             clickLabel->setText("Click: (outside map)");
                             return;
                         }

                         QString text = QString("Click: x=%1 y=%2  |  n=(%3, %4)")
                                            .arg(x, 0, 'f', 1)
                                            .arg(y, 0, 'f', 1)
                                            .arg(nx, 0, 'f', 4)
                                            .arg(ny, 0, 'f', 4);
                         if (gridCheck->isChecked() && gridCellSpin->value() > 0) {
                             const GridLayout grid = gridLayout();
                             text += QString("  |  cell (%1, %2)").arg(grid.colAt(x)).arg(grid.rowAt(y));
                         }
                         clickLabel->setText(text);

                         const int idx = findNearestFxInstanceIndex(x, y);
                         if (idx >= 0 && idx < fxList->count())
                             fxList->setCurrentRow(idx);
                     });

    QObject::connect(clearAllBtn, &QAbstractButton::clicked, this, [this, fadeSpin]() {
        const int fadeMs = fadeSpin->value();
        for (const FxInstance &inst : activeFx) {
            if (!inst.locked) {
                fadeOutAndRemoveInstance(inst, fadeMs);
                removeFxFromPerMapStore(inst);
            }
        }
        refreshFxListForCurrentMap();
        releaseFxLockAndClearArrow();
    });

    // Version indicator (in the status bar).
    verLabel->setText(QString("Build (compile): %1 | Plugin mtime: %2")
                          .arg(kCompileStamp)
                          .arg(GetPluginMTimeString()));

    // Default to current scene
    obs_source_t *cur = obs_frontend_get_current_scene();
    if (cur) {
        const char *curName = obs_source_get_name(cur);
        int idx = sceneCombo_->findText(QString::fromUtf8(curName));
        if (idx >= 0) {
            sceneCombo_->setCurrentIndex(idx);
            display->setSceneByName(QString::fromUtf8(curName));
        }
        obs_source_release(cur);
    }

    syncFogUiForCurrentMap();
    updateStagedOverlay();
}

OBSSource RPGWindow::ensureGridSource()
{
    if (gridSource_.Get())
        return gridSource_;

    // Reuse our grid source if it exists. Older versions made an image_source with the same name;
    // that one is left alone (and hidden in the map scene) and the new source gets another name.
    QString name = QString::fromUtf8(kGridSourceName);
    for (int n = 2;; ++n) {
        OBSSourceAutoRelease existing(obs_get_source_by_name(name.toUtf8().constData()));
        if (!existing.Get())
            break;
        if (rpg_is_grid_source(existing.Get())) {
            gridSource_ = OBSSource(existing.Get());
            return gridSource_;
        }
        name = QStringLiteral("%1 %2").arg(QString::fromUtf8(kGridSourceName)).arg(n);
    }

    OBSSourceAutoRelease src(obs_source_create(RPG_GRID_SOURCE_ID, name.toUtf8().constData(), nullptr, nullptr));
    if (src.Get())
        gridSource_ = OBSSource(src.Get());
    return gridSource_;
}

GridLayout RPGWindow::gridLayout() const
{
    obs_video_info ovi = {};
    const bool haveVideo = obs_get_video_info(&ovi) && ovi.base_width > 0 && ovi.base_height > 0;
    const int w = haveVideo ? (int)ovi.base_width : 1920;
    const int h = haveVideo ? (int)ovi.base_height : 1080;
    return GridLayout(gridCellSpin_ ? gridCellSpin_->value() : 50, gridWidthSpin_ ? gridWidthSpin_->value() : 1,
                      gridOffsetXSpin_ ? gridOffsetXSpin_->value() : 0,
                      gridOffsetYSpin_ ? gridOffsetYSpin_->value() : 0, w, h);
}

static QString GetGridSettingsPath()
{
    char *path = obs_module_config_path("rpg-map-effects-grid.json");
    QString qpath = path ? QString::fromUtf8(path) : QString();
    bfree(path);
    return qpath;
}

void RPGWindow::loadGridSettings()
{
    QFile f(GetGridSettingsPath());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.isEmpty())
        return;
    gridCellSpin_->setValue(o.value("cellSize").toInt(gridCellSpin_->value()));
    gridWidthSpin_->setValue(o.value("lineWidth").toInt(gridWidthSpin_->value()));
    gridOpacitySpin_->setValue(o.value("opacity").toInt(gridOpacitySpin_->value()));
    gridOffsetXSpin_->setValue(o.value("offsetX").toInt(0));
    gridOffsetYSpin_->setValue(o.value("offsetY").toInt(0));
    const QColor color(o.value("color").toString());
    if (color.isValid() && display)
        display->setGridColor(color);
}

void RPGWindow::saveGridSettings() const
{
    if (!gridCellSpin_ || !gridWidthSpin_ || !gridOpacitySpin_ || !gridOffsetXSpin_ || !gridOffsetYSpin_)
        return;
    const QString path = GetGridSettingsPath();
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonObject o;
    o.insert("cellSize", gridCellSpin_->value());
    o.insert("lineWidth", gridWidthSpin_->value());
    o.insert("opacity", gridOpacitySpin_->value());
    o.insert("offsetX", gridOffsetXSpin_->value());
    o.insert("offsetY", gridOffsetYSpin_->value());
    if (display)
        o.insert("color", display->gridColor().name(QColor::HexRgb));
    QFile out(path);
    if (out.open(QIODevice::WriteOnly))
        out.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

void RPGWindow::updateGridSourceSettings(obs_source_t *gridSource)
{
    if (!gridSource || !display || !gridCellSpin_)
        return;

    const QColor c = display->gridColor();
    // OBS color properties are 0xAABBGGRR.
    const uint32_t abgr = (uint32_t(c.alpha()) << 24) | (uint32_t(c.blue()) << 16) | (uint32_t(c.green()) << 8) |
                          uint32_t(c.red());
    OBSDataAutoRelease settings = obs_data_create();
    obs_data_set_int(settings, "cell_size", gridCellSpin_->value());
    obs_data_set_int(settings, "line_width", gridWidthSpin_->value());
    obs_data_set_int(settings, "offset_x", gridOffsetXSpin_->value());
    obs_data_set_int(settings, "offset_y", gridOffsetYSpin_->value());
    obs_data_set_int(settings, "color", abgr);
    obs_data_set_int(settings, "opacity", gridOpacitySpin_->value());
    obs_source_update(gridSource, settings);
}

void RPGWindow::syncGridOutputToScene(const QString &sceneName, bool showOnOutput)
{
    if (!gridSource_.Get() || sceneName.isEmpty())
        return;

    OBSSourceAutoRelease sceneSrc = GetSceneSourceByName(sceneName);
    if (!sceneSrc.Get())
        return;

    obs_scene_t *scene = obs_scene_from_source(sceneSrc.Get());
    if (!scene)
        return;

    addGridToSceneAndSetVisible(scene, gridSource_.Get(), showOnOutput);
}

QString RPGWindow::selectedCursorFilename() const
{
    if (selectedCursorFilename_.isEmpty())
        return QStringLiteral("cursor.png");
    return selectedCursorFilename_;
}

void RPGWindow::updateCursorSourceImage()
{
    if (!cursorSource_.Get())
        return;
    const QString filename = selectedCursorFilename();
    const CursorAsset *asset = get_cursor_asset_by_name(filename.toUtf8().constData());
    if (!asset || !asset->data || asset->size == 0)
        return;
    char *configPath = obs_module_config_path("cursor.png");
    if (!configPath)
        return;
    QString path = QString::fromUtf8(configPath);
    bfree(configPath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(reinterpret_cast<const char *>(asset->data), static_cast<qint64>(asset->size));
        file.close();
    }
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "file", path.toUtf8().constData());
    obs_data_set_bool(settings, "unload", false);
    obs_source_update(cursorSource_.Get(), settings);
    obs_data_release(settings);
}

OBSSource RPGWindow::ensureCursorSource()
{
    if (cursorSource_.Get())
        return cursorSource_;

    const QString filename = selectedCursorFilename();
    const CursorAsset *asset = get_cursor_asset_by_name(filename.toUtf8().constData());
    if (!asset || !asset->data || asset->size == 0)
        return cursorSource_;

    char *configPath = obs_module_config_path("cursor.png");
    if (!configPath) {
        return cursorSource_;
    }
    QString path = QString::fromUtf8(configPath);
    bfree(configPath);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(reinterpret_cast<const char *>(asset->data), static_cast<qint64>(asset->size));
        file.close();
    }

    obs_source_t *existing = obs_get_source_by_name(kCursorSourceName);
    if (existing) {
        cursorSource_ = OBSSource(existing);
        obs_source_release(existing);
        obs_data_t *settings = obs_data_create();
        obs_data_set_string(settings, "file", path.toUtf8().constData());
        obs_data_set_bool(settings, "unload", false);
        obs_source_update(cursorSource_.Get(), settings);
        obs_data_release(settings);
        return cursorSource_;
    }

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "file", path.toUtf8().constData());
    obs_data_set_bool(settings, "unload", false);
    obs_source_t *src = obs_source_create("image_source", kCursorSourceName, settings, nullptr);
    obs_data_release(settings);
    if (src)
        cursorSource_ = OBSSource(src);
    return cursorSource_;
}

void RPGWindow::releaseSceneCollectionRefs()
{
    setFogTool(FogTool::None);
    currentFog_ = nullptr;
    gridSource_ = nullptr;
    cursorSource_ = nullptr;
}

void RPGWindow::releaseOBSResources()
{
    currentFog_ = nullptr;
    if (display)
        display->releaseOBSResources();
}

RPGWindow::~RPGWindow()
{
    /* Release our refs only. Do not call obs_frontend_get_scenes/obs_enter_graphics here:
     * destructor runs during plugin unload when OBS may already be shutting down;
     * that can crash. Scenes will be torn down by OBS. */
}
