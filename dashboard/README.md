# NexusFS Operations Dashboard

The dashboard is a server-rendered Next.js control plane for NexusFS.

The browser never receives the NexusFS administrator token. Server components
and server actions call the authenticated administrator API directly.

## Configure

Copy `.env.example` to `.env.local` and set the same token used by `nexusfsd`.

```text
NEXUSFS_API_BASE_URL=http://127.0.0.1:8080
NEXUSFS_ADMIN_TOKEN=your-secure-token
```

## Validate

```powershell
npm run lint
npx tsc --noEmit
npm run build
```

## Run

```powershell
npm run dev
```

Open `http://127.0.0.1:3000`.
