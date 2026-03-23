# systemd-recalld Technical Specifications

## 1. Audio Capture

### 1.1 Supported Backends

| Backend     | Library    | API                | Min Version |
|-------------|------------|--------------------|-------------|
| ALSA        | libasound  | `snd_pcm_readi`   | 1.2.0       |
| PulseAudio  | libpulse   | `pa_simple_read`   | 15.0        |

### 1.2 Audio Parameters

| Parameter          | Default  | Range           | Description                       |
|--------------------|----------|-----------------|-----------------------------------|
| Sample rate        | 48000 Hz | 8000–192000     | Capture sample rate               |
| Channels           | 2        | 1–8             | Number of audio channels          |
| Bit depth          | 16-bit   | 16-bit (S16_LE) | Sample format                     |
| Bitrate (Opus)     | 128 kbps | 16–256          | Opus encoding bitrate             |
| Buffer size        | 500 ms   | 10–5000         | Capture buffer duration           |
| Silence threshold  | 11 ms    | 1–1000          | Minimum silence gap to detect     |
| Noise reduction    | Enabled  | true/false      | Apply noise reduction filter      |

### 1.3 Audio Pipeline

```
Microphone/System → ALSA/PulseAudio → PCM S16_LE buffer
    → Silence detection → Noise reduction → Storage
```

## 2. Video Capture

### 2.1 Supported Backends

| Backend  | Library           | API                    | Min Version |
|----------|-------------------|------------------------|-------------|
| X11      | libX11, libXext   | `XGetImage` (ZPixmap)  | 1.7         |
| Wayland  | xdg-desktop-portal| D-Bus portal API       | —           |

### 2.2 Video Parameters

| Parameter          | Default   | Range          | Description                     |
|--------------------|-----------|----------------|---------------------------------|
| Frame rate         | 15 fps    | 1–60           | Capture frame rate              |
| Codec              | H.264     | h264/hevc/vp9  | Encoding codec                  |
| Keyframe interval  | 150       | 1–600          | Frames between keyframes        |
| Hardware accel     | auto      | auto/vaapi/none| HW encoding acceleration        |
| Multi-monitor      | true      | true/false     | Capture all monitors            |
| Mouse cursor       | true      | true/false     | Include mouse cursor            |
| Resolution         | screen    | WxH or screen  | Capture resolution              |

### 2.3 Video Pipeline

```
Display Server → XGetImage/Portal → Raw pixel buffer (BGRA)
    → Privacy filter → Encoding (H.264/HEVC) → Storage
```

## 3. Storage

### 3.1 Database Schema

```sql
CREATE TABLE captures (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    type        INTEGER NOT NULL,       /* 0=audio, 1=video, 2=screenshot */
    timestamp   INTEGER NOT NULL,       /* Unix epoch seconds */
    duration_ms INTEGER DEFAULT 0,      /* Duration in milliseconds */
    data_size   INTEGER NOT NULL,       /* Data file size in bytes */
    source      TEXT,                   /* Capture source identifier */
    checksum    TEXT,                   /* SHA-256 of data file */
    encrypted   INTEGER DEFAULT 0,      /* 1 if data is encrypted */
    data_path   TEXT NOT NULL           /* Path to data file */
);

CREATE INDEX idx_captures_timestamp ON captures(timestamp);
CREATE INDEX idx_captures_type ON captures(type);

CREATE TABLE metadata (
    key   TEXT PRIMARY KEY,
    value TEXT
);
```

### 3.2 Storage Layout

```
/var/lib/systemd/recalld/
├── recalld.db              # SQLite database
├── audio_1711234567_abc.dat    # Encrypted audio data
├── video_1711234568_def.dat    # Encrypted video data
└── screenshot_1711234569_ghi.dat  # Encrypted screenshot
```

### 3.3 SQLite Configuration

| Setting      | Value    | Rationale                           |
|--------------|----------|-------------------------------------|
| Journal mode | WAL      | Better concurrent read/write        |
| Synchronous  | NORMAL   | Good durability/performance balance |
| Min version  | 3.35.0   | Required for math functions         |

### 3.4 Retention and Quota

- **Retention**: Captures older than `RetentionDays` (default 30) are deleted
  during periodic maintenance checks.
- **Quota**: When total storage exceeds `MaxStorageSize` (default 50 GB),
  oldest captures are deleted until under quota.
- **Maintenance frequency**: Every main loop iteration (approximately every
  5 seconds when idle).

## 4. Encryption

### 4.1 Algorithm

| Component      | Algorithm / Standard     |
|----------------|--------------------------|
| Cipher         | AES-256-GCM              |
| Key derivation | PBKDF2-HMAC-SHA256       |
| IV generation  | CSPRNG (OpenSSL RAND)    |
| Auth tag       | 128-bit GCM tag          |

### 4.2 Wire Format

```
┌─────────────┬──────────────────────────┬───────────────┐
│ IV (12 B)   │ Ciphertext (variable)    │ Tag (16 B)    │
└─────────────┴──────────────────────────┴───────────────┘
```

### 4.3 Key Management

- Encryption key is derived from user passphrase via PBKDF2
- Salt is randomly generated per key derivation
- Key material is zeroed from memory on cleanup (`explicit_bzero`)
- Key files must have restrictive permissions (0600)

## 5. D-Bus Interface

### 5.1 Service Identity

