/**
 * mk-proxy: Cloudflare Worker for microkernel cloud storage.
 *
 * Accepts WebSocket connections at /ws, authenticates with a pre-shared
 * token, then proxies KV operations with server-side key prefixing.
 *
 * Wire protocol: newline-delimited JSON over WebSocket.
 */

interface Env {
  KV: KVNamespace;
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
