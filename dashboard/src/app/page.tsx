import Link from "next/link";

import { runOperationAction } from "@/app/actions";
import {
  type FileCatalog,
  type Overview,
  formatBytes,
  formatDuration,
  getFiles,
  getOverview,
  shortId,
} from "@/lib/nexusfs";

export const dynamic = "force-dynamic";

type SearchParams = Promise<{
  status?: string;
  message?: string;
}>;

function Pill({
  label,
  tone,
}: {
  label: string;
  tone: "good" | "warn" | "bad" | "neutral";
}) {
  return (
    <span className={`pill pill-${tone}`}>
      <span />
      {label}
    </span>
  );
}

function Metric({
  label,
  value,
  detail,
  tone = "neutral",
}: {
  label: string;
  value: string;
  detail: string;
  tone?: "neutral" | "good" | "warn" | "bad";
}) {
  return (
    <article className={`metric metric-${tone}`}>
      <p>{label}</p>
      <strong>{value}</strong>
      <span>{detail}</span>
    </article>
  );
}

function OperationForm({
  operation,
  title,
  description,
  epoch,
  disabled,
}: {
  operation: "catalog-sync" | "repair" | "maintenance" | "rebalance";
  title: string;
  description: string;
  epoch: number;
  disabled: boolean;
}) {
  return (
    <form action={runOperationAction} className="operation">
      <input name="operation" type="hidden" value={operation} />
      {operation === "rebalance" && (
        <input name="membership_epoch" type="hidden" value={epoch} />
      )}

      <div>
        <h3>{title}</h3>
        <p>{description}</p>
      </div>

      <button disabled={disabled} type="submit">
        Run
      </button>
    </form>
  );
}

