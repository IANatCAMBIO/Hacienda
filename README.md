# Hacienda

Hacienda is my take on an Apple Reminders–style task list, coded in
classic C with GTK3 and SQLite — the companion app to
[Blue Notes](../orange_notes/), built the same way and **with the help
of Claude Code for edits, testing, and code organization**. No
Electron or interpreted code. Low resource usage, and runs the same on
macOS and Linux.

![Hacienda](Screenshot.png)

TLDR; your tasks live in a single SQLite file you can take anywhere.
You organize them in a Library window — lists in the sidebar (plus
rolled-up views for pinned, due-today and due-tomorrow tasks across
every list), tasks as tall rows that show the notes preview, subtasks
and a color-coded due date at a glance — and each task opens in its
own editor window: notes, one level of subtasks, file attachments, a
due date typed or picked from a calendar. It syncs both ways with
Google Tasks, so the phone in your pocket stays current. And if you
keep meeting notes in Blue Notes, its `!` action items can appear
right in the sidebar, checkable from here.

Want more detail?

- **[User Guide](User_Guide.md)** — everything in depth: the library,
  the task editor, settings, storage, Google Tasks sync, and the Blue
  Notes integration.
- **[Internals](Internals.md)** — for the curious: code layout, the
  database schema, and how the sync engine thinks.

## Syncing with Google Tasks

Press **Sync** and sign in once: the app opens your browser for
Google's consent page and receives its tokens over a localhost
redirect (RFC 8252 + PKCE). After that, syncs run silently — a
refresh token scoped to Google Tasks only is kept in `hacienda.ini`,
and the browser never reappears unless you Sign Out or revoke the
grant at myaccount.google.com/permissions.

Building it yourself? You also need to give your build an OAuth
client (it identifies the app to Google and grants nothing by
itself): in the Google Cloud console enable the **Google Tasks API**,
create a **Desktop app** OAuth client, and either drop the downloaded
`client_secret….json` next to the binary or bake the id and secret in
via `client_credentials.mk` (gitignored) so end users skip the step
entirely. The [User Guide](User_Guide.md) covers what syncs, what
stays local, and how conflicts resolve.

## Building

You'll need a C compiler, the GTK3, SQLite3 and libcurl development
files, and pkg-config. That's it.

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
