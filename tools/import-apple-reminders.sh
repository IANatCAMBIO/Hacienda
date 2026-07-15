#!/bin/sh
# =============================================================================
# import-apple-reminders.sh — migrate Apple Reminders into Hacienda.
#
# Usage:
#   tools/import-apple-reminders.sh [DB_PATH] [EXPORT_FILE]
#
#   DB_PATH     — the Hacienda database (default:
#                 ~/.local/share/hacienda/hacienda.db).
#   EXPORT_FILE — a previously produced export to (re)import, skipping
#                 the AppleScript step (mainly for testing/reruns).
#
# What it does:
#   1. Exports every Reminders list and reminder via AppleScript (name,
#      notes, due date, completed + completion date).  Property fetches
#      are BATCHED per list — one Apple Event per property, not one per
#      reminder — so large databases export in seconds.  The first run
#      pops macOS's automation-permission prompt for Reminders.
#   2. Imports into the Hacienda database with python3 (stdlib
#      sqlite3): lists are matched by name (existing lists are reused),
#      tasks are appended after existing ones, and a task whose exact
#      title already exists in the target list is skipped — so the
#      script is safe to re-run.
#
# Notes / limitations:
#   - Hacienda must NOT be running (no CLI/socket like Blue Notes; the
#     script writes the database directly).
#   - Apple exposes NO API for Reminders subtasks — they import as
#     top-level tasks of their list.
#   - Reminder due TIMES are dropped (Hacienda due dates are date-only,
#     matching what Google Tasks can sync); priorities and flags are not
#     carried over.
#   - Imported rows are stamped dirty: the NEXT SYNC PUSHES THEM ALL to
#     Google Tasks.  Import 500 reminders, expect 500 creations there.
# =============================================================================

set -e

DB="${1:-$HOME/.local/share/hacienda/hacienda.db}"
EXPORT="${2:-}"

if [ ! -f "$DB" ]; then
    echo "error: no Hacienda database at $DB" >&2
    echo "       (launch Hacienda once to create it, then retry)" >&2
    exit 1
fi

if pgrep -f '\./hacienda' >/dev/null 2>&1 || \
   pgrep -x hacienda >/dev/null 2>&1; then
    echo "error: Hacienda is running — quit it first (the importer" >&2
    echo "       writes the database directly)" >&2
    exit 1
fi

# --- 1. Export from Apple Reminders ------------------------------------------
# Records separated by ASCII RS (0x1E), fields by US (0x1F), so titles
# and note bodies may safely contain newlines and tabs.
#   "L" US <list name> RS
#   "R" US <list> US <title> US <notes> US <due YYYY-MM-DD> US
#       <true|false> US <completed YYYY-MM-DDTHH:MM:SS> RS
if [ -z "$EXPORT" ]; then
    EXPORT=$(mktemp /tmp/reminders-export.XXXXXX)
    trap 'rm -f "$EXPORT"' EXIT
    echo "Exporting from Apple Reminders (this may prompt for permission)..."
    osascript <<'EOS' > "$EXPORT"
on pad2(n)
    set t to n as text
    if length of t is 1 then set t to "0" & t
    return t
end pad2

on isoDate(d)
    if d is missing value then return ""
    return (year of d as text) & "-" & my pad2(month of d as integer) ¬
        & "-" & my pad2(day of d)
end isoDate

on isoDateTime(d)
    if d is missing value then return ""
    return my isoDate(d) & "T" & my pad2(hours of d) & ":" ¬
        & my pad2(minutes of d) & ":" & my pad2(seconds of d)
end isoDateTime

set US to character id 31
set RS to character id 30
set out to {}
tell application "Reminders"
    repeat with L in lists
        set listName to name of L
        set end of out to "L" & US & listName & RS
        -- One Apple Event per PROPERTY, not per reminder.
        set rNames to name of reminders of L
        set rBodies to body of reminders of L
        set rDues to due date of reminders of L
        set rDone to completed of reminders of L
        set rCompl to completion date of reminders of L
        repeat with i from 1 to count of rNames
            set b to item i of rBodies
            if b is missing value then set b to ""
            set end of out to "R" & US & listName & US ¬
                & (item i of rNames) & US & b & US ¬
                & my isoDate(item i of rDues) & US ¬
                & ((item i of rDone) as text) & US ¬
                & my isoDateTime(item i of rCompl) & RS
        end repeat
    end repeat
