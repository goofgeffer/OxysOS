/*
 * File: graphics/compositor.c
 * Purpose: Implements the compositor: a back buffer standing in for the
 *          framebuffer, an ordered list of layers composited over it, the
 *          accumulated region that has changed, and the presentation that
 *          carries that region to the display.
 * Key functions: CompositorInitialise, CompositorSurface, CompositorAddLayer,
 *          CompositorMoveLayer, CompositorInvalidate, CompositorPresent,
 *          CompositorReport.
 * References: kernel/include/oxys/compositor.h states what this implements, and
 *          docs/design/COMPOSITOR.md, Section 2, why.
 *
 * What this sub-task is for, in one paragraph.
 *
 *   Four earlier sub-tasks deferred something here, and all four were the same
 *   thing: the kernel drew directly upon the framebuffer, so anything that had
 *   to appear *over* something else had to remember what was beneath it. The
 *   pointer of sub-task 6.5 kept the pixels it covered and put them back before
 *   each move; the console of 6.4 scrolled by reading the framebuffer and
 *   writing it back one row higher; and KernelWriteString had to conceal a
 *   pointer it should never have known existed. A back buffer removes the cause
 *   rather than the symptoms: the display is composed in ordinary memory and
 *   carried to the adapter, so what is beneath a thing is simply still there.
 *
 * Why the layers are composited at presentation and not into the buffer.
 *
 *   A layer drawn into the back buffer would have to be undone before the next
 *   frame, which is the save-under again under a different name. Compositing
 *   during the copy costs nothing extra: the pixels of the changed region are
 *   being written to the framebuffer anyway, and a pixel a layer covers is
 *   simply composed differently on its way out. The back buffer is never
 *   disturbed, so what a layer covered needs no restoring — it was never
 *   overwritten.
 *
 * Where the reads went.
 *
 *   Nowhere. The colour beneath a layer comes from the back buffer, which is
 *   ordinary write-back memory; the framebuffer is written and never read. That
 *   is the whole of the gain over sub-task 6.4's arrangement, where a scroll
 *   read four megabytes back through a write-combining mapping in which reads
 *   are uncached — see docs/design/CONSOLE.md, Section 6.2.
 *
 * Concurrency. The back buffer, the damage rectangle and the layer table are
 * unsynchronised, and there is one flow of control that touches them. The
 * spinlock of sub-task 6.13 exists and has not been applied here; sub-task 6.14
 * is what makes them contended. What it must cover then is a presentation and
 * not a primitive: a processor drawing between the reading of the damage
 * rectangle and its clearing would have its work discarded, the region that
 * recorded it having been cleared by a presentation that never copied it. That
 * is a frame silently missing what was drawn into it, which is the failure this
 * note exists to name, and it is why the right place for the lock is the
 * surface's owner rather than the primitive — see docs/design/DRAWING.md,
 * limitation 5.
 */

#include <oxys/compositor.h>
#include <oxys/framebuffer.h>
#include <oxys/graphics.h>
#include <oxys/kernel.h>
#include <oxys/memory.h>
#include <oxys/vmm.h>

/* A layer: a surface, where it sits, its coverage, and whether it is shown. */
typedef struct CompositorLayer
{
    const GraphicsSurface *surface;
    const uint8_t *mask;
    int32_t x;
    int32_t y;
    bool visible;
    bool occupied;
} CompositorLayer;

static GraphicsSurface CompositorBack;
static GraphicsSurface CompositorFront;
static bool CompositorActive;
static bool CompositorSuspended;
static void *CompositorPages;
static size_t CompositorPageCount;

static CompositorLayer CompositorLayers[COMPOSITOR_LAYER_CAPACITY];
static size_t CompositorLayersUsed;

static GraphicsRectangle CompositorDamaged;
static uint64_t CompositorPresents;
static uint64_t CompositorPixels;

/* ------------------------------------------------------------------ damage */

/*
 * The smallest rectangle enclosing both.
 *
 * The damage is accumulated as a bounding rectangle and not as a list of them.
 * Two cells at opposite corners therefore present the whole screen, which is
 * this scheme's cost; what it buys is that no amount of drawing can exhaust it.
 * A list would need a bound, and a compositor that had run out of entries would
 * have to present everything — which is what this does at its worst, without
 * the bookkeeping.
 */
