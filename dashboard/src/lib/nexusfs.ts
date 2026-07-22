import "server-only";

export interface Peer {
  node_id: string;
  address: string;
  port: number;
  state: string;
  last_seen_unix_ms: number;
  consecutive_failures: number;
  last_error: string;
}

export interface Overview {
  service: {
    name: string;
    api_version: string;
    status: string;
    storage_root: string;
    chunk_size: number;
    uptime_milliseconds: number;
  };
  security: {
    peer_request_signing: boolean;
    admin_authentication: boolean;
    peer_requests_accepted: number;
    peer_requests_rejected: number;
    peer_replays_rejected: number;
    admin_requests_accepted: number;
    admin_requests_rejected: number;
  };
  storage: {
    manifests: number;
    complete_manifests: number;
    incomplete_manifests: number;
    logical_bytes: number;
    chunk_references: number;
    missing_chunks: number;
  };
  cluster: {
    enabled: boolean;
    cluster_id?: string;
    node_id?: string;
    membership_epoch?: number;
    replication_factor?: number;
    strict_replication?: boolean;
    configured_peers?: number;
    healthy_peers?: number;
    suspect_peers?: number;
    unavailable_peers?: number;
    unknown_peers?: number;
    peers?: Peer[];
  };
  http: {
    requests_total: number;
    requests_in_flight: number;
    requests_succeeded: number;
    requests_failed: number;
    connections_active: number;
  };
  operations: {
    catalog_sync_runs: number;
    repair_runs: number;
    rebalance_runs: number;
    rebalance_replays: number;
    rebalance_stale_epoch: number;
  };
}

export interface StoredFile {
  manifest_id: string;
  original_filename: string;
  file_size: number;
  chunk_size: number;
  chunk_count: number;
  missing_chunks: number;
  complete: boolean;
}

export interface FileCatalog {
  count: number;
  complete_manifests: number;
  incomplete_manifests: number;
  files: StoredFile[];
}

export type Operation = "catalog-sync" | "repair" | "maintenance" | "rebalance";

const defaultBaseUrl = "http://127.0.0.1:8080";

function config() {
  const token = process.env.NEXUSFS_ADMIN_TOKEN?.trim();

  if (!token) {
    throw new Error(
      "NEXUSFS_ADMIN_TOKEN is missing. Configure dashboard/.env.local.",
    );
  }

  return {
    token,
    baseUrl: (
      process.env.NEXUSFS_API_BASE_URL?.trim() || defaultBaseUrl
    ).replace(/\/+$/, ""),
  };
}

async function requestJson<T>(
  path: string,
  init: RequestInit = {},
): Promise<T> {
  const { token, baseUrl } = config();
  const headers = new Headers(init.headers);

  headers.set("Accept", "application/json");
  headers.set("Authorization", `Bearer ${token}`);

  if (init.body !== undefined) {
    headers.set("Content-Type", "application/json");
  }

  let response: Response;

  try {
    response = await fetch(`${baseUrl}${path}`, {
      ...init,
      headers,
      cache: "no-store",
      signal: AbortSignal.timeout(10_000),
    });
  } catch (error: unknown) {
    const detail = error instanceof Error ? error.message : "Unknown error.";
    throw new Error(`Could not reach NexusFS at ${baseUrl}. ${detail}`);
  }

  const text = await response.text();
  let payload: unknown = {};

  if (text.trim()) {
    try {
      payload = JSON.parse(text) as unknown;
    } catch {
      payload = { message: text };
    }
  }

  if (!response.ok) {
    const object =
      typeof payload === "object" && payload !== null
        ? (payload as { error?: { message?: string }; message?: string })
        : {};

    throw new Error(
      object.error?.message ??
        object.message ??
        `NexusFS returned HTTP ${response.status}.`,
    );
  }

  return payload as T;
}

export const getOverview = () =>
  requestJson<Overview>("/api/v1/admin/overview");

export const getFiles = () =>
  requestJson<FileCatalog>("/api/v1/admin/files");

export const runOperation = (
  operation: Operation,
  body: Record<string, unknown> = {},
) =>
  requestJson<unknown>(`/api/v1/admin/operations/${operation}`, {
    method: "POST",
    body: JSON.stringify(body),
  });

export function formatBytes(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return "0 B";

  const units = ["B", "KB", "MB", "GB", "TB"];
  const index = Math.min(
    Math.floor(Math.log(value) / Math.log(1024)),
    units.length - 1,
  );
  const scaled = value / 1024 ** index;

  return `${scaled.toFixed(index === 0 || scaled >= 100 ? 0 : 1)} ${units[index]}`;
}

export function formatDuration(milliseconds: number): string {
  const seconds = Math.max(0, Math.floor(milliseconds / 1000));
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m`;
  return `${seconds}s`;
}

export function shortId(value?: string): string {
  if (!value) return "—";
  return value.length <= 18
    ? value
    : `${value.slice(0, 8)}…${value.slice(-8)}`;
}
