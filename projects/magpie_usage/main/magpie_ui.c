// SPDX-License-Identifier: Apache-2.0

#include "magpie_ui.h"

#include <stdio.h>
#include <string.h>

#include "bundle_gsp.h"

/*
 * One render pass touches every authorable field the protocol can change:
 * header status/transport/summary, the counter, the page dots and, for each
 * slot, the name/plan, up to three window rows and the error/empty labels.
 *
 * Setters are asynchronous on IDF and immediate in the simulator, so nothing
 * here reads back what it just wrote.
 */

#define WINDOW_BLOCKS MAGPIE_WIN_MAX

static void record(magpie_ui_t *self, esp_gsp_err_t err)
{
    if (err != ESP_GSP_OK && self->last_error == ESP_GSP_OK) {
        self->last_error = err;
#ifdef MAGPIE_UI_DEBUG
        fprintf(stderr, "magpie_ui: setter failed: 0x%x\n", (unsigned)err);
#endif
    }
}

#ifdef MAGPIE_UI_DEBUG
#define DEBUG_FAIL(kind, key, err)                                          \
    do {                                                                     \
        if ((err) != ESP_GSP_OK) {                                            \
            fprintf(stderr, "magpie_ui: %s(0x%08x) -> 0x%x\n", kind,        \
                    (unsigned)(key), (unsigned)(err));                        \
        }                                                                     \
    } while (0)
#else
#define DEBUG_FAIL(kind, key, err) ((void)0)
#endif

static void set_text(magpie_ui_t *self, gsp_component_key_t key,
                     const char *text)
{
    const esp_gsp_err_t err =
        esp_gsp_component_set_text(self->ui, key, text ? text : "");
    DEBUG_FAIL("set_text", key, err);
    record(self, err);
}

static void set_visible(magpie_ui_t *self, gsp_component_key_t key, bool on)
{
    const esp_gsp_err_t err =
        esp_gsp_component_set_visible(self->ui, key, on);
    DEBUG_FAIL("set_visible", key, err);
    record(self, err);
}

static void set_value(magpie_ui_t *self, gsp_component_key_t key, int value)
{
    const esp_gsp_err_t err =
        esp_gsp_component_set_value(self->ui, key, value);
    DEBUG_FAIL("set_value", key, err);
    record(self, err);
}

static const gsp_component_key_t kNameKeys[MAGPIE_UI_PAGE_MAX] = {
    GSP_MAGPIE_OBJ_KEY_P0_NAME,  GSP_MAGPIE_OBJ_KEY_P1_NAME,
    GSP_MAGPIE_OBJ_KEY_P2_NAME,  GSP_MAGPIE_OBJ_KEY_P3_NAME,
    GSP_MAGPIE_OBJ_KEY_P4_NAME,  GSP_MAGPIE_OBJ_KEY_P5_NAME,
    GSP_MAGPIE_OBJ_KEY_P6_NAME,  GSP_MAGPIE_OBJ_KEY_P7_NAME,
    GSP_MAGPIE_OBJ_KEY_P8_NAME,  GSP_MAGPIE_OBJ_KEY_P9_NAME,
    GSP_MAGPIE_OBJ_KEY_P10_NAME, GSP_MAGPIE_OBJ_KEY_P11_NAME,
    GSP_MAGPIE_OBJ_KEY_P12_NAME, GSP_MAGPIE_OBJ_KEY_P13_NAME,
    GSP_MAGPIE_OBJ_KEY_P14_NAME, GSP_MAGPIE_OBJ_KEY_P15_NAME,
};

