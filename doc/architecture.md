# systemd-recalld Architecture

## Overview

systemd-recalld implements a client-server architecture for continuous desktop
capture and AI-powered query. The client daemon (`systemd-recalld`) runs on the
user's workstation, capturing audio and video. The server component
(`systemd-recall-query-service`) provides a REST API for searching through
captured data using AI models.

## System Architecture

```
┌──────────────────────────────────────────────────────┐
│                    Client Machine                    │
│                                                      │
│  ┌──────────────┐     ┌──────────────────────────┐   │
│  │ Audio Capture │     │ Video Capture            │   │
│  │              │     │                          │   │
│  │ ALSA backend │     │ X11 backend (XGetImage)  │   │
│  │ Pulse backend│     │ Wayland backend (portal) │   │
│  └──────┬───────┘     └────────────┬─────────────┘   │
│         │                          │                 │
│         └──────────┬───────────────┘                 │
│                    ▼                                 │
│         ┌──────────────────────┐                     │
│         │ Storage Manager      │                     │
│         │                      │                     │
│         │ SQLite (metadata)    │                     │
│         │ File store (data)    │                     │
│         │ AES-256-GCM encrypt  │                     │
│         └──────────────────────┘                     │
│                    │                                 │
│    ┌───────────────┼───────────────┐                 │
│    ▼               ▼               ▼                 │
│ ┌────────┐  ┌────────────┐  ┌──────────┐            │
│ │ D-Bus  │  │ Retention  │  │ Network  │            │
│ │Service │  │ Enforcement│  │ Transmit │            │
│ └────────┘  └────────────┘  └──────────┘            │
│    ▲                               │                 │
│    │                               │                 │
│ recallctl                          │                 │
│ (CLI tool)                         │                 │
└────────────────────────────────────┼─────────────────┘
                                     │ TLS
                                     ▼
┌──────────────────────────────────────────────────────┐
│                    Server Machine                    │
│                                                      │
│  ┌──────────────────────────────────────────────┐    │
│  │ HTTP/TLS Server                              │    │
│  │                                              │    │
│  │ POST /api/v1/auth   → PAM + JWT             │    │
│  │ POST /api/v1/query  → Query Service          │    │
│  └──────────────┬───────────────────────────────┘    │
│                 │                                    │
│  ┌──────────────▼───────────────────────────────┐    │
│  │ Query Service                                │    │
│  │                                              │    │
│  │ Whisper (audio transcription)                │    │
│  │ CLIP (video understanding)                   │    │
│  │ BERT (text embeddings)                       │    │
│  │ Flamingo (multimodal reasoning)              │    │
│  └──────────────────────────────────────────────┘    │
│                                                      │
│  ┌──────────────────────────────────────────────┐    │
│  │ Storage                                      │    │
│  │ SQLite + encrypted data files                │    │
│  └──────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘
```

## Component Details

### Audio Capture (`src/client/audio-capture.c`)

The audio capture module provides a backend-agnostic interface for recording
audio from the system. It supports:

- **ALSA backend**: Direct hardware access via `snd_pcm_readi()`. Used when
  PulseAudio is not available or when low-latency capture is needed.
- **PulseAudio backend**: Uses `pa_simple` API for capture from the default
  audio source. Preferred on modern desktop systems.
- **Auto-detection**: When configured with `AUDIO_BACKEND_AUTO`, the module
  probes PulseAudio first (by attempting to open a test stream), then falls
  back to ALSA.

Audio data is captured in a dedicated thread and delivered via callback to the
daemon's storage layer. The callback receives raw PCM data (S16LE format) along
with sample rate and channel count.

Configuration parameters:
- Sample rate (default: 48000 Hz)
- Channels (default: 2, stereo)
- Bitrate (default: 128 kbps for Opus encoding)
- Buffer size (default: 500 ms)
- Silence detection threshold (default: 11 ms)
- Noise reduction (default: enabled)

### Video Capture (`src/client/video-capture.c`)

The video capture module captures the desktop display:

- **X11 backend**: Uses `XGetImage()` to capture the root window at the
  configured frame rate. Supports multi-monitor via Xinerama/XRandR.
