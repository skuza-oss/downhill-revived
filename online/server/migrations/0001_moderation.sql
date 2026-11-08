-- The moderation log and the ban list (finding 161), in the D1 database bound as MODERATION.
-- Written by the Worker (src/guard.ts), read and changed by the operator's tools/moderate.mjs
-- through `wrangler d1 execute` under the operator's own Cloudflare login. No address is kept:
-- ip_hash is HMAC-SHA-256 of it with the IP_HASH_KEY secret.

-- One row per player who entered a room (a new member or a rejoin). Rows older than 14 days are
-- deleted every day (src/index.ts scheduled, src/guard.ts cleanup).
CREATE TABLE joins (
  id INTEGER PRIMARY KEY,
  at INTEGER NOT NULL,              -- ms since the Unix epoch
  room TEXT NOT NULL,               -- the join code
  member INTEGER NOT NULL,          -- the member id in that room
  kind TEXT NOT NULL,               -- host, guest or rejoin
  name TEXT NOT NULL,
  client_version TEXT NOT NULL,     -- hello.clientVersion
  country TEXT NOT NULL,            -- Cloudflare's two letters, '' unknown
  ip_hash TEXT NOT NULL,            -- '' when the connection had no address
  install_id TEXT                   -- hello.installId; NULL from releases that send none
);
CREATE INDEX joins_at ON joins (at);

-- Bans by address hash or install id, never by name (anyone can take any name). A ban with an
-- expiry is deleted the day after it ends; one without stays until it is lifted.
CREATE TABLE bans (
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL CHECK (kind IN ('ip', 'install')),
  value TEXT NOT NULL,
  created_at INTEGER NOT NULL,      -- ms since the Unix epoch
  expires_at INTEGER,               -- ms since the Unix epoch; NULL: until lifted
  reason TEXT NOT NULL DEFAULT '',  -- the operator's note
  name TEXT NOT NULL DEFAULT '',    -- the name in the join row the ban came from, for the operator
  UNIQUE (kind, value)
);
