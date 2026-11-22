/* graphcapture.c — Single source of truth for deterministic graph capture states. */
#include "ui/graphcapture.h"

#include <string.h>

static const BtGraphCaptureSpec CAPTURE_STATES[] = {
  {
    .name = "default-fit",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_FIT,
    .interact = BT_CAP_INTERACT_NONE,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 8u,
  },
  {
    .name = "hover-hub",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_FIT,
    .interact = BT_CAP_INTERACT_HOVER_HUB,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 12u,
  },
  {
    .name = "hover-small",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_FIT,
    .interact = BT_CAP_INTERACT_HOVER_SMALL,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 12u,
  },
  {
    .name = "selected",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_FIT,
    .interact = BT_CAP_INTERACT_SELECT_HUB,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 10u,
  },
  {
    .name = "zoom2x",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_ZOOM2X_DENSEST,
    .interact = BT_CAP_INTERACT_NONE,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 8u,
  },
  {
    .name = "3d",
    .mode = BT_GRAPH_MODE_3D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_3D_ORBIT,
    .interact = BT_CAP_INTERACT_NONE,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 10u,
  },
  {
    .name = "reduce-motion-on",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_FIT,
    .interact = BT_CAP_INTERACT_NONE,
    .reduce_motion = TRUE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 6u,
  },
  {
    .name = "reduce-motion-off",
    .mode = BT_GRAPH_MODE_2D,
    .quality = BT_GRAPH_QUALITY_HIGH,
    .cam = BT_CAP_CAM_FIT,
    .interact = BT_CAP_INTERACT_NONE,
    .reduce_motion = FALSE,
    .show_tags = FALSE,
    .show_ghosts = FALSE,
    .settle_frames = 8u,
  },
};

const BtGraphCaptureSpec *bt_graph_capture_states(gsize *count_out)
{
  if (count_out != NULL)
    *count_out = G_N_ELEMENTS(CAPTURE_STATES);
  return CAPTURE_STATES;
}

const BtGraphCaptureSpec *bt_graph_capture_find(const gchar *name)
{
  if (name == NULL || name[0] == '\0')
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS(CAPTURE_STATES); i++) {
    if (g_ascii_strcasecmp(CAPTURE_STATES[i].name, name) == 0)
      return &CAPTURE_STATES[i];
  }
  return NULL;
}
