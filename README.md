# Drive

Private unlimited backup to Telegram.

Drive automatically backs up your selected folders to a private Telegram channel. It runs in the background, syncs file changes instantly, and lets you restore them at any time.

## Install from .deb

```bash
sudo apt install ./drive_1.0.0_amd64.deb
```

Then search "Drive" in your application menu and launch it.

## Update / Reinstall

To update to a new version, just install the new .deb over the existing one:

```bash
sudo apt install ./drive_<new_version>_amd64.deb
```

Or if building from source:

```bash
git pull
./build-deb.sh 1.1.0
sudo apt install ./drive_1.1.0_amd64.deb
```

To fully remove and reinstall:

```bash
sudo apt remove drive
sudo apt install ./drive_1.0.0_amd64.deb
```

Your backup data in `~/.local/share/drive/` is preserved across reinstalls.

## Build from source

### Dependencies

```bash
sudo apt install build-essential cmake qtbase5-dev libqt5svg5-dev libsqlite3-dev libssl-dev zlib1g-dev
```

TDLib must be built separately (included in `build/` after initial setup):

```bash
cd td && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ../..
```

### Compile

```bash
mkdir build_app && cd build_app
cmake .. -DCMAKE_PREFIX_PATH=../build
make -j$(nproc)
```

### Package

```bash
./build-deb.sh 1.0.0
```

## How it works

1. Launch Drive from your application menu
2. Connect your Telegram account (phone number or QR code)
3. Select folders to back up
4. Drive monitors them and uploads changes automatically
5. Close the window — backup continues in the background via the tray icon
6. Reopen anytime to see status or restore files

## Restoring files

1. Open Drive and click **Restore...** in the Watched Folders section
2. Select which folders to restore (all are checked by default)
3. Choose a destination directory (defaults to `~/Drive-Restore`)
4. Click **Restore** — files are downloaded from Telegram and placed in the destination

Files that still exist locally are copied directly. Files only available in Telegram are downloaded first.

## Data

All data is stored in `~/.local/share/drive/`. No system-level configuration needed.
