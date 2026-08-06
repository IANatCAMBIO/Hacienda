# Lists

Make lists of things. If your things are tasks, you can give them a due date. If the task is too big, break it down into subtasks.

Lists is written in classic C with GTK3 and SQLite. It is a companion app to
[Records](https://github.com/IANatCAMBIO/records), built the same way and **with the help
of Claude Code for edits, testing, and code organization**. 
No Electron or interpreted code. 
Low resource usage, and runs on macOS and Linux.

![Lists](Screenshot.png)

TLDR; Your tasks live in a single SQLite file you can take anywhere.
You organize them in a Library window — lists in the sidebar, tasks in a listview. 
Add subtasks and a color-coded due date to each item. Lists also syncs both ways with
Google Tasks and Records action items. 

Want more detail?

- **[User Guide](User_Guide.md)** — everything in depth: the library,
  the task editor, settings, storage, Google Tasks sync, and the
  Records integration.
- **[Internals](Internals.md)** — for the curious: code layout, the
  database schema, and how the sync engine thinks.

## Syncing with Google Tasks

Open the Settings window to enable Google Tasks sync and login. Once authenticated,
Lists will automatically sync on the interval specified in settings. 

## Syncing with Records

You can also enable sync with Records in the Settings window. Specify the path to your
Records binary and tell Lists which list to file the Action Items into. Each `!` action
item becomes an ordinary task — with notes, subtasks, attachments and Google Tasks sync
like any other — while ticking it done or changing its due date is batched back to
Records on its own interval. The item's text belongs to the note, so edit that in
Records.


## Building

You'll need a C compiler, the GTK3, SQLite3 and libcurl development
files, and pkg-config.

macOS (MacPorts):

```sh
sudo port install pkgconf gtk3 +quartz curl
sudo port install gtk-osx-application-gtk3   # optional: native menu bar
make
make run
```

Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev \
                 libsqlite3-dev libcurl4-openssl-dev
make
make run
```

Lists uses emoji extensively (list icons, sidebar labels, task
markers). Debian ships the Noto Color Emoji font, but for Apple-style
emoji install the Apple Color Emoji TTF:

```sh
curl -L "https://github.com/samuelngs/apple-emoji-ttf/releases/download/macos-26-20260722-484daf4e/fonts-apple-color-emoji.deb" \
     -o fonts-apple-color-emoji.deb
sudo dpkg -i fonts-apple-color-emoji.deb
```

The Makefile auto-detects `gtk-mac-integration-gtk3`; if you install
it later, rebuild from clean (`make clean && make`) so every file sees
it. On macOS, `make app` wraps the binary into
`dist/Lists-<version>.app` (it still links against the MacPorts GTK
libraries, so the bundle runs on the machine that built it).

One more step if you want Google sync: set up `client_credentials.mk`
before you build, as described below. It is optional — without it the build still succeeds and everything except
the Sync sign-in works; add the file and rebuild whenever you're
ready (the Makefile tracks it, so a plain `make` picks up changes).

### Setting up client_credentials.mk

1. In the [Google Cloud console](https://console.cloud.google.com/),
   create a project (any name) and enable the **Google Tasks API**
   (*APIs & Services → Library*).
2. Configure the OAuth consent screen (*APIs & Services → OAuth
   consent screen*) — the app name you enter there is what the
   browser's consent page will show.
3. Create the client (*APIs & Services → Credentials → Create
   Credentials → OAuth client ID*, application type **Desktop app**)
   and note the client id and secret.
4. In the source directory:
   `cp client_credentials.mk.example client_credentials.mk`, fill in
   the two values, then `make clean && make`.

`client_credentials.mk` is gitignored, so your credentials never end
up in a commit — and Desktop-app client secrets are, per Google's own
docs, not confidential, so shipping them inside a binary you
distribute is the standard pattern. (Alternatively, the app also
accepts the console's downloaded `client_secret….json` placed next to
the binary at runtime — also gitignored — and that file takes
precedence over the baked-in client if both exist.) The
[User Guide](User_Guide.md) covers what syncs, what stays local, and
how conflicts resolve.
