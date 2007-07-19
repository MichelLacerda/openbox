/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   font.c for the Openbox window manager
   Copyright (c) 2006        Mikael Magnusson
   Copyright (c) 2003-2007   Dana Jansens
   Copyright (c) 2003        Derek Foreman

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "font.h"
#include "color.h"
#include "mask.h"
#include "theme.h"
#include "geom.h"
#include "instance.h"
#include "gettext.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <locale.h>

static void measure_font(const RrInstance *inst, RrFont *f)
{
    PangoFontMetrics *metrics;
    static PangoLanguage *lang = NULL;

    if (lang == NULL) {
#if PANGO_VERSION_MAJOR > 1 || \
    (PANGO_VERSION_MAJOR == 1 && PANGO_VERSION_MINOR >= 16)
        lang = pango_language_get_default();
#else
        gchar *locale, *p;
        /* get the default language from the locale
           (based on gtk_get_default_language in gtkmain.c) */
        locale = g_strdup(setlocale(LC_CTYPE, NULL));
        if ((p = strchr(locale, '.'))) *p = '\0'; /* strip off the . */
        if ((p = strchr(locale, '@'))) *p = '\0'; /* strip off the @ */
        lang = pango_language_from_string(locale);
        g_free(locale);
#endif
    }

    /* measure the ascent and descent */
    metrics = pango_context_get_metrics(inst->pango, f->font_desc, lang);
    f->ascent = pango_font_metrics_get_ascent(metrics);
    f->descent = pango_font_metrics_get_descent(metrics);
    pango_font_metrics_unref(metrics);

}

RrFont *RrFontOpen(const RrInstance *inst, const gchar *name, gint size,
                   RrFontWeight weight, RrFontSlant slant)
{
    RrFont *out;
    PangoWeight pweight;
    PangoStyle pstyle;

    out = g_new(RrFont, 1);
    out->inst = inst;
    out->ref = 1;
    out->font_desc = pango_font_description_new();
    out->layout = pango_layout_new(inst->pango);

    out->shortcut_underline = pango_attr_underline_new(PANGO_UNDERLINE_LOW);
    out->shortcut_underline->start_index = 0;
    out->shortcut_underline->end_index = 0;

    out->underline_attrlist = pango_attr_list_new();
    pango_attr_list_insert(out->underline_attrlist, out->shortcut_underline);

    switch (weight) {
    case RR_FONTWEIGHT_LIGHT:     pweight = PANGO_WEIGHT_LIGHT;     break;
    case RR_FONTWEIGHT_NORMAL:    pweight = PANGO_WEIGHT_NORMAL;    break;
    case RR_FONTWEIGHT_SEMIBOLD:  pweight = PANGO_WEIGHT_SEMIBOLD;  break;
    case RR_FONTWEIGHT_BOLD:      pweight = PANGO_WEIGHT_BOLD;      break;
    case RR_FONTWEIGHT_ULTRABOLD: pweight = PANGO_WEIGHT_ULTRABOLD; break;
    default: g_assert_not_reached();
    }

    switch (slant) {
    case RR_FONTSLANT_NORMAL:  pstyle = PANGO_STYLE_NORMAL;    break;
    case RR_FONTSLANT_ITALIC:  pstyle = PANGO_STYLE_ITALIC;    break;
    case RR_FONTSLANT_OBLIQUE: pstyle = PANGO_STYLE_OBLIQUE;   break;
    default: g_assert_not_reached();
    }

    /* setup the font */
    pango_font_description_set_family(out->font_desc, name);
    pango_font_description_set_weight(out->font_desc, pweight);
    pango_font_description_set_style(out->font_desc, pstyle);
    pango_font_description_set_size(out->font_desc, size * PANGO_SCALE);

    /* setup the layout */
    pango_layout_set_font_description(out->layout, out->font_desc);
    pango_layout_set_single_paragraph_mode(out->layout, TRUE);

    /* get the ascent and descent */
    measure_font(inst, out);

    return out;
}

RrFont *RrFontOpenDefault(const RrInstance *inst)
{
    return RrFontOpen(inst, RrDefaultFontFamily, RrDefaultFontSize,
                      RrDefaultFontWeight, RrDefaultFontSlant);
}

void RrFontRef(RrFont *f)
{
    ++f->ref;
}

void RrFontClose(RrFont *f)
{
    if (f) {
        if (--f->ref < 1) {
            pango_attr_list_unref(f->underline_attrlist);
            g_object_unref(f->layout);
            pango_font_description_free(f->font_desc);
            g_free(f);
        }
    }
}

static void font_measure_full(const RrFont *f, const gchar *str,
                              gint *x, gint *y, gint shadow_x, gint shadow_y)
{
    PangoRectangle rect;

    pango_layout_set_text(f->layout, str, -1);
    pango_layout_set_width(f->layout, -1);

    /* pango_layout_get_pixel_extents lies! this is the right way to get the
       size of the text's area */
    pango_layout_get_extents(f->layout, NULL, &rect);
#if PANGO_VERSION_MAJOR > 1 || \
    (PANGO_VERSION_MAJOR == 1 && PANGO_VERSION_MINOR >= 16)
    /* pass the logical rect as the ink rect, this is on purpose so we get the
       full area for the text */
    pango_extents_to_pixels(&rect, NULL);
#else
    rect.width = (rect.width + PANGO_SCALE - 1) / PANGO_SCALE;
    rect.height = (rect.height + PANGO_SCALE - 1) / PANGO_SCALE;
#endif
    *x = rect.width + ABS(shadow_x) + 4 /* we put a 2 px edge on each side */;
    *y = rect.height + ABS(shadow_y);
}

