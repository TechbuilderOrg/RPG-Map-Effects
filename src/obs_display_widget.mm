#if defined(__APPLE__)
#import <objc/objc.h>
#endif

#include "obs_display_widget.hpp"
#include "rpg_fog_source.h"
#include "grid_layout.h"

#include <QHideEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWindow>
#include <QTimer>
#include <QCursor>
#include <QApplication>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

extern "C" {
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <obs-frontend-api.h>
}

/** Draws the direction arrow on top of the display using Qt (so it always shows). */
class ArrowOverlay : public QWidget {
public:
    explicit ArrowOverlay(OBSDisplayWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        auto *disp = qobject_cast<OBSDisplayWidget *>(parent());
        if (!disp)
            return;
        float fromX = 0, fromY = 0, toX = 0, toY = 0;
        if (!disp->getDirectionArrow(fromX, fromY, toX, toY)) {
            // Arrow is hidden: clear overlay to transparent so previous arrow doesn't persist.
            QPainter p(this);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(rect(), Qt::transparent);
            return;
        }
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        if (!disp->canvasToWidget(fromX, fromY, width(), height(), x1, y1))
            return;
        if (!disp->canvasToWidget(toX, toY, width(), height(), x2, y2))
            return;
        QPainter p(this);
        // Always clear previous arrow completely so we only ever see one arrow.
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(rect(), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(0, 255, 255), 5));
        p.drawLine(x1, y1, x2, y2);
        float dx = float(x2 - x1);
        float dy = float(y2 - y1);
        float len = std::sqrt(dx * dx + dy * dy);
        if (len >= 1e-6f) {
            float ux = dx / len;
            float uy = dy / len;
            const float h = 20.0f;
            const float hw = 14.0f;
            float bx = float(x2) - ux * h;
            float by = float(y2) - uy * h;
            QPolygonF head;
            head << QPointF(x2, y2) << QPointF(bx - uy * hw, by + ux * hw)
                 << QPointF(bx + uy * hw, by - ux * hw);
            p.setBrush(QColor(0, 255, 255));
            p.drawPolygon(head);
        }
    }
};

static void GetScaleAndCenterPos(uint32_t baseCX, uint32_t baseCY, uint32_t windowCX,
                                 uint32_t windowCY, int &x, int &y, float &scale);

static bool MapWidgetPosToScene(OBSDisplayWidget *w, const QPointF &widgetPos, float &sceneX,
			       float &sceneY, float &normX, float &normY);

OBSDisplayWidget::OBSDisplayWidget(QWidget *parent) : QWidget(parent)
{
    // OBS display needs a native window handle
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_StaticContents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_DontCreateNativeAncestors);

    setMouseTracking(true);
    arrowOverlay_ = new ArrowOverlay(this);
    arrowOverlay_->setGeometry(0, 0, width(), height());
    arrowOverlay_->raise();

    // Fallback mouse tracking: poll global cursor so we still get sceneMouseMoved even
    // when Qt doesn't deliver move events without a pressed button (e.g. macOS native surfaces).
    mousePollTimer_ = new QTimer(this);
    mousePollTimer_->setInterval(33); // ~30 FPS
    connect(mousePollTimer_, &QTimer::timeout, this, [this]() {
        QPoint globalPos = QCursor::pos();
        QPointF localPos = mapFromGlobal(globalPos);

        float sx = 0.0f;
        float sy = 0.0f;
        float nx = 0.0f;
        float ny = 0.0f;

        const bool inside = MapWidgetPosToScene(this, localPos, sx, sy, nx, ny);
        const bool leftButtonDown = mouseLeftDown_;
        emit sceneMouseMoved(sx, sy, nx, ny, inside, leftButtonDown);
    });
    mousePollTimer_->start();
}

void OBSDisplayWidget::releaseOBSResources()
{
    destroyDisplay();
    if (sceneSource) {
        if (showing)
            obs_source_dec_showing(sceneSource);
        obs_source_release(sceneSource);
        sceneSource = nullptr;
    }
    showing = false;
}

OBSDisplayWidget::~OBSDisplayWidget()
{
    if (display || sceneSource)
        releaseOBSResources();
}

void OBSDisplayWidget::setSceneByName(const QString &sceneName)
{
    obs_source_t *newScene = nullptr;

    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);

    for (size_t i = 0; i < scenes.sources.num; i++) {
        obs_source_t *src = scenes.sources.array[i];
        const char *name = obs_source_get_name(src);
        if (sceneName == QString::fromUtf8(name)) {
            // Get a strong reference we own
            newScene = obs_source_get_ref(src);
            break;
        }
    }

    obs_frontend_source_list_free(&scenes);

    if (sceneSource) {
        if (showing)
            obs_source_dec_showing(sceneSource);
        obs_source_release(sceneSource);
    }

    if (newScene && showing)
        obs_source_inc_showing(newScene);

    sceneSource = newScene;
}

