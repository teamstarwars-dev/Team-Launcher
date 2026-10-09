'use strict';
// API Team Launcher : comptes, amis, messages, signaling vocal, presence.
// Authentification : Bearer <jeton> (64 hexas). Toutes les reponses sont
// du JSON. Les evenements temps reel passent par long-polling :
// GET /api/events?since=<dernier id recu>, reponse sous 25 s max.
const express = require('express');
const helmet = require('helmet');
const rateLimit = require('express-rate-limit');
const bcrypt = require('bcryptjs');
const crypto = require('crypto');
const { pool } = require('./db');
const { createSession, requireAuth, hashToken } = require('./auth');

const app = express();
app.disable('x-powered-by');
app.use(helmet({ contentSecurityPolicy: false }));
app.use(express.json({ limit: '64kb' }));

// Le client C++ ne fait que GET/POST : DELETE via X-HTTP-Method-Override.
app.use((req, res, next) => {
    const override = req.get('X-HTTP-Method-Override');
    if (req.method === 'POST' && override && /^[A-Z]+$/.test(override)) {
        req.method = override;
    }
    next();
});

const authLimiter = rateLimit({ windowMs: 15 * 60 * 1000, max: 30 });
const msgLimiter = rateLimit({ windowMs: 60 * 1000, max: 60 });
const callLimiter = rateLimit({ windowMs: 60 * 1000, max: 20 });

// --- utilitaires -----------------------------------------------------------

const USERNAME_RE = /^[a-zA-Z0-9_.]{3,24}$/;

function bad(res, code, msg) {
    return res.status(code).json({ error: msg });
}

// Paire ordonnee pour friendships (contrainte user_a < user_b).
function orderedPair(a, b) {
    return a < b ? [a, b] : [b, a];
}

async function findUser(username) {
    const { rows } = await pool.query(
        'SELECT id, username FROM users WHERE username = $1',
        [username]
    );
    return rows[0] || null;
}

async function areFriends(a, b) {
    const [x, y] = orderedPair(a, b);
    const { rows } = await pool.query(
        "SELECT 1 FROM friendships WHERE user_a = $1 AND user_b = $2 AND status = 'accepted'",
        [x, y]
    );
    return rows.length > 0;
}

async function pushEvent(userId, type, payload) {
    const { rows } = await pool.query(
        'INSERT INTO events (user_id, type, payload) VALUES ($1, $2, $3) RETURNING id',
        [userId, type, payload || {}]
    );
    // Borne anti-fuite : 500 derniers evenements par utilisateur.
    await pool.query(
        `DELETE FROM events WHERE user_id = $1 AND id <= (
             SELECT COALESCE(MAX(id) - 500, 0) FROM events WHERE user_id = $1
         )`,
        [userId]
    );
    return rows[0].id;
}

function publicUser(row) {
    return { id: row.id, username: row.username };
}

// --- sante -----------------------------------------------------------------

