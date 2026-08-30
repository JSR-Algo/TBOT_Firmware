/*******************************************************************************
 * Size: 20 px
 * Bpp: 4
 * Contains U+1EB5 for the Vietnamese READY_TO_CONNECT copy. Other glyphs
 * fall back to the production Puhui font.
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl.h"
#endif

#ifndef TBOT_VIETNAMESE_20_4
#define TBOT_VIETNAMESE_20_4 1
#endif

#if TBOT_VIETNAMESE_20_4

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+1EB5 "ẵ" */
    0x0, 0x0, 0x0, 0x0, 0x10, 0x0, 0x0, 0xaf,
    0xd5, 0x6f, 0x10, 0x0, 0x3f, 0x69, 0xff, 0xb0,
    0x0, 0x1, 0x40, 0x1, 0x40, 0x0, 0x0, 0xf,
    0x70, 0x2d, 0x70, 0x0, 0x0, 0x5f, 0xff, 0xb0,
    0x0, 0x0, 0x0, 0x2, 0x10, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x2a, 0xef, 0xfc,
    0x40, 0x0, 0x3f, 0xfa, 0x8a, 0xff, 0x40, 0xb,
    0xf3, 0x0, 0x5, 0xf9, 0x0, 0x46, 0x0, 0x0,
    0x1f, 0xb0, 0x0, 0x1, 0x36, 0x8c, 0xfb, 0x0,
    0x1a, 0xff, 0xfe, 0xaf, 0xb0, 0xd, 0xf9, 0x41,
    0x1, 0xfb, 0x3, 0xfb, 0x0, 0x0, 0x3f, 0xb0,
    0x3f, 0xc0, 0x0, 0xc, 0xfb, 0x0, 0xcf, 0xb7,
    0x8e, 0xff, 0xd0, 0x1, 0xae, 0xfd, 0xa2, 0xbf,
    0x10
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 178, .box_w = 11, .box_h = 19, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 7861, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};

extern const lv_font_t font_puhui_basic_20_4;


/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t tbot_vietnamese_20_4 = {
#else
lv_font_t tbot_vietnamese_20_4 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 25,          /*The maximum line height required by the font*/
    .base_line = 6,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -2,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = &font_puhui_basic_20_4,
#endif
    .user_data = NULL,
};



#endif /*#if TBOT_VIETNAMESE_20_4*/
