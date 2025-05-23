/* Copyright (C) 2001-2024 Artifex Software, Inc.
   All Rights Reserved.

   This software is provided AS-IS with no warranty, either express or
   implied.

   This software is distributed under license and may not be copied,
   modified or distributed except as expressly authorized under the terms
   of the license contained in the file LICENSE in this distribution.

   Refer to licensing information at http://www.artifex.com or contact
   Artifex Software, Inc.,  39 Mesa Street, Suite 108A, San Francisco,
   CA 94129, USA, for further information.
*/


/* Default implementation of text writing */
#include "gx.h"
#include "memory_.h"
#include "string_.h"
#include "gserrors.h"
#include "gsstruct.h"
#include "gxfixed.h"            /* ditto */
#include "gxarith.h"
#include "gxmatrix.h"
#include "gzstate.h"
#include "gxcoord.h"
#include "gxdevice.h"
#include "gxdevmem.h"
#include "gxchar.h"
#include "gxfont.h"
#include "gxfont0.h"
#include "gxfcache.h"
#include "gspath.h"
#include "gzpath.h"
#include "gxfcid.h"

/* Define whether the show operation uses the character outline data, */
/* as opposed to just needing the width (or nothing). */
#define SHOW_USES_OUTLINE(penum)\
  !SHOW_IS(penum, TEXT_DO_NONE | TEXT_DO_CHARWIDTH)

/* Structure descriptors */
public_st_gs_show_enum();
extern_st(st_gs_text_enum);
static
ENUM_PTRS_BEGIN(show_enum_enum_ptrs)
     return ENUM_USING(st_gs_text_enum, vptr, size, index - 5);
ENUM_PTR(0, gs_show_enum, pgs);
ENUM_PTR(1, gs_show_enum, show_gstate);
ENUM_PTR3(2, gs_show_enum, dev_cache, dev_cache2, dev_null);
ENUM_PTRS_END
static RELOC_PTRS_WITH(show_enum_reloc_ptrs, gs_show_enum *eptr)
{
    RELOC_USING(st_gs_text_enum, vptr, size);           /* superclass */
    RELOC_VAR(eptr->pgs);
    RELOC_VAR(eptr->show_gstate);
    RELOC_PTR3(gs_show_enum, dev_cache, dev_cache2, dev_null);
}
RELOC_PTRS_END

/* Forward declarations */
static int continue_kshow(gs_show_enum *);
static int continue_show(gs_show_enum *);
static int continue_show_update(gs_show_enum *);
static void show_set_scale(const gs_show_enum *, gs_log2_scale_point *log2_scale);
static int show_cache_setup(gs_show_enum *);
static int show_state_setup(gs_show_enum *);
static int show_origin_setup(gs_gstate *, fixed, fixed, gs_show_enum * penum);

/* Accessors for current_char and current_glyph. */
#define CURRENT_CHAR(penum) ((penum)->returned.current_char)
#define SET_CURRENT_CHAR(penum, chr)\
  ((penum)->returned.current_char = (chr))
#define CURRENT_GLYPH(penum) ((penum)->returned.current_glyph)
#define SET_CURRENT_GLYPH(penum, glyph)\
  ((penum)->returned.current_glyph = (glyph))

/* Allocate a show enumerator. */
gs_show_enum *
gs_show_enum_alloc(gs_memory_t * mem, gs_gstate * pgs, client_name_t cname)
{
    gs_show_enum *penum;

    rc_alloc_struct_1(penum, gs_show_enum, &st_gs_show_enum, mem,
                      return 0, cname);
    penum->rc.free = rc_free_text_enum;
    penum->auto_release = true; /* old API */
    /* Initialize pointers for GC */
    penum->text.operation = 0;  /* no pointers relevant */
    penum->dev = 0;
    penum->pgs = pgs;
    penum->show_gstate = 0;
    penum->dev_cache = 0;
    penum->dev_cache2 = 0;
    penum->fapi_log2_scale.x = penum->fapi_log2_scale.y = -1;
    penum->fapi_glyph_shift.x = penum->fapi_glyph_shift.y = 0;
    penum->dev_null = 0;
    penum->fstack.depth = -1;
    return penum;
}

/* ------ Driver procedure ------ */

static text_enum_proc_resync(gx_show_text_resync);
static text_enum_proc_process(gx_show_text_process);
static text_enum_proc_is_width_only(gx_show_text_is_width_only);
static text_enum_proc_current_width(gx_show_text_current_width);
static text_enum_proc_set_cache(gx_show_text_set_cache);
static text_enum_proc_retry(gx_show_text_retry);
static text_enum_proc_release(gx_show_text_release); /* not default */

static const gs_text_enum_procs_t default_text_procs = {
    gx_show_text_resync, gx_show_text_process,
    gx_show_text_is_width_only, gx_show_text_current_width,
    gx_show_text_set_cache, gx_show_text_retry,
    gx_show_text_release
};

int
gx_default_text_begin(gx_device * dev, gs_gstate * pgs1,
                      const gs_text_params_t * text, gs_font * font,
                      const gx_clip_path * pcpath,
                      gs_text_enum_t ** ppte)
{
    uint operation = text->operation;
    bool propagate_charpath = (operation & TEXT_DO_DRAW) != 0;
    int code;
    gs_gstate *pgs = (gs_gstate *)pgs1;
    gs_show_enum *penum;
    gs_memory_t * mem = pgs->memory;

    penum = gs_show_enum_alloc(mem, pgs, "gx_default_text_begin");
    if (!penum)
        return_error(gs_error_VMerror);
    code = gs_text_enum_init((gs_text_enum_t *)penum, &default_text_procs,
                             dev, pgs, text, font, pcpath, mem);
    if (code < 0) {
        gs_free_object(mem, penum, "gx_default_text_begin");
        return code;
    }
    penum->auto_release = false; /* new API */
    penum->level = pgs->level;
    penum->cc = 0;
    penum->continue_proc = continue_show;
    switch (penum->charpath_flag) {
    case cpm_false_charpath: case cpm_true_charpath:
        penum->can_cache = -1; break;
    case cpm_false_charboxpath: case cpm_true_charboxpath:
        penum->can_cache = 0; break;
    case cpm_charwidth:
    default:                    /* cpm_show */
        penum->can_cache = 1; break;
    }
    code = show_state_setup(penum);
    if (code < 0) {
        gs_text_release(pgs, (gs_text_enum_t *)penum, "gx_default_text_begin");
        penum = NULL;
        return code;
    }
    penum->show_gstate =
        (propagate_charpath && (pgs->in_charpath != 0) ?
         pgs->show_gstate : pgs);
    if (!(~operation & (TEXT_DO_NONE | TEXT_RETURN_WIDTH))) {
        /* This is stringwidth (or a PDF with text in rendering mode 3) . */
        gx_device_null *dev_null =
            gs_alloc_struct(mem, gx_device_null, &st_device_null,
                            "stringwidth(dev_null)");

        if (dev_null == 0) {
            gs_text_release(pgs, (gs_text_enum_t *)penum, "gx_default_text_begin");
            penum = NULL;
            return_error(gs_error_VMerror);
        }

        /* Set up a null device that forwards xfont requests properly. */
        /* We have to set the device up here, so the contents are
           initialised, and safe to free in the event of an error.
         */
        gs_make_null_device(dev_null, gs_currentdevice_inline(pgs), mem);

        /* Do an extra gsave and suppress output */
        if ((code = gs_gsave(pgs)) < 0) {
            gs_text_release(pgs, (gs_text_enum_t *)penum, "gx_default_text_begin");
            penum = NULL;
            gs_free_object(mem, dev_null, "gx_default_text_begin");
            return code;
        }
        penum->level = pgs->level;      /* for level check in show_update */
        pgs->ctm_default_set = false;
        penum->dev_null = dev_null;
        /* Retain this device, since it is referenced from the enumerator. */
        gx_device_retain((gx_device *)dev_null, true);
        gs_setdevice_no_init(pgs, (gx_device *) dev_null);
        /* Establish an arbitrary translation and current point. */
        gs_newpath(pgs);
        gx_translate_to_fixed(pgs, fixed_0, fixed_0);
        code = gx_path_add_point(pgs->path, fixed_0, fixed_0);
        if (code < 0) {
            gs_text_release(pgs, (gs_text_enum_t *)penum, "gx_default_text_begin");
            penum = NULL;
            gs_grestore(pgs);
            return code;
        }
    }
    *ppte = (gs_text_enum_t *)penum;
    return 0;
}

