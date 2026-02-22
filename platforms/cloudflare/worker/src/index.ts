/**
 * mk-proxy: Cloudflare Worker for microkernel cloud services.
 *
 * Accepts WebSocket connections at /ws, authenticates with a pre-shared
 * token, then proxies KV, D1, Queue, and AI operations with server-side
 * key prefixing.
 *
 * Wire protocol: newline-delimited JSON over WebSocket.
 */

interface Env {
  KV: KVNamespace;
  DB: D1Database;
  QUEUE?: Queue;   /* optional — uncomment in wrangler.toml after creating */
  AI: Ai;
  AUTH_TOKEN: string;
}

interface Session {
  ws: WebSocket;
  nodeId: string;
  authed: boolean;
  pingInterval: ReturnType<typeof setInterval> | null;
}

/* ── Key prefixing ────────────────────────────────────────────────── */

function prefixKey(nodeId: string, key: string): string {
  return `${nodeId}/${key}`;
}

function stripPrefix(nodeId: string, key: string): string {
  const prefix = `${nodeId}/`;
  return key.startsWith(prefix) ? key.slice(prefix.length) : key;
}

/* ── KV operations ────────────────────────────────────────────────── */

async function kvGet(
  env: Env, session: Session, reqId: number, key: string
): Promise<string> {
  const fullKey = prefixKey(session.nodeId, key);
  const value = await env.KV.get(fullKey);
  if (value === null) {
    return JSON.stringify({ req_id: reqId, type: "not_found" });
  }
  return JSON.stringify({ req_id: reqId, type: "value", value });
}

async function kvPut(
  env: Env, session: Session, reqId: number,
  key: string, value: string, ttl?: number
): Promise<string> {
  const fullKey = prefixKey(session.nodeId, key);
  const opts: KVNamespacePutOptions = {};
  if (ttl && ttl > 0) opts.expirationTtl = ttl;
  await env.KV.put(fullKey, value, opts);
  return JSON.stringify({ req_id: reqId, type: "ok" });
}

async function kvDelete(
  env: Env, session: Session, reqId: number, key: string
): Promise<string> {
  const fullKey = prefixKey(session.nodeId, key);
  await env.KV.delete(fullKey);
  return JSON.stringify({ req_id: reqId, type: "ok" });
}

async function kvList(
  env: Env, session: Session, reqId: number,
  prefix: string, limit: number
): Promise<string> {
  const fullPrefix = prefixKey(session.nodeId, prefix);
  const result = await env.KV.list({ prefix: fullPrefix, limit });
  const keys = result.keys.map((k) => stripPrefix(session.nodeId, k.name));
  return JSON.stringify({ req_id: reqId, type: "keys", keys });
}

/* ── D1 operations ────────────────────────────────────────────────── */

async function dbQuery(
  env: Env, session: Session, reqId: number,
  sql: string, params: string[]
): Promise<string> {
  const stmt = env.DB.prepare(sql).bind(...params);
  const result = await stmt.all();
  return JSON.stringify({
    req_id: reqId, type: "db_query_ok", rows: result.results
  });
}

async function dbExec(
  env: Env, session: Session, reqId: number,
  sql: string, params: string[]
): Promise<string> {
  const stmt = env.DB.prepare(sql).bind(...params);
  const result = await stmt.run();
  return JSON.stringify({
    req_id: reqId, type: "db_exec_ok",
    rows_affected: result.meta.changes
  });
}

/* ── Queue operations ─────────────────────────────────────────────── */

async function queuePush(
  env: Env, session: Session, reqId: number,
  body: string, delaySeconds: number
): Promise<string> {
  if (!env.QUEUE) {
    return JSON.stringify({
      req_id: reqId, type: "error",
      message: "queue not configured"
    });
  }
  const message = {
    node_id: session.nodeId,
    body,
    ts: new Date().toISOString(),
  };
  await env.QUEUE.send(message, {
    delaySeconds: delaySeconds > 0 ? delaySeconds : undefined
  } as any);
  return JSON.stringify({ req_id: reqId, type: "queue_push_ok" });
}

/* ── AI operations ────────────────────────────────────────────────── */

async function aiInfer(
  env: Env, reqId: number,
  model: string, prompt: string
): Promise<string> {
  const result = await env.AI.run(
    model || "@cf/meta/llama-3.1-8b-instruct",
    { prompt }
  ) as any;
  return JSON.stringify({
    req_id: reqId, type: "ai_infer_ok", result: result.response
  });
}