app.get('/api/health', async (req, res, next) => {
    try {
        await pool.query('SELECT 1');
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

// --- comptes ---------------------------------------------------------------

app.post('/api/accounts/register', authLimiter, async (req, res, next) => {
    try {
        const username = String(req.body.username || '').trim();
        const password = String(req.body.password || '');
        if (!USERNAME_RE.test(username)) {
            return bad(res, 400, 'pseudo invalide (3-24 : lettres, chiffres, _ et .)');
        }
        if (password.length < 8 || password.length > 128) {
            return bad(res, 400, 'mot de passe : 8 caracteres minimum');
        }
        if (await findUser(username)) {
            return bad(res, 409, 'pseudo deja pris');
        }
        const passHash = await bcrypt.hash(password, 10);
        const { rows } = await pool.query(
            'INSERT INTO users (username, pass_hash) VALUES ($1, $2) RETURNING id, username',
            [username, passHash]
        );
        const token = await createSession(rows[0].id);
        res.status(201).json({ token, user: publicUser(rows[0]) });
    } catch (e) {
        next(e);
    }
});

app.post('/api/accounts/login', authLimiter, async (req, res, next) => {
    try {
        const username = String(req.body.username || '').trim();
        const password = String(req.body.password || '');
        const { rows } = await pool.query(
            'SELECT id, username, pass_hash FROM users WHERE username = $1',
            [username]
        );
        if (rows.length === 0) return bad(res, 401, 'identifiants invalides');
        const ok = await bcrypt.compare(password, rows[0].pass_hash);
        if (!ok) return bad(res, 401, 'identifiants invalides');
        const token = await createSession(rows[0].id);
        res.json({ token, user: publicUser(rows[0]) });
    } catch (e) {
        next(e);
    }
});

app.get('/api/me', requireAuth, (req, res) => {
    res.json({ user: publicUser(req.user) });
});

app.post('/api/accounts/logout', requireAuth, async (req, res, next) => {
    try {
        const header = req.get('Authorization') || '';
        const token = header.replace(/^Bearer /i, '').trim().toLowerCase();
        await pool.query('DELETE FROM sessions WHERE token_hash = $1', [
            hashToken(token),
        ]);
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

// --- presence ----------------------------------------------------------------
// En ligne = statut <> 'offline' ET activite vue il y a moins de 2 minutes
// (last_seen rafraichi a chaque requete authentifiee).
app.post('/api/presence', requireAuth, async (req, res, next) => {
    try {
        const status = String(req.body.status || 'online');
        if (!['online', 'busy', 'offline'].includes(status)) {
            return bad(res, 400, 'statut inconnu');
        }
        await pool.query('UPDATE users SET status = $1 WHERE id = $2', [
            status,
            req.user.id,
        ]);
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

// --- amis --------------------------------------------------------------------

app.get('/api/friends', requireAuth, async (req, res, next) => {
    try {
        const me = req.user.id;
        const { rows } = await pool.query(
            `SELECT u.id, u.username, u.status,
                    (SELECT MAX(s.last_seen) FROM sessions s WHERE s.user_id = u.id) AS seen,
                    f.status AS rel, f.requested_by
             FROM friendships f
             JOIN users u ON u.id = CASE WHEN f.user_a = $1 THEN f.user_b ELSE f.user_a END
             WHERE (f.user_a = $1 OR f.user_b = $1) AND f.status IN ('pending', 'accepted')`,
            [me]
        );
        const friends = [];
        const pendingIn = [];
        const pendingOut = [];
        for (const r of rows) {
            const online =
                r.status !== 'offline' &&
                r.seen &&
                Date.now() - new Date(r.seen).getTime() < 120000;
            if (r.rel === 'accepted') {
                friends.push({ username: r.username, online: !!online });
            } else if (r.requested_by === me) {
                pendingOut.push(r.username);
            } else {
                pendingIn.push(r.username);
            }
        }
        res.json({ friends, pendingIn, pendingOut });
    } catch (e) {
        next(e);
    }
});

app.post('/api/friends/request', requireAuth, async (req, res, next) => {
    try {
        const name = String(req.body.username || '').trim();
        const other = await findUser(name);
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        if (other.id === req.user.id) return bad(res, 400, 'impossible avec soi-meme');
        const [a, b] = orderedPair(req.user.id, other.id);
        const { rows } = await pool.query(
            'SELECT status FROM friendships WHERE user_a = $1 AND user_b = $2',
            [a, b]
        );
        if (rows.length > 0) return bad(res, 409, 'demande deja en cours ou deja amis');
        await pool.query(
            `INSERT INTO friendships (user_a, user_b, status, requested_by)
             VALUES ($1, $2, 'pending', $3)`,
            [a, b, req.user.id]
        );
        await pushEvent(other.id, 'friend.request', {
            from: publicUser(req.user),
        });
        res.status(201).json({ ok: true });
    } catch (e) {
        next(e);
    }
});

app.post('/api/friends/accept', requireAuth, async (req, res, next) => {
    try {
        const name = String(req.body.username || '').trim();
        const other = await findUser(name);
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        const [a, b] = orderedPair(req.user.id, other.id);
        const { rowCount } = await pool.query(
            `UPDATE friendships SET status = 'accepted'
             WHERE user_a = $1 AND user_b = $2 AND status = 'pending'
               AND requested_by = $3`,
            [a, b, other.id]
        );
        if (rowCount === 0) return bad(res, 404, 'aucune demande a accepter');
        await pushEvent(other.id, 'friend.accepted', {
            by: publicUser(req.user),
        });
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

app.post('/api/friends/decline', requireAuth, async (req, res, next) => {
    try {
        const name = String(req.body.username || '').trim();
        const other = await findUser(name);
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        const [a, b] = orderedPair(req.user.id, other.id);
        await pool.query(
            `DELETE FROM friendships
             WHERE user_a = $1 AND user_b = $2 AND status = 'pending'`,
            [a, b]
        );
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

app.delete('/api/friends/:username', requireAuth, async (req, res, next) => {
    try {
        const other = await findUser(String(req.params.username || ''));
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        const [a, b] = orderedPair(req.user.id, other.id);
        await pool.query(
            'DELETE FROM friendships WHERE user_a = $1 AND user_b = $2',
            [a, b]
        );
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

app.post('/api/friends/block', requireAuth, async (req, res, next) => {
    try {
        const name = String(req.body.username || '').trim();
        const other = await findUser(name);
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        if (other.id === req.user.id) return bad(res, 400, 'impossible avec soi-meme');
        const [a, b] = orderedPair(req.user.id, other.id);
        await pool.query(
            `INSERT INTO friendships (user_a, user_b, status, requested_by)
             VALUES ($1, $2, 'blocked', $3)
             ON CONFLICT (user_a, user_b)
             DO UPDATE SET status = 'blocked', requested_by = $3`,
            [a, b, req.user.id]
        );
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
});

// --- messages ------------------------------------------------------------------

app.get('/api/messages/:username', requireAuth, async (req, res, next) => {
    try {
        const other = await findUser(String(req.params.username || ''));
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        if (!(await areFriends(req.user.id, other.id))) {
            return bad(res, 403, 'conversation reservee aux amis');
        }
        const limit = Math.min(Math.max(parseInt(req.query.limit, 10) || 50, 1), 100);
        const before = parseInt(req.query.before, 10) || 0;
        const params = [req.user.id, other.id];
        let beforeClause = '';
        if (before > 0) {
            beforeClause = 'AND m.id < $3';
            params.push(before);
        }
        const { rows } = await pool.query(
            `SELECT m.id, m.sender, m.body, m.created_at, u.username AS sender_name
             FROM messages m JOIN users u ON u.id = m.sender
             WHERE ((m.sender = $1 AND m.recipient = $2)
                 OR (m.sender = $2 AND m.recipient = $1)) ${beforeClause}
             ORDER BY m.id DESC LIMIT ${limit}`,
            params
        );
        res.json({
            messages: rows.reverse().map((m) => ({
                id: m.id,
                mine: m.sender === req.user.id,
                sender: m.sender_name,
                text: m.body,
                at: m.created_at,
            })),
        });
    } catch (e) {
        next(e);
    }
});

app.post('/api/messages', requireAuth, msgLimiter, async (req, res, next) => {
    try {
        const name = String(req.body.to || '').trim();
        const text = String(req.body.text || '');
        if (text.length < 1 || text.length > 2000) {
            return bad(res, 400, 'message : 1 a 2000 caracteres');
        }
        const other = await findUser(name);
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        if (!(await areFriends(req.user.id, other.id))) {
            return bad(res, 403, 'envoi reserve aux amis');
        }
        const { rows } = await pool.query(
            'INSERT INTO messages (sender, recipient, body) VALUES ($1, $2, $3) RETURNING id, created_at',
            [req.user.id, other.id, text]
        );
        await pushEvent(other.id, 'message.new', {
            message: {
                id: rows[0].id,
                sender: req.user.username,
                text,
                at: rows[0].created_at,
            },
        });
        res.status(201).json({ id: rows[0].id, at: rows[0].created_at });
    } catch (e) {
        next(e);
    }
});

// --- appels (signaling ; l'audio reste sur le lobby Discord) --------------------

app.get('/api/calls/active', requireAuth, async (req, res, next) => {
    try {
        const { rows } = await pool.query(
            `SELECT c.id, c.status, c.lobby_id,
                    me.username AS peer, me.id AS peer_id,
                    (c.caller = $1) AS outgoing
             FROM calls c JOIN users me ON me.id = CASE WHEN c.caller = $1 THEN c.callee ELSE c.caller END
             WHERE (c.caller = $1 OR c.callee = $1) AND c.status IN ('ringing', 'active')
             ORDER BY c.created_at DESC LIMIT 1`,
            [req.user.id]
        );
        if (rows.length === 0) return res.json({ call: null });
        const c = rows[0];
        res.json({
            call: {
                id: c.id,
                status: c.status,
                lobbyId: c.lobby_id,
                peer: c.peer,
                peerId: c.peer_id,
                outgoing: c.outgoing,
            },
        });
    } catch (e) {
        next(e);
    }
});

app.post('/api/calls/invite', requireAuth, callLimiter, async (req, res, next) => {
    try {
        const name = String(req.body.to || '').trim();
        const lobbyId = String(req.body.lobbyId || '');
        if (lobbyId.length < 4 || lobbyId.length > 128) {
            return bad(res, 400, 'lobby invalide');
        }
        const other = await findUser(name);
        if (!other) return bad(res, 404, 'utilisateur introuvable');
        if (!(await areFriends(req.user.id, other.id))) {
            return bad(res, 403, 'appel reserve aux amis');
        }
        // Un seul appel vivant a la fois par utilisateur.
        await pool.query(
            `UPDATE calls SET status = 'ended', updated_at = now()
             WHERE (caller = $1 OR callee = $1) AND status IN ('ringing', 'active')`,
            [req.user.id]
        );
        const callId = crypto.randomBytes(8).toString('hex');
        await pool.query(
            `INSERT INTO calls (id, caller, callee, lobby_id, status)
             VALUES ($1, $2, $3, $4, 'ringing')`,
            [callId, req.user.id, other.id, lobbyId]
        );
        await pushEvent(other.id, 'call.invite', {
            callId,
            from: publicUser(req.user),
            lobbyId,
        });
        res.status(201).json({ callId });
    } catch (e) {
        next(e);
    }
});

async function callAction(req, res, next, allowedStatuses, onlyCallee, nextStatus, eventType) {
    try {
        const { rows } = await pool.query('SELECT * FROM calls WHERE id = $1', [
            String(req.params.id || ''),
        ]);
        if (rows.length === 0) return bad(res, 404, 'appel introuvable');
        const call = rows[0];
        const me = req.user.id;
        const isCaller = call.caller === me;
        const isCallee = call.callee === me;
        if (!isCaller && !isCallee) return bad(res, 403, 'pas concerne');
        if (!allowedStatuses.includes(call.status) || (onlyCallee && !isCallee)) {
            return bad(res, 409, 'transition impossible');
        }
        await pool.query(
            "UPDATE calls SET status = $1, updated_at = now() WHERE id = $2",
            [nextStatus, call.id]
        );
        const otherId = isCaller ? call.callee : call.caller;
        await pushEvent(otherId, eventType, {
            callId: call.id,
            by: publicUser(req.user),
            lobbyId: call.lobby_id,
        });
        res.json({ ok: true });
    } catch (e) {
        next(e);
    }
}

app.post('/api/calls/:id/accept', requireAuth, (req, res, next) =>
    callAction(req, res, next, ['ringing'], true, 'active', 'call.accept')
);

app.post('/api/calls/:id/decline', requireAuth, (req, res, next) =>
    callAction(req, res, next, ['ringing'], true, 'ended', 'call.decline')
);

app.post('/api/calls/:id/hangup', requireAuth, (req, res, next) =>
    callAction(req, res, next, ['ringing', 'active'], false, 'ended', 'call.hangup')
);

// --- evenements (long-polling) ---------------------------------------------------
// GET /api/events?since=<id> : renvoie les evenements plus recents, en
// attendant jusqu'a ~25 s s'il n'y en a pas. Les evenements renvoyes sont
// supprimes (distribution unique).
app.get('/api/events', requireAuth, async (req, res, next) => {
    try {
        const since = parseInt(req.query.since, 10) || 0;
        const deadline = Date.now() + 25000;
        let closed = false;
        req.on('close', () => {
            closed = true;
        });
        for (;;) {
            const { rows } = await pool.query(
                'SELECT id, type, payload, created_at FROM events WHERE user_id = $1 AND id > $2 ORDER BY id ASC LIMIT 100',
                [req.user.id, since]
            );
            if (rows.length > 0 || Date.now() >= deadline || closed) {
                if (rows.length > 0 && !closed) {
                    const maxId = rows[rows.length - 1].id;
                    await pool.query(
                        'DELETE FROM events WHERE user_id = $1 AND id <= $2',
                        [req.user.id, maxId]
                    );
                }
                if (!closed) {
                    res.json({
                        events: rows.map((e) => ({
                            id: e.id,
                            type: e.type,
                            data: e.payload,
                            at: e.created_at,
                        })),
                    });
                }
                return;
            }
            await new Promise((r) => setTimeout(r, 1000));
        }
    } catch (e) {
        if (!res.headersSent) next(e);
    }
});

// --- admin ---------------------------------------------------------------------

app.get('/api/admin/stats', async (req, res, next) => {
    try {
        const token = req.get('X-Admin-Token') || req.query.token || '';
        if (!process.env.ADMIN_TOKEN || token !== process.env.ADMIN_TOKEN) {
            return bad(res, 403, 'interdit');
        }
        const [[users], [sess], [msgs], [calls]] = await Promise.all([
            pool.query('SELECT COUNT(*)::int AS n FROM users'),
            pool.query('SELECT COUNT(*)::int AS n FROM sessions'),
            pool.query('SELECT COUNT(*)::int AS n FROM messages'),
            pool.query("SELECT COUNT(*)::int AS n FROM calls WHERE status IN ('ringing','active')"),
        ]);
        res.json({
            users: users.rows[0].n,
            sessions: sess.rows[0].n,
            messages: msgs.rows[0].n,
            liveCalls: calls.rows[0].n,
        });
    } catch (e) {
        next(e);
    }
});

// --- erreurs ---------------------------------------------------------------------

app.use((req, res) => {
    res.status(404).json({ error: 'introuvable' });
});

// eslint-disable-next-line no-unused-vars
app.use((err, req, res, next) => {
    // JSON malforme (curl Windows et ses guillemets, client casse...) :
    // 400 lisible, pas 500.
    if (err instanceof SyntaxError && err.status === 400) {
        return res.status(400).json({ error: 'json invalide' });
    }
    console.error('[api]', err && err.message ? err.message : err);
    res.status(500).json({ error: 'erreur interne' });
});

module.exports = app;
