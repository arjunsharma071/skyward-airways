# Docker and Cloud Deployment

The container image runs the actual C++17 HTTP server. The server serves the
existing frontend and REST API; PostgreSQL stores the application records.
Compose keeps PostgreSQL on a private Docker network and publishes the web
service only on host loopback by default.

## Requirements and configuration

- Docker Engine with Docker Compose v2 and BuildKit.
- A host running Docker for the container architecture being deployed.
- Keep `data/` and database backups private. Use synthetic data for demos.

Copy the example environment file and replace every `REPLACE_*` value:

```bash
cp .env.example .env
openssl rand -hex 32
openssl rand -hex 32
```

Use the first generated value as `POSTGRES_PASSWORD` and the second as
`ADMIN_PASSWORD` (production requires at least 16 characters); set a
non-default `ADMIN_USERNAME`. Restrict access to `.env` (`chmod 600 .env` on
Linux/macOS), and never commit it. The Compose file constructs `DATABASE_URL`
from `POSTGRES_USER`,
`POSTGRES_PASSWORD`, and `POSTGRES_DB`; the database password is not embedded
in the frontend or image. In cloud environments, inject the database URL and
admin credentials from the provider's secret manager instead of storing them
in source control.

`HTTP_PORT` selects the host loopback port (default `8080`). `SERVER_HOST`,
`SERVER_PORT`, and `DATA_DIRECTORY` are set for the container by Compose. Set
`LEGACY_DATA_PATH` to the directory containing the existing legacy files; it
defaults to `./data` and is mounted read-only.

## Back up and import existing records

Before deploying, make a separate backup of the existing data files and verify
the copies. On Linux/macOS:

```bash
backup="data-backups/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$backup"
cp -a data/. "$backup/"
sha256sum data/* "$backup"/*
```

On Windows, use the PowerShell backup procedure in [BUILD.md](BUILD.md). Do not
delete or replace the originals or the verified backup.

On its first database-backed startup, the application initializes
`database/schema.sql` idempotently and transactionally imports
`flights.txt`, `passengers.txt`, and `bookings.txt`. Import rows are tracked in
`skyward_legacy_imports`; existing database records with conflicting content
cause an explicit startup error instead of being overwritten. The source data
directory is read-only in Compose. Plaintext legacy passenger passwords are
hashed in PostgreSQL during application loading; the original source files
remain unchanged. The importer is one-time per source filename/database; take
a backup and review the import ledger before moving to a different legacy
dataset.

## Run with Docker Compose

From the project root after preparing `.env` and backing up `data/`:

```bash
docker compose config -q
docker compose up --build -d
docker compose ps
curl --fail http://127.0.0.1:8080/api/health
```

Open <http://localhost:8080/>. The database is not published to a host port.
Compose mounts the legacy files read-only, runs the application as an
unprivileged user with a read-only container filesystem, drops Linux
capabilities, and applies a health check. `docker compose restart backend`
reconnects the app to the database while retaining records in the named
`postgres_data` volume.

To stop without removing data:

```bash
docker compose down
```

Do **not** use `docker compose down --volumes` unless intentionally deleting
the PostgreSQL data volume after verified backups.

## Database backup and restore

The named Docker volume is durable across container recreation but is not a
backup. Regularly create an off-host, access-controlled PostgreSQL dump:

```bash
mkdir -p database-backups
docker compose exec -T postgres sh -c \
  'pg_dump -U "$POSTGRES_USER" -d "$POSTGRES_DB" --format=custom' \
  > "database-backups/skyward-$(date -u +%Y%m%dT%H%M%SZ).dump"
```

Encrypt and copy the dump to approved off-host storage, and periodically test a
restore into a separate disposable database. Before restoring over an existing
deployment, take another current dump. Example restore into the configured
database (this replaces matching schema objects):

