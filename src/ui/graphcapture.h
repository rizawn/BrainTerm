/* graphcapture.h — Canonical table of deterministic Brain Graph capture states. */
#ifndef BT_GRAPHCAPTURE_H
#define BT_GRAPHCAPTURE_H

#include <glib.h>

#include "graph_state.h"

G_BEGIN_DECLS

typedef enum {
  BT_CAP_CAM_FIT = 0,
  BT_CAP_CAM_ZOOM2X_DENSEST,
  BT_CAP_CAM_3D_ORBIT
} BtGraphCaptureCam;

typedef enum {
  BT_CAP_INTERACT_NONE = 0,
  BT_CAP_INTERACT_HOVER_HUB,
  BT_CAP_INTERACT_HOVER_SMALL,
  BT_CAP_INTERACT_SELECT_HUB
} BtGraphCaptureInteract;

typedef struct {
  const gchar *name;
  BtGraphMode mode;
  BtGraphQuality quality;
  BtGraphCaptureCam cam;
  BtGraphCaptureInteract interact;
  gboolean reduce_motion;
  gboolean show_tags;
  gboolean show_ghosts;
  guint settle_frames;
} BtGraphCaptureSpec;

const BtGraphCaptureSpec *bt_graph_capture_states(gsize *count_out);
const BtGraphCaptureSpec *bt_graph_capture_find(const gchar *name);

G_END_DECLS

#endif
