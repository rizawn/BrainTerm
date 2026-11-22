CREATE TABLE notes(
  id TEXT PRIMARY KEY,
  path TEXT UNIQUE NOT NULL,
  title TEXT NOT NULL DEFAULT '',
  body TEXT NOT NULL DEFAULT '',
  created INTEGER NOT NULL,
  updated INTEGER NOT NULL,
  source TEXT NOT NULL CHECK(source IN ('human','ai','import')),
  agent TEXT,
  fm_hash TEXT NOT NULL,
  body_hash TEXT NOT NULL,
  parse_error INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE tags(
  note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
  tag TEXT NOT NULL,
  origin TEXT NOT NULL CHECK(origin IN ('human','ai')),
  confidence REAL,
  PRIMARY KEY(note_id, tag, origin)
);
CREATE INDEX idx_tags_tag ON tags(tag);

CREATE TABLE edges(
  src TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
  dst TEXT NOT NULL,
  kind TEXT NOT NULL CHECK(kind IN ('link','similar','temporal','tagged')),
  weight REAL NOT NULL DEFAULT 1.0,
  PRIMARY KEY(src, dst, kind)
);
CREATE INDEX idx_edges_dst ON edges(dst, kind);

CREATE TABLE sessions(
  id TEXT PRIMARY KEY,
  tool TEXT NOT NULL,
  cwd TEXT NOT NULL,
  started INTEGER NOT NULL,
  ended INTEGER,
  resume_cmd TEXT,
  note_id TEXT REFERENCES notes(id) ON DELETE SET NULL
);

CREATE TABLE embeddings(
  note_id TEXT NOT NULL REFERENCES notes(id) ON DELETE CASCADE,
  chunk INTEGER NOT NULL,
  vec BLOB NOT NULL,
  PRIMARY KEY(note_id, chunk)
);

CREATE VIRTUAL TABLE notes_fts USING fts5(
  title, body, content='notes', content_rowid='rowid', tokenize='unicode61'
);

CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN
  INSERT INTO notes_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);
END;

CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN
  INSERT INTO notes_fts(notes_fts, rowid, title, body) VALUES('delete', old.rowid, old.title, old.body);
END;

CREATE TRIGGER notes_au AFTER UPDATE ON notes BEGIN
  INSERT INTO notes_fts(notes_fts, rowid, title, body) VALUES('delete', old.rowid, old.title, old.body);
  INSERT INTO notes_fts(rowid, title, body) VALUES (new.rowid, new.title, new.body);
END;