RrSize *RrFontMeasureString(const RrFont *f, const gchar *str,
                            gint shadow_x, gint shadow_y)
{
    RrSize *size;
    size = g_new(RrSize, 1);
    font_measure_full(f, str, &size->width, &size->height, shadow_x, shadow_y);
    return size;
}

gint RrFontHeight(const RrFont *f, gint shadow_y)
{
    return (f->ascent + f->descent) / PANGO_SCALE + ABS(shadow_y);
}

static inline int font_calculate_baseline(RrFont *f, gint height)
{
/* For my own reference:
 *   _________
 *  ^space/2  ^height     ^baseline
 *  v_________|_          |
 *            | ^ascent   |   _           _
 *            | |         |  | |_ _____ _| |_ _  _
 *            | |         |  |  _/ -_) \ /  _| || |
 *            | v_________v   \__\___/_\_\\__|\_, |
 *            | ^descent                      |__/
 *  __________|_v
 *  ^space/2  |
 *  V_________v
 */
    return (((height * PANGO_SCALE) /* height of the space in pango units */
             - (f->ascent + f->descent)) /* minus space taken up by text */
            / 2 /* divided by two -> half of the empty space (this is the top
                   of the text) */
            + f->ascent) /* now move down to the baseline */
        / PANGO_SCALE; /* back to pixels */
}

void RrFontDraw(XftDraw *d, RrTextureText *t, RrRect *area)
{
    gint x,y,w,h;
    XftColor c;
    gint mw;
    PangoRectangle rect;
    PangoEllipsizeMode ell;

    /* center the text vertically
       We do this centering based on the 'baseline' since different fonts have
       different top edges. It looks bad when the whole string is moved when 1
       character from a non-default language is included in the string */
    y = area->y +
        font_calculate_baseline(t->font, area->height);

    /* the +2 and -4 leave a small blank edge on the sides */
    x = area->x + 2;
    w = area->width - 4;
    h = area->height;

    switch (t->ellipsize) {
    case RR_ELLIPSIZE_NONE:
        ell = PANGO_ELLIPSIZE_NONE;
        break;
    case RR_ELLIPSIZE_START:
        ell = PANGO_ELLIPSIZE_START;
        break;
    case RR_ELLIPSIZE_MIDDLE:
        ell = PANGO_ELLIPSIZE_MIDDLE;
        break;
    case RR_ELLIPSIZE_END:
        ell = PANGO_ELLIPSIZE_END;
        break;
    }

    pango_layout_set_text(t->font->layout, t->string, -1);
    pango_layout_set_width(t->font->layout, w * PANGO_SCALE);
    pango_layout_set_ellipsize(t->font->layout, ell);

    /* * * end of setting up the layout * * */

    pango_layout_get_pixel_extents(t->font->layout, NULL, &rect);
    mw = rect.width;

    /* pango_layout_set_alignment doesn't work with
       pango_xft_render_layout_line */
    switch (t->justify) {
    case RR_JUSTIFY_LEFT:
        break;
    case RR_JUSTIFY_RIGHT:
        x += (w - mw);
        break;
    case RR_JUSTIFY_CENTER:
        x += (w - mw) / 2;
        break;
    }

    if (t->shadow_offset_x || t->shadow_offset_y) {
        c.color.red = t->shadow_color->r | t->shadow_color->r << 8;
        c.color.green = t->shadow_color->g | t->shadow_color->g << 8;
        c.color.blue = t->shadow_color->b | t->shadow_color->b << 8;
        c.color.alpha = 0xffff * t->shadow_alpha / 255;
        c.pixel = t->shadow_color->pixel;

        /* see below... */
        pango_xft_render_layout_line
            (d, &c, pango_layout_get_line(t->font->layout, 0),
             (x + t->shadow_offset_x) * PANGO_SCALE,
             (y + t->shadow_offset_y) * PANGO_SCALE);
    }

    c.color.red = t->color->r | t->color->r << 8;
    c.color.green = t->color->g | t->color->g << 8;
    c.color.blue = t->color->b | t->color->b << 8;
    c.color.alpha = 0xff | 0xff << 8; /* fully opaque text */
    c.pixel = t->color->pixel;

    if (t->shortcut) {
        const gchar *c = t->string + t->shortcut_pos;
        PangoAttribute *pa;
        PangoAttrList *al;

        if (t->shortcut_color) {
            pa = pango_attr_foreground_new
                (t->shortcut_color->r + (t->shortcut_color->r << 8),
                 t->shortcut_color->g + (t->shortcut_color->g << 8),
                 t->shortcut_color->b + (t->shortcut_color->b << 8));
            al = pango_attr_list_new();
            pango_attr_list_insert(al, pa);
        }
        else {
            pa = t->font->shortcut_underline;
            al = t->font->underline_attrlist;
            pango_attr_list_ref(al);
        }

        pa->start_index = t->shortcut_pos;
        pa->end_index = t->shortcut_pos + (g_utf8_next_char(c) - c);

        pango_layout_set_attributes(t->font->layout, al);
        pango_attr_list_unref(al);
    }

    /* layout_line() uses y to specify the baseline
       The line doesn't need to be freed, it's a part of the layout */
    pango_xft_render_layout_line
        (d, &c, pango_layout_get_line(t->font->layout, 0),
         x * PANGO_SCALE, y * PANGO_SCALE);

    if (t->shortcut)
        pango_layout_set_attributes(t->font->layout, NULL);
}