obs_source_t *OBSDisplayWidget::getSceneSourceRef() const
{
    return sceneSource ? obs_source_get_ref(sceneSource) : nullptr;
}

void OBSDisplayWidget::setDirectionArrow(float fromX, float fromY, float toX, float toY)
{
    {
        std::lock_guard<std::mutex> lock(directionArrowMutex_);
        directionArrowShow_ = true;
        directionArrowFromX_ = fromX;
        directionArrowFromY_ = fromY;
        directionArrowToX_ = toX;
        directionArrowToY_ = toY;
    }
    if (arrowOverlay_)
        arrowOverlay_->update();
}

void OBSDisplayWidget::clearDirectionArrow()
{
    {
        std::lock_guard<std::mutex> lock(directionArrowMutex_);
        directionArrowShow_ = false;
    }
    if (arrowOverlay_)
        arrowOverlay_->update();
}

bool OBSDisplayWidget::getDirectionArrow(float &fromX, float &fromY, float &toX, float &toY) const
{
    std::lock_guard<std::mutex> lock(directionArrowMutex_);
    if (!directionArrowShow_)
        return false;
    fromX = directionArrowFromX_;
    fromY = directionArrowFromY_;
    toX = directionArrowToX_;
    toY = directionArrowToY_;
    return true;
}

void OBSDisplayWidget::setStagedItemIds(const std::vector<int64_t> &ids)
{
    std::lock_guard<std::mutex> lock(overlayMutex_);
    stagedItemIds_ = ids;
}

void OBSDisplayWidget::setBrushOverlay(bool show, float x, float y, float radius)
{
    std::lock_guard<std::mutex> lock(overlayMutex_);
    brushShow_ = show;
    brushX_ = x;
    brushY_ = y;
    brushRadius_ = radius;
}

void OBSDisplayWidget::setRectOverlay(bool show, float x0, float y0, float x1, float y1)
{
    std::lock_guard<std::mutex> lock(overlayMutex_);
    rectShow_ = show;
    rectX0_ = x0;
    rectY0_ = y0;
    rectX1_ = x1;
    rectY1_ = y1;
}

bool OBSDisplayWidget::canvasToWidget(float canvasX, float canvasY, int widgetW, int widgetH,
                                      int &outX, int &outY) const
{
    if (widgetW <= 0 || widgetH <= 0)
        return false;
    obs_video_info ovi = {};
    if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
        return false;
    const qreal dpr = devicePixelRatioF();
    const uint32_t ww = uint32_t(std::max(1, int(widgetW * dpr)));
    const uint32_t wh = uint32_t(std::max(1, int(widgetH * dpr)));
    const uint32_t sw = ovi.base_width;
    const uint32_t sh = ovi.base_height;
    int viewX = 0;
    int viewY = 0;
    float scale = 1.0f;
    GetScaleAndCenterPos(sw, sh, ww, wh, viewX, viewY, scale);
    outX = int((viewX + canvasX * scale) / dpr);
    outY = int((viewY + canvasY * scale) / dpr);
    return true;
}

void OBSDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (!showing) {
        showing = true;
        if (sceneSource)
            obs_source_inc_showing(sceneSource);
    }

    // Defer display creation until the surface is exposed/painted.
    update();
}

void OBSDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    destroyDisplay();

    if (showing) {
        showing = false;
        if (sceneSource)
            obs_source_dec_showing(sceneSource);
    }
}

void OBSDisplayWidget::paintEvent(QPaintEvent *e)
{
    createDisplay();
    QWidget::paintEvent(e);
}

void OBSDisplayWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);

    createDisplay();

    if (display) {
        const qreal dpr = devicePixelRatioF();
        obs_display_resize(display, uint32_t(width() * dpr), uint32_t(height() * dpr));
    }
    if (arrowOverlay_) {
        arrowOverlay_->setGeometry(0, 0, width(), height());
        arrowOverlay_->raise();
    }
}