static GraphicsRectangle CompositorUnion(GraphicsRectangle first, GraphicsRectangle second)
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
    GraphicsRectangle result;

    if (GraphicsRectangleIsEmpty(first))
    {
        return second;
    }

    if (GraphicsRectangleIsEmpty(second))
    {
        return first;
    }

    left = (first.x < second.x) ? first.x : second.x;
    top = (first.y < second.y) ? first.y : second.y;
    right = ((first.x + (int32_t)first.width) > (second.x + (int32_t)second.width))
                ? (first.x + (int32_t)first.width)
                : (second.x + (int32_t)second.width);
    bottom = ((first.y + (int32_t)first.height) > (second.y + (int32_t)second.height))
                 ? (first.y + (int32_t)first.height)
                 : (second.y + (int32_t)second.height);

    result.x = left;
    result.y = top;
    result.width = (uint32_t)(right - left);
    result.height = (uint32_t)(bottom - top);

    return result;
}

/* Where a layer stands, in the back buffer's coordinates. */
static GraphicsRectangle CompositorLayerBounds(const CompositorLayer *layer)
{
    GraphicsRectangle bounds = { 0, 0, 0U, 0U };

    if ((layer == NULL) || (layer->surface == NULL))
    {
        return bounds;
    }

    bounds.x = layer->x;
    bounds.y = layer->y;
    bounds.width = layer->surface->width;
    bounds.height = layer->surface->height;

    return bounds;
}

void CompositorInvalidate(GraphicsRectangle region)
{
    if (!CompositorActive)
    {
        return;
    }

    region = GraphicsRectangleIntersect(region, GraphicsSurfaceBounds(&CompositorBack));

    if (GraphicsRectangleIsEmpty(region))
    {
        return;
    }

    CompositorDamaged = CompositorUnion(CompositorDamaged, region);
}

void CompositorInvalidateAll(void)
{
    if (CompositorActive)
    {
        CompositorDamaged = GraphicsSurfaceBounds(&CompositorBack);
    }
}

GraphicsRectangle CompositorDamage(void)
{
    return CompositorDamaged;
}

/* ------------------------------------------------------------------ layers */

size_t CompositorAddLayer(const GraphicsSurface *surface, const uint8_t *mask, int32_t x,
                          int32_t y)
{
    if (surface == NULL)
    {
        return COMPOSITOR_LAYER_NONE;
    }

    for (size_t index = 0U; index < COMPOSITOR_LAYER_CAPACITY; ++index)
    {
        if (CompositorLayers[index].occupied)
        {
            continue;
        }

        CompositorLayers[index].surface = surface;
        CompositorLayers[index].mask = mask;
        CompositorLayers[index].x = x;
        CompositorLayers[index].y = y;
        CompositorLayers[index].visible = false;
        CompositorLayers[index].occupied = true;
        ++CompositorLayersUsed;

        return index;
    }

    return COMPOSITOR_LAYER_NONE;
}

void CompositorRemoveLayer(size_t layer)
{
    if ((layer >= COMPOSITOR_LAYER_CAPACITY) || !CompositorLayers[layer].occupied)
    {
        return;
    }

    /*
     * What it covered must be carried out again. A layer withdrawn without that
     * would leave its last appearance standing upon the display, which is the
     * same fault as a layer moved without marking where it was.
     */
    if (CompositorLayers[layer].visible)
    {
        CompositorInvalidate(CompositorLayerBounds(&CompositorLayers[layer]));
    }

    CompositorLayers[layer].occupied = false;
    CompositorLayers[layer].visible = false;
    CompositorLayers[layer].surface = NULL;
    CompositorLayers[layer].mask = NULL;
    --CompositorLayersUsed;
}

void CompositorMoveLayer(size_t layer, int32_t x, int32_t y)
{
    CompositorLayer *entry;

    if ((layer >= COMPOSITOR_LAYER_CAPACITY) || !CompositorLayers[layer].occupied)
    {
        return;
    }

    entry = &CompositorLayers[layer];

    if ((entry->x == x) && (entry->y == y))
    {
        return;
    }

    /*
     * Where it was, and where it now is.
     *
     * This is the whole of what replaces the save-under. A layer that marked
     * only its new position would leave its old one standing upon the display
     * until something else happened to change those pixels — which upon a
     * pointer moved across a static screen is a trail, and is exactly the fault
     * the save-under was written to prevent.
     */
    if (entry->visible)
    {
        CompositorInvalidate(CompositorLayerBounds(entry));
    }

    entry->x = x;
    entry->y = y;

    if (entry->visible)
    {
        CompositorInvalidate(CompositorLayerBounds(entry));
    }
}

void CompositorSetLayerVisible(size_t layer, bool visible)
{
    if ((layer >= COMPOSITOR_LAYER_CAPACITY) || !CompositorLayers[layer].occupied ||
        (CompositorLayers[layer].visible == visible))
    {
        return;
    }

    CompositorLayers[layer].visible = visible;
    CompositorInvalidate(CompositorLayerBounds(&CompositorLayers[layer]));
}