/* Compute the number of characters in a text. */
int
gs_text_count_chars(gs_gstate * pgs, gs_text_params_t *text, gs_memory_t * mem)
{
    font_proc_next_char_glyph((*next_proc)) = pgs->font->procs.next_char_glyph;

    if (next_proc == gs_default_next_char_glyph)
        return text->size;
    else {
        /* Do it the hard way. */
        gs_text_enum_t tenum;   /* use a separate enumerator */
        gs_char tchr;
        gs_glyph tglyph;
        int size = 0;
        int code;

        size = 0;

        code = gs_text_enum_init(&tenum, &default_text_procs,
                             NULL, NULL, text, pgs->root_font,
                             NULL, mem);
        if (code < 0)
            return code;
        while ((code = (*next_proc)(&tenum, &tchr, &tglyph)) != 2) {
            if (code < 0)
                break;
            ++size;
        }
        if (code < 0)
            return code;
        return size;
    }
}

/* An auxiliary functions for pdfwrite to process type 3 fonts. */
int
gx_hld_stringwidth_begin(gs_gstate * pgs, gx_path **path)
{
    int code;

    code = gs_gsave(pgs);
    if (code < 0)
        return code;
    gs_newpath(pgs);
    *path = pgs->path;
    gx_translate_to_fixed(pgs, fixed_0, fixed_0);
    return gx_path_add_point(pgs->path, fixed_0, fixed_0);
}

int
gx_default_text_restore_state(gs_text_enum_t *pte)
{
    gs_show_enum *penum;
    gs_gstate *pgs;

    if (SHOW_IS(pte, TEXT_DO_NONE))
        return 0;
    penum = (gs_show_enum *)pte;
    pgs = penum->pgs;
    return gs_grestore(pgs);
}
/* ------ Width/cache setting ------ */

static int
    set_cache_device(gs_show_enum *penum, gs_gstate *pgs,
                     double llx, double lly, double urx, double ury);

/* This is the default implementation of text enumerator set_cache. */
static int
gx_show_text_set_cache(gs_text_enum_t *pte, const double *pw,
                          gs_text_cache_control_t control)
{
    gs_show_enum *const penum = (gs_show_enum *)pte;
    gs_gstate *pgs = penum->pgs;
    gs_font *pfont = gs_rootfont(pgs);

    /* Detect zero FontMatrix now for Adobe compatibility with CET tests.
       Note that matrixe\\ces like [1 0 0 0 0 0] are used in comparefiles
       to compute a text width. See also gs_text_begin. */
    if (pfont->FontMatrix.xx == 0 && pfont->FontMatrix.xy == 0 &&
        pfont->FontMatrix.yx == 0 && pfont->FontMatrix.yy == 0)
        return_error(gs_error_undefinedresult); /* sic! : CPSI compatibility */
    switch (control) {
    case TEXT_SET_CHAR_WIDTH:
        return set_char_width(penum, pgs, pw[0], pw[1]);
    case TEXT_SET_CACHE_DEVICE: {
        int code = set_char_width(penum, pgs, pw[0], pw[1]);    /* default is don't cache */

        if (code < 0)
            return code;
        if (SHOW_IS_ALL_OF(penum, TEXT_DO_NONE | TEXT_INTERVENE)) /* cshow */
            return code;
        return set_cache_device(penum, pgs, pw[2], pw[3], pw[4], pw[5]);
    }
    case TEXT_SET_CACHE_DEVICE2: {
        int code;
        bool retry = (penum->width_status == sws_retry);

        if (pfont->WMode) {
            float vx = pw[8], vy = pw[9];
            gs_fixed_point pvxy, dvxy;

            gs_fixed_point rewind_pvxy;
            int rewind_code;

            if ((code = gs_point_transform2fixed(&pgs->ctm, -vx, -vy, &pvxy)) < 0 ||
                (code = gs_distance_transform2fixed(&pgs->ctm, vx, vy, &dvxy)) < 0
                )
                return 0;               /* don't cache */
            if ((code = set_char_width(penum, pgs, pw[6], pw[7])) < 0)
                return code;
            if (SHOW_IS_ALL_OF(penum, TEXT_DO_NONE | TEXT_INTERVENE))
                return code;
            /* Adjust the origin by (vx, vy). */
            gx_translate_to_fixed(pgs, pvxy.x, pvxy.y);
            code = set_cache_device(penum, pgs, pw[2], pw[3], pw[4], pw[5]);
            if (code != 1) {
                if (retry) {
                   rewind_code = gs_point_transform2fixed(&pgs->ctm, vx, vy, &rewind_pvxy);
                   if (rewind_code < 0) {
                       /* If the control passes here, something is wrong. */
                       return_error(gs_error_unregistered);
                   }
                   /* Rewind the origin by (-vx, -vy) if the cache is failed. */
                   gx_translate_to_fixed(pgs, rewind_pvxy.x, rewind_pvxy.y);
                }
                return code;
            }
            /* Adjust the character origin too. */
            (penum->cc)->offset.x += dvxy.x;
            (penum->cc)->offset.y += dvxy.y;
        } else {
            code = set_char_width(penum, pgs, pw[0], pw[1]);
            if (code < 0)
                return code;
            if (SHOW_IS_ALL_OF(penum, TEXT_DO_NONE | TEXT_INTERVENE))
                return code;
            code = set_cache_device(penum, pgs, pw[2], pw[3], pw[4], pw[5]);
        }
        return code;
    }
    default:
        return_error(gs_error_rangecheck);
    }
}

/* Set the character width. */
/* Note that this returns 1 if the current show operation is */
/* non-displaying (stringwidth or cshow). */
int
set_char_width(gs_show_enum *penum, gs_gstate *pgs, double wx, double wy)
{
    int code;

    if (penum->width_status != sws_none && penum->width_status != sws_retry)
        return_error(gs_error_undefined);
    code = gs_distance_transform2fixed(&pgs->ctm, wx, wy, &penum->wxy);
    if (code < 0 && penum->cc == 0) {
        /* Can't represent in 'fixed', use floats. */
        code = gs_distance_transform(wx, wy, &ctm_only(pgs), &penum->wxy_float);
        penum->wxy.x = penum->wxy.y = 0;
        penum->use_wxy_float = true;
    } else {
        penum->use_wxy_float = false;
        penum->wxy_float.x = penum->wxy_float.y = 0;
    }
    if (code < 0)
        return code;
    /* Check whether we're setting the scalable width */
    /* for a cached xfont character. */
    if (penum->cc != 0) {
        penum->cc->wxy = penum->wxy;
        penum->width_status = sws_cache_width_only;
    } else {
        penum->width_status = sws_no_cache;
    }
    if (SHOW_IS_ALL_OF(penum, TEXT_DO_NONE | TEXT_INTERVENE)) /* cshow */
        gs_nulldevice(pgs);
    return !SHOW_IS_DRAWING(penum);
}