void OBSDisplayWidget::createDisplay()
{
    if (display)
        return;

    // For child widgets, `windowHandle()` may refer to the top-level window.
    // We still need the widget's own native view/handle for the display target.
    QWindow *topLevel = window()->windowHandle();
    if (!topLevel || !topLevel->isExposed())
        return;

    // Ensure the widget has a native handle/view
    if (!winId())
        return;

    gs_init_data info = {};
    const qreal dpr = devicePixelRatioF();
    info.cx = uint32_t(width() * dpr);
    info.cy = uint32_t(height() * dpr);
    info.format = GS_BGRA;
    info.zsformat = GS_ZS_NONE;

#if defined(__APPLE__)
    // Match OBS' OBSQTDisplay approach: for a Qt native window on macOS,
    // winId() is the NSView*; Qt may expose it as WId (unsigned long long). __bridge: no ownership transfer.
    info.window.view = (__bridge id)(void *)winId();
#elif defined(_WIN32)
    info.window.hwnd = (void *)winId();
#else
    // On Linux/X11, winId() is typically the XID.
    info.window.id = (uint64_t)winId();
#endif

    display = obs_display_create(&info, 0);
    if (!display)
        return;

    obs_display_add_draw_callback(display, &OBSDisplayWidget::DrawCallback, this);
}

void OBSDisplayWidget::destroyDisplay()
{
    if (!display)
        return;

    obs_display_remove_draw_callback(display, &OBSDisplayWidget::DrawCallback, this);
    obs_display_destroy(display);
    display = nullptr;

    obs_enter_graphics();
    if (ghostTexrender_)
        gs_texrender_destroy(ghostTexrender_);
    if (ghostEffect_)
        gs_effect_destroy(ghostEffect_);
    obs_leave_graphics();
    ghostTexrender_ = nullptr;
    ghostEffect_ = nullptr;
    ghostEffectFailed_ = false;
}

void OBSDisplayWidget::setGridOverlay(bool show, int cellSizePx)
{
    gridShow_ = show;
    gridCellSize_ = (cellSizePx > 0) ? cellSizePx : 50;
}

static inline void GetScaleAndCenterPos(uint32_t baseCX, uint32_t baseCY, uint32_t windowCX,
					uint32_t windowCY, int &x, int &y, float &scale)
{
	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);

	int newCX = 0;
	int newCY = 0;

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = int(windowCY);
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = int(windowCX);
		newCY = int(float(windowCX) / float(baseAspect));
	}

	x = int(windowCX / 2u - uint32_t(newCX) / 2u);
	y = int(windowCY / 2u - uint32_t(newCY) / 2u);
}

static bool MapWidgetPosToScene(OBSDisplayWidget *w, const QPointF &widgetPos, float &sceneX,
			       float &sceneY, float &normX, float &normY)
{
	if (!w)
		return false;

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
		return false;

	const uint32_t sw = ovi.base_width;
	const uint32_t sh = ovi.base_height;

	const qreal dpr = w->devicePixelRatioF();
	const float px = float(widgetPos.x() * dpr);
	const float py = float(widgetPos.y() * dpr);

	const uint32_t ww = uint32_t(std::max(1, int(w->width() * dpr)));
	const uint32_t wh = uint32_t(std::max(1, int(w->height() * dpr)));

	int viewX = 0;
	int viewY = 0;
	float scale = 1.0f;
	GetScaleAndCenterPos(sw, sh, ww, wh, viewX, viewY, scale);

	const float viewW = float(sw) * scale;
	const float viewH = float(sh) * scale;

	if (px < float(viewX) || py < float(viewY) || px >= float(viewX) + viewW ||
	    py >= float(viewY) + viewH)
		return false;

	sceneX = (px - float(viewX)) / scale;
	sceneY = (py - float(viewY)) / scale;

	// Clamp just in case of edge float precision
	sceneX = std::max(0.0f, std::min(sceneX, float(sw)));
	sceneY = std::max(0.0f, std::min(sceneY, float(sh)));

	normX = (sw > 0) ? (sceneX / float(sw)) : 0.0f;
	normY = (sh > 0) ? (sceneY / float(sh)) : 0.0f;

	return true;
}