static const gsp_component_key_t kPlanKeys[MAGPIE_UI_PAGE_MAX] = {
    GSP_MAGPIE_OBJ_KEY_P0_PLAN,  GSP_MAGPIE_OBJ_KEY_P1_PLAN,
    GSP_MAGPIE_OBJ_KEY_P2_PLAN,  GSP_MAGPIE_OBJ_KEY_P3_PLAN,
    GSP_MAGPIE_OBJ_KEY_P4_PLAN,  GSP_MAGPIE_OBJ_KEY_P5_PLAN,
    GSP_MAGPIE_OBJ_KEY_P6_PLAN,  GSP_MAGPIE_OBJ_KEY_P7_PLAN,
    GSP_MAGPIE_OBJ_KEY_P8_PLAN,  GSP_MAGPIE_OBJ_KEY_P9_PLAN,
    GSP_MAGPIE_OBJ_KEY_P10_PLAN, GSP_MAGPIE_OBJ_KEY_P11_PLAN,
    GSP_MAGPIE_OBJ_KEY_P12_PLAN, GSP_MAGPIE_OBJ_KEY_P13_PLAN,
    GSP_MAGPIE_OBJ_KEY_P14_PLAN, GSP_MAGPIE_OBJ_KEY_P15_PLAN,
};

static const gsp_component_key_t kErrorKeys[MAGPIE_UI_PAGE_MAX] = {
    GSP_MAGPIE_OBJ_KEY_P0_ERROR,  GSP_MAGPIE_OBJ_KEY_P1_ERROR,
    GSP_MAGPIE_OBJ_KEY_P2_ERROR,  GSP_MAGPIE_OBJ_KEY_P3_ERROR,
    GSP_MAGPIE_OBJ_KEY_P4_ERROR,  GSP_MAGPIE_OBJ_KEY_P5_ERROR,
    GSP_MAGPIE_OBJ_KEY_P6_ERROR,  GSP_MAGPIE_OBJ_KEY_P7_ERROR,
    GSP_MAGPIE_OBJ_KEY_P8_ERROR,  GSP_MAGPIE_OBJ_KEY_P9_ERROR,
    GSP_MAGPIE_OBJ_KEY_P10_ERROR, GSP_MAGPIE_OBJ_KEY_P11_ERROR,
    GSP_MAGPIE_OBJ_KEY_P12_ERROR, GSP_MAGPIE_OBJ_KEY_P13_ERROR,
    GSP_MAGPIE_OBJ_KEY_P14_ERROR, GSP_MAGPIE_OBJ_KEY_P15_ERROR,
};

static const gsp_component_key_t kEmptyKeys[MAGPIE_UI_PAGE_MAX] = {
    GSP_MAGPIE_OBJ_KEY_P0_EMPTY,  GSP_MAGPIE_OBJ_KEY_P1_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P2_EMPTY,  GSP_MAGPIE_OBJ_KEY_P3_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P4_EMPTY,  GSP_MAGPIE_OBJ_KEY_P5_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P6_EMPTY,  GSP_MAGPIE_OBJ_KEY_P7_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P8_EMPTY,  GSP_MAGPIE_OBJ_KEY_P9_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P10_EMPTY, GSP_MAGPIE_OBJ_KEY_P11_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P12_EMPTY, GSP_MAGPIE_OBJ_KEY_P13_EMPTY,
    GSP_MAGPIE_OBJ_KEY_P14_EMPTY, GSP_MAGPIE_OBJ_KEY_P15_EMPTY,
};

static const gsp_component_key_t kDotKeys[MAGPIE_UI_PAGE_MAX] = {
    GSP_MAGPIE_OBJ_KEY_DOT0,  GSP_MAGPIE_OBJ_KEY_DOT1,
    GSP_MAGPIE_OBJ_KEY_DOT2,  GSP_MAGPIE_OBJ_KEY_DOT3,
    GSP_MAGPIE_OBJ_KEY_DOT4,  GSP_MAGPIE_OBJ_KEY_DOT5,
    GSP_MAGPIE_OBJ_KEY_DOT6,  GSP_MAGPIE_OBJ_KEY_DOT7,
    GSP_MAGPIE_OBJ_KEY_DOT8,  GSP_MAGPIE_OBJ_KEY_DOT9,
    GSP_MAGPIE_OBJ_KEY_DOT10, GSP_MAGPIE_OBJ_KEY_DOT11,
    GSP_MAGPIE_OBJ_KEY_DOT12, GSP_MAGPIE_OBJ_KEY_DOT13,
    GSP_MAGPIE_OBJ_KEY_DOT14, GSP_MAGPIE_OBJ_KEY_DOT15,
};

