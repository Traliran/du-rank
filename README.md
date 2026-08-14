# du-rank

A tiny, single-file **disk usage analyzer** for Linux / POSIX systems. It tells you
which directories and files are eating the most space on your disk — including all
nested folders and files — with pretty colored output.

```console
$ du-rank --allsys --top 8

du-rank v1.0.0
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

- **Two scan modes**: `--allsys` (whole filesystem from `/`) and `--usr` (your `~`).
- **Top-level breakdown** plus an **all-levels ranking** of the biggest
  directories and files.
- Sizes are based on **allocated blocks** (`st_blocks`), so it reports real disk
  usage, not apparent file size.
- **Constant memory**: only the top N entries are kept, so it stays lightweight
  even on very large filesystems.
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
```

## How it works

The program walks the directory tree depth-first without following symlinks
(so it cannot loop). For every directory it sums the size of everything beneath
it and keeps the top N results in a small min-heap — no big in-memory lists.

Notes:

- When scanning `/`, the virtual/pseudo filesystems `/dev`, `/proc`, `/run` and
  `/sys` are skipped — they hold no real user data.
- Directories you cannot read (permission denied) are skipped silently. For a
  complete scan of `/`, run as root (`sudo`).
- Disk usage is measured in 512-byte blocks (`st_blocks`), matching tools like
  `du`.

## License

GNU GPL
