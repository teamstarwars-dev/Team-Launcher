'use strict';
// Point d'entree : lit .env local (optionnel), demarre HTTP.
const path = require('path');
const fs = require('fs');

const envFile = path.join(__dirname, '..', '.env');
if (fs.existsSync(envFile)) {
    for (const line of fs.readFileSync(envFile, 'utf8').split('\n')) {
        const t = line.trim();
        if (!t || t.startsWith('#') || !t.includes('=')) continue;
        const i = t.indexOf('=');
        const k = t.slice(0, i).trim();
        const v = t.slice(i + 1).trim();
        if (k && !(k in process.env)) process.env[k] = v;
    }
}

const app = require('./app');

const port = parseInt(process.env.PORT, 10) || 3000;
app.listen(port, () => {
    console.log(`[api] ecoute sur :${port}`);
});
