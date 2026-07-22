# NexusFS Architecture

## 1. System Overview

NexusFS is organized as a layered distributed storage system:

```mermaid
flowchart LR
    subgraph Clients
        StorageCLI[nexusfs]
        AdminCLI[nexusfsctl]
        Browser[Browser]
    end

    subgraph Operations
        Dashboard[Next.js dashboard]
        AdminAPI[Administrator API]
        Metrics[Metrics registry]
        Logs[JSON logger]
    end

    subgraph Core
        Service[NexusFsService]
        Metadata[Metadata coordinator]
        Rebalancer[Placement rebalancer]
        Repair[Replica repair]
        Maintenance[Replica maintenance]
    end

    subgraph Storage
        ChunkStore[Chunk store]
        ManifestStore[Manifest store]
        DurableFiles[Durable file protocol]
        Recovery[Startup recovery]
    end

    subgraph Cluster
        Membership[Persistent membership]
        PeerTransport[Authenticated peer transport]
        Heartbeats[Heartbeat scheduler]
        Catalog[Distributed metadata catalog]
        Journal[Operation journal]
    end

    StorageCLI --> Service
    AdminCLI --> AdminAPI
    Browser --> Dashboard
    Dashboard --> AdminAPI
    AdminAPI --> Service
    AdminAPI --> Metrics
    AdminAPI --> Logs

    Service --> ChunkStore
    Service --> ManifestStore
    Service --> Metadata
    Service --> Repair
    Service --> Maintenance
    Service --> Rebalancer

    ChunkStore --> DurableFiles
    ManifestStore --> DurableFiles
    Recovery --> ChunkStore
    Recovery --> ManifestStore

    Metadata --> Catalog
    Metadata --> PeerTransport
    Repair --> PeerTransport
    Maintenance --> PeerTransport
    Rebalancer --> PeerTransport
    Rebalancer --> Journal
    Heartbeats --> PeerTransport
    Membership --> Metadata
    Membership --> Rebalancer
```

## 2. Executable Boundaries

### `nexusfs`

A thin local storage CLI. It parses commands and invokes `NexusFsService` for
store, list, inspect, verify, and restore workflows.

### `nexusfsd`

The long-running daemon. It owns:

- the HTTP server
- storage service lifetime
- cluster membership
- peer transport
- heartbeat scheduling
- metadata synchronization
- replica maintenance scheduling
- metrics
- structured logging
- administrator security

### `nexusfsctl`

A command-line client for the authenticated administrator API.

### Next.js dashboard

A server-rendered operations UI. Server components and server actions call the
administrator API using a server-side bearer token. Browser code never receives
that token.

## 3. Data Model

### Chunk identity

```text
chunk_id = SHA-256(chunk_bytes)
```

Chunk files are immutable and stored below a two-character shard directory.

### Manifest identity

```text
manifest_id = SHA-256(canonical_manifest_bytes)
```

A manifest contains:

- format version
- original filename
- original byte size
- configured chunk size
- ordered chunk hashes

The ordering of chunk references is part of the file identity.

## 4. Local Persistence Protocol

NexusFS avoids publishing partially written objects:

```text
write temporary file
    ↓
flush and close
    ↓
read back and verify
    ↓
atomically rename to final content-addressed path
```

Startup recovery scans storage directories for interrupted temporary artifacts
and reconciles them before normal service begins.

## 5. Store Workflow

```mermaid
sequenceDiagram
    participant Client
    participant Service
    participant ChunkStore
    participant ManifestStore
    participant Metadata
    participant Peers

    Client->>Service: store(file)
    Service->>Service: split into fixed-size chunks
    loop every chunk
        Service->>Service: SHA-256
        Service->>ChunkStore: durable put-if-absent
    end
    Service->>ManifestStore: durable canonical manifest
    Service->>Metadata: publish manifest ownership
    Service->>Peers: replicate chunks according to policy
    Service-->>Client: manifest ID and replication result
```

Strict replication can make the store operation fail when the configured replica
requirement cannot be satisfied.

## 6. Restore and Repair Workflow

