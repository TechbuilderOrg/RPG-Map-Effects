#pragma once
#include <QColor>
#include <QMap>
#include <QMainWindow>
#include <QSet>
#include <QString>
#include <vector>

#include <obs.hpp>

#include "grid_layout.h"

class OBSDisplayWidget;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QToolButton;
class QLineEdit;
class QTimer;

class RPGWindow : public QMainWindow {
    Q_OBJECT
public:
    RPGWindow();
    ~RPGWindow() override;

    /** Release OBS resources (display, scene ref). Call on OBS_FRONTEND_EVENT_EXIT to avoid shutdown crash. */
    void releaseOBSResources();
    /** Drop our strong refs to sources of the current scene collection so OBS can free them
     *  (OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP: exit or switching collections). */
    void releaseSceneCollectionRefs();

    /** Reveal every staged FX on the current battlemap (toolbar button and hotkey). */
    void triggerAllStaged();
    /** Reveal the first staged FX (in list order) on the current battlemap. */
    void triggerNextStaged();

private:
    OBSSource ensureGridSource();
    /** Push the current grid settings to the on-output grid source. */
    void updateGridSourceSettings(obs_source_t *gridSource);
    void syncGridOutputToScene(const QString &sceneName, bool showOnOutput);

    struct FxInstance {
        QString mapSceneUuid;
        QString effectUuid;
        int64_t sceneItemId = 0;
        QString label;        // user-visible label text on the map
        QString templateName; // template scene name used for this instance
        bool locked = false;  // when true, excluded from Clear all
        /** Staged FX are placed on the map but hidden on output until triggered. The GM preview
         *  draws them as translucent ghosts so they can be moved / rotated / resized first. */
        bool staged = false;
        // Spawn settings remembered so a staged FX plays exactly as configured when triggered.
        bool sequence = true;
        int lifetimeSec = 0;
        int fadeMs = 600;
    };
    static bool sameFx(const FxInstance &a, const FxInstance &b);

private:
    OBSDisplayWidget *display = nullptr;

    // Grid settings (toolbar). Saved to the module config so they survive restarts.
    QSpinBox *gridCellSpin_ = nullptr;
    QSpinBox *gridWidthSpin_ = nullptr;
    QSpinBox *gridOpacitySpin_ = nullptr;
    QSpinBox *gridOffsetXSpin_ = nullptr;
    QSpinBox *gridOffsetYSpin_ = nullptr;
    QFrame *gridAlignPopup_ = nullptr;
    /** Next left click on the map sets the grid offset so a grid corner lands there. */
    bool gridAlignMode_ = false;
    /** Layout of the grid on the current canvas (shared by output image, preview and snapping). */
    GridLayout gridLayout() const;
    void loadGridSettings();
    void saveGridSettings() const;

    QColor labelColor_ = Qt::white;

    float lastClickX = 0.0f;
    float lastClickY = 0.0f;
    bool lastClickInside = false;

    /** Spawn workflow: user clicks Spawn button then clicks map to place. */
    bool spawnMode_ = false;
    QToolButton *spawnBtn_ = nullptr;
    QLabel *spawnLabel_ = nullptr;

    /** FX for the currently selected battlemap only (what we show in the sidebar). */
    std::vector<FxInstance> activeFx;
    /** All FX keyed by battlemap scene UUID; switching maps shows only that map's FX. */
    QMap<QString, std::vector<FxInstance>> fxByMapSceneUuid_;
    QString currentMapSceneUuid_;

    /** Right-click menu lock: move, rotate, or resize the selected FX until left-click releases. */
    enum class FxLockMode { None, Move, Rotate, Resize };
    FxLockMode fxLockMode_ = FxLockMode::None;

    /** Resize mode: initial scale and distance from FX center when Resize was entered. */
    float resizeStartScale_ = 1.0f;
    float resizeStartDistance_ = 100.0f;

    QListWidget *fxList = nullptr;
    QLineEdit *labelEdit = nullptr;
    QComboBox *sceneCombo_ = nullptr;
    QToolButton *cursorBtn_ = nullptr;
    QFrame *cursorPopup_ = nullptr;
    QString selectedCursorFilename_ = QStringLiteral("cursor.png");

    OBSSource gridSource_;
    OBSSource cursorSource_;