/* Accent overlay for the current page dot. */
static const gsp_component_key_t kDotOnKeys[MAGPIE_UI_PAGE_MAX] = {
    GSP_MAGPIE_OBJ_KEY_DOT0_ON,  GSP_MAGPIE_OBJ_KEY_DOT1_ON,
    GSP_MAGPIE_OBJ_KEY_DOT2_ON,  GSP_MAGPIE_OBJ_KEY_DOT3_ON,
    GSP_MAGPIE_OBJ_KEY_DOT4_ON,  GSP_MAGPIE_OBJ_KEY_DOT5_ON,
    GSP_MAGPIE_OBJ_KEY_DOT6_ON,  GSP_MAGPIE_OBJ_KEY_DOT7_ON,
    GSP_MAGPIE_OBJ_KEY_DOT8_ON,  GSP_MAGPIE_OBJ_KEY_DOT9_ON,
    GSP_MAGPIE_OBJ_KEY_DOT10_ON, GSP_MAGPIE_OBJ_KEY_DOT11_ON,
    GSP_MAGPIE_OBJ_KEY_DOT12_ON, GSP_MAGPIE_OBJ_KEY_DOT13_ON,
    GSP_MAGPIE_OBJ_KEY_DOT14_ON, GSP_MAGPIE_OBJ_KEY_DOT15_ON,
};

/* Each window block is a container; hiding it hides its label/bar children. */
static const gsp_component_key_t kWinRowKeys[MAGPIE_UI_PAGE_MAX][WINDOW_BLOCKS] = {
    {GSP_MAGPIE_OBJ_KEY_P0_W0, GSP_MAGPIE_OBJ_KEY_P0_W1,
     GSP_MAGPIE_OBJ_KEY_P0_W2},
    {GSP_MAGPIE_OBJ_KEY_P1_W0, GSP_MAGPIE_OBJ_KEY_P1_W1,
     GSP_MAGPIE_OBJ_KEY_P1_W2},
    {GSP_MAGPIE_OBJ_KEY_P2_W0, GSP_MAGPIE_OBJ_KEY_P2_W1,
     GSP_MAGPIE_OBJ_KEY_P2_W2},
    {GSP_MAGPIE_OBJ_KEY_P3_W0, GSP_MAGPIE_OBJ_KEY_P3_W1,
     GSP_MAGPIE_OBJ_KEY_P3_W2},
    {GSP_MAGPIE_OBJ_KEY_P4_W0, GSP_MAGPIE_OBJ_KEY_P4_W1,
     GSP_MAGPIE_OBJ_KEY_P4_W2},
    {GSP_MAGPIE_OBJ_KEY_P5_W0, GSP_MAGPIE_OBJ_KEY_P5_W1,
     GSP_MAGPIE_OBJ_KEY_P5_W2},
    {GSP_MAGPIE_OBJ_KEY_P6_W0, GSP_MAGPIE_OBJ_KEY_P6_W1,
     GSP_MAGPIE_OBJ_KEY_P6_W2},
    {GSP_MAGPIE_OBJ_KEY_P7_W0, GSP_MAGPIE_OBJ_KEY_P7_W1,
     GSP_MAGPIE_OBJ_KEY_P7_W2},
    {GSP_MAGPIE_OBJ_KEY_P8_W0, GSP_MAGPIE_OBJ_KEY_P8_W1,
     GSP_MAGPIE_OBJ_KEY_P8_W2},
    {GSP_MAGPIE_OBJ_KEY_P9_W0, GSP_MAGPIE_OBJ_KEY_P9_W1,
     GSP_MAGPIE_OBJ_KEY_P9_W2},
    {GSP_MAGPIE_OBJ_KEY_P10_W0, GSP_MAGPIE_OBJ_KEY_P10_W1,
     GSP_MAGPIE_OBJ_KEY_P10_W2},
    {GSP_MAGPIE_OBJ_KEY_P11_W0, GSP_MAGPIE_OBJ_KEY_P11_W1,
     GSP_MAGPIE_OBJ_KEY_P11_W2},
    {GSP_MAGPIE_OBJ_KEY_P12_W0, GSP_MAGPIE_OBJ_KEY_P12_W1,
     GSP_MAGPIE_OBJ_KEY_P12_W2},
    {GSP_MAGPIE_OBJ_KEY_P13_W0, GSP_MAGPIE_OBJ_KEY_P13_W1,
     GSP_MAGPIE_OBJ_KEY_P13_W2},
    {GSP_MAGPIE_OBJ_KEY_P14_W0, GSP_MAGPIE_OBJ_KEY_P14_W1,
     GSP_MAGPIE_OBJ_KEY_P14_W2},
    {GSP_MAGPIE_OBJ_KEY_P15_W0, GSP_MAGPIE_OBJ_KEY_P15_W1,
     GSP_MAGPIE_OBJ_KEY_P15_W2},
};

