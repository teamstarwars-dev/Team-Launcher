'use strict';
// Pool Postgres partage. DATABASE_URL obligatoire.
const { Pool } = require('pg');

if (!process.env.DATABASE_URL) {
    console.error('FATAL : DATABASE_URL manquante (voir .env.example).');
    process.exit(1);
}

const pool = new Pool({
    connectionString: process.env.DATABASE_URL,
    max: 10,
    idleTimeoutMillis: 30000,
});

pool.on('error', (err) => {
    console.error('[db] erreur inattendue du pool :', err.message);
});

module.exports = { pool };
