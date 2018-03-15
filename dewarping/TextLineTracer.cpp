/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C)  Joseph Artsimovich <joseph.artsimovich@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "TextLineTracer.h"
#include "TextLineRefiner.h"
#include "DetectVertContentBounds.h"
#include "TowardsLineTracer.h"
#include "GridLineTraverser.h"
#include "TaskStatus.h"
#include "DebugImages.h"
#include "NumericTraits.h"
#include "ToLineProjector.h"
#include "LineBoundedByRect.h"
#include "DistortionModelBuilder.h"
#include "DistortionModel.h"
#include "imageproc/BinaryImage.h"
#include "imageproc/Binarize.h"
#include "imageproc/Grayscale.h"
#include "imageproc/Scale.h"
#include "imageproc/Constants.h"
#include "imageproc/GaussBlur.h"
#include "imageproc/Sobel.h"
#include "imageproc/Morphology.h"
#include "imageproc/RasterOp.h"
#include "imageproc/RasterOpGeneric.h"
#include "imageproc/SeedFill.h"
#include "imageproc/LocalMinMaxGeneric.h"
#include "imageproc/SEDM.h"
#include <QPainter>
#include <boost/foreach.hpp>
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/lambda/if.hpp>
#include <cmath>

using namespace imageproc;

namespace dewarping {
void TextLineTracer::trace(const GrayImage& input,
                           const Dpi& dpi,
                           const QRect& content_rect,
                           DistortionModelBuilder& output,
                           const TaskStatus& status,
                           DebugImages* dbg) {
    using namespace boost::lambda;

    GrayImage downscaled(downscale(input, dpi));
    if (dbg) {
        dbg->add(downscaled, "downscaled");
    }

    const int downscaled_width = downscaled.width();
    const int downscaled_height = downscaled.height();

    const double downscale_x_factor = double(downscaled_width) / input.width();
    const double downscale_y_factor = double(downscaled_height) / input.height();
    QTransform to_orig;
    to_orig.scale(1.0 / downscale_x_factor, 1.0 / downscale_y_factor);

    const QRect downscaled_content_rect(to_orig.inverted().mapRect(content_rect));
    const Dpi downscaled_dpi(qRound(dpi.horizontal() * downscale_x_factor),
                             qRound(dpi.vertical() * downscale_y_factor));

    BinaryImage binarized(binarizeWolf(downscaled, QSize(31, 31)));
    if (dbg) {
        dbg->add(binarized, "binarized");
    }
    // detectVertContentBounds() is sensitive to clutter and speckles, so let's try to remove it.
    sanitizeBinaryImage(binarized, downscaled_content_rect);
    if (dbg) {
        dbg->add(binarized, "sanitized");
    }

    std::pair<QLineF, QLineF> vert_bounds(detectVertContentBounds(binarized, dbg));
    if (dbg) {
        dbg->add(visualizeVerticalBounds(binarized.toQImage(), vert_bounds), "vert_bounds");
    }

    std::list<std::vector<QPointF>> polylines;
    extractTextLines(polylines, stretchGrayRange(downscaled), vert_bounds, dbg);
    if (dbg) {
        dbg->add(visualizePolylines(downscaled, polylines), "traced");
    }

    filterShortCurves(polylines, vert_bounds.first, vert_bounds.second);
    filterOutOfBoundsCurves(polylines, vert_bounds.first, vert_bounds.second);
    if (dbg) {
        dbg->add(visualizePolylines(downscaled, polylines), "filtered1");
    }

    Vec2f unit_down_vector(calcAvgUnitVector(vert_bounds));
    unit_down_vector /= sqrt(unit_down_vector.squaredNorm());
    if (unit_down_vector[1] < 0) {
        unit_down_vector = -unit_down_vector;
    }
    TextLineRefiner refiner(downscaled, Dpi(200, 200), unit_down_vector);
    refiner.refine(polylines, /*iterations=*/100, dbg);

    filterEdgyCurves(polylines);
    if (dbg) {
        dbg->add(visualizePolylines(downscaled, polylines), "filtered2");
    }


    // Transform back to original coordinates and output.

    vert_bounds.first = to_orig.map(vert_bounds.first);
    vert_bounds.second = to_orig.map(vert_bounds.second);
    output.setVerticalBounds(vert_bounds.first, vert_bounds.second);

    for (std::vector<QPointF>& polyline : polylines) {
        for (QPointF& pt : polyline) {
            pt = to_orig.map(pt);
        }
        output.addHorizontalCurve(polyline);
    }
}  // TextLineTracer::trace

GrayImage TextLineTracer::downscale(const GrayImage& input, const Dpi& dpi) {
    // Downscale to 200 DPI.
    QSize downscaled_size(input.size());
    if ((dpi.horizontal() < 180) || (dpi.horizontal() > 220) || (dpi.vertical() < 180) || (dpi.vertical() > 220)) {
        downscaled_size.setWidth(std::max<int>(1, input.width() * 200 / dpi.horizontal()));
        downscaled_size.setHeight(std::max<int>(1, input.height() * 200 / dpi.vertical()));
    }

    return scaleToGray(input, downscaled_size);
}

void TextLineTracer::sanitizeBinaryImage(BinaryImage& image, const QRect& content_rect) {
    // Kill connected components touching the borders.
    BinaryImage seed(image.size(), WHITE);
    seed.fillExcept(seed.rect().adjusted(1, 1, -1, -1), BLACK);

    BinaryImage touching_border(seedFill(seed.release(), image, CONN8));
    rasterOp<RopSubtract<RopDst, RopSrc>>(image, touching_border.release());

    // Poor man's despeckle.
    BinaryImage content_seeds(openBrick(image, QSize(2, 3), WHITE));
    rasterOp<RopOr<RopSrc, RopDst>>(content_seeds, openBrick(image, QSize(3, 2), WHITE));
    image = seedFill(content_seeds.release(), image, CONN8);
    // Clear margins.
    image.fillExcept(content_rect, WHITE);
}

/**
 * Returns false if the curve contains both significant convexities and concavities.
 */
bool TextLineTracer::isCurvatureConsistent(const std::vector<QPointF>& polyline) {
    const size_t num_nodes = polyline.size();

    if (num_nodes <= 1) {
        // Even though we can't say anything about curvature in this case,
        // we don't like such gegenerate curves, so we reject them.
        return false;
    } else if (num_nodes == 2) {
        // These are fine.
        return true;
    }
    // Threshold angle between a polyline segment and a normal to the previous one.
    const auto cos_threshold = static_cast<const float>(cos((90.0f - 6.0f) * constants::DEG2RAD));
    const float cos_sq_threshold = cos_threshold * cos_threshold;
    bool significant_positive = false;
    bool significant_negative = false;

    Vec2f prev_normal(polyline[1] - polyline[0]);
    std::swap(prev_normal[0], prev_normal[1]);
    prev_normal[0] = -prev_normal[0];
    float prev_normal_sqlen = prev_normal.squaredNorm();

    for (size_t i = 1; i < num_nodes - 1; ++i) {
        const Vec2f next_segment(polyline[i + 1] - polyline[i]);
        const float next_segment_sqlen = next_segment.squaredNorm();

        float cos_sq = 0;
        const float sqlen_mult = prev_normal_sqlen * next_segment_sqlen;
        if (sqlen_mult > std::numeric_limits<float>::epsilon()) {
            const float dot = prev_normal.dot(next_segment);
            cos_sq = std::fabs(dot) * dot / sqlen_mult;
        }

        if (std::fabs(cos_sq) >= cos_sq_threshold) {
            if (cos_sq > 0) {
                significant_positive = true;
            } else {
                significant_negative = true;
            }
        }

        prev_normal[0] = -next_segment[1];
        prev_normal[1] = next_segment[0];
        prev_normal_sqlen = next_segment_sqlen;
    }

    return !(significant_positive && significant_negative);
}  // TextLineTracer::isCurvatureConsistent

bool TextLineTracer::isInsideBounds(const QPointF& pt, const QLineF& left_bound, const QLineF& right_bound) {
    QPointF left_normal_inside(left_bound.normalVector().p2() - left_bound.p1());
    if (left_normal_inside.x() < 0) {
        left_normal_inside = -left_normal_inside;
    }
    const QPointF left_vec(pt - left_bound.p1());
    if (left_normal_inside.x() * left_vec.x() + left_normal_inside.y() * left_vec.y() < 0) {
        return false;
    }

    QPointF right_normal_inside(right_bound.normalVector().p2() - right_bound.p1());
    if (right_normal_inside.x() > 0) {
        right_normal_inside = -right_normal_inside;
    }
    const QPointF right_vec(pt - right_bound.p1());
    if (right_normal_inside.x() * right_vec.x() + right_normal_inside.y() * right_vec.y() < 0) {
        return false;
    }

    return true;
}

void TextLineTracer::filterShortCurves(std::list<std::vector<QPointF>>& polylines,
                                       const QLineF& left_bound,
                                       const QLineF& right_bound) {
    const ToLineProjector proj1(left_bound);
    const ToLineProjector proj2(right_bound);

    auto it(polylines.begin());
    const auto end(polylines.end());
    while (it != end) {
        assert(!it->empty());
        const QPointF front(it->front());
        const QPointF back(it->back());
        const double front_proj_len = proj1.projectionDist(front);
        const double back_proj_len = proj2.projectionDist(back);
        const double chord_len = QLineF(front, back).length();

        if (front_proj_len + back_proj_len > 0.3 * chord_len) {
            polylines.erase(it++);
        } else {
            ++it;
        }
    }
}

void TextLineTracer::filterOutOfBoundsCurves(std::list<std::vector<QPointF>>& polylines,
                                             const QLineF& left_bound,
                                             const QLineF& right_bound) {
    auto it(polylines.begin());
    const auto end(polylines.end());
    while (it != end) {
        if (!isInsideBounds(it->front(), left_bound, right_bound)
            && !isInsideBounds(it->back(), left_bound, right_bound)) {
            polylines.erase(it++);
        } else {
            ++it;
        }
    }
}

void TextLineTracer::filterEdgyCurves(std::list<std::vector<QPointF>>& polylines) {
    auto it(polylines.begin());
    const auto end(polylines.end());
    while (it != end) {
        if (!isCurvatureConsistent(*it)) {
            polylines.erase(it++);
        } else {
            ++it;
        }
    }
}

void TextLineTracer::extractTextLines(std::list<std::vector<QPointF>>& out,
                                      const imageproc::GrayImage& image,
                                      const std::pair<QLineF, QLineF>& bounds,
                                      DebugImages* dbg) {
    using namespace boost::lambda;

    const int width = image.width();
    const int height = image.height();
    const QSize size(image.size());
    const Vec2f direction(calcAvgUnitVector(bounds));
    Grid<float> main_grid(image.width(), image.height(), 0);
    Grid<float> aux_grid(image.width(), image.height(), 0);

    const float downscale = 1.0f / (255.0f * 8.0f);
    horizontalSobel<float>(width, height, image.data(), image.stride(), _1 * downscale, aux_grid.data(),
                           aux_grid.stride(), _1 = _2, _1, main_grid.data(), main_grid.stride(), _1 = _2);
    verticalSobel<float>(width, height, image.data(), image.stride(), _1 * downscale, aux_grid.data(),
                         aux_grid.stride(), _1 = _2, _1, main_grid.data(), main_grid.stride(),
                         _1 = _1 * direction[0] + _2 * direction[1]);
    if (dbg) {
        dbg->add(visualizeGradient(image, main_grid), "first_dir_deriv");
    }

    gaussBlurGeneric(size, 6.0f, 6.0f, main_grid.data(), main_grid.stride(), _1, main_grid.data(), main_grid.stride(),
                     _1 = _2);
    if (dbg) {
        dbg->add(visualizeGradient(image, main_grid), "first_dir_deriv_blurred");
    }

    horizontalSobel<float>(width, height, main_grid.data(), main_grid.stride(), _1, aux_grid.data(), aux_grid.stride(),
                           _1 = _2, _1, aux_grid.data(), aux_grid.stride(), _1 = _2);
    verticalSobel<float>(width, height, main_grid.data(), main_grid.stride(), _1, main_grid.data(), main_grid.stride(),
                         _1 = _2, _1, main_grid.data(), main_grid.stride(), _1 = _2);
    rasterOpGeneric(aux_grid.data(), aux_grid.stride(), size, main_grid.data(), main_grid.stride(),
                    _2 = _1 * direction[0] + _2 * direction[1]);
    if (dbg) {
        dbg->add(visualizeGradient(image, main_grid), "second_dir_deriv");
    }

    float max = 0;
    rasterOpGeneric(main_grid.data(), main_grid.stride(), size, if_then(_1 > var(max), var(max) = _1));
    const float threshold = max * 15.0f / 255.0f;

    BinaryImage initial_binarization(image.size());
    rasterOpGeneric(initial_binarization, main_grid.data(), main_grid.stride(),
                    if_then_else(_2 > threshold, _1 = uint32_t(1), _1 = uint32_t(0)));
    if (dbg) {
        dbg->add(initial_binarization, "initial_binarization");
    }

    rasterOpGeneric(main_grid.data(), main_grid.stride(), size, aux_grid.data(), aux_grid.stride(),
                    _2 = bind((float (*)(float)) & std::fabs, _1));
    if (dbg) {
        dbg->add(visualizeGradient(image, aux_grid), "abs");
    }

    gaussBlurGeneric(size, 12.0f, 12.0f, aux_grid.data(), aux_grid.stride(), _1, aux_grid.data(), aux_grid.stride(),
                     _1 = _2);
    if (dbg) {
        dbg->add(visualizeGradient(image, aux_grid), "blurred");
    }

    rasterOpGeneric(main_grid.data(), main_grid.stride(), size, aux_grid.data(), aux_grid.stride(),
                    _2 += _1 - bind((float (*)(float)) & std::fabs, _1));
    if (dbg) {
        dbg->add(visualizeGradient(image, aux_grid), "+= diff");
    }

    BinaryImage post_binarization(image.size());
    rasterOpGeneric(post_binarization, aux_grid.data(), aux_grid.stride(),
                    if_then_else(_2 > threshold, _1 = uint32_t(1), _1 = uint32_t(0)));
    if (dbg) {
        dbg->add(post_binarization, "post_binarization");
    }

    BinaryImage obstacles(image.size());
    rasterOpGeneric(obstacles, aux_grid.data(), aux_grid.stride(),
                    if_then_else(_2 < -threshold, _1 = uint32_t(1), _1 = uint32_t(0)));
    if (dbg) {
        dbg->add(obstacles, "obstacles");
    }

    Grid<float>().swap(aux_grid);  // Save memory.
    initial_binarization = closeWithObstacles(initial_binarization, obstacles, QSize(21, 21));
    if (dbg) {
        dbg->add(initial_binarization, "initial_closed");
    }

    obstacles.release();  // Save memory.
    rasterOp<RopAnd<RopDst, RopSrc>>(post_binarization, initial_binarization);
    if (dbg) {
        dbg->add(post_binarization, "post &&= initial");
    }

    initial_binarization.release();  // Save memory.
    const SEDM sedm(post_binarization);

    std::vector<QPoint> seeds;
    QLineF mid_line(calcMidLine(bounds.first, bounds.second));
    findMidLineSeeds(sedm, mid_line, seeds);
    if (dbg) {
        dbg->add(visualizeMidLineSeeds(image, post_binarization, bounds, mid_line, seeds), "seeds");
    }

    post_binarization.release();  // Save memory.
    for (const QPoint seed : seeds) {
        std::vector<QPointF> polyline;

        {
            TowardsLineTracer tracer(&sedm, &main_grid, bounds.first, seed);
            while (const QPoint* pt = tracer.trace(10.0f)) {
                polyline.emplace_back(*pt);
            }
            std::reverse(polyline.begin(), polyline.end());
        }

        polyline.emplace_back(seed);

        {
            TowardsLineTracer tracer(&sedm, &main_grid, bounds.second, seed);
            while (const QPoint* pt = tracer.trace(10.0f)) {
                polyline.emplace_back(*pt);
            }
        }

        out.emplace_back();
        out.back().swap(polyline);
    }
}  // TextLineTracer::extractTextLines

Vec2f TextLineTracer::calcAvgUnitVector(const std::pair<QLineF, QLineF>& bounds) {
    Vec2f v1(bounds.first.p2() - bounds.first.p1());
    v1 /= sqrt(v1.squaredNorm());

    Vec2f v2(bounds.second.p2() - bounds.second.p1());
    v2 /= sqrt(v2.squaredNorm());

    Vec2f v3(v1 + v2);
    v3 /= sqrt(v3.squaredNorm());

    return v3;
}

BinaryImage TextLineTracer::closeWithObstacles(const BinaryImage& image,
                                               const BinaryImage& obstacles,
                                               const QSize& brick) {
    BinaryImage mask(closeBrick(image, brick));
    rasterOp<RopSubtract<RopDst, RopSrc>>(mask, obstacles);

    return seedFill(image, mask, CONN4);
}

void TextLineTracer::findMidLineSeeds(const SEDM& sedm, QLineF mid_line, std::vector<QPoint>& seeds) {
    lineBoundedByRect(mid_line, QRect(QPoint(0, 0), sedm.size()).adjusted(0, 0, -1, -1));

    const uint32_t* sedm_data = sedm.data();
    const int sedm_stride = sedm.stride();

    QPoint prev_pt;
    int32_t prev_level = 0;
    int dir = 1;  // Distance growing.
    GridLineTraverser traverser(mid_line);
    while (traverser.hasNext()) {
        const QPoint pt(traverser.next());
        const int32_t level = sedm_data[pt.y() * sedm_stride + pt.x()];
        if ((level - prev_level) * dir < 0) {
            // Direction changed.
            if (dir > 0) {
                seeds.push_back(prev_pt);
            }
            dir *= -1;
        }

        prev_pt = pt;
        prev_level = level;
    }
}

QLineF TextLineTracer::calcMidLine(const QLineF& line1, const QLineF& line2) {
    QPointF intersection;
    if (line1.intersect(line2, &intersection) == QLineF::NoIntersection) {
        // Lines are parallel.
        const QPointF p1(line2.p1());
        const QPointF p2(ToLineProjector(line1).projectionPoint(p1));
        const QPointF origin(0.5 * (p1 + p2));
        const QPointF vector(line2.p2() - line2.p1());

        return QLineF(origin, origin + vector);
    } else {
        // Lines do intersect.
        Vec2d v1(line1.p2() - line1.p1());
        Vec2d v2(line2.p2() - line2.p1());
        v1 /= sqrt(v1.squaredNorm());
        v2 /= sqrt(v2.squaredNorm());

        return QLineF(intersection, intersection + 0.5 * (v1 + v2));
    }
}

QImage TextLineTracer::visualizeVerticalBounds(const QImage& background, const std::pair<QLineF, QLineF>& bounds) {
    QImage canvas(background.convertToFormat(QImage::Format_RGB32));

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(Qt::blue);
    pen.setWidthF(2.0);
    painter.setPen(pen);
    painter.setOpacity(0.7);

    painter.drawLine(bounds.first);
    painter.drawLine(bounds.second);

    return canvas;
}

QImage TextLineTracer::visualizeGradient(const QImage& background, const Grid<float>& grad) {
    const int width = grad.width();
    const int height = grad.height();
    const int grad_stride = grad.stride();
    // First let's find the maximum and minimum values.
    float min_value = NumericTraits<float>::max();
    float max_value = NumericTraits<float>::min();

    const float* grad_line = grad.data();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float value = grad_line[x];
            if (value < min_value) {
                min_value = value;
            } else if (value > max_value) {
                max_value = value;
            }
        }
        grad_line += grad_stride;
    }

    float scale = std::max(max_value, -min_value);
    if (scale > std::numeric_limits<float>::epsilon()) {
        scale = 255.0f / scale;
    }

    QImage overlay(width, height, QImage::Format_ARGB32_Premultiplied);
    auto* overlay_line = (uint32_t*) overlay.bits();
    const int overlay_stride = overlay.bytesPerLine() / 4;

    grad_line = grad.data();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float value = grad_line[x] * scale;
            const int magnitude = qBound(0, static_cast<const int&>(std::round(std::fabs(value))), 255);
            if (value < 0) {
                overlay_line[x] = qRgba(0, 0, magnitude, magnitude);
            } else {
                overlay_line[x] = qRgba(magnitude, 0, 0, magnitude);
            }
        }
        grad_line += grad_stride;
        overlay_line += overlay_stride;
    }

    QImage canvas(background.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    QPainter painter(&canvas);
    painter.drawImage(0, 0, overlay);

    return canvas;
}  // TextLineTracer::visualizeGradient