void
gx_compute_text_oversampling(const gs_show_enum * penum, const gs_font *pfont,
                             int alpha_bits, gs_log2_scale_point *p_log2_scale)
{
    gs_log2_scale_point log2_scale;

    if (alpha_bits == 1)
        log2_scale.x = log2_scale.y = 0;
    else if (pfont->PaintType != 0) {
        /* Don't oversample artificially stroked fonts. */
        log2_scale.x = log2_scale.y = 0;
    } else if (!penum->is_pure_color) {
        /* Don't oversample characters for rendering in non-pure color. */
        log2_scale.x = log2_scale.y = 0;
    } else {
        int excess;

        /* Get maximal scale according to cached bitmap size. */
        show_set_scale(penum, &log2_scale);
        /* Reduce the scale to fit into alpha bits. */
        excess = log2_scale.x + log2_scale.y - alpha_bits;
        while (excess > 0) {
            if (log2_scale.y > 0) {
                log2_scale.y --;
                excess--;
                if (excess == 0)
                    break;
            }
            if (log2_scale.x > 0) {
                log2_scale.x --;
                excess--;
            }
        }
    }
    *p_log2_scale = log2_scale;
}

/* Compute glyph raster parameters */
static int
compute_glyph_raster_params(gs_show_enum *penum, bool in_setcachedevice, int *alpha_bits,
                    int *depth,
                    gs_fixed_point *subpix_origin, gs_log2_scale_point *log2_scale)
{
    gs_gstate *pgs = penum->pgs;
    gx_device *dev = gs_currentdevice_inline(pgs);
    int code;

    *alpha_bits = (*dev_proc(dev, get_alpha_bits)) (dev, go_text);
    if (in_setcachedevice) {
        /* current point should already be in penum->origin */
    } else {
        code = gx_path_current_point_inline(pgs, &penum->origin);
        if (code < 0) {
            /* For cshow, having no current point is acceptable. */
            if (!SHOW_IS(penum, TEXT_DO_NONE))
                return code;
            penum->origin.x = penum->origin.y = 0;      /* arbitrary */
        }
    }
    if (penum->fapi_log2_scale.x != -1)
        *log2_scale = penum->fapi_log2_scale;
    else
        gx_compute_text_oversampling(penum, penum->current_font, *alpha_bits, log2_scale);
    /*  We never oversample over the device alpha_bits,
     * so that we don't need to scale down. Perhaps it may happen
     * that we underuse alpha_bits due to a big character raster,
     * so we must compute log2_depth more accurately :
     */
    *depth = (log2_scale->x + log2_scale->y == 0 ?
        1 : min(log2_scale->x + log2_scale->y, *alpha_bits));
    if (gs_currentaligntopixels(penum->current_font->dir) == 0) {
        int scx = -(1L << (_fixed_shift - log2_scale->x));
        int rdx =  1L << (_fixed_shift - 1 - log2_scale->x);

#       if 1 /* Ever align Y to pixels to provide an uniform glyph height. */
            subpix_origin->y = 0;
#       else
            int scy = -(1L << (_fixed_shift - log2_scale->y));
            int rdy =  1L << (_fixed_shift - 1 - log2_scale->y);

            subpix_origin->y = ((penum->origin.y + rdy) & scy) & (fixed_1 - 1);
#       endif
        subpix_origin->x = ((penum->origin.x + rdx) & scx) & (fixed_1 - 1);
    } else
        subpix_origin->x = subpix_origin->y = 0;
    return 0;
}