end tell
set AppleScript's text item delimiters to ""
return out as text
EOS
    echo "Exported $(wc -c < "$EXPORT" | tr -d ' ') bytes."
fi

# --- 2. Import into the Hacienda database ----------------------------------
python3 - "$DB" "$EXPORT" <<'EOF'
import sqlite3, sys, time
from datetime import datetime

db_path, export_path = sys.argv[1], sys.argv[2]
US, RS = "\x1f", "\x1e"
now = int(time.time())

data = open(export_path, encoding="utf-8", errors="replace").read()

con = sqlite3.connect(db_path)
cur = con.cursor()
try:
    cur.execute("SELECT COUNT(*) FROM lists")
    cur.execute("SELECT completed_at FROM tasks LIMIT 0")
except sqlite3.Error:
    sys.exit("error: %s is not a current Hacienda database "
             "(launch the app once to create/migrate it)" % db_path)


def local_midnight(iso):
    """'YYYY-MM-DD' -> unix local midnight, or 0."""
    if not iso:
        return 0
    try:
        return int(datetime.strptime(iso, "%Y-%m-%d").timestamp())
    except ValueError:
        return 0


def local_datetime(iso):
    """'YYYY-MM-DDTHH:MM:SS' -> unix, or 0."""
    if not iso:
        return 0
    try:
        return int(datetime.strptime(iso, "%Y-%m-%dT%H:%M:%S").timestamp())
    except ValueError:
        return 0


def ensure_list(name):
    """Existing visible list of this name, else a new one appended."""
    row = cur.execute(
        "SELECT id FROM lists WHERE name = ? AND deleted = 0",
        (name,)).fetchone()
    if row is not None:
        return row[0], False
    cur.execute(
        "INSERT INTO lists(name, emoji, position, updated_at) VALUES(?,"
        " '', (SELECT COALESCE(MAX(position), 0) + 1 FROM lists), ?)",
        (name, now))
    return cur.lastrowid, True


list_ids = {}                        # list name -> rowid
next_pos = {}                        # list rowid -> next task position
stats = {"lists_new": 0, "lists_reused": 0, "tasks": 0, "skipped": 0}

cur.execute("BEGIN")
for rec in data.split(RS):
    rec = rec.strip("\n")
    if not rec:
        continue
    f = rec.split(US)
    if f[0] == "L" and len(f) >= 2:
        lid, created = ensure_list(f[1])
        list_ids[f[1]] = lid
        stats["lists_new" if created else "lists_reused"] += 1
    elif f[0] == "R" and len(f) >= 7:
        _, list_name, title, notes, due, done, completed = f[:7]
        if not title:
            continue
        lid = list_ids.get(list_name)
        if lid is None:
            lid, created = ensure_list(list_name)
            list_ids[list_name] = lid
            stats["lists_new" if created else "lists_reused"] += 1
        # Re-run safety: an identical title already in the list is the
        # same reminder from a previous import.
        dup = cur.execute(
            "SELECT 1 FROM tasks WHERE list_id = ? AND title = ? AND "
            "deleted = 0 AND parent_id IS NULL", (lid, title)).fetchone()
        if dup is not None:
            stats["skipped"] += 1
            continue
        if lid not in next_pos:
            next_pos[lid] = cur.execute(
                "SELECT COALESCE(MAX(position), 0) + 1 FROM tasks "
                "WHERE list_id = ? AND parent_id IS NULL",
                (lid,)).fetchone()[0]
        is_done = 1 if done == "true" else 0
        completed_at = local_datetime(completed) if is_done else 0
        if is_done and completed_at == 0:
            completed_at = now
        cur.execute(
            "INSERT INTO tasks(list_id, title, notes, due, done, "
            "completed_at, position, updated_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
            (lid, title, notes, local_midnight(due), is_done,
             completed_at, next_pos[lid], now))
        next_pos[lid] += 1
        stats["tasks"] += 1
con.commit()
con.close()

print("Imported %d task(s) into %d list(s) (%d new, %d reused); "
      "%d duplicate(s) skipped." % (
          stats["tasks"], stats["lists_new"] + stats["lists_reused"],
          stats["lists_new"], stats["lists_reused"], stats["skipped"]))
print("Launch Hacienda — the next sync will push the imported tasks "
      "to Google Tasks.")
EOF
