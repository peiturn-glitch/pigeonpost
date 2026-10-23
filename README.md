# PigeonPost

A self-hosted web email client in the spirit of Thunderbird, with a companion Android app. The server is written in C++20 and the web client in Vue 3. Everything runs in one Docker container plus MariaDB, and connects to your existing mail accounts over IMAP and SMTP: Gmail, iCloud, Fastmail, your own server, and so on.

The user interface is in English by default, with Swedish available from the sign-in page and the layout settings (the choice is stored in the browser). The full handbook is in Swedish: [docs/README.sv.md](docs/README.sv.md). This page covers what the software does and how to run it.

## What it does

- **Several users, several accounts each.** An administrator creates users; every user adds their own mail accounts. Nobody sees anyone else's mail.
- **A fast local mirror of your mail.** Folders, headers, flags and message text are cached in MariaDB, so lists, reading and full-text search take milliseconds and keep working when the mail server is slow, down or over quota. Changes still go straight to the mail server.
- **Gmail without app passwords.** Sign in with Google (OAuth 2.0, XOAUTH2 for IMAP and SMTP) once you have created an OAuth client.
- **Three-pane layout** with configurable panes, a unified **Unread** view across all accounts, server-side search with filters, keyboard shortcuts, dark mode.
- **Rich-text compose** with inline images and attachments, drafts, reply/reply all/forward with the original's attachments, multiple sender identities per account with their own SMTP settings and signatures.
- **Safe HTML.** Messages render in a sandboxed iframe without scripts; remote content is blocked until you allow it for a message or a sender.
- **Sorting with AI.** Gemini, OpenAI, Mistral or any OpenAI-compatible service (for example Ollama) suggests a folder per message. You approve, it learns from corrections, and it can sort new mail or a whole folder in the background with history and undo. Nothing is sent anywhere unless you turn it on and supply your own API key.
- **Backups** of an account to a zip file, to another IMAP account or to Google Drive, on demand or on a schedule.
- **Thunderbird import and export** of accounts, identities, SMTP servers and signatures (`prefs.js`).
- **Android app support:** a delta endpoint for clients that keep their own copy of the cache, push notifications through Firebase Cloud Messaging, and distribution of the app's APK from the server so it can update itself. The app itself is a separate project and not part of this repository.

## Run it with the Docker image

You need Docker with Compose. The image is published as `peiturn555/pigeonpost` on Docker Hub (`linux/amd64` and `linux/arm64`).

```bash
mkdir pigeonpost && cd pigeonpost
curl -fsSLO https://raw.githubusercontent.com/peiturn-glitch/pigeonpost/main/docker-compose.yml
curl -fsSL -o .env https://raw.githubusercontent.com/peiturn-glitch/pigeonpost/main/.env.example

# Fill in the secrets (keep this file: SECRET_KEY and DB_PASSWORD must be the same at every start)
sed -i "s/^SECRET_KEY=.*/SECRET_KEY=$(openssl rand -hex 32)/; s/^DB_PASSWORD=.*/DB_PASSWORD=$(openssl rand -hex 24)/" .env
nano .env   # ADMIN_USERNAME, ADMIN_PASSWORD, PUBLIC_URL, OPERATOR_NAME, OPERATOR_CONTACT

docker compose pull
docker compose up -d
```

Open http://localhost:8080 and log in as the administrator. Add users under **Meny → Hantera användare**; each user then adds their mail accounts. The account wizard fills in server settings for Gmail, Outlook, iCloud and Yahoo from the address.

If you leave `ADMIN_USERNAME` and `ADMIN_PASSWORD` empty, the first visitor gets a setup page and becomes administrator, so do that before anyone else can reach the server.

To update later:

```bash
docker compose pull && docker compose up -d
```

Pin a version with `PIGEONPOST_VERSION=1.2.0` in `.env`. Tags: `latest` and `X.Y.Z` for releases, `edge` for the current `main` branch.

### Put it behind HTTPS

The container speaks plain HTTP on port 80 (published as 8080 above). Put a reverse proxy with TLS in front of it (Caddy, Traefik, nginx) if anyone but you can reach it, and set `PUBLIC_URL` to the address users see, for example `https://mail.example.com`. Google sign-in requires an `https` URL with a domain name. The proxy's read timeout should be at least a couple of minutes, since IMAP operations on slow providers can take a while.

## Run it from source

```bash
git clone https://github.com/peiturn-glitch/pigeonpost.git
cd pigeonpost
cp .env.example .env    # then fill in SECRET_KEY, DB_PASSWORD and the rest as above

docker compose -f docker-compose.yml -f docker-compose.build.yml up -d --build
```