/* Set up the cache device if relevant. */
/* Return 1 if we just set up a cache device. */
/* Used by setcachedevice and setcachedevice2. */
static int
set_cache_device(gs_show_enum * penum, gs_gstate * pgs, double llx, double lly,
                 double urx, double ury)
{
    gs_glyph glyph;
    int code = 0;

    /* See if we want to cache this character. */
    if (pgs->in_cachedevice)    /* no recursion! */
        return 0;
    if (SHOW_IS_ALL_OF(penum, TEXT_DO_NONE | TEXT_INTERVENE)) { /* cshow */
        if_debug0m('k', penum->memory, "[k]no cache: cshow");
        code = gs_nulldevice(pgs);
        if (code < 0)
            return code;
        return 0;
    }
    pgs->in_cachedevice = CACHE_DEVICE_NOT_CACHING;     /* disable color/gray/image operators */
    /* We can only use the cache if we know the glyph. */
    glyph = CURRENT_GLYPH(penum);
    if (glyph == GS_NO_GLYPH)
        return 0;
    /* We can only use the cache if ctm is unchanged */
    /* (aside from a possible translation). */
    if (penum->can_cache <= 0 || !pgs->char_tm_valid) {
        if_debug2m('k', penum->memory, "[k]no cache: can_cache=%d, char_tm_valid=%d\n",
                   penum->can_cache, (int)pgs->char_tm_valid);
        return 0;
    } {
        const gs_font *pfont = pgs->font;
        gs_font_dir *dir = pfont->dir;
        int alpha_bits, depth;
        gs_log2_scale_point log2_scale;
        gs_fixed_point subpix_origin;
        static const fixed max_cdim[3] =
        {
#define max_cd(n)\
            (fixed_1 << (ARCH_SIZEOF_SHORT * 8 - n)) - (fixed_1 >> n) * 3
            max_cd(0), max_cd(1), max_cd(2)
#undef max_cd
        };
        ushort iwidth, iheight;
        cached_char *cc;
        gs_fixed_rect clip_box;
        gs_fixed_point cll, clr, cul, cur, cdim;

        /* Reject setcachedevice arguments that are too big and, probably, invalid */
        /* The threshold is arbitrary. A font from bug 692832 has a 1237340,       */
        /* normal fonts should have about 1000. */
        if (fabs(llx) > 32000. || fabs(lly) > 32000. || fabs(urx) > 32000. || fabs(ury) >= 32000.)
            return 0;           /* don't cache */

        /* Compute the bounding box of the transformed character. */
        /* Since we accept arbitrary transformations, the extrema */
        /* may occur in any order; however, we can save some work */
        /* by observing that opposite corners before transforming */
        /* are still opposite afterwards. */

        if ((code = gs_distance_transform2fixed(&pgs->ctm, llx, lly, &cll)) < 0 ||
            (code = gs_distance_transform2fixed(&pgs->ctm, llx, ury, &clr)) < 0 ||
            (code = gs_distance_transform2fixed(&pgs->ctm, urx, lly, &cul)) < 0 ||
         (code = gs_distance_transform2fixed(&pgs->ctm, urx, ury, &cur)) < 0
            )
            return 0;           /* don't cache */
        {
            fixed ctemp;

#define swap(a, b) ctemp = a, a = b, b = ctemp
#define make_min(a, b) if ( (a) > (b) ) swap(a, b)

            make_min(cll.x, cur.x);
            make_min(cll.y, cur.y);
            make_min(clr.x, cul.x);
            make_min(clr.y, cul.y);
#undef make_min
#undef swap
        }
        /* Now take advantage of symmetry. */
        if (clr.x < cll.x)
            cll.x = clr.x, cur.x = cul.x;
        if (clr.y < cll.y)
            cll.y = clr.y, cur.y = cul.y;
        /* Now cll and cur are the extrema of the box. */
        code = compute_glyph_raster_params(penum, true, &alpha_bits, &depth,
           &subpix_origin, &log2_scale);
        if (code < 0)
            return code;
#ifdef DEBUG
        if (gs_debug_c('k')) {
            dmlprintf6(pgs->memory, "[k]cbox=[%g %g %g %g] scale=%dx%d\n",
                       fixed2float(cll.x), fixed2float(cll.y),
                       fixed2float(cur.x), fixed2float(cur.y),
                       1 << log2_scale.x, 1 << log2_scale.y);
            dmlprintf6(pgs->memory, "[p]  ctm=[%g %g %g %g %g %g]\n",
                       pgs->ctm.xx, pgs->ctm.xy, pgs->ctm.yx, pgs->ctm.yy,
                       pgs->ctm.tx, pgs->ctm.ty);
        }
#endif
        cdim.x = cur.x - cll.x;
        cdim.y = cur.y - cll.y;
        if (cdim.x > max_cdim[log2_scale.x] ||
            cdim.y > max_cdim[log2_scale.y]
            )
            return 0;           /* much too big */
        iwidth = ((ushort) fixed2int_var(cdim.x) + 3) << log2_scale.x;
        iheight = ((ushort) fixed2int_var(cdim.y) + 3) << log2_scale.y;
        if_debug3m('k', penum->memory, "[k]iwidth=%u iheight=%u dev_cache %s\n",
                   (uint) iwidth, (uint) iheight,
                   (penum->dev_cache == 0 ? "not set" : "set"));
        if (penum->dev_cache == 0) {
            code = show_cache_setup(penum);
            if (code < 0)
                return code;
        }
        code = gx_alloc_char_bits(dir, penum->dev_cache,
                                  iwidth, iheight, &log2_scale, depth, &cc);
        if (code < 0)
            return code;

        if (cc == 0) {
            /* too big for cache or no cache */
            gx_path box_path;

            if (penum->current_font->FontType != ft_user_defined &&
                penum->current_font->FontType != ft_PDF_user_defined &&
                penum->current_font->FontType != ft_PCL_user_defined &&
                penum->current_font->FontType != ft_GL2_stick_user_defined &&
                penum->current_font->FontType != ft_CID_user_defined) {
                /* Most fonts don't paint outside bbox,
                   so render with no clipping. */
                return 0;
            }
            /* Render with a clip. */
            /* show_proceed already did gsave. */
            pgs->in_cachedevice = CACHE_DEVICE_NONE; /* Provide a correct grestore on error. */
            clip_box.p.x = penum->origin.x - fixed_ceiling(-cll.x);
            clip_box.p.y = penum->origin.y - fixed_ceiling(-cll.y);
            clip_box.q.x = clip_box.p.x + int2fixed(iwidth);
            clip_box.q.y = clip_box.p.y + int2fixed(iheight);
            gx_path_init_local(&box_path, pgs->memory);
            code = gx_path_add_rectangle(&box_path, clip_box.p.x, clip_box.p.y,
                                                    clip_box.q.x, clip_box.q.y);
            if (code < 0)
                return code;
            code = gx_cpath_clip(pgs, pgs->clip_path, &box_path, gx_rule_winding_number);
            if (code < 0)
                return code;
            gx_path_free(&box_path, "set_cache_device");
            pgs->in_cachedevice = CACHE_DEVICE_NONE_AND_CLIP;
            return 0;
        }
        /* The mins handle transposed coordinate systems.... */
        /* Truncate the offsets to avoid artifacts later. */
        cc->offset.x = fixed_ceiling(-cll.x) + fixed_1;
        cc->offset.y = fixed_ceiling(-cll.y) + fixed_1;
        if_debug4m('k', penum->memory, "[k]width=%u, height=%u, offset=[%g %g]\n",
                   (uint) iwidth, (uint) iheight,
                   fixed2float(cc->offset.x),
                   fixed2float(cc->offset.y));
        pgs->in_cachedevice = CACHE_DEVICE_NONE; /* Provide correct grestore */
        if ((code = gs_gsave(pgs)) < 0) {
            gx_free_cached_char(dir, cc);
            return code;
        }
        /* Nothing can go wrong now.... */
        penum->cc = cc;
        cc->code = glyph;
        cc->wmode = gs_rootfont(pgs)->WMode;
        cc->wxy = penum->wxy;
        cc->subpix_origin = subpix_origin;
        if (penum->pair != 0)
            cc_set_pair(cc, penum->pair);
        else
            cc->pair = 0;
        /* Install the device */
        gx_set_device_only(pgs, (gx_device *) penum->dev_cache);
        pgs->ctm_default_set = false;
        /* Adjust the transformation in the graphics context */
        /* so that the character lines up with the cache. */
        gx_translate_to_fixed(pgs,
                              (cc->offset.x + subpix_origin.x) << log2_scale.x,
                              (cc->offset.y + subpix_origin.y) << log2_scale.y);
        if ((log2_scale.x | log2_scale.y) != 0)
            gx_scale_char_matrix(pgs, 1 << log2_scale.x,
                                 1 << log2_scale.y);
        /* Set the initial matrix for the cache device. */
        penum->dev_cache->initial_matrix = ctm_only(pgs);
        /* Set the oversampling factor. */
        penum->log2_scale.x = log2_scale.x;
        penum->log2_scale.y = log2_scale.y;
        /* Reset the clipping path to match the metrics. */
        clip_box.p.x = clip_box.p.y = 0;
        clip_box.q.x = int2fixed(iwidth);
        clip_box.q.y = int2fixed(iheight);
        if ((code = gx_clip_to_rectangle(pgs, &clip_box)) < 0)
            goto fail;
        code = gx_set_device_color_1(pgs);     /* write 1's */
        if (code < 0)
            goto fail;
        gs_swapcolors_quick(pgs);
        code = gx_set_device_color_1(pgs);     /* write 1's */
        if (code < 0)
            goto fail;
        gs_swapcolors_quick(pgs);
        pgs->in_cachedevice = CACHE_DEVICE_CACHING;
    }
    penum->width_status = sws_cache;
    return 1;

fail:
    gs_grestore(pgs);
    return code;
}

/* Return the cache device status. */
gs_in_cache_device_t
gs_incachedevice(const gs_gstate *pgs)
{
    return pgs->in_cachedevice;
}

/* ------ Enumerator ------ */

/*
 * Set the encode_char procedure in an enumerator.
 */
static void
show_set_encode_char(gs_show_enum * penum)
{
    penum->encode_char =
        (SHOW_IS(penum, TEXT_FROM_GLYPHS | TEXT_FROM_SINGLE_GLYPH) ?
         gs_no_encode_char :
         gs_show_current_font(penum)->procs.encode_char);
}

/*
 * Resync a text operation with a different set of parameters.
 * Currently this is implemented only for changing the data source.
 */
static int
gx_show_text_resync(gs_text_enum_t *pte, const gs_text_enum_t *pfrom)
{
    gs_show_enum *const penum = (gs_show_enum *)pte;
    int old_index = pte->index;

    if ((pte->text.operation ^ pfrom->text.operation) & ~TEXT_FROM_ANY)
        return_error(gs_error_rangecheck);
    pte->text = pfrom->text;
    if (pte->index == old_index) {
        show_set_encode_char(penum);
        return 0;
    } else
        return show_state_setup(penum);
}

/* Do the next step of a show (or stringwidth) operation */
static int
gx_show_text_process(gs_text_enum_t *pte)
{
    gs_show_enum *const penum = (gs_show_enum *)pte;

    return (*penum->continue_proc)(penum);
}

/* Continuation procedures */
static int show_update(gs_show_enum * penum);
static int show_move(gs_show_enum * penum);
static int show_proceed(gs_show_enum * penum);
static int show_finish(gs_show_enum * penum);
static int
continue_show_update(gs_show_enum * penum)
{
    int code = show_update(penum);

    if (code < 0)
        return code;
    code = show_move(penum);
    if (code != 0)
        return code;
    return show_proceed(penum);
}
static int
continue_show(gs_show_enum * penum)
{
    return show_proceed(penum);
}
/* For kshow, the CTM or font may have changed, so we have to reestablish */
/* the cached values in the enumerator. */
static int
continue_kshow(gs_show_enum * penum)
{   int code;
    gs_gstate *pgs = penum->pgs;

    if (pgs->font != penum->orig_font)
        gs_setfont(pgs, penum->orig_font);

    code = show_state_setup(penum);

    if (code < 0)
        return code;
    return show_proceed(penum);
}