/* Per-window keys are dense; keep them as tables instead of switch chains. */
static const gsp_component_key_t kWinLabelKeys[MAGPIE_UI_PAGE_MAX][WINDOW_BLOCKS] = {
    {GSP_MAGPIE_OBJ_KEY_P0_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P0_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P0_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P1_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P1_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P1_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P2_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P2_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P2_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P3_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P3_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P3_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P4_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P4_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P4_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P5_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P5_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P5_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P6_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P6_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P6_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P7_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P7_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P7_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P8_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P8_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P8_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P9_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P9_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P9_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P10_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P10_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P10_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P11_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P11_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P11_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P12_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P12_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P12_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P13_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P13_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P13_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P14_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P14_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P14_W2_LABEL},
    {GSP_MAGPIE_OBJ_KEY_P15_W0_LABEL, GSP_MAGPIE_OBJ_KEY_P15_W1_LABEL,
     GSP_MAGPIE_OBJ_KEY_P15_W2_LABEL},
};

static const gsp_component_key_t kWinUsedKeys[MAGPIE_UI_PAGE_MAX][WINDOW_BLOCKS] = {
    {GSP_MAGPIE_OBJ_KEY_P0_W0_USED, GSP_MAGPIE_OBJ_KEY_P0_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P0_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P1_W0_USED, GSP_MAGPIE_OBJ_KEY_P1_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P1_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P2_W0_USED, GSP_MAGPIE_OBJ_KEY_P2_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P2_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P3_W0_USED, GSP_MAGPIE_OBJ_KEY_P3_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P3_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P4_W0_USED, GSP_MAGPIE_OBJ_KEY_P4_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P4_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P5_W0_USED, GSP_MAGPIE_OBJ_KEY_P5_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P5_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P6_W0_USED, GSP_MAGPIE_OBJ_KEY_P6_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P6_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P7_W0_USED, GSP_MAGPIE_OBJ_KEY_P7_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P7_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P8_W0_USED, GSP_MAGPIE_OBJ_KEY_P8_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P8_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P9_W0_USED, GSP_MAGPIE_OBJ_KEY_P9_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P9_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P10_W0_USED, GSP_MAGPIE_OBJ_KEY_P10_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P10_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P11_W0_USED, GSP_MAGPIE_OBJ_KEY_P11_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P11_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P12_W0_USED, GSP_MAGPIE_OBJ_KEY_P12_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P12_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P13_W0_USED, GSP_MAGPIE_OBJ_KEY_P13_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P13_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P14_W0_USED, GSP_MAGPIE_OBJ_KEY_P14_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P14_W2_USED},
    {GSP_MAGPIE_OBJ_KEY_P15_W0_USED, GSP_MAGPIE_OBJ_KEY_P15_W1_USED,
     GSP_MAGPIE_OBJ_KEY_P15_W2_USED},
};