void OBSDisplayWidget::DrawCallback(void *data, uint32_t cx, uint32_t cy)
{
    auto *self = static_cast<OBSDisplayWidget *>(data);
    if (!self || !self->sceneSource || cx == 0 || cy == 0)
        return;

	// Use OBS base (canvas) resolution for scenes
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0)
		return;

	const uint32_t sw = ovi.base_width;
	const uint32_t sh = ovi.base_height;

	int viewX = 0;
	int viewY = 0;
	float scale = 1.0f;
	GetScaleAndCenterPos(sw, sh, cx, cy, viewX, viewY, scale);

	const int viewCX = int(float(sw) * scale);
	const int viewCY = int(float(sh) * scale);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();

	// Clear entire render target
	struct vec4 clear_color;
	vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_set_viewport(0, 0, (int)cx, (int)cy);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	// Now render the scene letterboxed/pillarboxed inside the centered viewport
	gs_set_viewport(viewX, viewY, viewCX, viewCY);
	gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
	gs_matrix_identity();

	// Fog sources draw semi-transparent while this flag is set (GM can see under the fog).
	rpg_set_gm_preview_rendering(true);
	obs_source_video_render(self->sceneSource);
	self->drawStagedGhosts();
	rpg_set_gm_preview_rendering(false);

    // Optional grid overlay in scene space (preview-only helper). Same layout as the output grid image.
    if (self->gridShow_ && self->gridCellSize_ > 0) {
        const GridLayout grid(self->gridCellSize_, self->gridLineWidth_, self->gridOffsetX_, self->gridOffsetY_,
                              int(sw), int(sh));
        if (grid.valid()) {
            auto emitRect = [](float x0, float y0, float x1, float y1) {
                gs_vertex2f(x0, y0);
                gs_vertex2f(x1, y0);
                gs_vertex2f(x0, y1);
                gs_vertex2f(x1, y0);
                gs_vertex2f(x1, y1);
                gs_vertex2f(x0, y1);
            };
            const float w = float(grid.lineWidth);
            const float top = float(grid.lineY(0)), bottom = float(grid.bottom());
            const float left = float(grid.lineX(0)), right = float(grid.right());

            gs_render_start(true);
            for (int k = 0; k <= grid.cols(); ++k)
                emitRect(float(grid.lineX(k)), top, float(grid.lineX(k)) + w, bottom);
            for (int k = 0; k <= grid.rows(); ++k)
                emitRect(left, float(grid.lineY(k)), right, float(grid.lineY(k)) + w);

            gs_vertbuffer_t *vb = gs_render_save();
            if (vb) {
                gs_load_vertexbuffer(vb);
                gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
                gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");

                const QColor c = self->gridColor_;
                const int alpha = std::clamp(c.alpha() * self->gridOpacity_ / 100, 0, 255);
                const uint32_t argb =
                    (uint32_t(alpha) << 24) | (uint32_t(c.red()) << 16) |
                    (uint32_t(c.green()) << 8) | uint32_t(c.blue());

                gs_effect_set_color(color, argb);
                gs_blend_state_push();
                gs_enable_blending(true);
                gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
                while (gs_effect_loop(solid, "Solid"))
                    gs_draw(GS_TRIS, 0, 0);
                gs_blend_state_pop();
                gs_load_vertexbuffer(nullptr);
                gs_vertexbuffer_destroy(vb);
            }
        }
    }

	self->drawToolOverlays();

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

/** Draw a closed polyline / circle outline in canvas space with the solid effect. */
static void DrawSolidLineStrip(const std::vector<vec2> &pts, uint32_t argb)
{
    if (pts.size() < 2)
        return;
    gs_render_start(true);
    for (const vec2 &p : pts)
        gs_vertex2f(p.x, p.y);
    gs_vertbuffer_t *vb = gs_render_save();
    if (!vb)
        return;
    gs_load_vertexbuffer(vb);
    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), argb);
    while (gs_effect_loop(solid, "Solid"))
        gs_draw(GS_LINESTRIP, 0, 0);
    gs_load_vertexbuffer(nullptr);
    gs_vertexbuffer_destroy(vb);
}

static std::vector<vec2> CirclePoints(float cx, float cy, float r, int segments = 64)
{
    std::vector<vec2> pts;
    pts.reserve(size_t(segments) + 1);
    for (int i = 0; i <= segments; ++i) {
        const float a = float(i) / float(segments) * 6.2831853f;
        vec2 p;
        vec2_set(&p, cx + std::cos(a) * r, cy + std::sin(a) * r);
        pts.push_back(p);
    }
    return pts;
}