- **Wayland backend**: Designed for xdg-desktop-portal or wlr-screencopy
  protocol integration. Currently a stub awaiting portal API stabilization.
- **Auto-detection**: Checks `XDG_SESSION_TYPE`, then `WAYLAND_DISPLAY` and
  `DISPLAY` environment variables.

Frames are captured in a dedicated thread with configurable frame rate
(default: 15 fps). Each frame is delivered via callback with raw pixel data,
dimensions, and format information.

### Storage Manager (`src/common/recalld-storage.c`)

The storage layer manages captured data using a two-tier approach:

1. **SQLite database**: Stores metadata (ID, type, timestamp, duration, size,
   source, checksum, encryption status). Uses WAL mode for concurrent access
   and indexes on timestamp and capture type for efficient queries.

2. **File storage**: Raw capture data is written to individual files in the
   data directory. Files are named with capture type, timestamp, and random
   suffix to avoid collisions.

The storage manager enforces:
- **Retention policies**: Deletes captures older than the configured retention
  period (default: 30 days).
- **Storage quotas**: When total storage exceeds the configured maximum
  (default: 50 GB), oldest captures are deleted first.

### Encryption (`src/common/recalld-encryption.c`)

All stored data can be encrypted using AES-256-GCM:

- **Key derivation**: PBKDF2-HMAC-SHA256 with configurable iteration count
  derives the encryption key from a passphrase and random salt.
- **Encryption format**: `IV (12 bytes) || ciphertext || auth tag (16 bytes)`
- **Random IV**: Each encryption operation generates a fresh 12-byte nonce
  using OpenSSL's `RAND_bytes()`.
- **Authentication**: GCM mode provides authenticated encryption, detecting
  any tampering with the ciphertext.

### D-Bus Service (`src/client/dbus-service.c`)

The D-Bus interface at `org.freedesktop.systemd.recalld` exposes:

**Methods:**
- `StartCapture()` — Begin audio and video capture
- `StopCapture()` — Stop all active captures
- `Pause()` — Temporarily pause capture
- `Resume()` — Resume paused capture
- `GetStatus() → (s status, x storage_used, i capture_count)`
- `ListCaptures(s type, x from, x to) → a(xsxix)`

**Properties:**
- `Capturing` (boolean) — Whether capture is active
- `Paused` (boolean) — Whether capture is paused
- `CaptureCount` (int32) — Total number of stored captures
- `StorageUsed` (int64) — Total storage used in bytes

### Daemon (`src/client/recalld-daemon.c`)

The main daemon orchestrates all client components:

1. Loads configuration from `/etc/systemd/recalld/recalld.conf`
2. Opens the storage database
3. Initializes audio and video capture contexts
4. Registers the D-Bus service
5. Notifies systemd of readiness (`READY=1`)
6. Enters the main event loop:
   - Processes D-Bus messages
   - Runs periodic retention/quota enforcement
   - Sends watchdog notifications
7. On shutdown, stops capture and notifies systemd (`STOPPING=1`)

### recallctl (`src/client/recallctl.c`)

The command-line tool communicates with the daemon exclusively through D-Bus.
It provides a simple interface for all daemon operations without requiring
direct access to storage files or configuration.

### Network Transmitter (`src/client/network-transmit.c`)

The network transmitter module handles client-to-server data transmission using
two specialized protocols and a bulk fallback:

- **Reverse RTP** (audio): The client acts as the RTP sender, pushing audio
  frames to the server. Each packet contains a standard 12-byte RTP header
  (RFC 3550) with payload type 111 (Opus), followed by the audio payload.
  "Reverse" because the client initiates the connection to the server, inverting
  the traditional media streaming model where a server sends to clients.

- **Reverse VNC** (video): The client acts as a VNC server, pushing
  FramebufferUpdate messages (RFB protocol) to the server which acts as a
  VNC viewer. Each frame is sent as an RFB FramebufferUpdate with a single
  rectangle covering the full screen. Supports Raw (default), ZLib, and Tight
  encodings.

- **Bulk HTTPS**: For batch uploads of stored data, the transmitter sends
  a binary `RCLD` framed message containing a BulkIngestHeader followed by
  the capture payload.