    /** Selected cursor asset filename. */
    QString selectedCursorFilename() const;
    /** Create or return the cursor image source (uses selected cursor asset). Adds to scene only when user chooses Show cursor. */
    OBSSource ensureCursorSource();
    /** Write selected cursor asset to config file and update cursor source if it exists. */
    void updateCursorSourceImage();

    void fadeOutAndRemoveInstance(const FxInstance &inst, int fadeMs);
    int findNearestFxInstanceIndex(float sceneX, float sceneY) const;
    /** Returns the scene source UUID for the given battlemap scene name, or empty if not found. */
    QString getMapSceneUuidFromName(const QString &sceneName) const;
    /** Reload activeFx and fxList from fxByMapSceneUuid_ for the current map. */
    void refreshFxListForCurrentMap();
    /** Remove the given FX from fxByMapSceneUuid_ (by effectUuid + sceneItemId). */
    void removeFxFromPerMapStore(const FxInstance &inst);
    /** Update the stored copy of this FX's label in fxByMapSceneUuid_. */
    void syncStoredLabel(const FxInstance &inst, const QString &newLabel);
    /** Update the stored copy of this FX's locked state in fxByMapSceneUuid_. */
    void syncStoredLocked(const FxInstance &inst, bool locked);
    /** Clear move/rotate lock and direction arrow. */
    void releaseFxLockAndClearArrow();
    /** Get scene item for this instance (non-owning; do not use after scene changes). */
    obs_sceneitem_t *getSceneItemForInstance(const FxInstance &inst) const;
    /** Convert canvas (base) coords to scene space. Returns false if sizes invalid. */
    bool canvasToScene(obs_source_t *mapSource, float canvasX, float canvasY, float &outSceneX, float &outSceneY) const;
    /** Convert scene position to canvas (base) space. Returns false if sizes invalid. */
    bool scenePosToCanvas(obs_source_t *mapSource, float posX, float posY, float &outCX, float &outCY) const;
    /** Clear the FX at the given list row: fade out, remove from store/list, release lock. */
    void clearFxAtRow(int row, int fadeMs);

    // --- Staged FX ---
    /** Reveal a staged FX on output (runs its sequence, starts its lifetime). */
    void triggerFx(const FxInstance &inst);
    /** Fade out and remove the FX after lifetimeSec (if it still exists then). */
    void scheduleLifetime(const FxInstance &inst, int lifetimeSec, int fadeMs);
    /** Drop the preview "showing" reference held while an FX is staged. */
    void releaseStagedShowing(const QString &effectUuid);
    /** Push staged FX of the current map to the preview and refresh trigger button / list tags. */
    void updateStagedOverlay();
    QListWidgetItem *makeFxListItem(const FxInstance &inst) const;
    /** Effect UUIDs whose FX source we called obs_source_inc_showing on while staged. */
    QSet<QString> stagedShowingUuids_;
    QPushButton *triggerAllBtn_ = nullptr;
    QPushButton *triggerSelectedBtn_ = nullptr;

    // --- Fog of war ---
    enum class FogTool { None, Brush, Rect };
    FogTool fogTool_ = FogTool::None;
    bool fogStrokeActive_ = false;
    float fogLastX_ = 0.0f;
    float fogLastY_ = 0.0f;
    float fogRectX0_ = 0.0f;
    float fogRectY0_ = 0.0f;
    float fogRectX1_ = 0.0f;
    float fogRectY1_ = 0.0f;
    /** Fog source of the current battlemap (empty when the map has no fog). */
    OBSSource currentFog_;
    QCheckBox *fogEnableCheck_ = nullptr;
    QPushButton *fogBrushBtn_ = nullptr;
    QPushButton *fogRectBtn_ = nullptr;
    QRadioButton *fogRevealRadio_ = nullptr;
    QSpinBox *fogBrushSpin_ = nullptr;
    QSpinBox *fogGmOpacitySpin_ = nullptr;
    QToolButton *fogColorBtn_ = nullptr;

    /** Find (and optionally create + add) the fog source in the current battlemap scene. */
    OBSSource fogSourceForCurrentMap(bool create);
    /** Show or hide fog on the current battlemap (creating the fog source on first use). */
    void setFogEnabledForCurrentMap(bool enabled);
    /** Refresh currentFog_ and the fog controls after switching battlemaps. */
    void syncFogUiForCurrentMap();
    void setFogTool(FogTool tool);
    bool fogRevealMode() const;
    void setFogSetting(const char *name, long long value);
};