/* Update position */
static int
show_update(gs_show_enum * penum)
{
    gs_gstate *pgs = penum->pgs;
    cached_char *cc = penum->cc;
    int code;

    /* Update position for last character */
    switch (penum->width_status) {
        case sws_none:
        case sws_retry:
            /* Adobe interpreters assume a character width of 0, */
            /* even though the documentation says this is an error.... */
            penum->wxy.x = penum->wxy.y = 0;
            penum->wxy_float.x = penum->wxy_float.y = 0;
            penum->use_wxy_float = false;
            break;
        case sws_cache:
            /* Finish installing the cache entry. */
            /* If the BuildChar/BuildGlyph procedure did a save and a */
            /* restore, it already undid the gsave in setcachedevice. */
            /* We have to check for this by comparing levels. */
            switch (pgs->level - penum->level) {
                default:
                    gx_free_cached_char(penum->orig_font->dir, penum->cc);
                    return_error(gs_error_invalidfont);         /* WRONG */
                case 2:
                    code = gs_grestore(pgs);
                    if (code < 0)
                        return code;
                case 1:
                    ;
            }
            {   cached_fm_pair *pair;

                code = gx_lookup_fm_pair(pgs->font, &char_tm_only(pgs),
                            &penum->log2_scale, penum->charpath_flag != cpm_show, &pair);
                if (code < 0)
                    return code;
                code = gx_add_cached_char(pgs->font->dir, penum->dev_cache,
                               cc, pair, &penum->log2_scale);
                if (code < 0)
                    return code;
            }
            if (!SHOW_USES_OUTLINE(penum) ||
                penum->charpath_flag != cpm_show
                )
                break;
            /* falls through */
        case sws_cache_width_only:
            /* Copy the bits to the real output device. */
            code = gs_grestore(pgs);
            if (code < 0)
                return code;
            code = gs_gstate_color_load(pgs);
            if (code < 0)
                return code;
            return gx_image_cached_char(penum, cc);
        case sws_no_cache:
            ;
    }
    if (penum->charpath_flag != cpm_show) {
        if (pgs->level <= penum->level) {
            return_error(gs_error_invalidfont);
        }
        /* Move back to the character origin, so that */
        /* show_move will get us to the right place. */
        code = gx_path_add_point(pgs->show_gstate->path,
                                 penum->origin.x, penum->origin.y);
        if (code < 0)
            return code;
    }
    return gs_grestore(pgs);
}

/* Move to next character */
static inline int
show_fast_move(gs_gstate * pgs, gs_fixed_point * pwxy)
{
    return gs_moveto_aux(pgs, pgs->path,
                              pgs->current_point.x + fixed2float(pwxy->x),
                              pgs->current_point.y + fixed2float(pwxy->y));
}

/* Get the current character code. */
int gx_current_char(const gs_text_enum_t * pte)
{
    const gs_show_enum *penum = (const gs_show_enum *)pte;
    gs_char chr = CURRENT_CHAR(penum) & 0xff;
    int fdepth = penum->fstack.depth;

    if (fdepth > 0) {
        /* Add in the shifted font number. */
        uint fidx = penum->fstack.items[fdepth - 1].index;

        switch (((gs_font_type0 *) (penum->fstack.items[fdepth - 1].font))->data.FMapType) {
        case fmap_1_7:
        case fmap_9_7:
            chr += fidx << 7;
            break;
        case fmap_CMap:
            chr = CURRENT_CHAR(penum);  /* the full character */
            if (!penum->cmap_code)
                break;
            /* falls through */
        default:
            chr += fidx << 8;
        }
    }
    return chr;
}

static int
show_move(gs_show_enum * penum)
{
    gs_gstate *pgs = penum->pgs;
    int code;
    double dx = 0, dy = 0;

    /* Specifically for applying PDF word spacing, if single_byte_space == true
       we'll only apply the delta for single byte character codes == space.s_char
       See psi/zchar.c zpdfwidthshow and zpdfawidthshow for more detail
     */
    if (SHOW_IS_ADD_TO_SPACE(penum)
        && (!penum->single_byte_space
        || penum->bytes_decoded == 1)) {
        gs_char chr = gx_current_char((const gs_text_enum_t *)penum);

        if (chr == penum->text.space.s_char) {
            dx = penum->text.delta_space.x;
            dy = penum->text.delta_space.y;
        }
    }

    if (SHOW_IS(penum, TEXT_REPLACE_WIDTHS)) {
        gs_point dpt;

        code = gs_text_replaced_width(&penum->text, penum->xy_index - 1, &dpt);
        if (code < 0)
            return code;
        dpt.x += dx;
        dpt.y += dy;
        code = gs_distance_transform2fixed(&pgs->ctm, dpt.x, dpt.y, &penum->wxy);
        if (code < 0)
            return code;
    } else {
        if (SHOW_IS_ADD_TO_ALL(penum)) {
            dx += penum->text.delta_all.x;
            dy += penum->text.delta_all.y;
        }
        if (!is_fzero2(dx, dy)) {
            gs_fixed_point dxy;

            code = gs_distance_transform2fixed(&pgs->ctm, dx, dy, &dxy);
            if (code < 0)
                return code;
            penum->wxy.x += dxy.x;
            penum->wxy.y += dxy.y;
        }
    }
    if (SHOW_IS_ALL_OF(penum, TEXT_DO_NONE | TEXT_INTERVENE)) {
        /* HACK for cshow */
        penum->continue_proc = continue_kshow;
        return TEXT_PROCESS_INTERVENE;
    }
    /* wxy is in device coordinates */
    {
        int code;

        if (penum->use_wxy_float)
            code = gs_moveto_aux(pgs, pgs->path,
                    pgs->current_point.x + penum->wxy_float.x + fixed2float(penum->wxy.x),
                    pgs->current_point.y + penum->wxy_float.y + fixed2float(penum->wxy.y));
        else
            code = show_fast_move(pgs, &penum->wxy);
        if (code < 0)
            return code;
    }
    /* Check for kerning, but not on the last character. */
    if (SHOW_IS_DO_KERN(penum) && penum->index < penum->text.size) {
        penum->continue_proc = continue_kshow;
        return TEXT_PROCESS_INTERVENE;
    }
    return 0;
}

static inline int
get_next_char_glyph(gs_show_enum * penum, gs_char *chr, gs_glyph *glyph)
{
    gs_font *rfont =
        (penum->fstack.depth < 0 ? penum->pgs->font : penum->fstack.items[0].font);
    penum->xy_index++;

    return rfont->procs.next_char_glyph((gs_text_enum_t *)penum, chr, glyph);
}