static const gsp_component_key_t kWinResetKeys[MAGPIE_UI_PAGE_MAX][WINDOW_BLOCKS] = {
    {GSP_MAGPIE_OBJ_KEY_P0_W0_RESET, GSP_MAGPIE_OBJ_KEY_P0_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P0_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P1_W0_RESET, GSP_MAGPIE_OBJ_KEY_P1_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P1_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P2_W0_RESET, GSP_MAGPIE_OBJ_KEY_P2_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P2_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P3_W0_RESET, GSP_MAGPIE_OBJ_KEY_P3_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P3_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P4_W0_RESET, GSP_MAGPIE_OBJ_KEY_P4_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P4_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P5_W0_RESET, GSP_MAGPIE_OBJ_KEY_P5_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P5_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P6_W0_RESET, GSP_MAGPIE_OBJ_KEY_P6_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P6_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P7_W0_RESET, GSP_MAGPIE_OBJ_KEY_P7_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P7_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P8_W0_RESET, GSP_MAGPIE_OBJ_KEY_P8_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P8_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P9_W0_RESET, GSP_MAGPIE_OBJ_KEY_P9_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P9_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P10_W0_RESET, GSP_MAGPIE_OBJ_KEY_P10_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P10_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P11_W0_RESET, GSP_MAGPIE_OBJ_KEY_P11_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P11_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P12_W0_RESET, GSP_MAGPIE_OBJ_KEY_P12_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P12_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P13_W0_RESET, GSP_MAGPIE_OBJ_KEY_P13_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P13_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P14_W0_RESET, GSP_MAGPIE_OBJ_KEY_P14_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P14_W2_RESET},
    {GSP_MAGPIE_OBJ_KEY_P15_W0_RESET, GSP_MAGPIE_OBJ_KEY_P15_W1_RESET,
     GSP_MAGPIE_OBJ_KEY_P15_W2_RESET},
};

static const gsp_component_key_t kWinBarKeys[MAGPIE_UI_PAGE_MAX][WINDOW_BLOCKS] = {
    {GSP_MAGPIE_OBJ_KEY_P0_W0_BAR, GSP_MAGPIE_OBJ_KEY_P0_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P0_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P1_W0_BAR, GSP_MAGPIE_OBJ_KEY_P1_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P1_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P2_W0_BAR, GSP_MAGPIE_OBJ_KEY_P2_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P2_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P3_W0_BAR, GSP_MAGPIE_OBJ_KEY_P3_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P3_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P4_W0_BAR, GSP_MAGPIE_OBJ_KEY_P4_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P4_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P5_W0_BAR, GSP_MAGPIE_OBJ_KEY_P5_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P5_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P6_W0_BAR, GSP_MAGPIE_OBJ_KEY_P6_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P6_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P7_W0_BAR, GSP_MAGPIE_OBJ_KEY_P7_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P7_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P8_W0_BAR, GSP_MAGPIE_OBJ_KEY_P8_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P8_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P9_W0_BAR, GSP_MAGPIE_OBJ_KEY_P9_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P9_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P10_W0_BAR, GSP_MAGPIE_OBJ_KEY_P10_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P10_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P11_W0_BAR, GSP_MAGPIE_OBJ_KEY_P11_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P11_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P12_W0_BAR, GSP_MAGPIE_OBJ_KEY_P12_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P12_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P13_W0_BAR, GSP_MAGPIE_OBJ_KEY_P13_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P13_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P14_W0_BAR, GSP_MAGPIE_OBJ_KEY_P14_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P14_W2_BAR},
    {GSP_MAGPIE_OBJ_KEY_P15_W0_BAR, GSP_MAGPIE_OBJ_KEY_P15_W1_BAR,
     GSP_MAGPIE_OBJ_KEY_P15_W2_BAR},
};

static void render_page(magpie_ui_t *self, uint8_t slot);

static void render_header(magpie_ui_t *self)
{
    magpie_header_text_t header;
    magpie_header_text(&self->state, &header);
    if (self->shown_header_valid &&
        memcmp(&header, &self->shown_header, sizeof(header)) == 0) {
        return;
    }
    set_text(self, GSP_MAGPIE_OBJ_KEY_STATUS, header.status);
    set_text(self, GSP_MAGPIE_OBJ_KEY_TRANSPORT, header.transport);
    set_text(self, GSP_MAGPIE_OBJ_KEY_TIGHTEST, header.tightest);
    set_text(self, GSP_MAGPIE_OBJ_KEY_COUNTER, header.counter);
    self->shown_header = header;
    self->shown_header_valid = true;
}

