-- 0002_links_resolution.sql: persisted link resolutions derived from notes + BtIndexSnapshot (L1).
CREATE TABLE link_resolutions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  src_note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
  src_path TEXT NOT NULL,
  raw_target TEXT NOT NULL,
  resolved_path TEXT,
  status TEXT NOT NULL CHECK(status IN ('resolved','ambiguous','unresolved','external','blocked')),
  kind TEXT NOT NULL CHECK(kind IN ('note','file','dir','url','same_note')),
  line_no INTEGER NOT NULL DEFAULT 1
);

CREATE INDEX idx_link_res_src ON link_resolutions(src_note_id);
CREATE INDEX idx_link_res_status ON link_resolutions(status);
CREATE INDEX idx_link_res_resolved ON link_resolutions(resolved_path);