/* Process next character */
static int
show_proceed(gs_show_enum * penum)
{
    gs_gstate *pgs = penum->pgs;
    gs_font *pfont;
    cached_fm_pair *pair = 0;
    gs_font *rfont =
        (penum->fstack.depth < 0 ? pgs->font : penum->fstack.items[0].font);
    int wmode = rfont->WMode;
    gs_char chr;
    gs_glyph glyph;
    int code, start;
    cached_char *cc;
    gs_log2_scale_point log2_scale;

    if (penum->charpath_flag == cpm_show && SHOW_USES_OUTLINE(penum)) {
        code = gs_gstate_color_load(pgs);
        if (code < 0)
            return code;
    }
  more:                 /* Proceed to next character */
    pfont = (penum->fstack.depth < 0 ? pgs->font :
             penum->fstack.items[penum->fstack.depth].font);
    penum->current_font = pfont;
    /* can_cache >= 0 allows us to use cached characters, */
    /* even if we can't make new cache entries. */
    if (penum->can_cache >= 0) {
        /* Loop with cache */
        for (;;) {
            start = penum->index;
            switch ((code = get_next_char_glyph(penum, &chr, &glyph))) {
                default:        /* error */
                    return code;
                case 2: /* done */
                    return show_finish(penum);
                case 1: /* font change */
                    pfont = penum->fstack.items[penum->fstack.depth].font;
                    penum->current_font = pfont;
                    pgs->char_tm_valid = false;
                    show_state_setup(penum);
                    pair = 0;
                    penum->pair = 0;
                    /* falls through */
                case 0: /* plain char */
                    /*
                     * We don't need to set penum->current_char in the
                     * normal cases, but it's needed for widthshow,
                     * kshow, and one strange client, so we may as well
                     * do it here.
                     */
                    SET_CURRENT_CHAR(penum, chr);
                    /*
                     * Store glyph now, because pdfwrite needs it while
                     * synthezising bitmap fonts (see assign_char_code).
                     */
                    if (glyph == GS_NO_GLYPH) {
                        glyph = (*penum->encode_char)(pfont, chr,
                                                      GLYPH_SPACE_NAME);
                        SET_CURRENT_GLYPH(penum, glyph);
                    } else
                        SET_CURRENT_GLYPH(penum, glyph);
                    penum->bytes_decoded = penum->index - start;
                    penum->is_pure_color = gs_color_writes_pure(penum->pgs); /* Save
                                 this data for compute_glyph_raster_params to work
                                 independently on the color change in BuildChar.
                                 Doing it here because cshow proc may modify
                                 the graphic state.
                                 */
                    {
                        int alpha_bits, depth;
                        gs_fixed_point subpix_origin;

                        code = compute_glyph_raster_params(penum, false,
                                    &alpha_bits, &depth, &subpix_origin, &log2_scale);
                        if (code < 0)
                            return code;
                        if (pair == 0) {
                            code = gx_lookup_fm_pair(pfont, &char_tm_only(pgs), &log2_scale,
                                penum->charpath_flag != cpm_show, &pair);
                            if (code < 0)
                                return code;
                        }
                        penum->pair = pair;
                        if (glyph == GS_NO_GLYPH || SHOW_IS_ALL_OF(penum, TEXT_NO_CACHE)) {
                            cc = 0;
                            goto no_cache;
                        }
                        cc = gx_lookup_cached_char(pfont, pair, glyph, wmode,
                                                   depth, &subpix_origin);
                    }
                    if (cc == 0) {
                        goto no_cache;
                    }
                    /* Character is in cache. */
                    /* We might be doing .charboxpath or stringwidth; */
                    /* check for these now. */
                    if (penum->charpath_flag == cpm_charwidth) {
                        /* This is charwidth.  Just move by the width. */
                        DO_NOTHING;
                    } else if (penum->charpath_flag != cpm_show) {
                        /* This is .charboxpath. Get the bounding box */
                        /* and append it to a path. */
                        gx_path box_path;
                        gs_fixed_point pt;
                        fixed llx, lly, urx, ury;

                        code = gx_path_current_point(pgs->path, &pt);
                        if (code < 0)
                            return code;
                        llx = fixed_rounded(pt.x - cc->offset.x) +
                            int2fixed(penum->ftx);
                        lly = fixed_rounded(pt.y - cc->offset.y) +
                            int2fixed(penum->fty);
                        urx = llx + int2fixed(cc->width),
                            ury = lly + int2fixed(cc->height);
                        gx_path_init_local(&box_path, pgs->memory);
                        code =
                            gx_path_add_rectangle(&box_path, llx, lly,
                                                  urx, ury);
                        if (code >= 0)
                            code =
                                gx_path_add_char_path(pgs->show_gstate->path,
                                                      &box_path,
                                                      penum->charpath_flag);
                        if (code >= 0)
                            code = gx_path_add_point(pgs->path, pt.x, pt.y);
                        gx_path_free(&box_path, "show_proceed(box path)");
                        if (code < 0)
                            return code;
                    } else if (SHOW_IS_DRAWING(penum)) {
                        code = gx_image_cached_char(penum, cc);
                        if (code < 0)
                            return code;
                        else if (code > 0) {
                            cc = 0;
                            goto no_cache;
                        }
                    }
                    penum->use_wxy_float = false;
                    penum->wxy_float.x = penum->wxy_float.y = 0;
                    if (SHOW_IS_SLOW(penum)) {
                        /* Split up the assignment so that the */
                        /* Watcom compiler won't reserve esi/edi. */
                        penum->wxy.x = cc->wxy.x;
                        penum->wxy.y = cc->wxy.y;
                        code = show_move(penum);
                    } else
                        code = show_fast_move(pgs, &cc->wxy);
                    if (code) {
                        /* Might be kshow, glyph is stored above. */
                        return code;
                    }
            }
        }
    } else {
        start = penum->index;
        /* Can't use cache */
        switch ((code = get_next_char_glyph(penum, &chr, &glyph))) {
            default:
                return code;
            case 2:
                return show_finish(penum);
            case 1:
                pfont = penum->fstack.items[penum->fstack.depth].font;
                penum->current_font = pfont;
                show_state_setup(penum);
                pair = 0;
            case 0:
                {   int alpha_bits, depth;
                    gs_log2_scale_point log2_scale;
                    gs_fixed_point subpix_origin;

                    penum->bytes_decoded = penum->index - start;
                    code = compute_glyph_raster_params(penum, false, &alpha_bits, &depth, &subpix_origin, &log2_scale);
                    if (code < 0)
                        return code;
                    if (pair == 0) {
                        code = gx_lookup_fm_pair(pfont, &char_tm_only(pgs), &log2_scale,
                                penum->charpath_flag != cpm_show, &pair);
                        if (code < 0)
                            return code;
                    }
                    penum->pair = pair;
                }
        }
        SET_CURRENT_CHAR(penum, chr);
        if (glyph == GS_NO_GLYPH) {
            glyph = (*penum->encode_char)(pfont, chr, GLYPH_SPACE_NAME);
        }
        SET_CURRENT_GLYPH(penum, glyph);
        cc = 0;
    }
  no_cache:
    /*
     * We must call the client's rendering code.  Normally,
     * we only do this if the character is not cached (cc = 0);
     * however, we also must do this if we have an xfont but
     * are using scalable widths.  In this case, and only this case,
     * we get here with cc != 0.  penum->current_char and penum->current_glyph
     * has already been set.
     */
    if ((code = gs_gsave(pgs)) < 0)
        return code;
    /* Set the font to the current descendant font. */
    pgs->font = pfont;
    /* Reset the in_cachedevice flag, so that a recursive show */
    /* will use the cache properly. */
    pgs->in_cachedevice = CACHE_DEVICE_NONE;
    /* Set the charpath data in the graphics context if necessary, */
    /* so that fill and stroke will add to the path */
    /* rather than having their usual effect. */
    pgs->in_charpath = penum->charpath_flag;
    pgs->show_gstate =
        (penum->show_gstate == pgs ? pgs->saved : penum->show_gstate);
    pgs->stroke_adjust = false; /* per specification */
    {
        gs_fixed_point cpt;

        if ((code = gx_path_current_point_inline(pgs, &cpt)) < 0) {
            /* For cshow, having no current point is acceptable. */
            if (!SHOW_IS(penum, TEXT_DO_NONE))
                goto rret;
            cpt.x = cpt.y = 0;  /* arbitrary */
        }
        penum->origin.x = cpt.x;
        penum->origin.y = cpt.y;
        /* Normally, char_tm is valid because of show_state_setup, */
        /* but if we're in a cshow, it may not be. */
        gs_currentcharmatrix(pgs, NULL, true);
        if (pgs->ctm.txy_fixed_valid && pgs->char_tm.txy_fixed_valid) {
            fixed tx = pgs->ctm.tx_fixed;
            fixed ty = pgs->ctm.ty_fixed;

            gs_settocharmatrix(pgs);
            cpt.x += pgs->ctm.tx_fixed - tx;
            cpt.y += pgs->ctm.ty_fixed - ty;
        } else  {
            double tx = pgs->ctm.tx;
            double ty = pgs->ctm.ty;
            double fpx, fpy;

            gs_settocharmatrix(pgs);
            fpx = fixed2float(cpt.x) + (pgs->ctm.tx - tx);
            fpy = fixed2float(cpt.y) + (pgs->ctm.ty - ty);
            if (!(f_fits_in_bits(fpx, fixed_int_bits)
                && f_fits_in_bits(fpy, fixed_int_bits))) {
                gs_note_error(code = gs_error_limitcheck);
                goto rret;
            }
            cpt.x = float2fixed(fpx);
            cpt.y = float2fixed(fpy);
        }
        if (((code = gs_newpath(pgs)) < 0) ||
            ((code = show_origin_setup(pgs, cpt.x, cpt.y, penum)) < 0))
            goto rret;
    }
    penum->width_status = sws_none;
    penum->continue_proc = continue_show_update;
    /* Reset the sampling scale. */
    penum->log2_scale.x = penum->log2_scale.y = 0;
    /* Try using the build procedure in the font. */
    /* < 0 means error, 0 means success, 1 means failure. */
    penum->cc = cc;             /* set this now for build procedure */
    code = (*pfont->procs.build_char)(penum, pgs, pfont,
                                      chr, glyph);
    if (code < 0) {
        discard(gs_note_error(code));
        goto rret;
    }
    if (code == 0) {
        code = show_update(penum);
        if (code < 0)
            goto rret;
        /* Note that show_update does a grestore.... */
        code = show_move(penum);
        if (code)
            return code;        /* ... so don't go to rret here. */
        goto more;
    }
    /*
     * Some BuildChar procedures do a save before the setcachedevice,
     * and a restore at the end.  If we waited to allocate the cache
     * device until the setcachedevice, we would attempt to free it
     * after the restore.  Therefore, allocate it now.
     */
    if (penum->dev_cache == 0) {
        code = show_cache_setup(penum);
        if (code < 0)
            goto rret;
    }
    return TEXT_PROCESS_RENDER;
    /* If we get an error while setting up for BuildChar, */
    /* we must undo the partial setup. */
rret:
    while (pgs->level > penum->level) {
        gs_grestore(pgs);
    }
    return code;
}

