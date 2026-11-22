/* bterror.h — BrainTerm error domain (GError out-param convention). */
#ifndef BT_ERROR_H
#define BT_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

#define BT_ERROR (bt_error_quark())

typedef enum {
  BT_ERROR_PATH_REJECTED, /* vault_resolve() refused the path (§6.A4) */
  BT_ERROR_IO,            /* filesystem failure */
  BT_ERROR_NOT_FOUND,     /* vault root or file missing (R8) */
  BT_ERROR_ENCODING,      /* invalid UTF-8 or unrepresentable name */
  BT_ERROR_INVALID        /* bad argument */
} BtErrorCode;

GQuark bt_error_quark(void);

G_END_DECLS

#endif
