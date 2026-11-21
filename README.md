# TelegramFS

## Installation

On Windows x64, install WinFsp (**Core**) and the Visual C++ Redistributable (x64). [Latest WinFsp release](https://github.com/winfsp/winfsp/releases/latest) · [Visual C++](https://aka.ms/vs/17/release/vc_redist.x64.exe)

Download `TelegramFS-windows-x64` from **Actions → Artifacts**, extract the ZIP, and open PowerShell in the folder containing `telegramfs.exe`.

## Quick start

1. Run `.\telegramfs.exe login`. Get your `API ID` and `API hash` under **API development tools**. [my.telegram.org](https://my.telegram.org)
2. Run `.\telegramfs.exe channel-create TelegramFS` to create a private channel. To use an existing private channel, find its ID with `.\telegramfs.exe channels`.
3. Run `notepad (.\telegramfs.exe config)`. Under `[telegram]`, replace `channel_id = 0` with the channel ID, including the minus sign, and save.
4. Run `.\telegramfs.exe mount`. The drive appears under **This PC**. Run `.\telegramfs.exe unmount` to unmount it.

## Configuration

Unmount and remount the drive to apply changes.

| Option | Default | Description |
| --- | --- | --- |
| `filesystem.mount_point` | `"T:"` | Any available drive letter. |
| `filesystem.auto_mount` | `false` | Set to `true` and run `.\telegramfs.exe startup enable` to mount at Windows sign-in. |
| `filesystem.read_only` | `false` | Set to `true` to mount the drive as read-only. |
| `telegram.channel_id` | `0` | Negative ID of the private channel. |
| `telegram.import_channels` | `[]` | DriveInfinity channel IDs to open in read-only mode. |
| `telegram.parallel_downloads`, `telegram.parallel_uploads` | `4` each | From 1 to 16 concurrent transfers per direction. |
| `telegram.poll_seconds` | `60` | How often to check for changes, from 10 to 3600 seconds. |
| `cache.memory_limit_mb` | `64` | From 0 to 4096 MiB. Set to `0` to disable the memory cache. |
| `cache.disk_limit_gb` | `10` | From 1 to 4096 GiB of disk cache. |
| `cache.prefetch` | `true` | Enables read-ahead when at least two concurrent downloads are allowed. |
| `cache.write_back` | `true` | With `true`, a flush confirms the local save. With `false`, a flush waits for Telegram to confirm. |
| `security.encryption` | `false` | For a new channel, run `.\telegramfs.exe key-init` before setting this to `true`. Use `key-restore` to recover a channel key. |
| `logging.level` | `"error"` | `"error"` or `"info"`. |

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos. [WinFsp](https://github.com/winfsp/winfsp)