async function aiEmbed(
  env: Env, reqId: number,
  model: string, text: string
): Promise<string> {
  const result = await env.AI.run(
    model || "@cf/baai/bge-small-en-v1.5",
    { text: [text] }
  ) as any;
  return JSON.stringify({
    req_id: reqId, type: "ai_embed_ok", embedding: result.data[0]
  });
}

/* ── Message handler ──────────────────────────────────────────────── */

async function handleMessage(
  env: Env, session: Session, data: string
): Promise<string | null> {
  let msg: Record<string, unknown>;
  try {
    msg = JSON.parse(data);
  } catch {
    return JSON.stringify({ type: "error", message: "invalid JSON" });
  }

  const msgType = msg.type as string;

  /* Auth */
  if (msgType === "auth") {
    const token = msg.token as string;
    if (token !== env.AUTH_TOKEN) {
      return JSON.stringify({ type: "error", message: "auth failed" });
    }
    session.nodeId = msg.node_id as string;
    session.authed = true;
    return JSON.stringify({ type: "auth_ok", tenant: session.nodeId });
  }

  /* Require auth for everything else */
  if (!session.authed) {
    return JSON.stringify({ type: "error", message: "not authenticated" });
  }

  /* Pong */
  if (msgType === "pong") {
    return null; /* no response */
  }

  const reqId = msg.req_id as number;

  try {
    switch (msgType) {
      /* KV */
      case "kv_get":
        return await kvGet(env, session, reqId, msg.key as string);

      case "kv_put":
        return await kvPut(
          env, session, reqId,
          msg.key as string, msg.value as string,
          msg.ttl as number | undefined
        );

      case "kv_delete":
        return await kvDelete(env, session, reqId, msg.key as string);

      case "kv_list":
        return await kvList(
          env, session, reqId,
          (msg.prefix as string) || "",
          (msg.limit as number) || 100
        );

      /* D1 */
      case "db_query":
        return await dbQuery(
          env, session, reqId,
          msg.sql as string, (msg.params as string[]) || []
        );

      case "db_exec":
        return await dbExec(
          env, session, reqId,
          msg.sql as string, (msg.params as string[]) || []
        );

      /* Queue */
      case "queue_push":
        return await queuePush(
          env, session, reqId,
          msg.body as string,
          (msg.delay_seconds as number) || 0
        );

      /* AI */
      case "ai_infer":
        return await aiInfer(
          env, reqId,
          msg.model as string, msg.prompt as string
        );

      case "ai_embed":
        return await aiEmbed(
          env, reqId,
          msg.model as string, msg.text as string
        );

      default:
        return JSON.stringify({
          req_id: reqId, type: "error",
          message: `unknown type: ${msgType}`
        });
    }
  } catch (err) {
    return JSON.stringify({
      req_id: reqId, type: "error",
      message: err instanceof Error ? err.message : "internal error"
    });
  }
}

/* ── WebSocket handler ────────────────────────────────────────────── */

function handleWebSocket(env: Env): Response {
  const [client, server] = Object.values(new WebSocketPair());

  const session: Session = {
    ws: server,
    nodeId: "",
    authed: false,
    pingInterval: null,
  };

  server.accept();

  /* Ping keepalive every 30s */
  session.pingInterval = setInterval(() => {
    try {
      server.send(JSON.stringify({ type: "ping" }));
    } catch {
      if (session.pingInterval) clearInterval(session.pingInterval);
    }
  }, 30000);

  server.addEventListener("message", async (event) => {
    const data = typeof event.data === "string"
      ? event.data
      : new TextDecoder().decode(event.data as ArrayBuffer);

    const response = await handleMessage(env, session, data);
    if (response) {
      try { server.send(response); } catch { /* closed */ }
    }
  });

  server.addEventListener("close", () => {
    if (session.pingInterval) clearInterval(session.pingInterval);
  });

  server.addEventListener("error", () => {
    if (session.pingInterval) clearInterval(session.pingInterval);
  });

  return new Response(null, { status: 101, webSocket: client });
}

/* ── Fetch handler ────────────────────────────────────────────────── */

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === "/ws") {
      if (request.headers.get("Upgrade") !== "websocket") {
        return new Response("Expected WebSocket", { status: 426 });
      }
      return handleWebSocket(env);
    }

    /* Health check */
    if (url.pathname === "/health") {
      return new Response("ok");
    }

    return new Response("Not Found", { status: 404 });
  },
};
