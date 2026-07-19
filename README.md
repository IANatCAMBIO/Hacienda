# Hacienda

hacienda /hä″sē-ĕn′də, ä″sē-/
noun

    1. A large estate in a Spanish-speaking region.  
    2. The house of the owner of such an estate.  
    3. A large estate where work of any kind is done, as agriculture, manufacturing, mining, or raising of animals; a cultivated farm, with a good house, in distinction from a farming establishment with rude huts for herdsmen, etc.; -- a word used in Spanish-American regions. 

Like a traditional hacienda, this app is desinged to help you get things done. 

Make lists of things. If your things are tasks, you can give them a due date. If the task is too big, break it down into subtasks.

Hacienda is written in classic C with GTK3 and SQLite. It is a companion app to
[Blue Notes](https://github.com/IANatCAMBIO/blue_notes), built the same way and **with the help
of Claude Code for edits, testing, and code organization**. 
No Electron or interpreted code. 
Low resource usage, and runs on macOS and Linux.

![Hacienda](Screenshot.png)

TLDR; Your tasks live in a single SQLite file you can take anywhere.
You organize them in a Library window — lists in the sidebar, tasks in a listview. 
Add subtasks and a color-coded due date to each item. Hacienda, also syncs both ways with
Google Tasks and Blue Notes action items. 

Want more detail?

- **[User Guide](User_Guide.md)** — everything in depth: the library,
  the task editor, settings, storage, Google Tasks sync, and the Blue
  Notes integration.
- **[Internals](Internals.md)** — for the curious: code layout, the
  database schema, and how the sync engine thinks.

## Syncing with Google Tasks

Open the Settings window to enable Google Tasks sync and login. Once authenticated,
Hacienda will automatically sync on the interval specified in settings. 

## Syncing with Blue Notes

You can also enable sync with Blue Notes in the Settings window. Just specify the path to 
your Blue Notes binary, and tell Hacienda where to put the Action Items. 


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

The Makefile auto-detects `gtk-mac-integration-gtk3`; if you install
it later, rebuild from clean (`make clean && make`) so every file sees
it. On macOS, `make app` wraps the binary into
`dist/Hacienda-<version>.app` (it still links against the MacPorts GTK
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