/*
 * Prepare to retry rendering of the current character.  (This is only used
 * in one place in zchar1.c; a different approach may be better.)
 */
static int
gx_show_text_retry(gs_text_enum_t *pte)
{
    gs_show_enum *const penum = (gs_show_enum *)pte;

    if (penum->cc) {
        gs_font *pfont = penum->current_font;

        gx_free_cached_char(pfont->dir, penum->cc);
        penum->cc = 0;
    }
    gs_grestore(penum->pgs);
    penum->width_status = sws_retry;
    penum->log2_scale.x = penum->log2_scale.y = 0;
    penum->pair = 0;
    return 0;
}

/* Finish show or stringwidth */
static int
show_finish(gs_show_enum * penum)
{
    gs_gstate *pgs = penum->pgs;
    int code = 0, rcode;

    if ((penum->text.operation & TEXT_DO_FALSE_CHARPATH) ||
        (penum->text.operation & TEXT_DO_TRUE_CHARPATH)) {
        if (pgs->path->current_subpath)
            pgs->path->last_charpath_segment = pgs->path->current_subpath->last;
    }
    if (penum->auto_release)
        penum->procs->release((gs_text_enum_t *)penum, "show_finish");

    if (!SHOW_IS_STRINGWIDTH(penum))
       return 0;

    /* Save the accumulated width before returning, if we are not in PDF text rendering mode 3, */
    /* and undo the extra gsave. */
    if (!(penum->text.operation & TEXT_RENDER_MODE_3))
        code = gs_currentpoint(pgs, &penum->returned.total_width);
    rcode = gs_grestore(pgs);

    return (code < 0 ? code : rcode);
}

/* Release the structure. */
static void
gx_show_text_release(gs_text_enum_t *pte, client_name_t cname)
{
    gs_show_enum *const penum = (gs_show_enum *)pte;

    penum->cc = 0;
    if (penum->dev_cache2) {
        gx_device_retain((gx_device *)penum->dev_cache2, false);
        penum->dev_cache2 = 0;
    }
    if (penum->dev_cache) {
        gx_device_retain((gx_device *)penum->dev_cache, false);
        penum->dev_cache = 0;
    }
    if (penum->dev_null) {
        gx_device_retain((gx_device *)penum->dev_null, false);
        penum->dev_null = 0;
    }
    gx_default_text_release(pte, cname);
}

/* ------ Miscellaneous accessors ------ */

/* Return the charpath mode. */
gs_char_path_mode
gs_show_in_charpath(const gs_show_enum * penum)
{
    return penum->charpath_flag;
}

/* Return true if we only need the width from the rasterizer */
/* and can short-circuit the full rendering of the character, */
/* false if we need the actual character bits. */
/* This is only meaningful just before calling gs_setcharwidth or */
/* gs_setcachedevice[2]. */
/* Note that we can't do this if the procedure has done any extra [g]saves. */
static bool
gx_show_text_is_width_only(const gs_text_enum_t *pte)
{
    const gs_show_enum *const penum = (const gs_show_enum *)pte;

    /* penum->cc will be non-zero iff we are calculating */
    /* the scalable width for an xfont character. */
    return ((!SHOW_USES_OUTLINE(penum) || penum->cc != 0) &&
            penum->pgs->level == penum->level + 1);
}

/* Return the width of the just-enumerated character (for cshow). */
static int
gx_show_text_current_width(const gs_text_enum_t *pte, gs_point *pwidth)
{
    const gs_show_enum *const penum = (const gs_show_enum *)pte;

    return gs_idtransform(penum->pgs,
                          fixed2float(penum->wxy.x),
                          fixed2float(penum->wxy.y), pwidth);
}

/* Return the current font for cshow. */
gs_font *
gs_show_current_font(const gs_show_enum * penum)
{
    return (penum->fstack.depth < 0 ? penum->pgs->font :
            penum->fstack.items[penum->fstack.depth].font);
}

/* ------ Internal routines ------ */

