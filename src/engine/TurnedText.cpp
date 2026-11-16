// MuPDF reports errors with setjmp and longjmp, which skip C++ destructors, and these functions run inside its
// fz_try blocks, so nothing here has a destructor.

#include "engine/TurnedText.h"

#include <algorithm>
#include <cmath>

namespace papertrail::engine {

namespace {

// The draw device's own ways of drawing text, kept from the first draw device given. Every draw device has the same.
struct TextCallbacks {
    decltype(fz_device::fill_text) fill = nullptr;
    decltype(fz_device::stroke_text) stroke = nullptr;
    decltype(fz_device::clip_text) clip = nullptr;
    decltype(fz_device::clip_stroke_text) clipStroke = nullptr;
};

TextCallbacks &drawDevicesOwn()
{
    static TextCallbacks callbacks;
    return callbacks;
}

// Whether a glyph drawn with `m` is neither upright nor turned a quarter, mirrored or not.
bool turned(fz_matrix m)
{
    const float largest = std::max({std::fabs(m.a), std::fabs(m.b), std::fabs(m.c), std::fabs(m.d)});
    const float tiny = largest * 1e-4f;
    const bool upright = std::fabs(m.b) <= tiny && std::fabs(m.c) <= tiny;
    const bool quarterTurned = std::fabs(m.a) <= tiny && std::fabs(m.d) <= tiny;
    return !upright && !quarterTurned;
}

bool drawnFromOutlines(const fz_text *text, fz_matrix ctm)
{
    bool anyTurned = false;
    for (const fz_text_span *span = text->head; span; span = span->next) {
        // A Type 3 font's glyphs aren't FreeType's, and vertical text is placed in ways outlines alone don't follow.
        if (!span->font || !span->font->ft_face || span->wmode)
            return false;
        anyTurned = anyTurned || turned(fz_concat(span->trm, ctm));
    }
    return anyTurned;
}

void moveTo(fz_context *ctx, void *path, float x, float y)
{
    fz_moveto(ctx, static_cast<fz_path *>(path), x, y);
}

void lineTo(fz_context *ctx, void *path, float x, float y)
{
    fz_lineto(ctx, static_cast<fz_path *>(path), x, y);
}

void curveTo(fz_context *ctx, void *path, float x1, float y1, float x2, float y2, float x3, float y3)
{
    fz_curveto(ctx, static_cast<fz_path *>(path), x1, y1, x2, y2, x3, y3);
}

void closePath(fz_context *ctx, void *path)
{
    fz_closepath(ctx, static_cast<fz_path *>(path));
}

const fz_path_walker kCopier = {moveTo, lineTo, curveTo, closePath, nullptr, nullptr, nullptr, nullptr};

// Every glyph's outline in one path, in the text's own space, as the draw device would place each glyph.
fz_path *outlineOf(fz_context *ctx, const fz_text *text)
{
    fz_path *path = fz_new_path(ctx);
    fz_path *glyph = nullptr;
    fz_var(glyph);
    fz_try(ctx) {
        for (const fz_text_span *span = text->head; span; span = span->next) {
            for (int i = 0; i < span->len; ++i) {
                const fz_text_item &item = span->items[i];
                if (item.gid < 0)
                    continue;
                fz_matrix placed = span->trm;
                placed.e = item.x;
                placed.f = item.y;
                glyph = fz_outline_glyph(ctx, span->font, item.gid, placed);
                if (glyph) {
                    fz_walk_path(ctx, glyph, &kCopier, path);
                    fz_drop_path(ctx, glyph);
                    glyph = nullptr;
                }
            }
        }
    }
    fz_catch(ctx) {
        fz_drop_path(ctx, glyph);
        fz_drop_path(ctx, path);
        fz_rethrow(ctx);
    }
    return path;
}

// Each of these hands the outline to the device's own path drawing, directly rather than through fz_fill_path and
// the rest, which would count a second clip for the one the caller already counted.

void fillText(fz_context *ctx, fz_device *device, const fz_text *text, fz_matrix ctm, fz_colorspace *colorspace,
              const float *color, float alpha, fz_color_params params)
{
    if (!drawnFromOutlines(text, ctm)) {
        drawDevicesOwn().fill(ctx, device, text, ctm, colorspace, color, alpha, params);
        return;
    }
    fz_path *path = outlineOf(ctx, text);
    fz_try(ctx) {
        device->fill_path(ctx, device, path, 0, ctm, colorspace, color, alpha, params);
    }
    fz_always(ctx) {
        fz_drop_path(ctx, path);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

void strokeText(fz_context *ctx, fz_device *device, const fz_text *text, const fz_stroke_state *stroke, fz_matrix ctm,
                fz_colorspace *colorspace, const float *color, float alpha, fz_color_params params)
{
    if (!drawnFromOutlines(text, ctm)) {
        drawDevicesOwn().stroke(ctx, device, text, stroke, ctm, colorspace, color, alpha, params);
        return;
    }
    fz_path *path = outlineOf(ctx, text);
    fz_try(ctx) {
        device->stroke_path(ctx, device, path, stroke, ctm, colorspace, color, alpha, params);
    }
    fz_always(ctx) {
        fz_drop_path(ctx, path);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

void clipText(fz_context *ctx, fz_device *device, const fz_text *text, fz_matrix ctm, fz_rect scissor)
{
    if (!drawnFromOutlines(text, ctm)) {
        drawDevicesOwn().clip(ctx, device, text, ctm, scissor);
        return;
    }
    fz_path *path = outlineOf(ctx, text);
    fz_try(ctx) {
        device->clip_path(ctx, device, path, 0, ctm, scissor);
    }
    fz_always(ctx) {
        fz_drop_path(ctx, path);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

void clipStrokeText(fz_context *ctx, fz_device *device, const fz_text *text, const fz_stroke_state *stroke,
                    fz_matrix ctm, fz_rect scissor)
{
    if (!drawnFromOutlines(text, ctm)) {
        drawDevicesOwn().clipStroke(ctx, device, text, stroke, ctm, scissor);
        return;
    }
    fz_path *path = outlineOf(ctx, text);
    fz_try(ctx) {
        device->clip_stroke_path(ctx, device, path, stroke, ctm, scissor);
    }
    fz_always(ctx) {
        fz_drop_path(ctx, path);
    }
    fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

} // namespace

void drawTurnedTextFromOutlines(fz_device *drawDevice)
{
    if (!drawDevice || drawDevice->fill_text == fillText)
        return;
    // Each callback stands in for one only if the device has both it and the path drawing it uses.
    if (!drawDevice->fill_text || !drawDevice->stroke_text || !drawDevice->clip_text || !drawDevice->clip_stroke_text
        || !drawDevice->fill_path || !drawDevice->stroke_path || !drawDevice->clip_path
        || !drawDevice->clip_stroke_path)
        return;
    TextCallbacks &own = drawDevicesOwn();
    if (!own.fill) {
        own.fill = drawDevice->fill_text;
        own.stroke = drawDevice->stroke_text;
        own.clip = drawDevice->clip_text;
        own.clipStroke = drawDevice->clip_stroke_text;
    }
    drawDevice->fill_text = fillText;
    drawDevice->stroke_text = strokeText;
    drawDevice->clip_text = clipText;
    drawDevice->clip_stroke_text = clipStrokeText;
}

} // namespace papertrail::engine