static void render_page(magpie_ui_t *self, uint8_t slot)
{
    magpie_page_text_t page;
    const bool in_range = slot < self->state.pages_visible;
    const bool has_data = magpie_page_text(&self->state, slot, &page);
    /* Skip a page whose strings and range state are already on screen.
     * Countdowns are derived from the sender timestamp, so a new snapshot
     * changes them and the comparison still catches it. */
    if (self->shown_page_valid[slot] && self->shown_page_count[slot] == in_range &&
        memcmp(&page, &self->shown_page[slot], sizeof(page)) == 0) {
        return;
    }
    set_text(self, kNameKeys[slot], page.name);
    set_text(self, kPlanKeys[slot], page.plan);
    set_visible(self, kErrorKeys[slot], page.error);
    set_visible(self, kEmptyKeys[slot], has_data ? page.empty : true);
    for (uint8_t window = 0; window < WINDOW_BLOCKS; ++window) {
        const bool show = has_data && !page.error && window < page.window_count;
        set_visible(self, kWinRowKeys[slot][window], show);
        if (!show) {
            continue;
        }
        set_text(self, kWinLabelKeys[slot][window], page.window_label[window]);
        set_text(self, kWinUsedKeys[slot][window], page.window_used[window]);
        set_text(self, kWinResetKeys[slot][window], page.window_reset[window]);
        set_value(self, kWinBarKeys[slot][window],
                  (int)page.window_used_percent[window]);
    }
}

/* Dots exist for every authored page; only the reported ones are shown, and
 * the tail is hidden once when the count shrinks. */
static void render_dots(magpie_ui_t *self)
{
    const uint8_t visible = (uint8_t)self->state.pages_visible;
    for (uint8_t slot = 0; slot < MAGPIE_UI_PAGE_MAX; ++slot) {
        if (slot < visible) {
            set_visible(self, kDotKeys[slot], true);
            set_visible(self, kDotOnKeys[slot], slot == self->page);
        } else if (slot < self->rendered_pages) {
            /* Hide only what was previously shown. */
            set_visible(self, kDotKeys[slot], false);
            set_visible(self, kDotOnKeys[slot], false);
        }
    }
}



esp_gsp_err_t magpie_ui_init(esp_gsp_handle_t ui, magpie_ui_t *out)
{
    if (!ui || !out) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out = (magpie_ui_t){0};
    out->ui = ui;
    magpie_state_init(&out->state);
    out->page = 0;
    out->rendered_pages = 0;
    record(out, esp_gsp_page_flow_set_page(ui, GSP_MAGPIE_OBJ_KEY_PAGES, 0,
                                           false));
    render_dots(out);
    render_header(out);
    /* Pages are filled by pump(): GSP queues setters with bounded capacity and
     * an application must not submit a whole snapshot in one burst.
     * rendered_pages stays 0 so every authored page is written once. */
    out->pending_first = 0;
    out->pending_low = 0;
    out->pending_high = MAGPIE_UI_PAGE_MAX;
    out->pending_count = MAGPIE_UI_PAGE_MAX;
    out->rendered_pages = 0;
    out->dirty = true;
    return out->last_error;
}

void magpie_ui_deinit(magpie_ui_t *self)
{
    if (!self) {
        return;
    }
    self->ui = NULL;
    self->dirty = false;
}

uint8_t magpie_ui_page(const magpie_ui_t *self)
{
    if (!self) {
        return 0;
    }
    return self->page < self->state.pages_visible ? self->page : 0;
}

bool magpie_ui_dirty(const magpie_ui_t *self)
{
    return self && self->dirty;
}