/* Initialize the gstate-derived parts of a show enumerator. */
/* We do this both when starting the show operation, */
/* and when returning from the kshow callout. */
/* Uses only penum->pgs, penum->fstack. */
static int
show_state_setup(gs_show_enum * penum)
{
    gs_gstate *pgs = penum->pgs;
    gx_clip_path *pcpath;
    gs_font *pfont;

    if (penum->fstack.depth <= 0) {
        pfont = pgs->font;
        if (pfont->FontType == ft_CID_encrypted) {
            /* doing 'cid glyphshow',
               assuming penum->operation has TEXT_FROM_SINGLE_GLYPH */
            gs_matrix mat;
            int fidx;
            int code = ((gs_font_cid0 *)pfont)->cidata.glyph_data((gs_font_base *)pfont,
                                penum->text.data.d_glyph, NULL, &fidx);
            if (code < 0) { /* failed to load glyph data, reload glyph for CID 0 */
               code = ((gs_font_cid0 *)pfont)->cidata.glyph_data((gs_font_base *)pfont,
                            (gs_glyph)(GS_MIN_CID_GLYPH + 0), NULL, &fidx);
               if (code < 0)
                   return_error(gs_error_invalidfont);
            }
            gs_matrix_multiply(&(gs_cid0_indexed_font(pfont, fidx)->FontMatrix),
                                &pfont->FontMatrix, &mat);
            gs_setcharmatrix(pgs, &mat);
        } else {
            gs_currentcharmatrix(pgs, NULL, 1); /* make char_tm valid */
        }
    } else {
        /* We have to concatenate the parent's FontMatrix as well. */
        gs_matrix mat;
        const gx_font_stack_item_t *pfsi =
            &penum->fstack.items[penum->fstack.depth];

        pfont = pfsi->font;
        gs_matrix_multiply(&pfont->FontMatrix,
                           &pfsi[-1].font->FontMatrix, &mat);
        if (pfont->FontType == ft_CID_encrypted) {
            /* concatenate the Type9 leaf's matrix */
            gs_matrix_multiply(&(gs_cid0_indexed_font(pfont, pfsi->index)->FontMatrix),
                                &mat, &mat);
        }
        gs_setcharmatrix(pgs, &mat);
    }
    penum->current_font = pfont;
    if (penum->can_cache >= 0 &&
        gx_effective_clip_path(pgs, &pcpath) >= 0
        ) {
        gs_fixed_rect cbox;

        gx_cpath_inner_box(pcpath, &cbox);
        /* Since characters occupy an integral number of pixels, */
        /* we can (and should) round the inner clipping box */
        /* outward rather than inward. */
        penum->ibox.p.x = fixed2int_var(cbox.p.x);
        penum->ibox.p.y = fixed2int_var(cbox.p.y);
        penum->ibox.q.x = fixed2int_var_ceiling(cbox.q.x);
        penum->ibox.q.y = fixed2int_var_ceiling(cbox.q.y);
        gx_cpath_outer_box(pcpath, &cbox);
        penum->obox.p.x = fixed2int_var(cbox.p.x);
        penum->obox.p.y = fixed2int_var(cbox.p.y);
        penum->obox.q.x = fixed2int_var_ceiling(cbox.q.x);
        penum->obox.q.y = fixed2int_var_ceiling(cbox.q.y);
        if (pgs->ctm.txy_fixed_valid && pgs->char_tm.txy_fixed_valid) {
            penum->ftx = (int)fixed2long(pgs->char_tm.tx_fixed -
                                         pgs->ctm.tx_fixed);
            penum->fty = (int)fixed2long(pgs->char_tm.ty_fixed -
                                         pgs->ctm.ty_fixed);
        } else {
            double fdx = pgs->char_tm.tx - pgs->ctm.tx;
            double fdy = pgs->char_tm.ty - pgs->ctm.ty;

#define int_bits (ARCH_SIZEOF_INT * 8 - 1)
            if (!(f_fits_in_bits(fdx, int_bits) &&
                  f_fits_in_bits(fdy, int_bits))
                )
                return_error(gs_error_limitcheck);
#undef int_bits
            penum->ftx = (int)fdx;
            penum->fty = (int)fdy;
        }
    }
    show_set_encode_char(penum);
    return 0;
}

/* Set the suggested oversampling scale for character rendering. */
static void
show_set_scale(const gs_show_enum * penum, gs_log2_scale_point *log2_scale)
{
    /*
     * Decide whether to oversample.
     * We have to decide this each time setcachedevice is called.
     */
    const gs_gstate *pgs = NULL;

    if (gs_object_type(penum->pgs->memory, penum) == &st_gs_show_enum) {
        pgs = penum->pgs;
    } else {
            pgs = (gs_gstate *)penum->pgs;
    }

    if (pgs != NULL && (penum->charpath_flag == cpm_show ||
         penum->charpath_flag == cpm_charwidth) &&
        SHOW_USES_OUTLINE(penum)
        /* && gx_path_is_void_inline(pgs->path) */
        ) {
        const gs_font_base *pfont = (const gs_font_base *)penum->current_font;
        gs_fixed_point extent;
        int code = gs_distance_transform2fixed(&pgs->char_tm,
                                  pfont->FontBBox.q.x - pfont->FontBBox.p.x,
                                  pfont->FontBBox.q.y - pfont->FontBBox.p.y,
                                               &extent);

        if (code >= 0) {
            int sx =
            (any_abs(extent.x) < int2fixed(60) ? 2 :
             any_abs(extent.x) < int2fixed(200) ? 1 :
             0);
            int sy =
            (any_abs(extent.y) < int2fixed(60) ? 2 :
             any_abs(extent.y) < int2fixed(200) ? 1 :
             0);

            /* If we oversample at all, make sure we do it */
            /* in both X and Y. */
            if (sx == 0 && sy != 0)
                sx = 1;
            else if (sy == 0 && sx != 0)
                sy = 1;
            log2_scale->x = sx;
            log2_scale->y = sy;
            return;
        }
    }
    /* By default, don't scale. */
    log2_scale->x = log2_scale->y = 0;
}

/* Set up the cache device and related information. */
/* Note that we always allocate both cache devices, */
/* even if we only use one of them. */
static int
show_cache_setup(gs_show_enum * penum)
{
    gs_gstate *pgs = penum->pgs;
    gs_memory_t *mem = penum->memory;
    gx_device_memory *dev =
        gs_alloc_struct_immovable(mem, gx_device_memory, &st_device_memory,
                        "show_cache_setup(dev_cache)");
    gx_device_memory *dev2 =
        gs_alloc_struct_immovable(mem, gx_device_memory, &st_device_memory,
                        "show_cache_setup(dev_cache2)");

    if (dev == 0 || dev2 == 0) {
        /*
         * The structure is full of garbage so must not call the
         * finalize method but still need to free the structure
         */
        gs_set_object_type(mem, dev2, &st_bytes);
        gs_set_object_type(mem, dev, &st_bytes);
        gs_free_object(mem, dev2, "show_cache_setup(dev_cache2)");
        gs_free_object(mem, dev, "show_cache_setup(dev_cache)");
        return_error(gs_error_VMerror);
    }
    /*
     * We only initialize the devices for the sake of the GC,
     * (since we have to re-initialize dev as either a mem_mono
     * or a mem_abuf device before actually using it) and also
     * to set its memory pointer.
     */
    gs_make_mem_mono_device(dev, mem, gs_currentdevice_inline(pgs));
    penum->dev_cache = dev;
    gs_make_mem_mono_device(dev2, mem, gs_currentdevice_inline(pgs));
    penum->dev_cache2 = dev2;
    dev->HWResolution[0] = pgs->device->HWResolution[0];
    dev->HWResolution[1] = pgs->device->HWResolution[1];
    /* Retain these devices, since they are referenced from the enumerator. */
    gx_device_retain((gx_device *)dev, true);
    gx_device_retain((gx_device *)dev2, true);
    return 0;
}

/* Set the character origin as the origin of the coordinate system. */
/* Used before rendering characters, and for moving the origin */
/* in setcachedevice2 when WMode=1. */
static int
show_origin_setup(gs_gstate * pgs, fixed cpt_x, fixed cpt_y, gs_show_enum * penum)
{
    if (penum->charpath_flag == cpm_show) {
        /* Round the translation in the graphics state. */
        /* This helps prevent rounding artifacts later. */
        if (gs_currentaligntopixels(penum->current_font->dir) == 0) {
            int scx = -(1L << (_fixed_shift - penum->log2_scale.x));
            int scy = -(1L << (_fixed_shift - penum->log2_scale.y));
            int rdx =  1L << (_fixed_shift - 1 - penum->log2_scale.x);
            int rdy =  1L << (_fixed_shift - 1 - penum->log2_scale.y);

            cpt_x = (cpt_x + rdx) & scx;
            cpt_y = (cpt_y + rdy) & scy;
        } else {
            cpt_x = fixed_rounded(cpt_x);
            cpt_y = fixed_rounded(cpt_y);
        }
    }
    /*
     * BuildChar procedures expect the current point to be undefined,
     * so we omit the gx_path_add_point with ctm.t*_fixed.
     */
    return gx_translate_to_fixed(pgs, cpt_x, cpt_y);
}