bool CompositorLayerIsVisible(size_t layer)
{
    return (layer < COMPOSITOR_LAYER_CAPACITY) && CompositorLayers[layer].occupied &&
           CompositorLayers[layer].visible;
}

size_t CompositorLayerCount(void)
{
    return CompositorLayersUsed;
}

/* ------------------------------------------------------------------- setup */

bool CompositorInitialise(void)
{
    uint32_t width;
    uint32_t height;
    uint8_t bytes;
    uint64_t bytes_needed;

    CompositorActive = false;
    CompositorSuspended = false;
    CompositorLayersUsed = 0U;

    for (size_t index = 0U; index < COMPOSITOR_LAYER_CAPACITY; ++index)
    {
        CompositorLayers[index].occupied = false;
        CompositorLayers[index].visible = false;
        CompositorLayers[index].surface = NULL;
        CompositorLayers[index].mask = NULL;
    }

    CompositorPresents = 0U;
    CompositorPixels = 0U;
    CompositorDamaged.x = 0;
    CompositorDamaged.y = 0;
    CompositorDamaged.width = 0U;
    CompositorDamaged.height = 0U;

    if (!GraphicsSurfaceFromFramebuffer(&CompositorFront))
    {
        return false;
    }

    width = CompositorFront.width;
    height = CompositorFront.height;
    bytes = CompositorFront.bytes_per_pixel;

    /*
     * The back buffer is packed tightly: its pitch is the width times the pixel
     * size, with none of the padding the adapter asked for. That padding is a
     * property of the hardware's row addressing and not of the image, and a
     * buffer that reproduced it would carry bytes nothing ever reads.
     */
    bytes_needed = (uint64_t)width * height * bytes;
    CompositorPageCount = (size_t)(AlignUp(bytes_needed, PAGE_SIZE) / PAGE_SIZE);
    CompositorPages = KernelPagesAllocate(CompositorPageCount);

    if (CompositorPages == NULL)
    {
        return false;
    }

    if (!GraphicsSurfaceInitialise(&CompositorBack, CompositorPages, width, height,
                                   width * bytes, bytes))
    {
        KernelPagesFree(CompositorPages, CompositorPageCount);
        CompositorPages = NULL;
        return false;
    }

    CompositorActive = true;

    /*
     * The buffer arrives holding whatever the arena last had in it, and the
     * display holds the boot log. Clearing it here would blank the screen at the
     * first presentation; the console replays its own record into it instead,
     * which is what puts the log back. What must be true is only that the whole
     * of it is presented once, so that no stale page of the arena is left
     * standing upon the display.
     */
    GraphicsClear(&CompositorBack, 0U);
    CompositorInvalidateAll();

    return true;
}

bool CompositorIsActive(void)
{
    return CompositorActive;
}

GraphicsSurface *CompositorSurface(void)
{
    return CompositorActive ? &CompositorBack : NULL;
}

/* -------------------------------------------------------------- presenting */

/*
 * Copies one row's span of the back buffer to the framebuffer.
 *
 * The word path is the same one the blit of sub-task 6.3 uses and for the same
 * reason: a four-byte pixel written as four bytes costs four stores and the loop
 * that generates them. Both surfaces must permit it, the source being read at
 * the same alignment the destination is written at.
 */
static void CompositorCopySpan(uint32_t row, int32_t left, uint32_t count)
{
    const volatile uint8_t *source =
        CompositorBack.pixels + ((uint64_t)row * CompositorBack.pitch) +
        ((uint64_t)(uint32_t)left * CompositorBack.bytes_per_pixel);
    volatile uint8_t *destination =
        CompositorFront.pixels + ((uint64_t)row * CompositorFront.pitch) +
        ((uint64_t)(uint32_t)left * CompositorFront.bytes_per_pixel);
    const uint64_t bytes = (uint64_t)count * CompositorBack.bytes_per_pixel;

    if (CompositorBack.whole_words && CompositorFront.whole_words &&
        (CompositorBack.bytes_per_pixel == 4U))
    {
        const volatile uint32_t *from = (const volatile uint32_t *)(const void *)source;
        volatile uint32_t *to = (volatile uint32_t *)(void *)destination;

        for (uint32_t index = 0U; index < count; ++index)
        {
            to[index] = from[index];
        }

        return;
    }

    for (uint64_t index = 0U; index < bytes; ++index)
    {
        destination[index] = source[index];
    }
}