| Property   | Value                                  |
|------------|----------------------------------------|
| Bus name   | `org.freedesktop.systemd.recalld`     |
| Object path| `/org/freedesktop/systemd/recalld`    |
| Interface  | `org.freedesktop.systemd.recalld`     |

### 5.2 Methods

| Method           | Arguments | Returns     | Description              |
|------------------|-----------|-------------|--------------------------|
| `StartCapture`   | —         | —           | Start audio+video capture|
| `StopCapture`    | —         | —           | Stop all captures        |
| `Pause`          | —         | —           | Pause active captures    |
| `Resume`         | —         | —           | Resume paused captures   |
| `GetStatus`      | —         | `sxi`       | Status, storage, count   |
| `ListCaptures`   | `sxx`     | `a(xsxix)`  | List captures by filter  |

### 5.3 Properties

| Property       | Type | Access | Flags           |
|----------------|------|--------|-----------------|
| `Capturing`    | `b`  | read   | emits-change    |
| `Paused`       | `b`  | read   | emits-change    |
| `CaptureCount` | `i`  | read   | —               |
| `StorageUsed`  | `x`  | read   | —               |

## 6. REST API (Query Service)

### 6.1 Endpoints

#### POST /api/v1/auth

Request:
```json
{
    "username": "string",
    "password": "string"
}
```

Response (200):
```json
{
    "token": "eyJ..."
}
```

Response (401):
```json
{
    "error": "authentication_failed"
}
```

#### POST /api/v1/query

Headers: `Authorization: Bearer <token>`

Request:
```json
{
    "query": "what happened in the meeting?",
    "time_from": 1711234567,
    "time_to": 1711320967,
    "content_types": "audio,video",
    "max_results": 50,
    "confidence_threshold": 0.7
}
```

Response (200):
```json
{
    "total_matches": 42,
    "processing_time_ms": 125.3,
    "results": [
        {
            "id": 1234,
            "type": "audio",
            "confidence": 0.95,
            "timestamp": 1711234567,
            "duration_ms": 15000
        }
    ]
}
```

### 6.2 Authentication Flow

```
Client → POST /api/v1/auth (username, password)
Server → PAM authenticate → generate JWT → return token

Client → POST /api/v1/query (Bearer token)
Server → validate JWT → process query → return results
```

### 6.3 JWT Token Format

| Field | Value           |
|-------|-----------------|
| `alg` | HS256           |
| `sub` | username        |
| `iat` | issue timestamp |
| `exp` | expiry timestamp|

## 7. Systemd Integration

### 7.1 Client Service Unit

| Directive             | Value                                         |
|-----------------------|-----------------------------------------------|
| Type                  | notify                                        |
| Capabilities          | CAP_SYS_ADMIN, CAP_DAC_OVERRIDE, CAP_NET_ADMIN|
| ProtectSystem         | strict                                        |
| ProtectHome           | read-only                                     |
| PrivateTmp            | yes                                           |
| IOSchedulingClass     | best-effort                                   |
| IOSchedulingPriority  | 5                                             |
| WatchdogSec           | 30                                            |
| MemoryMax             | 2G                                            |

### 7.2 Server Service Unit

| Directive             | Value                              |
|-----------------------|------------------------------------|
| Type                  | notify                             |
| Capabilities          | CAP_NET_BIND_SERVICE               |
| ProtectSystem         | strict                             |
| ProtectHome           | yes                                |
| PrivateTmp            | yes                                |
| PrivateDevices        | yes                                |
| IOSchedulingClass     | best-effort                        |
| IOSchedulingPriority  | 3                                  |
| WatchdogSec           | 60                                 |
| MemoryMax             | 8G                                 |

## 8. Privacy Controls

### 8.1 Application Exclusion

Applications listed in `ExcludeApplications` are never captured. Window titles
matching patterns in `ExcludeWindows` (glob-style) are also excluded.

### 8.2 Privacy Modes

| Mode   | Behavior                                         |
|--------|--------------------------------------------------|
| off    | No privacy filtering                             |
| smart  | Auto-detect sensitive content (passwords, etc.)  |
| strict | Only capture explicitly whitelisted applications |

### 8.3 Consent

When `ConsentRequired=true` (default), the daemon will not start capture until
user consent is obtained through the D-Bus interface.

## 9. Build Requirements

### 9.1 Required Dependencies

| Library      | Min Version | Purpose                    |
|--------------|-------------|----------------------------|
| libsystemd   | 249         | sd-bus, sd-daemon, sd-notify|
| SQLite3      | 3.35.0      | Metadata storage           |
| OpenSSL      | 1.1.0       | AES-256-GCM, HMAC, PBKDF2 |
| D-Bus        | 1.12        | IPC                        |
| pthreads     | —           | Capture threads            |

### 9.2 Optional Dependencies

| Library    | Purpose                 | Feature Flag |
|------------|-------------------------|--------------|
| libasound  | ALSA audio capture      | HAVE_ALSA    |
| libpulse   | PulseAudio capture      | HAVE_PULSE   |
| libX11     | X11 video capture       | HAVE_X11     |
| libXext    | X11 extensions          | —            |
| libopus    | Opus audio encoding     | HAVE_OPUS    |
| libvpx     | VP9 video encoding      | HAVE_VPX     |
| libpam     | PAM authentication      | HAVE_PAM     |

### 9.3 Build Commands

```bash
meson setup build
meson compile -C build
meson install -C build
```
