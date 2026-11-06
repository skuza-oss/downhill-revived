-- The operator's switches (finding 161), in the same D1 database (MODERATION): changed by
-- tools/moderate.mjs through `wrangler d1 execute`, read by the Worker and the rooms
-- (src/guard.ts onlineState, kept 30 s per isolate). No deploy is needed to change them.
--
--   online           'off' refuses new rooms, joins and race starts (the kill switch); anything
--                    else, or no row, is on
--   online_message   what players read while online is off ('' or no row: src/guard.ts OFF_MESSAGE)
CREATE TABLE settings (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL,
  updated_at INTEGER NOT NULL       -- ms since the Unix epoch
);
