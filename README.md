# Vault

Private unlimited file backup powered by Telegram. Files in watched folders are synced automatically to your own Telegram channel — no third-party servers, no subscriptions, no size limits.

## Features

- Automatic sync via inotify — changes are detected and uploaded instantly
- Restore any file or folder from the web UI
- QR code or phone number authentication
- SQLite-backed file index for fast lookups
- Lightweight single-binary with embedded web interface

## Dependencies

- [TDLib](https://github.com/tdlib/td) (Telegram Database Library)
- [Crow](https://github.com/CrowCpp/Crow) (HTTP framework)
- SQLite3
- CMake 3.10+
- C++17 compiler

## Build

```bash
# Clone and build TDLib first (see TDLib docs)
# Then:
mkdir build_app && cd build_app
cmake ..
make
```

## Setup

Set your Telegram API credentials before running:

```bash
export API_ID=your_api_id
export API_HASH=your_api_hash
```

Get these from https://my.telegram.org

## Run

```bash
./build_app/drive
```

Open http://localhost:8080 in your browser.

## How it works

1. Authenticate with your Telegram account
2. Vault creates (or finds) a private channel called "backup"
3. Add folders to watch — files are uploaded as documents to the channel
4. Changes and new files are detected via inotify and synced automatically
5. Restore files through the web interface at any time
