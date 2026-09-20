# du-rank

A tiny, single-file **disk usage analyzer** for Linux / POSIX systems. It tells you
which directories and files are eating the most space on your disk — including all
nested folders and files — with pretty colored output.

```console
$ du-rank --allsys --top 8

du-rank v1.2.0
============================
Scan path     : /
Total size    : 249.7 GiB
Files         : 1,742,331
Directories   : 302,887
Top N         : 8

TOP-LEVEL ENTRIES (direct children, largest first)
------------------------------------------
   1  173.6 GiB   69.5%  ████████████████..  /usr/
   2   43.1 GiB   17.3%  ████..............  /var/
   3   12.0 GiB    4.8%  ██................  /home/
   4    6.4 GiB    2.6%  █.................  /snap/
   5    4.3 GiB    1.7%  █.................  /opt/

LARGEST DIRECTORIES (all levels)
------------------------------------------
   1  173.6 GiB   69.5%  ████████████████..  /usr/
   2   43.1 GiB   17.3%  ████..............  /var/
   3   38.2 GiB   15.3%  ████..............  /usr/share/
   ...
```

## Features

- **Four scan modes**: `--allsys` (whole filesystem from `/`), `--usr`
  (your `~`), `--drive <NAME>` (one mounted drive by its `/dev` name) and
  `--dubl-scan` (duplicate-file detection).
- **Top-level breakdown** plus an **all-levels ranking** of the biggest
  directories and files.
- **Duplicate scan**: finds groups of files that share the same name, size and
  modification time (`--dubl-scan`).
- Sizes are based on **allocated blocks** (`st_blocks`), so it reports real disk
  usage, not apparent file size.
- **One filesystem per scan**: anything mounted from another device is skipped
  at any depth — `/tmp` on tmpfs, `/dev`, `/proc`, `/sys`, Docker overlay/shm
  mounts under `/var/lib/docker`, other disks under `/mnt` or `/media`, etc.
- **Hard links are counted once**, just like symlinks are never followed (only
  inodes with `nlink > 1` are tracked, in a small lazily-allocated hash set).
- **Constant memory**: only the top N entries are kept, so it stays lightweight
  even on very large filesystems (regular scans).
- Colored, human-readable output with percentage bars (auto-disabled when piped).
- No dependencies beyond the standard C library. No libraries, no config, no bloat.

## Requirements

- Linux (or any POSIX system with `opendir`, `lstat`, `isatty`)
- A C compiler (gcc, clang, …)
- `make`

## Build

```console
make
```

Install system-wide (optional):

```console
sudo make install
# installs to /usr/local/bin/du-rank
```

Clean up:

```console
make clean
```

## Usage

```console
du-rank [OPTIONS] [PATH]
```

| Mode | Description |
| ---- | ----------- |
| `--allsys` | Analyze the whole filesystem, starting at `/` |
| `--usr`    | Analyze the current user's home directory (`~`) |
| `--drive <NAME>` | Analyze one mounted drive by its `/dev` name (e.g. `sdb1`) |
| `--dubl-scan` | Find duplicate files by name + size + mtime |

| Option        | Description |
| ------------- | ----------- |
| `--top <N>`   | Show top N results (default: 10) |
| `--no-color`  | Disable colored output |
| `-h, --help`  | Show help and exit |

If a `PATH` is given it overrides the mode and any directory can be scanned.
With no arguments at all, `--usr` is assumed.

### Examples

```console
# What eats the most space on the whole system?
sudo du-rank --allsys

# What is eating your home directory?
du-rank --usr

# Top 5 biggest files in /var only
du-rank --top 5 /var

# What eats space on the sdb1 drive? (resolved via /proc/self/mounts)
du-rank --drive sdb1

# Find duplicate files (same name + size + mtime) in your home directory
du-rank --dubl-scan --usr
```

## How it works

The program walks the directory tree depth-first without following symlinks
(so it cannot loop). For every directory it sums the size of everything beneath
it and keeps the top N results in a small min-heap — no big in-memory lists.

With `--dubl-scan` the same walk records every regular file (its path, name,
apparent size and modification time), sorts the list by name, then size, then
mtime, and reports any run of ≥2 files that match on all three fields. A group
means the files share a name, a size and a modification time — they are very
likely identical copies. No file contents are compared, and the "reclaimable"
figure shows how much allocated disk space you would free by keeping one copy
per group (hard links are counted once, so they never form a group).

Notes:

- Every scan stays on the scan root's filesystem (like `du -x`): directories
  and files from other devices are skipped wherever they are mounted. That is
  why `--allsys` ignores `/tmp` when it is a tmpfs, Docker's virtual mounts
  deep inside `/var/lib/docker`, and any separately mounted disks. To analyze
  such a disk, point at it directly: `du-rank --drive sdb1` (the name is looked
  up in `/proc/self/mounts`, `/dev/` is prepended when missing) or pass its
  mountpoint as `PATH`.
- When scanning `/`, the virtual/pseudo filesystems `/dev`, `/proc`, `/run` and
  `/sys` are skipped by name as well — they hold no real user data.
- Directories you cannot read (permission denied) are skipped silently. For a
  complete scan of `/`, run as root (`sudo`).
- Disk usage is measured in 512-byte blocks (`st_blocks`), matching tools like
  `du`.
- Regular scans keep only the top N entries in memory. `--dubl-scan` keeps one
  small record per file, so memory use grows with the number of files — use it
  on a specific directory for large filesystems.

## License

GNU GPL