```mermaid
sequenceDiagram
    participant Client
    participant Service
    participant ManifestStore
    participant ChunkStore
    participant Repair
    participant Peer

    Client->>Service: restore(manifest ID)
    Service->>ManifestStore: load and verify manifest
    loop ordered chunk references
        Service->>ChunkStore: load and verify chunk
        alt chunk missing or invalid
            Service->>Repair: recover chunk
            Repair->>Peer: authenticated chunk request
            Peer-->>Repair: candidate bytes
            Repair->>Repair: verify SHA-256 identity
            Repair->>ChunkStore: durable local commit
        end
    end
    Service->>Service: reconstruct temporary output
    Service->>Service: validate size and chunk count
    Service-->>Client: atomically finalized file
```

## 7. Cluster Membership

Membership state is persisted rather than reconstructed only from command-line
arguments. Each accepted topology change advances a membership epoch.

The epoch is used to:

- reject stale rebalancing requests
- fence outdated placement decisions
- make topology-dependent operations explicit
- preserve a deterministic current membership view

NexusFS does not claim to implement consensus. Membership changes are expected to
be coordinated by the administrator or a future consensus layer.

## 8. Metadata Ownership and Catalogs

Ownership is deterministic for a given manifest and membership view. Owners
publish manifest metadata, and nodes exchange authenticated metadata catalogs.

The synchronizer can:

- detect catalog differences
- retrieve missing manifests
- publish locally known metadata
- recover owner data after failure
- converge repeated exchanges without duplicating immutable objects

## 9. Replica Maintenance

Replica maintenance separates detection from repair:

1. enumerate locally referenced chunks
2. inspect observed remote replicas
3. identify under-replicated content
4. request or create verified replicas
5. update metrics and structured logs

The background scheduler repeats this workflow at a configured interval.

## 10. Rebalancing and Idempotency

Rebalancing accepts:

- an operation ID
- an expected membership epoch

The operation journal records durable progress. Replaying the same operation ID
is safe, while a stale expected epoch is rejected.

```text
request
    ↓
validate operation ID
    ↓
validate membership epoch
    ↓
load or create journal record
    ↓
calculate deterministic placement
    ↓
execute required transfers
    ↓
persist completion
```

## 11. Peer Security

Peer requests include signed request metadata. Validation covers:

- shared cluster secret
- request method and target
- body digest
- timestamp window
- nonce uniqueness
- constant-time signature comparison

Accepted nonces are retained for the replay window so that a captured request
cannot be submitted again successfully.

## 12. Administrator Security

Administrator routes require a bearer token. The token is compared in constant
time.

For the dashboard:

```text
Browser
    ↓
Next.js server action or server component
    ↓ Authorization: Bearer <token from .env.local>
NexusFS administrator API
```

The browser receives rendered data, not the token.

## 13. Observability

NexusFS exposes metrics covering:

- HTTP requests and active connections
- accepted and rejected peer requests
- replay rejection
- accepted and rejected administrator requests
- heartbeat and transport outcomes
- replication and repair runs
- metadata synchronization
- maintenance
- rebalancing and stale-epoch rejection

Structured JSON logs contain event names and contextual fields suitable for
machine parsing.

## 14. Concurrency Model

The system uses explicit service boundaries and synchronized mutable state.
Background schedulers own their worker lifetimes and are stopped during normal
daemon shutdown.

Immutable content identities reduce coordination requirements for chunk and
manifest writes: concurrent attempts to publish the same valid object converge
on the same final path.

## 15. Failure Boundaries

| Failure | Expected behavior |
|---|---|
| Duplicate chunk write | Reuse the existing content-addressed object |
| Interrupted temporary write | Remove or reconcile it during startup recovery |
| Missing local chunk | Attempt verified peer-backed repair |
| Corrupted peer response | Reject it after content-hash verification |
| Replayed peer request | Reject it using the nonce cache |
| Stale rebalance request | Reject it using membership-epoch fencing |
| Repeated rebalance operation | Resume or return the durable idempotent result |
| Unavailable peer | Record health failure and continue according to policy |
| Dashboard disconnected | Render a disconnected state without exposing secrets |

## 16. Explicit Non-Goals of the Current Release

- consensus
- quorum reads and writes
- internal TLS termination
- multi-datacenter replication
- erasure coding
- garbage collection
- at-rest encryption
- production SLO claims

These are future extensions rather than hidden assumptions.
