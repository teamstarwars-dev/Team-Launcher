-- Team Launcher API : schema Postgres.
-- Appliquer une fois : psql "$DATABASE_URL" -f db/schema.sql

CREATE EXTENSION IF NOT EXISTS citext;

CREATE TABLE IF NOT EXISTS users (
    id         SERIAL PRIMARY KEY,
    username   CITEXT NOT NULL UNIQUE,
    pass_hash  TEXT NOT NULL,
    status     TEXT NOT NULL DEFAULT 'online',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS sessions_user_idx ON sessions(user_id);

CREATE TABLE IF NOT EXISTS friendships (
    user_a       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    user_b       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    status       TEXT NOT NULL DEFAULT 'pending',
    requested_by INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (user_a, user_b),
    CHECK (user_a < user_b),
    CHECK (status IN ('pending', 'accepted', 'blocked'))
);
CREATE INDEX IF NOT EXISTS friendships_user_idx ON friendships(user_a, user_b);

CREATE TABLE IF NOT EXISTS messages (
    id         SERIAL PRIMARY KEY,
    sender     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    recipient  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    body       TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS messages_pair_idx ON messages(sender, recipient, id);

CREATE TABLE IF NOT EXISTS calls (
    id         TEXT PRIMARY KEY,
    caller     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    callee     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    lobby_id   TEXT NOT NULL,
    status     TEXT NOT NULL DEFAULT 'ringing',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (status IN ('ringing', 'active', 'ended'))
);
CREATE INDEX IF NOT EXISTS calls_peer_idx ON calls(caller, callee, status);

CREATE TABLE IF NOT EXISTS events (
    id         SERIAL PRIMARY KEY,
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    type       TEXT NOT NULL,
    payload    JSONB NOT NULL DEFAULT '{}',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS events_user_idx ON events(user_id, id);
