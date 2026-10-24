#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/**
 * Grid geometry shared by the output grid image, the preview grid, snapping and the cell readout,
 * so they always agree.
 *
 * Only whole squares are laid out: every line is a full-width band [p, p + lineWidth) on the
 * left/top edge of its cell, and a closing line follows the last whole cell. Every square
 * therefore has the same size, including at the canvas edges, where a strip narrower than one
 * cell is left without grid. When the squares fill the canvas exactly (e.g. 60 px on 1920x1080)
 * there is no room after the last square for the closing line, so it is moved inside the canvas
 * edge; only then is the last square narrower, by at most the line width.
 *
 * The offset (wrapped to [0, cell)) moves the grid so it can be lined up with the squares printed
 * on a battlemap image.
 */
struct GridLayout {
    int cell = 50;
    int lineWidth = 1;
    int offsetX = 0;
    int offsetY = 0;
    int canvasW = 0;
    int canvasH = 0;

    GridLayout(int cellSize, int lineW, int offX, int offY, int width, int height)
        : cell(std::max(1, cellSize)),
          lineWidth(std::clamp(lineW, 1, std::max(1, cellSize - 1))),
          offsetX(wrap(offX, std::max(1, cellSize))),
          offsetY(wrap(offY, std::max(1, cellSize))),
          canvasW(width),
          canvasH(height)
    {
    }

    static int wrap(int v, int m) { return ((v % m) + m) % m; }

    /** Number of whole squares across / down. */
    int cols() const { return std::max(0, (canvasW - offsetX) / cell); }
    int rows() const { return std::max(0, (canvasH - offsetY) / cell); }
    bool valid() const { return cols() > 0 && rows() > 0; }

    /** Left / top edge of line k (k = 0 .. cols() / rows()). */
    int lineX(int k) const { return std::min(offsetX + k * cell, canvasW - lineWidth); }
    int lineY(int k) const { return std::min(offsetY + k * cell, canvasH - lineWidth); }
    /** Right / bottom edge of the whole grid (end of the closing line). */
    int right() const { return lineX(cols()) + lineWidth; }
    int bottom() const { return lineY(rows()) + lineWidth; }

    /** Square containing a canvas position (may be outside 0 .. cols()-1 near the edges). */
    int colAt(float x) const { return int(std::floor((x - float(offsetX)) / float(cell))); }
    int rowAt(float y) const { return int(std::floor((y - float(offsetY)) / float(cell))); }

    /** Center of the nearest whole square's interior (between its lines). */
    float snapX(float x) const
    {
        const int c = std::clamp(colAt(x), 0, std::max(0, cols() - 1));
        return float(lineX(c)) + float(cell + lineWidth) * 0.5f;
    }
    float snapY(float y) const
    {
        const int r = std::clamp(rowAt(y), 0, std::max(0, rows() - 1));
        return float(lineY(r)) + float(cell + lineWidth) * 0.5f;
    }

    /** Expand a span to whole squares, lines included (for rectangle tools). */
    void snapSpanX(float &x0, float &x1) const
    {
        const int c0 = std::clamp(colAt(std::min(x0, x1)), 0, cols());
        const int c1 = std::clamp(colAt(std::max(x0, x1)) + 1, 0, cols());
        x0 = float(lineX(c0));
        x1 = float(lineX(c1) + lineWidth);
    }
    void snapSpanY(float &y0, float &y1) const
    {
        const int r0 = std::clamp(rowAt(std::min(y0, y1)), 0, rows());
        const int r1 = std::clamp(rowAt(std::max(y0, y1)) + 1, 0, rows());
        y0 = float(lineY(r0));
        y1 = float(lineY(r1) + lineWidth);
    }
};

/**
 * Emit the grid's line rectangles (canvas units) for drawing at `pixelsPerUnit` screen pixels per
 * canvas pixel. Every line edge is snapped to a whole screen pixel and lines are at least one
 * screen pixel wide, so a scaled-down grid keeps evenly spaced, equally visible lines (spacing
 * varies by at most one screen pixel) instead of thin lines vanishing between pixels. At 1.0 this
 * is the exact canvas grid. Horizontal lines are split at the vertical lines so translucent lines
 * do not overlap at the crossings. Calls rect(x0, y0, x1, y1). (Not named "emit": Qt defines that as a
 * macro.)
 */
template<class RectFn> void ForEachGridRect(const GridLayout &grid, float pixelsPerUnit, RectFn &&rect)
{
    if (!grid.valid() || pixelsPerUnit <= 0.0f)
        return;
    const float s = pixelsPerUnit;
    const float lw = std::max(1.0f, std::round(float(grid.lineWidth) * s)) / s;
    std::vector<float> xs(size_t(grid.cols()) + 1), ys(size_t(grid.rows()) + 1);
    for (int k = 0; k <= grid.cols(); ++k)
        xs[size_t(k)] = std::round(float(grid.lineX(k)) * s) / s;
    for (int k = 0; k <= grid.rows(); ++k)
        ys[size_t(k)] = std::round(float(grid.lineY(k)) * s) / s;

    const float top = ys.front(), bottom = ys.back() + lw;
    for (float x : xs)
        rect(x, top, x + lw, bottom);
    for (float y : ys) {
        for (size_t k = 0; k + 1 < xs.size(); ++k) {
            if (xs[k + 1] > xs[k] + lw)
                rect(xs[k] + lw, y, xs[k + 1], y + lw);
        }
    }
}