export default async function Home({
  searchParams,
}: {
  searchParams: SearchParams;
}) {
  const query = await searchParams;
  const [overviewResult, filesResult] = await Promise.allSettled([
    getOverview(),
    getFiles(),
  ]);

  const overview: Overview | null =
    overviewResult.status === "fulfilled" ? overviewResult.value : null;

  const files: FileCatalog | null =
    filesResult.status === "fulfilled" ? filesResult.value : null;

  const error =
    overviewResult.status === "rejected"
      ? String(overviewResult.reason)
      : filesResult.status === "rejected"
        ? String(filesResult.reason)
        : null;

  const cluster = overview?.cluster;
  const peers = cluster?.peers ?? [];
  const epoch = cluster?.membership_epoch ?? 0;
  const configured = cluster?.configured_peers ?? 0;
  const healthy = cluster?.healthy_peers ?? 0;
  const suspect = cluster?.suspect_peers ?? 0;
  const unavailable = cluster?.unavailable_peers ?? 0;
  const degraded = suspect + unavailable > 0;
  const standalone = !cluster?.enabled || configured === 0;
  const storageHealthy =
    (overview?.storage.missing_chunks ?? 0) === 0 &&
    (overview?.storage.incomplete_manifests ?? 0) === 0;

  const successfulRequests =
    overview?.http.requests_succeeded ?? 0;
  const failedRequests =
    overview?.http.requests_failed ?? 0;
  const completedRequests =
    successfulRequests + failedRequests;

  const successRate =
    completedRequests === 0
      ? 100
      : (successfulRequests / completedRequests) * 100;

  return (
    <div className="shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand-mark">N</div>
          <div>
            <strong>NexusFS</strong>
            <span>Control plane</span>
          </div>
        </div>

        <nav>
          <a href="#overview">Overview</a>
          <a href="#cluster">Cluster</a>
          <a href="#storage">Storage</a>
          <a href="#operations">Operations</a>
          <a href="#security">Security</a>
        </nav>

        <div className="connection">
          <span className={overview ? "dot online" : "dot offline"} />
          <div>
            <strong>{overview ? "Connected" : "Disconnected"}</strong>
            <span>{overview?.service.api_version ?? "Admin API"}</span>
          </div>
        </div>
      </aside>

      <main>
        <header className="topbar">
          <div>
            <p className="eyebrow">Distributed storage operations</p>
            <h1>NexusFS Dashboard</h1>
            <p className="subtitle">
              Monitor storage integrity, peer health, security and
              maintenance workflows.
            </p>
          </div>

          <Link className="button" href="/">
            Refresh
          </Link>
        </header>

        {query.message && (
          <section
            className={`message ${
              query.status === "success" ? "message-good" : "message-bad"
            }`}
          >
            {query.message}
          </section>
        )}

        {error && (
          <section className="message message-bad">
            <strong>Dashboard connection unavailable.</strong>
            <span>{error}</span>
            <span>
              Configure <code>dashboard/.env.local</code> and restart
              the dashboard.
            </span>
          </section>
        )}

        <section className="hero-grid" id="overview">
          <article className="hero">
            <div>
              <p className="eyebrow">System state</p>
              <h2>
                {overview ? "Storage service is online" : "Waiting for NexusFS"}
              </h2>
              <p>
                {overview
                  ? `Serving ${overview.storage.manifests} manifests from ${overview.service.storage_root}.`
                  : "The dashboard is ready for the authenticated administrator API."}
              </p>

              <div className="hero-meta">
                <span>
                  Uptime{" "}
                  {overview
                    ? formatDuration(overview.service.uptime_milliseconds)
                    : "—"}
                </span>
                <span>
                  Chunk size{" "}
                  {overview
                    ? formatBytes(overview.service.chunk_size)
                    : "—"}
                </span>
              </div>
            </div>

            <div className={overview ? "orb orb-good" : "orb orb-bad"}>
              {overview ? "✓" : "!"}
            </div>
          </article>

          <article className="health-card">
            <div className="heading">
              <div>
                <p className="eyebrow">Cluster health</p>
                <h2>
                  {standalone
                    ? "Standalone"
                    : degraded
                      ? "Degraded"
                      : "Healthy"}
                </h2>
              </div>
              <Pill
                label={
                  standalone
                    ? "Standalone"
                    : degraded
                      ? "Degraded"
                      : "Healthy"
                }
                tone={standalone ? "neutral" : degraded ? "warn" : "good"}
              />
            </div>

            <div className="ratio">
              <span>
                {standalone ? "Cluster mode" : "Healthy peers"}
              </span>
              <strong>
                {standalone
                  ? "No remote peers"
                  : `${healthy}/${configured}`}
              </strong>
            </div>

            <div className="bar">
              <span
                style={{
                  width:
                    configured > 0
                      ? `${(healthy / configured) * 100}%`
                      : "100%",
                }}
              />
            </div>

            <div className="health-stats">
              <div>
                <strong>{healthy}</strong>
                <span>Healthy</span>
              </div>
              <div>
                <strong>{suspect}</strong>
                <span>Suspect</span>
              </div>
              <div>
                <strong>{unavailable}</strong>
                <span>Unavailable</span>
              </div>
            </div>
          </article>
        </section>

        <section className="metrics">
          <Metric
            detail={`${overview?.storage.complete_manifests ?? 0} complete`}
            label="Stored manifests"
            tone={
              (overview?.storage.incomplete_manifests ?? 0) > 0
                ? "warn"
                : "good"
            }
            value={String(overview?.storage.manifests ?? 0)}
          />
          <Metric
            detail={`${overview?.storage.chunk_references ?? 0} chunk references`}
            label="Logical storage"
            value={formatBytes(overview?.storage.logical_bytes ?? 0)}
          />
          <Metric
            detail={
              storageHealthy
                ? "All manifests complete"
                : `${overview?.storage.incomplete_manifests ?? 0} incomplete`
            }
            label="Missing chunks"
            tone={storageHealthy ? "good" : "bad"}
            value={String(overview?.storage.missing_chunks ?? 0)}
          />
          <Metric
            detail={`${overview?.http.requests_failed ?? 0} failed requests`}
            label="HTTP success rate"
            tone={successRate >= 99 ? "good" : "warn"}
            value={`${successRate.toFixed(1)}%`}
          />
        </section>

        <section className="panel" id="cluster">
          <div className="heading">
            <div>
              <p className="eyebrow">Cluster topology</p>
              <h2>Nodes and peer health</h2>
              <p>Membership, replication policy and heartbeat state.</p>
            </div>

            <div className="epoch">
              <span>Membership epoch</span>
              <strong>{epoch || "—"}</strong>
            </div>
          </div>

          <div className="summary">
            <div>
              <span>Cluster ID</span>
              <code title={cluster?.cluster_id}>
                {shortId(cluster?.cluster_id)}
              </code>
            </div>
            <div>
              <span>Local node</span>
              <code title={cluster?.node_id}>{shortId(cluster?.node_id)}</code>
            </div>
            <div>
              <span>Replication factor</span>
              <strong>{cluster?.replication_factor ?? 1}</strong>
            </div>
            <div>
              <span>Strict replication</span>
              <Pill
                label={cluster?.strict_replication ? "Enabled" : "Disabled"}
                tone={cluster?.strict_replication ? "good" : "neutral"}
              />
            </div>
          </div>

          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Peer</th>
                  <th>Endpoint</th>
                  <th>Health</th>
                  <th>Failures</th>
                  <th>Last error</th>
                </tr>
              </thead>
              <tbody>
                {peers.length > 0 ? (
                  peers.map((peer) => (
                    <tr key={peer.node_id}>
                      <td>
                        <code title={peer.node_id}>{shortId(peer.node_id)}</code>
                      </td>
                      <td>
                        <code>
                          {peer.address}:{peer.port}
                        </code>
                      </td>
                      <td>
                        <Pill
                          label={peer.state}
                          tone={
                            peer.state === "healthy"
                              ? "good"
                              : peer.state === "suspect"
                                ? "warn"
                                : "bad"
                          }
                        />
                      </td>
                      <td>{peer.consecutive_failures}</td>
                      <td>{peer.last_error || "None"}</td>
                    </tr>
                  ))
                ) : (
                  <tr>
                    <td className="empty" colSpan={5}>
                      No remote peers configured. NexusFS is operating as a
                      standalone node.
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </section>

        <section className="panel" id="storage">
          <div className="heading">
            <div>
              <p className="eyebrow">Content catalog</p>
              <h2>Stored files</h2>
              <p>Manifest completeness, chunk counts and logical sizes.</p>
            </div>
          </div>

          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>File</th>
                  <th>Manifest</th>
                  <th>Size</th>
                  <th>Chunks</th>
                  <th>Missing</th>
                  <th>Status</th>
                </tr>
              </thead>
              <tbody>
                {(files?.files ?? []).length > 0 ? (
                  files?.files.map((file) => (
                    <tr key={file.manifest_id}>
                      <td>
                        <strong>{file.original_filename}</strong>
                      </td>
                      <td>
                        <code title={file.manifest_id}>
                          {shortId(file.manifest_id)}
                        </code>
                      </td>
                      <td>{formatBytes(file.file_size)}</td>
                      <td>{file.chunk_count}</td>
                      <td>{file.missing_chunks}</td>
                      <td>
                        <Pill
                          label={file.complete ? "Complete" : "Incomplete"}
                          tone={file.complete ? "good" : "bad"}
                        />
                      </td>
                    </tr>
                  ))
                ) : (
                  <tr>
                    <td className="empty" colSpan={6}>
                      No files are available in the administrator catalog.
                    </td>
                  </tr>
                )}
              </tbody>
            </table>
          </div>
        </section>

        <div className="lower-grid">
          <section className="panel" id="operations">
            <div className="heading">
              <div>
                <p className="eyebrow">Administrator controls</p>
                <h2>Cluster operations</h2>
                <p>Run authenticated recovery and maintenance workflows.</p>
              </div>
            </div>

            <div className="operation-grid">
              <OperationForm
                description="Exchange metadata catalogs and recover missing manifests."
                disabled={standalone}
                epoch={epoch}
                operation="catalog-sync"
                title="Synchronize catalog"
              />
              <OperationForm
                description="Recover missing chunks and restore replica placement."
                disabled={!overview}
                epoch={epoch}
                operation="repair"
                title="Repair replicas"
              />
              <OperationForm
                description="Perform a proactive storage and replica health cycle."
                disabled={!overview}
                epoch={epoch}
                operation="maintenance"
                title="Run maintenance"
              />
              <OperationForm
                description="Apply epoch-fenced placement for the current membership view."
                disabled={standalone || epoch <= 0}
                epoch={epoch}
                operation="rebalance"
                title="Rebalance placement"
              />
            </div>
          </section>

          <section className="panel" id="security">
            <div className="heading">
              <div>
                <p className="eyebrow">Trust boundaries</p>
                <h2>Security posture</h2>
                <p>Signed peers, admin access and replay protection.</p>
              </div>
            </div>

            <div className="security-list">
              <div>
                <span>Peer request signing</span>
                <Pill
                  label={
                    overview?.security.peer_request_signing
                      ? "Enabled"
                      : "Disabled"
                  }
                  tone={
                    overview?.security.peer_request_signing
                      ? "good"
                      : "neutral"
                  }
                />
              </div>
              <div>
                <span>Administrator authentication</span>
                <Pill
                  label={
                    overview?.security.admin_authentication
                      ? "Enabled"
                      : "Disabled"
                  }
                  tone={
                    overview?.security.admin_authentication
                      ? "good"
                      : "neutral"
                  }
                />
              </div>
              <div>
                <span>Replay attempts rejected</span>
                <strong>
                  {overview?.security.peer_replays_rejected ?? 0}
                </strong>
              </div>
              <div>
                <span>Peer requests rejected</span>
                <strong>
                  {overview?.security.peer_requests_rejected ?? 0}
                </strong>
              </div>
              <div>
                <span>Admin requests rejected</span>
                <strong>
                  {overview?.security.admin_requests_rejected ?? 0}
                </strong>
              </div>
            </div>
          </section>
        </div>

        <footer>
          <span>NexusFS distributed storage control plane</span>
          <span>Rendered {new Date().toLocaleString()}</span>
        </footer>
      </main>
    </div>
  );
}
