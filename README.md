# systemd-recalld

**The way people think Windows Recall works.**

systemd-recalld is a desktop audio and video capture daemon for Linux,
designed to work with systemd. It continuously captures your desktop
activity — audio from microphones and system output, video from your
display — and stores it locally with strong encryption. A companion
query service powered by AI models lets you search through your captured
history using natural language.

> **Note:** systemd-recalld is *not* an official component of the
> [systemd project](https://github.com/systemd/systemd). It is an
> independent project that integrates with systemd through its public
> APIs (sd-bus, sd-daemon, sd-notify, service units).

Unlike Microsoft's Windows Recall which is purely local, systemd-recalld
is designed with a client-server architecture: the capture daemon runs
on your workstation while the query service can run on a separate
machine with GPU acceleration for AI inference.

## Architecture

```
┌─────────────────────────────────────────────┐
│                  Client                     │
│                                             │
│  ┌─────────────┐  ┌──────────────────────┐  │
│  │ Audio       │  │ Video                │  │
│  │ Capture     │  │ Capture              │  │
│  │ (ALSA/      │  │ (X11/Wayland         │  │
│  │  PulseAudio)│  │  reverse VNC)        │  │
│  └──────┬──────┘  └──────────┬───────────┘  │
│         │                    │              │
│         └────────┬───────────┘              │
│                  ▼                          │
│         ┌────────────────┐                  │
│         │ Storage        │                  │
│         │ (SQLite +      │                  │
│         │  AES-256-GCM)  │                  │
│         └────────────────┘                  │
│                  │                          │
│         ┌────────────────┐                  │
│         │ D-Bus Service  │◄── recallctl     │
│         └────────────────┘                  │
└─────────────────────────────────────────────┘
                   │
                   ▼ (transmission)
┌─────────────────────────────────────────────┐
│                  Server                     │
│                                             │
│  ┌────────────────┐  ┌──────────────────┐   │
│  │ HTTP/TLS API   │  │ PAM Auth + JWT   │   │
│  └───────┬────────┘  └──────────────────┘   │
│          ▼                                  │
│  ┌────────────────────────────────────────┐ │
│  │ Query Service                          │ │
│  │ (Whisper, CLIP, BERT, Flamingo)        │ │
│  └────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

## Components

### systemd-recalld (Client Daemon)

- **Audio capture** via ALSA or PulseAudio with Opus encoding, silence
  detection, and noise reduction
- **Video capture** via X11 (XGetImage) or Wayland (xdg-desktop-portal)
  with H.264/HEVC hardware-accelerated encoding
- **Encrypted storage** using SQLite metadata with AES-256-GCM encrypted
  data files
- **D-Bus interface** for runtime control
- **Privacy controls** with application exclusion lists and smart privacy
  mode

### recallctl (Command-Line Tool)

Control the capture daemon via D-Bus:

```
recallctl status          Show capture status
recallctl start           Start capturing
recallctl stop            Stop capturing
recallctl pause           Pause capturing
recallctl resume          Resume capturing
recallctl list [TYPE]     List captures (audio, video, screenshot)
```

### systemd-recall-query-service (Server)

- **REST API** with TLS and JWT authentication
- **PAM authentication** for user credentials
- **AI-powered search** using Whisper (audio), CLIP (video), BERT (text),
  and Flamingo (multimodal) models
- **Configurable** rate limiting, caching, and result filtering

## Building

Requirements:

- Meson >= 0.60.0
- GCC or Clang with C11 support
- libsystemd >= 249
- SQLite >= 3.35.0
- OpenSSL >= 1.1.0
- D-Bus

Optional:

- ALSA (libasound) — for ALSA audio capture
- PulseAudio (libpulse) — for PulseAudio audio capture
- X11 (libX11, libXext) — for X11 video capture
- libopus — for Opus audio encoding
- libvpx — for VP9 video encoding
- PAM (libpam) — for PAM authentication

```bash
meson setup build
meson compile -C build
meson install -C build
```

## Configuration

### Client

Edit `/etc/systemd/recalld/recalld.conf`:

```ini
[Service]
Enabled=true
StoragePath=/var/lib/systemd/recalld
MaxStorageSize=50G
RetentionDays=30

[AudioCapture]
Enabled=true
SampleRate=48000
BitrateKbps=128

[VideoCapture]
Enabled=true
Codec=h264
FrameRate=15

[Privacy]
ExcludeApplications=password-manager,banking-app
EncryptionEnabled=true
ConsentRequired=true
```

### Server

Edit `/etc/systemd/recall-query-service/recall-query-service.conf`:

```ini
[API]
Port=8080
TLSEnabled=true

[Authentication]
AuthMethod=pam
PAMServiceName=systemd-recall
# Generate JWT secret: openssl rand -base64 64 > jwt-secret
JWTSecret=/etc/systemd/recall-query-service/jwt-secret
```

## Usage

```bash
# Enable and start the capture daemon
systemctl enable --now systemd-recalld

# Check status
recallctl status

# Start/stop/pause capture
recallctl start
recallctl pause
recallctl resume

# List captured data
recallctl list audio
recallctl list video

# Enable the query service (on server)
systemctl enable --now systemd-recall-query-service

# Query via API
curl -X POST https://server:8080/api/v1/auth \
  -d '{"username":"user","password":"pass"}'

curl -X POST https://server:8080/api/v1/query \
  -H "Authorization: Bearer <token>" \
  -d '{"query":"what did I discuss in yesterday meeting?"}'
```

## Privacy & Security

- All captured data is encrypted at rest with AES-256-GCM
- Application and window exclusion lists prevent capturing sensitive content
- Consent is required before capture begins (configurable)
- Data minimization reduces what is stored
- Configurable retention policies automatically purge old data
- Storage quotas prevent unbounded growth
- TLS encryption for all network transmission
- PAM-based authentication for query access
- JWT tokens with configurable expiration

## License

LGPL-2.1-or-later
