'use strict';
// Authentification par jeton Bearer. Le jeton en clair ne vit que cote
// client ; ici on ne stocke que son SHA-256.
const crypto = require('crypto');
const { pool } = require('./db');

function hashToken(token) {
    return crypto.createHash('sha256').update(token, 'utf8').digest('hex');
}

function newToken() {
    return crypto.randomBytes(32).toString('hex');
}

async function createSession(userId) {
    const token = newToken();
    await pool.query(
        'INSERT INTO sessions (token_hash, user_id) VALUES ($1, $2)',
        [hashToken(token), userId]
    );
    return token;
}

// Middleware : verifie le Bearer, attache req.user {id, username} et
// rafraichit last_seen (sert aussi de battement de presence).
async function requireAuth(req, res, next) {
    try {
        const header = req.get('Authorization') || '';
        const m = /^Bearer ([0-9a-f]{64})$/i.exec(header.trim());
        if (!m) return res.status(401).json({ error: 'authentification requise' });
        const { rows } = await pool.query(
            `SELECT u.id, u.username FROM sessions s
             JOIN users u ON u.id = s.user_id
             WHERE s.token_hash = $1`,
            [hashToken(m[1].toLowerCase())]
        );
        if (rows.length === 0) {
            return res.status(401).json({ error: 'session invalide' });
        }
        req.user = rows[0];
        pool.query('UPDATE sessions SET last_seen = now() WHERE token_hash = $1', [
            hashToken(m[1].toLowerCase()),
        ]).catch(() => {});
        next();
    } catch (e) {
        next(e);
    }
}

module.exports = { hashToken, newToken, createSession, requireAuth };
