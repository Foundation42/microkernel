/**
 * Versioned D1 schema migrations for mk-proxy.
 *
 * Runs automatically on each WebSocket auth. Uses a _migrations table
 * to track applied versions; idempotent (CREATE TABLE IF NOT EXISTS).
 */

interface Migration {
    version: number;
    name: string;
    statements: string[];
}

const MIGRATIONS: Migration[] = [
    {
        version: 1,
        name: "create_logs_and_config",
        statements: [
            `CREATE TABLE IF NOT EXISTS logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts TEXT NOT NULL DEFAULT (datetime('now')),
                node_id TEXT NOT NULL DEFAULT '',
                msg TEXT NOT NULL DEFAULT ''
            )`,
            `CREATE TABLE IF NOT EXISTS config (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL DEFAULT '',
                updated_at TEXT NOT NULL DEFAULT (datetime('now'))
            )`
        ]
    }
];

export async function ensureMigrations(db: D1Database): Promise<void> {
    // 1. Bootstrap tracker
    await db.exec(`CREATE TABLE IF NOT EXISTS _migrations (
        version INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        applied TEXT NOT NULL DEFAULT (datetime('now'))
    )`);

    // 2. Get current version
    const row = await db.prepare("SELECT MAX(version) as v FROM _migrations").first();
    const current = (row?.v as number) ?? 0;

    // 3. Apply pending
    for (const m of MIGRATIONS) {
        if (m.version > current) {
            for (const stmt of m.statements) {
                await db.exec(stmt);
            }
            await db.prepare("INSERT INTO _migrations (version, name) VALUES (?, ?)")
                .bind(m.version, m.name).run();
        }
    }
}
