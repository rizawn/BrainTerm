/* mdparse.h — Markdown body extraction via MD4C (§5.2, §7.3, §11.1). */
#ifndef BT_MDPARSE_H
#define BT_MDPARSE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  gchar *first_h1;     /* NULL if no level-1 heading */
  GPtrArray *wikilinks;/* gchar* unique targets (never from code fences/spans) */
  GPtrArray *tags;     /* gchar* unique inline #tags (never from code) */
  gchar *plain_body;   /* plain text for FTS5 */
} BtMdExtract;

BtMdExtract *bt_mdparse_extract(const gchar *markdown, gsize len);
void bt_mdparse_free(BtMdExtract *ex);

G_END_DECLS

#endif
