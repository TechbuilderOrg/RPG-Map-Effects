#pragma once
#include <QWidget>
#include <QPaintEngine>
#include <QString>
#include <QColor>

#include <cstdint>
#include <mutex>
#include <vector>

class QMouseEvent;
class ArrowOverlay;
class QTimer;

extern "C" {
#include <obs.h>
}

class OBSDisplayWidget : public QWidget {
    Q_OBJECT
public:
    explicit OBSDisplayWidget(QWidget *parent = nullptr);
    ~OBSDisplayWidget() override;

    void setSceneByName(const QString &sceneName);
    obs_source_t *getSceneSourceRef() const;

    void setGridOverlay(bool show, int cellSizePx);
    void setGridColor(const QColor &color) { gridColor_ = color; }
    void setGridLineWidth(int pixels) { gridLineWidth_ = (pixels < 1) ? 1 : ((pixels > 10) ? 10 : pixels); }
    bool gridVisible() const { return gridShow_; }
    int gridCellSize() const { return gridCellSize_; }
    int gridLineWidth() const { return gridLineWidth_; }
    QColor gridColor() const { return gridColor_; }

    void setDirectionArrow(float fromX, float fromY, float toX, float toY);
    void clearDirectionArrow();

    /** Scene item ids (in the displayed scene) of staged FX. They are hidden on output, so the
     *  preview draws them itself as translucent "ghosts" with a marker. */
    void setStagedItemIds(const std::vector<int64_t> &ids);

    /** Fog tool overlays (preview only): round brush outline, or a rectangle being dragged. */
    void setBrushOverlay(bool show, float x, float y, float radius);
    void setRectOverlay(bool show, float x0, float y0, float x1, float y1);

    /** Convert canvas (scene) coordinates to widget coordinates for overlay drawing. */
    bool canvasToWidget(float canvasX, float canvasY, int widgetW, int widgetH,
                        int &outX, int &outY) const;

    /** For overlay: get current arrow state (returns false if not showing). */
    bool getDirectionArrow(float &fromX, float &fromY, float &toX, float &toY) const;

    /** Release OBS display and scene source. Call on OBS_FRONTEND_EVENT_EXIT so destructor does not touch OBS during shutdown. */
    void releaseOBSResources();

signals:
    void sceneClicked(float sceneX, float sceneY, float normX, float normY, bool inside);
    void sceneRightClicked(float sceneX, float sceneY, float normX, float normY, bool inside);
    void sceneLeftReleased();
    void sceneMouseMoved(float sceneX, float sceneY, float normX, float normY, bool inside, bool leftButtonDown);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

    // Prevent Qt from painting over the native surface.
    QPaintEngine *paintEngine() const override { return nullptr; }

private:
    void createDisplay();
    void destroyDisplay();

    static void DrawCallback(void *data, uint32_t cx, uint32_t cy);
    void drawStagedGhosts();
    void drawToolOverlays();

    obs_display_t *display = nullptr;
    obs_source_t *sceneSource = nullptr;
    bool showing = false;

    bool gridShow_ = false;
    int gridCellSize_ = 50;
    int gridLineWidth_ = 1;
    QColor gridColor_ = QColor(96, 96, 96, 255);

    mutable std::mutex directionArrowMutex_;
    bool directionArrowShow_ = false;
    float directionArrowFromX_ = 0.0f;
    float directionArrowFromY_ = 0.0f;
    float directionArrowToX_ = 0.0f;
    float directionArrowToY_ = 0.0f;

    ArrowOverlay *arrowOverlay_ = nullptr;

    mutable std::mutex overlayMutex_;
    std::vector<int64_t> stagedItemIds_;
    bool brushShow_ = false;
    float brushX_ = 0.0f;
    float brushY_ = 0.0f;
    float brushRadius_ = 0.0f;
    bool rectShow_ = false;
    float rectX0_ = 0.0f;
    float rectY0_ = 0.0f;
    float rectX1_ = 0.0f;
    float rectY1_ = 0.0f;

    // Graphics thread only (destroyed in destroyDisplay).
    gs_texrender_t *ghostTexrender_ = nullptr;
    gs_effect_t *ghostEffect_ = nullptr;
    bool ghostEffectFailed_ = false;

    QTimer *mousePollTimer_ = nullptr;
    bool mouseLeftDown_ = false;
};