The transmitter supports:
- TLS encryption for all connections
- Configurable bandwidth limits
- Automatic retry with reconnection on failure
- Per-connection statistics tracking (bytes/packets sent, failures, retransmissions)

Configuration via `[Network]` section in `recalld.conf`:
- `Compression` — "none", "low", "medium", "high"
- `MaxBandwidthKbps` — bandwidth cap (0 = unlimited)
- `RetryCount` / `RetryDelaySeconds` — retry policy
- `UseMeteredConnections` — whether to transmit over metered links

### HTTP Server (`src/server/http-server.c`)

The server provides a REST API with:

- **TLS support**: Optional TLS using OpenSSL `SSL_CTX`. Falls back to plain
  HTTP if certificate/key files are not available.
- **Authentication endpoint** (`POST /api/v1/auth`): Accepts username/password
  in JSON, authenticates via PAM, returns a JWT token.
- **Query endpoint** (`POST /api/v1/query`): Accepts natural language queries
  with optional time range and content type filters. Requires Bearer token.
- **Ingest endpoint** (`POST /api/v1/ingest`): Receives captured audio/video
  data from remote clients. Accepts JSON with `type`, `duration_ms`, `source`,
  and `data` fields, or raw binary body. Ingested data is stored in the server's
  storage and becomes queryable immediately.
- **Connection handling**: Each client connection is handled synchronously in
  the accept thread. A production deployment should add a thread pool.

### PAM Authentication (`src/server/pam-auth.c`)

PAM integration provides flexible, system-standard authentication:

- Uses the `systemd-recall` PAM service (configurable)
- Supports both authentication (`pam_authenticate`) and account validation
  (`pam_acct_mgmt`)
- Compiled conditionally — when PAM is not available, all auth functions
  return failure

### JWT (`src/server/jwt.c`)

JWT token handling for API authentication:

- **Algorithm**: HMAC-SHA256 (HS256)
- **Claims**: `sub` (subject/username), `iat` (issued at), `exp` (expiration)
- **Secret**: Loaded from a file at startup; never logged or exposed
- **Validation**: Signature verification with constant-time comparison,
  expiration check

### Query Service (`src/server/query-service.c`)

The query service provides time-based and type-based filtering of stored
captures, operating on both locally stored and remotely ingested data:

- **Query processing**: Searches across all capture types (audio, video,
  screenshot) with optional time range and content type filters. Results are
  ranked by timestamp.
- **Data ingestion**: The `query_service_ingest()` function receives captured
  data from remote clients (via the HTTP ingest endpoint) and stores it in the
  server-side storage. Ingested data is immediately available for queries.
- **AI model integration**: Whisper (audio), CLIP (video), BERT (text), and
  Flamingo (multimodal) are designed to be loaded as separate inference modules.
  The current implementation queries the storage layer directly.

## Build System

The project uses Meson as its build system:

```
meson.build                 # Root: project definition, dependencies, install rules
├── src/common/meson.build  # Static library: librecalld-common.a
├── src/client/meson.build  # Static library: librecalld-client.a
│                           # Executables: systemd-recalld, recallctl
└── src/server/meson.build  # Static library: librecalld-server.a
                            # Executable: systemd-recall-query-service
```

Optional dependencies are detected at configure time and exposed as
preprocessor defines (`HAVE_ALSA`, `HAVE_PULSE`, `HAVE_X11`, `HAVE_PAM`, etc.)
via the generated `config.h`.

## Security Model

1. **Privilege separation**: The client daemon requires `CAP_SYS_ADMIN`,
   `CAP_DAC_OVERRIDE`, and `CAP_NET_ADMIN` for device access. The server
   only needs `CAP_NET_BIND_SERVICE`.
2. **Filesystem isolation**: Both services use `ProtectSystem=strict`,
   `ProtectHome=yes/read-only`, and `PrivateTmp=yes`.
3. **Encryption at rest**: All captured data is encrypted with AES-256-GCM.
4. **Encryption in transit**: TLS for client-server communication.
5. **Authentication**: PAM for user credentials, JWT for API session tokens.
6. **Privacy controls**: Application and window exclusion lists, consent
   requirements, data minimization.