The Dockerfile builds in three stages: Node builds the web client, Debian builds the C++ server, and a slim Debian image runs the binary, which serves both the API and the static files. Nothing needs to be installed on the host besides Docker.

For frontend development with hot reload, run the server in Docker and Vite locally. Vite proxies `/api` to port 8080:

```bash
docker compose --profile dev up -d      # also starts GreenMail, a test mail server that accepts any login
cd frontend && npm install && npm run dev
```

Add an account with the **Testserver** preset, for example `test@localhost` with password `test`, and send yourself a message to `test@localhost`.

### Tests

The end-to-end suites run Node scripts against a throwaway stack with GreenMail and mock services for Google, AI providers and Firebase. See [tests/ai-mock/README.md](tests/ai-mock/README.md) for the stack and run them in this order on a fresh stack:

```bash
node tests/mail/apitest.mjs
node tests/ai-mock/apitest.mjs
node tests/cache/apitest.mjs
node tests/changes/apitest.mjs
node tests/push-mock/apitest.mjs
node tests/oauth-mock/apitest.mjs   # see tests/oauth-mock/README.md
```

## Configuration

All settings are environment variables; `docker-compose.yml` reads them from `.env`.

| Variable | Default | Description |
|---|---|---|
| `ADMIN_USERNAME`, `ADMIN_PASSWORD` | *(empty)* | Creates the first administrator at start while the database has no users. Password at least 8 characters. |
| `SECRET_KEY` | *(generated)* | Key for the AES-256-GCM encryption of mail passwords, tokens and API keys. If empty, one is generated into `/data/secret.key`. Changing it makes stored passwords unreadable. |
| `DB_HOST`, `DB_PORT`, `DB_NAME`, `DB_USER`, `DB_PASSWORD` | `localhost`, `3306`, `brevduva`, `brevduva`, *(required)* | MariaDB connection. The compose file sets the host to `db` and the name and user to `pigeonpost`. MariaDB 10.3 or newer. |
| `PUBLIC_URL` | *(request host)* | The address users reach the app at. Base for Google's callback URL; always set it behind a proxy. |
| `OPERATOR_NAME`, `OPERATOR_CONTACT` | `the operator of this service`, *(empty)* | Shown on `/privacy.html` and `/terms.html`, which Google requires for OAuth verification. The pages are templates in `frontend/public`; mount your own files as `/app/public/privacy.html` and `/app/public/terms.html` to replace them entirely. |
| `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET` | *(empty)* | Enables Google sign-in for Gmail. See below. |
| `AI_EXTRA_PROVIDERS` | *(empty)* | Extra AI services for sorting as `Name=https://host/v1`, comma separated. OpenAI-compatible, no API key. Users can only pick services from this list, so the server cannot be made to call arbitrary URLs. |
| `MAIL_CACHE` | `1` | `0` turns the mail cache off; everything is then read from the mail server. |
| `RAW_CACHE_MB` | `1024` | Size limit for recently opened messages with attachments kept on disk in `/data/cache/raw`. `0` disables. |
| `FCM_SERVICE_ACCOUNT_FILE` | *(empty)* | Path to a Firebase service-account JSON key. Enables push notifications for the Android app. |
| `BREVDUVA_ANDROID_DIR` | `/data/android` | Directory with the Android APK and `version.json`, served without login under `/android` so the app can update itself. `/android` is off if the directory is missing. |
| `TZ` | `UTC` | Time zone for dates in backup file names. |
| `BREVDUVA_PORT` | `80` | Port inside the container. |
| `BREVDUVA_DATA_DIR`, `BREVDUVA_BACKUP_DIR` | `/data`, `/data/backups` | Where `secret.key`, the raw cache and backup files live (volume `pigeonpost-data`). |

Internal names (`BREVDUVA_*` variables, the `X-Brevduva` request header, the `brevduva-server` binary) come from the project's original name and are kept for compatibility.

## Google sign-in for Gmail

Google requires every installation to have its own OAuth client. Once, as administrator:

1. Create a project in the [Google Cloud Console](https://console.cloud.google.com/).
2. Under **Google Auth Platform → Branding**, set the app name and contact. Choose **Internal** if all users are in your Google Workspace organisation (no review needed) or **External** for ordinary Gmail accounts.
3. Under **Data Access**, add the scope `https://mail.google.com/` (and `https://www.googleapis.com/auth/drive.file` if you want backups to Google Drive; then also enable the **Google Drive API**).
4. Under **Clients**, create a **Web application** client with the authorised redirect URI `<PUBLIC_URL>/api/oauth/google/callback`.
5. Set `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET` and `PUBLIC_URL`, and restart. The log says `Google-inloggning för Gmail: aktiverad`.

Things to know: `https://mail.google.com/` is a restricted scope, so an unverified external app shows a warning and is limited to 100 users; with publishing status **Testing** logins expire after 7 days, so switch to **In production**; the redirect URI must be `http://localhost…` or an `https` URL with a domain name. Google's verification asks for a privacy policy and terms page, which the server provides at `/privacy.html` and `/terms.html` using `OPERATOR_NAME` and `OPERATOR_CONTACT`.

## Android app

The companion Android app (package `cc.pigeonpost.app`) is not part of this repository, but the server has everything it needs:

- **Delta sync:** `GET /api/accounts/:id/changes?since=` returns only what changed since the client's last sync, so a phone can keep a local mirror of the cache.
- **Push notifications:** create a Firebase project, enable the Cloud Messaging API (v1), create a service account with the role *Firebase Cloud Messaging API Admin*, download its JSON key, mount it into the container and point `FCM_SERVICE_ACCOUNT_FILE` at it. Phones register under `/api/devices`; the server sends a data message with sender and subject when the sync sees new mail in a watched folder. Push is off until the key exists.
- **Distribution:** put the APK and a `version.json` (`{ "versionName", "versionCode", "minSdk", "url", "sha256", "notes" }`) in a directory mounted as `/data/android` (`BREVDUVA_ANDROID_DIR`). It is served without login under `/android`, and the app checks it for updates. Without the directory, `/android` is off.

## Security notes

- Login passwords are stored as scrypt hashes. Mail passwords, Google tokens and AI API keys are encrypted with `SECRET_KEY`, since they must be sent to other servers in clear.
- The mail cache stores headers and message text unencrypted in the database so that they can be searched. Protect the database accordingly.
- Sessions are HttpOnly, `SameSite=Strict` cookies, valid 14 days; the database stores only a SHA-256 of the token. Every modifying API call also requires the header `X-Brevduva: 1` (CSRF protection). Ten failed logins in 15 minutes lock a username temporarily.
- HTML mail never runs scripts. Attachments are downloaded as files, except images, so an attachment cannot run in the app's origin.
- Each user only ever sees their own mail accounts; other users' accounts give 404, also for administrators.

## Known limitations

- Outlook.com and Microsoft 365 require OAuth2, which is not supported yet. Without Google sign-in, Gmail needs an app password, as do iCloud and Yahoo with two-factor authentication.
- No address book or calendar. Messages cannot be moved between accounts. Changes (move, delete, send) need the mail server to be reachable; they are not queued on the server (the Android app does queue them).

## Architecture

```
Browser (Vue 3 + Pinia) / Android app
        │  JSON over HTTP (/api/…)
        ▼
brevduva-server (C++20, cpp-httplib)
  ├─ api.cpp            REST endpoints, per-user authorisation, CSRF check
  ├─ mail_service.cpp   One IMAP connection per account, reused between calls; reads from the cache when possible
  ├─ mail_cache.cpp     Sync threads with their own IMAP and database connections; change log for the app
  ├─ raw_cache.cpp      Recently opened messages with attachments as files on disk
  ├─ imap.cpp           Own IMAP4rev1 client (literals, pipelining, MOVE/UIDPLUS, mUTF-7)
  ├─ smtp.cpp           Sending through libcurl
  ├─ mime.cpp           Parsing and building messages with GMime 3
  ├─ auth.cpp           Users, scrypt, sessions, brute-force protection
  ├─ oauth.cpp          Google OAuth 2.0 with PKCE, token exchange, refresh and revocation
  ├─ push.cpp           Firebase Cloud Messaging (HTTP v1) for the Android app
  ├─ backup.cpp, gdrive.cpp, zip_writer.cpp   Backups to zip, IMAP or Google Drive
  ├─ ai_sort.cpp, ai.cpp                      Sorting with OpenAI-compatible AI services
  ├─ thunderbird_prefs.cpp                    Thunderbird prefs.js import and export
  ├─ db.cpp, db_cache.cpp                     MariaDB: users, sessions, accounts, cache, migrations
  └─ net.cpp            TCP/TLS with OpenSSL, STARTTLS, certificate verification
        │
        ▼
IMAP and SMTP servers              MariaDB
```

## License

MIT, see [LICENSE](LICENSE).