/*
 * Composites one layer into the framebuffer, over what the back buffer holds.
 *
 * The colour beneath comes from the back buffer and not from the framebuffer.
 * That is the point of the whole arrangement: the framebuffer is written and
 * never read, so a pointer moved across the screen costs its own area in writes
 * and nothing in uncached reads.
 */
static void CompositorComposeLayer(const CompositorLayer *layer, GraphicsRectangle region)
{
    const GraphicsRectangle bounds = CompositorLayerBounds(layer);
    const GraphicsRectangle overlap = GraphicsRectangleIntersect(bounds, region);

    if (GraphicsRectangleIsEmpty(overlap))
    {
        return;
    }

    for (int32_t y = overlap.y; y < (overlap.y + (int32_t)overlap.height); ++y)
    {
        for (int32_t x = overlap.x; x < (overlap.x + (int32_t)overlap.width); ++x)
        {
            const int32_t column = x - layer->x;
            const int32_t row = y - layer->y;
            const uint8_t coverage =
                (layer->mask == NULL)
                    ? 255U
                    : layer->mask[((uint32_t)row * layer->surface->width) + (uint32_t)column];
            uint32_t colour;

            if (coverage == 0U)
            {
                continue;
            }

            colour = GraphicsPixelAt(layer->surface, column, row);

            if (coverage != 255U)
            {
                uint8_t source_red;
                uint8_t source_green;
                uint8_t source_blue;
                uint8_t under_red;
                uint8_t under_green;
                uint8_t under_blue;
                const uint32_t beneath = GraphicsPixelAt(&CompositorBack, x, y);

                FramebufferDecode(colour, &source_red, &source_green, &source_blue);
                FramebufferDecode(beneath, &under_red, &under_green, &under_blue);

                colour = FramebufferEncode(
                    (uint8_t)((((uint32_t)source_red * coverage) +
                               ((uint32_t)under_red * (255U - coverage)) + 127U) / 255U),
                    (uint8_t)((((uint32_t)source_green * coverage) +
                               ((uint32_t)under_green * (255U - coverage)) + 127U) / 255U),
                    (uint8_t)((((uint32_t)source_blue * coverage) +
                               ((uint32_t)under_blue * (255U - coverage)) + 127U) / 255U));
            }

            GraphicsPutPixel(&CompositorFront, x, y, colour);
        }
    }
}

void CompositorSuspend(void)
{
    CompositorSuspended = true;
}

void CompositorPresent(void)
{
    GraphicsRectangle region;

    if (!CompositorActive || CompositorSuspended)
    {
        return;
    }

    region = GraphicsRectangleIntersect(CompositorDamaged,
                                        GraphicsSurfaceBounds(&CompositorBack));

    if (GraphicsRectangleIsEmpty(region))
    {
        /* Nothing changed. A presentation that writes nothing is not an error;
         * it is what the echo loop does between keystrokes. */
        CompositorDamaged.width = 0U;
        CompositorDamaged.height = 0U;
        return;
    }

    for (int32_t y = region.y; y < (region.y + (int32_t)region.height); ++y)
    {
        CompositorCopySpan((uint32_t)y, region.x, region.width);
    }

    for (size_t index = 0U; index < COMPOSITOR_LAYER_CAPACITY; ++index)
    {
        if (CompositorLayers[index].occupied && CompositorLayers[index].visible)
        {
            CompositorComposeLayer(&CompositorLayers[index], region);
        }
    }

    ++CompositorPresents;
    CompositorPixels += (uint64_t)region.width * region.height;

    CompositorDamaged.x = 0;
    CompositorDamaged.y = 0;
    CompositorDamaged.width = 0U;
    CompositorDamaged.height = 0U;
}

uint64_t CompositorPresentCount(void)
{
    return CompositorPresents;
}

uint64_t CompositorPixelsPresented(void)
{
    return CompositorPixels;
}

void CompositorReport(void)
{
    if (!CompositorActive)
    {
        KernelWriteString("Compositor: inactive; drawing reaches the display directly.\n");
        return;
    }

    KernelWriteString("Compositor: back buffer ");
    KernelWriteDecimal((uint64_t)CompositorBack.width);
    KernelWriteString(" by ");
    KernelWriteDecimal((uint64_t)CompositorBack.height);
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)CompositorPageCount * (PAGE_SIZE / 1024U));
    KernelWriteString(" KiB, ");
    KernelWriteDecimal((uint64_t)CompositorLayersUsed);
    KernelWriteString(" layer(s).\n");

    KernelWriteString("Compositor: presentations ");
    KernelWriteDecimal(CompositorPresents);
    KernelWriteString(", pixels carried ");
    KernelWriteDecimal(CompositorPixels);
    KernelWriteString(", framebuffer reads 0.\n");
}