```bash
cat database-backups/skyward-backup.dump |
  docker compose exec -T postgres sh -c \
    'pg_restore --clean --if-exists --no-owner -U "$POSTGRES_USER" -d "$POSTGRES_DB"'
```

Protect dumps as sensitive passenger data. Also use the cloud provider's
automated backups, point-in-time recovery, encryption-at-rest, and restore
testing when using managed PostgreSQL.

## Cloud deployment with HTTPS

1. Build and publish the image from an approved CI runner or deployment
   pipeline for the target architecture. The included GitHub Actions workflow
   builds AMD64 and ARM64 images; that build check is not a substitute for
   runtime testing on the target architecture.
2. Provide `APP_ENV=production`, `SERVER_HOST=0.0.0.0`, `SERVER_PORT=8080`,
   `DATA_DIRECTORY=/app/data`, `DATABASE_URL`, `ADMIN_USERNAME`, and
   `ADMIN_PASSWORD` through the platform's configuration and secret manager.
   The production C++ process refuses to start without the database URL and
   explicit admin credentials.
3. Use a private managed PostgreSQL endpoint or private container network.
   Restrict database ingress to the application, require encrypted database
   transport where supported, and configure backups and recovery.
4. Put a maintained TLS reverse proxy or managed HTTPS load balancer in front
   of the app. Terminate TLS there, renew certificates automatically, redirect
   HTTP to HTTPS, and apply request-size limits and appropriate rate limiting.
   For a host-based proxy, keep the application published on `127.0.0.1:8080`
   and proxy to that loopback address. Allow inbound public traffic only on
   ports 80/443; do not expose port 8080 or PostgreSQL to the Internet.
5. Store logs, database backups, and secrets according to the host's retention
   and access-control policy. Test health checks, graceful termination, and
   restore procedures before production use.

For a cloud host using an external PostgreSQL service, the built image can be
started with the provider's secret injection mechanism (the following shows
the shape only; supply real values from protected configuration, not command
history):

```bash
docker run --name skyward-airways \
  --restart unless-stopped \
  --read-only --tmpfs /tmp:size=16m,mode=1777 \
  --cap-drop ALL --security-opt no-new-privileges:true \
  -p 127.0.0.1:8080:8080 \
  -e APP_ENV=production \
  -e SERVER_HOST=0.0.0.0 -e SERVER_PORT=8080 \
  -e DATA_DIRECTORY=/app/data \
  -e DATABASE_URL="$DATABASE_URL" \
  -e ADMIN_USERNAME="$ADMIN_USERNAME" \
  -e ADMIN_PASSWORD="$ADMIN_PASSWORD" \
  skyward-airways-backend:approved-tag
```

Publish the image to a private registry and pin deployments to an immutable
digest. Do not pass real credentials inline in shell commands or put them in
the browser, HTML, JavaScript, Docker image, or repository.

## Important application limits

- The backend currently keeps its application snapshot and bearer-token
  sessions in process memory and replaces a complete record type in a
  transaction when saving. Run **one active backend instance**; horizontal
  scaling or multiple writers can overwrite each other's stale snapshots.
  PostgreSQL persistence alone does not make this adapter multi-instance safe.
- Sessions expire when the process restarts. The app does not currently
  implement distributed sessions, request throttling, TLS, or a real payment
  flow; address edge protections at the proxy/platform layer and do not process
  real payments with this demo.
- Refund totals, loyalty points/tiers, and flight status are demonstrations.
  There is no live airline/GDS integration, payment provider, fare inventory
  feed, or flight operations feed.
- The schema initializer uses version 1 and idempotent DDL. Back up the
  database before future schema changes; implement and test explicit,
  forward-only migrations before altering or transforming stored records.
- CI validates Linux builds and builds multi-architecture images. Windows
  Code Integrity remains enforced on the user's Windows device; do not attempt
  to bypass it or run an unsigned executable that policy blocks. Linux,
  macOS, AMD64, and ARM64 runtime support must be verified on each target
  environment before being claimed.