void magpie_ui_show(magpie_ui_t *self, const magpie_state_t *state,
                    uint8_t page)
{
    if (!self || !self->ui || !state) {
        return;
    }
    const uint8_t previous_pages = self->rendered_pages;
    self->state = *state;
    /* Keep showing the page the user swiped to; the incoming snapshot carries
     * the store's page, which the UI already owns. */
    const uint8_t wanted = page < self->state.pages_visible ? page : 0;
    magpie_state_set_page(&self->state, wanted);
    if (wanted != self->page) {
        record(self, esp_gsp_page_flow_set_page(
                   self->ui, GSP_MAGPIE_OBJ_KEY_PAGES, wanted, true));
        self->page = wanted;
    }
    /* Header first: it carries the link state. */
    render_header(self);
    render_dots(self);
    /* Queue the pages around the visible one. Everything else is rendered
     * lazily when the user swipes to it, so a 16-subscription snapshot does
     * not submit hundreds of setters per refresh. */
    self->pending_first = 0;
    const uint8_t low = self->page > 0 ? (uint8_t)(self->page - 1) : 0;
    uint8_t high = self->page + 2;
    if (high > self->state.pages_visible) {
        high = (uint8_t)self->state.pages_visible;
    }
    if (high < previous_pages && previous_pages > high) {
        /* Pages that just fell out of range still need their stale text
         * cleared once. */
        self->pending_low = high;
        self->pending_high = previous_pages;
        self->pending_count = previous_pages;
    } else {
        self->pending_low = low;
        self->pending_high = high;
        self->pending_count = high;
    }
    self->rendered_pages = (uint8_t)self->state.pages_visible;
    self->dirty = self->pending_first < self->pending_count;
    magpie_ui_pump(self);
}

void magpie_ui_pump(magpie_ui_t *self)
{
    if (!self || !self->ui) {
        return;
    }
    /* Follow the user's swipe first so the pager stays authoritative. */
    (void)magpie_ui_sync_page(self);
    if (!self->dirty) {
        return;
    }
    uint8_t applied = 0;
    while (applied < MAGPIE_UI_PAGES_PER_PUMP &&
           self->pending_first < self->pending_count) {
        const uint8_t slot = self->pending_first++;
        ++applied;
        if (slot < self->pending_low || slot >= self->pending_high) {
            /* Slots outside the range are not rendered; the loop keeps the
             * index monotonic so the budget stays predictable. */
            continue;
        }
        render_page(self, slot);
    }
    self->dirty = self->pending_first < self->pending_count;
}

void magpie_ui_refresh_status(magpie_ui_t *self)
{
    if (!self || !self->ui) {
        return;
    }
    (void)magpie_ui_sync_page(self);
    render_header(self);
}

bool magpie_ui_sync_page(magpie_ui_t *self)
{
    if (!self || !self->ui) {
        return false;
    }
    uint16_t page = 0;
    if (esp_gsp_page_flow_get_page(self->ui, GSP_MAGPIE_OBJ_KEY_PAGES, &page) !=
        ESP_GSP_OK) {
        return false;
    }
    const uint8_t limit = self->state.pages_visible > 1
                              ? (uint8_t)(self->state.pages_visible - 1)
                              : 0;
    const uint8_t shown = page > limit ? limit : (uint8_t)page;
    if (shown == self->page) {
        return false;
    }
    /* A swipe past the last reported subscription snaps back to a live page,
     * so an empty trailing page never becomes the resting state. */
    self->page = shown;
    /* The header text is derived from the state copy, so keep it in sync. */
    magpie_state_set_page(&self->state, shown);
    if (page != shown) {
        record(self, esp_gsp_page_flow_set_page(self->ui,
                                               GSP_MAGPIE_OBJ_KEY_PAGES, shown,
                                               true));
    }
    render_dots(self);
    render_header(self);
    /* Fill in the neighbourhood of the page that just became visible. */
    const uint8_t low = shown > 0 ? (uint8_t)(shown - 1) : 0;
    uint8_t high = shown + 2;
    if (high > self->state.pages_visible) {
        high = (uint8_t)self->state.pages_visible;
    }
    self->pending_first = low;
    self->pending_low = low;
    self->pending_high = high;
    self->pending_count = high;
    self->dirty = low < high;
    return true;
}
