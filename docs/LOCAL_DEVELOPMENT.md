# Local Product Environment

NexusFS can be run locally as two cooperating processes:

1. `nexusfsd` — the C++20 storage daemon and authenticated administrator API.
2. `dashboard` — the Next.js operations control plane.

## Prerequisites

- A successful Debug build at `build/Debug/nexusfsd.exe`
- Node.js and npm
- Dashboard dependencies, installed automatically on the first launch when
  `dashboard/node_modules` is absent

## One-command startup on Windows

From the repository root:

```powershell
.\scripts\start-local.ps1
```

The launcher:

- validates ports 8080 and 3000
- creates `dashboard/.env.local` with a cryptographically random administrator
  token when one does not already exist
- starts the daemon and dashboard in separate PowerShell windows
- uses the same administrator token for both processes

Open:

```text
http://127.0.0.1:3000
```

## Individual launchers

Daemon only:

```powershell
.\scriptsun-daemon.ps1
```

Dashboard only:

```powershell
.\scriptsun-dashboard.ps1
```

## Security notes

`dashboard/.env.local` is intentionally ignored by Git and must never be
committed. The Next.js application reads the administrator token only on the
server. Browser requests do not receive the token.

For a multi-node local cluster, set the same `NEXUSFS_CLUSTER_SECRET` in every
node process before starting the daemons.

## Stop the environment

Press `Ctrl+C` in both launcher windows, or close both windows. NexusFS handles
its normal shutdown signal and stops its background schedulers before exiting.