void OBSDisplayWidget::drawStagedGhosts()
{
    std::vector<int64_t> ids;
    {
        std::lock_guard<std::mutex> lock(overlayMutex_);
        ids = stagedItemIds_;
    }
    if (ids.empty() || !sceneSource)
        return;
    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    if (!scene)
        return;

    if (!ghostEffect_ && !ghostEffectFailed_) {
        ghostEffect_ = rpg_create_effect();
        ghostEffectFailed_ = !ghostEffect_;
    }
    if (!ghostTexrender_)
        ghostTexrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

    static constexpr uint32_t kStagedColor = 0xFFFFA000; // amber (ARGB)

    for (int64_t id : ids) {
        obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, id);
        if (!item)
            continue;
        obs_source_t *src = obs_sceneitem_get_source(item);
        const uint32_t w = src ? obs_source_get_width(src) : 0;
        const uint32_t h = src ? obs_source_get_height(src) : 0;

        if (ghostEffect_ && ghostTexrender_ && w > 0 && h > 0) {
            // Render the hidden FX into a texture, then draw it translucent with the item's transform.
            gs_texrender_reset(ghostTexrender_);
            if (gs_texrender_begin(ghostTexrender_, w, h)) {
                vec4 zero;
                vec4_zero(&zero);
                gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
                gs_ortho(0.0f, float(w), 0.0f, float(h), -100.0f, 100.0f);
                gs_blend_state_push();
                gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
                                           GS_BLEND_INVSRCALPHA);
                obs_source_video_render(src);
                gs_blend_state_pop();
                gs_texrender_end(ghostTexrender_);

                gs_texture_t *tex = gs_texrender_get_texture(ghostTexrender_);
                if (tex) {
                    matrix4 m;
                    obs_sceneitem_get_draw_transform(item, &m);
                    gs_matrix_push();
                    gs_matrix_mul(&m);
                    gs_blend_state_push();
                    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
                    gs_effect_set_texture(gs_effect_get_param_by_name(ghostEffect_, "image"), tex);
                    gs_effect_set_float(gs_effect_get_param_by_name(ghostEffect_, "opacity"), 0.55f);
                    while (gs_effect_loop(ghostEffect_, "DrawOpacity"))
                        gs_draw_sprite(tex, 0, w, h);
                    gs_blend_state_pop();
                    gs_matrix_pop();
                }
            }
        }

        // Marker at the FX anchor (FX items are center-aligned) so even invisible content is findable.
        vec2 pos;
        obs_sceneitem_get_pos(item, &pos);
        DrawSolidLineStrip(CirclePoints(pos.x, pos.y, 26.0f, 32), kStagedColor);
        DrawSolidLineStrip(CirclePoints(pos.x, pos.y, 30.0f, 32), kStagedColor);
        vec2 a, b;
        vec2_set(&a, pos.x - 12.0f, pos.y);
        vec2_set(&b, pos.x + 12.0f, pos.y);
        DrawSolidLineStrip({a, b}, kStagedColor);
        vec2_set(&a, pos.x, pos.y - 12.0f);
        vec2_set(&b, pos.x, pos.y + 12.0f);
        DrawSolidLineStrip({a, b}, kStagedColor);
    }
}

void OBSDisplayWidget::drawToolOverlays()
{
    bool brushShow, rectShow;
    float bx, by, br, x0, y0, x1, y1;
    {
        std::lock_guard<std::mutex> lock(overlayMutex_);
        brushShow = brushShow_;
        bx = brushX_;
        by = brushY_;
        br = brushRadius_;
        rectShow = rectShow_;
        x0 = rectX0_;
        y0 = rectY0_;
        x1 = rectX1_;
        y1 = rectY1_;
    }
    static constexpr uint32_t kToolColor = 0xFF40E0FF; // cyan (ARGB)
    if (brushShow && br > 0.0f)
        DrawSolidLineStrip(CirclePoints(bx, by, br), kToolColor);
    if (rectShow) {
        std::vector<vec2> pts(5);
        vec2_set(&pts[0], x0, y0);
        vec2_set(&pts[1], x1, y0);
        vec2_set(&pts[2], x1, y1);
        vec2_set(&pts[3], x0, y1);
        pts[4] = pts[0];
        DrawSolidLineStrip(pts, kToolColor);
    }
}

void OBSDisplayWidget::mousePressEvent(QMouseEvent *e)
{
	float sx = 0.0f;
	float sy = 0.0f;
	float nx = 0.0f;
	float ny = 0.0f;
	const bool inside = MapWidgetPosToScene(this, e->position(), sx, sy, nx, ny);

	if (e->button() == Qt::LeftButton) {
		mouseLeftDown_ = true;
		QWidget::mousePressEvent(e);
		emit sceneClicked(sx, sy, nx, ny, inside);
		return;
	}
	if (e->button() == Qt::RightButton) {
		QWidget::mousePressEvent(e);
		emit sceneRightClicked(sx, sy, nx, ny, inside);
		return;
	}
	QWidget::mousePressEvent(e);
}

void OBSDisplayWidget::mouseMoveEvent(QMouseEvent *e)
{
	// We rely on the polling timer to emit sceneMouseMoved so that we always
	// get updates even when the mouse is not pressed. Just forward to base.
	QWidget::mouseMoveEvent(e);
}

void OBSDisplayWidget::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		mouseLeftDown_ = false;
		clearDirectionArrow();
		emit sceneLeftReleased();
	}
	QWidget::mouseReleaseEvent(e);
}