QImage TextLineTracer::visualizeMidLineSeeds(const QImage& background,
                                             const BinaryImage& overlay,
                                             std::pair<QLineF, QLineF> bounds,
                                             QLineF mid_line,
                                             const std::vector<QPoint>& seeds) {
    QImage canvas(background.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);

    painter.drawImage(QPoint(0, 0), overlay.toAlphaMask(QColor(0xff, 0x00, 0x00, 120)));

    lineBoundedByRect(bounds.first, background.rect());
    lineBoundedByRect(bounds.second, background.rect());
    lineBoundedByRect(mid_line, background.rect());

    QPen pen(QColor(0x00, 0x00, 0xff, 180));
    pen.setWidthF(5.0);
    painter.setPen(pen);
    painter.drawLine(bounds.first);
    painter.drawLine(bounds.second);

    pen.setColor(QColor(0x00, 0xff, 0x00, 180));
    painter.setPen(pen);
    painter.drawLine(mid_line);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x2d, 0x00, 0x6d, 255));
    QRectF rect(0, 0, 7, 7);
    for (const QPoint pt : seeds) {
        rect.moveCenter(pt + QPointF(0.5, 0.5));
        painter.drawEllipse(rect);
    }

    return canvas;
}  // TextLineTracer::visualizeMidLineSeeds

QImage TextLineTracer::visualizePolylines(const QImage& background,
                                          const std::list<std::vector<QPointF>>& polylines,
                                          const std::pair<QLineF, QLineF>* vert_bounds) {
    QImage canvas(background.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(Qt::blue);
    pen.setWidthF(3.0);
    painter.setPen(pen);

    for (const std::vector<QPointF>& polyline : polylines) {
        if (!polyline.empty()) {
            painter.drawPolyline(&polyline[0], static_cast<int>(polyline.size()));
        }
    }

    if (vert_bounds) {
        painter.drawLine(vert_bounds->first);
        painter.drawLine(vert_bounds->second);
    }

    return canvas;
}
}  // namespace dewarping