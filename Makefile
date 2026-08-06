# =============================================================================
# Lists — Makefile
#
# Builds the Lists application (a GTK3 + SQLite task-list app written
# in plain C — the companion app to Blue Notes).  Requires GTK3, SQLite3
# and libcurl, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk3 +quartz curl
#
# Targets:
#     make          — build the `lists` binary
#     make clean    — remove build artifacts (including dist/)
#     make run      — build and launch the app
#     make app      — macOS .app bundle → dist/Lists-<version>.app
#                     (needs the macOS sips/iconutil tools; the bundle
#                     still depends on the MacPorts GTK libraries)
# =============================================================================

# Semantic version — the single source: baked into the binary (BT_VERSION,
# shown in the About dialog).
VERSION  := 3.0.5

# The compiler to use.  clang is the system compiler on macOS.
CC       := cc

# pkg-config binary.  MacPorts installs into /opt/local/bin, which may not
# be on PATH in every shell, so fall back to the absolute path if needed.
PKGCONF  := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# Optional macOS menu-bar integration (MacPorts: gtk-osx-application-gtk3;
# the pkg-config module is gtk-mac-integration-gtk3).  When present, the
# Settings window offers moving the menu into the native macOS menu bar;
# without it the option shows as unavailable.  After toggling the
# dependency, run `make clean && make` so every object sees the new flags.
#
# Detected BEFORE the flags below because it joins the pkg-config module
# list rather than appending a second --libs run: gtk-mac-integration
# depends on GTK itself, so two separate runs hand the linker every GTK
# library twice ("ld: warning: ignoring duplicate libraries").  One run
# over all modules lets pkg-config collapse them.
HAVE_GTKOSX := $(shell $(PKGCONF) --exists gtk-mac-integration-gtk3 && echo 1)

# Every pkg-config module the build needs, resolved in a single query.
PKGS     := gtk+-3.0 sqlite3 libcurl
ifeq ($(HAVE_GTKOSX),1)
PKGS    += gtk-mac-integration-gtk3
endif

# Compiler flags: C11, broad warnings, debug symbols, plus the include
# paths for the modules above.
CFLAGS   := -std=c11 -Wall -Wextra -g \
            -DBT_VERSION='"$(VERSION)"' \
            $(shell $(PKGCONF) --cflags $(PKGS))
ifeq ($(HAVE_GTKOSX),1)
CFLAGS  += -DHAVE_GTKOSX
endif

# Linker flags: those same libraries, plus libm.
LDFLAGS  := $(shell $(PKGCONF) --libs $(PKGS)) -lm

# The app's own Google OAuth client, baked into the binary so users just
# click Sync and sign in — no configuration.  One-time developer setup:
# create client_credentials.mk (gitignored) with two lines —
#   GOOGLE_CLIENT_ID     = <id>.apps.googleusercontent.com
#   GOOGLE_CLIENT_SECRET = <secret>
# from a "Desktop app" OAuth client in the Google Cloud console (Google
# Tasks API enabled).  Installed-app client secrets are not confidential
# (Google's own docs) — baking them in is the standard desktop-app
# pattern.
-include client_credentials.mk
ifneq ($(GOOGLE_CLIENT_ID),)
CFLAGS  += -DBT_GOOGLE_CLIENT_ID='"$(GOOGLE_CLIENT_ID)"'
endif
ifneq ($(GOOGLE_CLIENT_SECRET),)
CFLAGS  += -DBT_GOOGLE_CLIENT_SECRET='"$(GOOGLE_CLIENT_SECRET)"'
endif

# All C source files that make up the application.
SRCS     := src/main.c \
            src/app.c \
            src/bnotes.c \
            src/bnsync.c \
            src/db.c \
            src/json.c \
            src/http.c \
            src/oauth.c \
            src/gtasks.c \
            src/library_window.c \
            src/editor_window.c \
            src/settings_window.c

# Object files derived from the source list (build/ mirrors src/).
OBJS     := $(SRCS:src/%.c=build/%.o)

# The final executable name.
BIN      := lists

# Default target: build the application binary (and keep the clangd
# compilation database fresh — it only regenerates on Makefile changes).
all: $(BIN) compile_commands.json

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/.  Every object depends on all
# headers for simplicity (the project is small enough that full rebuilds
# on header change are cheap), and on the Makefile so a VERSION bump
# recompiles the baked-in BT_VERSION.
build/%.o: src/%.c $(wildcard src/*.h) Makefile $(wildcard client_credentials.mk)
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# --- clangd / IDE support ----------------------------------------------------
# compile_commands.json gives clangd the same include paths as the real
# build (without it the IDE reports "gtk/gtk.h not found").  Regenerated
# whenever the Makefile changes; machine-specific, so it stays gitignored.
# JSONFLAGS escapes the double quotes in CFLAGS (e.g. -DBT_VERSION='"…"')
# for embedding in a JSON string: make turns each " into \\\", the shell's
# double-quoting collapses that to \", which is what JSON needs.
JSONFLAGS := $(subst ",\\\",$(CFLAGS))

compile_commands.json: Makefile $(wildcard client_credentials.mk)
	@{ echo '['; \
	first=1; \
	for f in $(SRCS); do \
	  [ $$first -eq 1 ] || echo ','; first=0; \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s -c %s"}' \
	    "$(CURDIR)" "$$f" "$(CC) $(JSONFLAGS)" "$$f"; \
	done; \
	echo; echo ']'; } > $@
	@echo "wrote $@"

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# Remove all build artifacts.
clean:
	rm -rf build $(BIN) $(DIST)

# =============================================================================
# Optional packaging targets — everything lands in dist/.
# =============================================================================

DIST     := dist

# --- macOS .app bundle -------------------------------------------------------
# A minimal bundle around the binary: icons/ and the defaults ini sit next
# to the executable inside Contents/MacOS (the app resolves both relative
# to argv[0]).  document.png becomes the bundle icon via sips + iconutil.
# The binary still links against the MacPorts GTK dylibs (absolute install
# names), so the bundle runs on this machine but is NOT self-contained.
# The live lists.ini is NEVER copied (it holds the refresh token);
# the OAuth client json IS copied when present so Sync sign-in works from
# the bundle (installed-app client secrets are not confidential — the
# same rationale as the baked client_credentials.mk defaults).

APP_DIR  := $(DIST)/Lists-$(VERSION).app
ICONSET  := $(DIST)/document.iconset

app: $(BIN)
	@command -v iconutil >/dev/null || \
	  { echo "error: iconutil/sips not found — 'make app' is macOS-only"; \
	    exit 1; }
	rm -rf "$(APP_DIR)" "$(ICONSET)"
	mkdir -p "$(APP_DIR)/Contents/MacOS" "$(APP_DIR)/Contents/Resources" \
	         "$(ICONSET)"
	# The executable is named "Lists": for NIB-less apps (the
	# gtkosx menubar is built programmatically) macOS titles the app
	# menu with the PROCESS name, not CFBundleName — the binary's
	# filename is the only lever.  argv[0]-relative lookups (icons,
	# ini, client json) resolve by directory, so the rename is harmless.
	cp $(BIN) "$(APP_DIR)/Contents/MacOS/Lists"
	cp -R icons "$(APP_DIR)/Contents/MacOS/icons"
	cp lists.ini.defaults "$(APP_DIR)/Contents/MacOS/"
	@if [ -f client_secret.apps.googleusercontent.com.json ]; then \
	  cp client_secret.apps.googleusercontent.com.json \
	     "$(APP_DIR)/Contents/MacOS/"; \
	fi
	find "$(APP_DIR)" -name .DS_Store -delete
	for sz in 16 32 128 256 512; do \
	  sips -z $$sz $$sz icons/document.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}.png" >/dev/null; \
	  dbl=$$((sz * 2)); \
	  sips -z $$dbl $$dbl icons/document.png \
	       --out "$(ICONSET)/icon_$${sz}x$${sz}@2x.png" >/dev/null; \
	done
	iconutil -c icns -o "$(APP_DIR)/Contents/Resources/document.icns" \
	         "$(ICONSET)"
	rm -rf "$(ICONSET)"
	printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	  '<plist version="1.0">' \
	  '<dict>' \
	  '  <key>CFBundleName</key><string>Lists</string>' \
	  '  <key>CFBundleDisplayName</key><string>Lists</string>' \
	  '  <key>CFBundleIdentifier</key><string>org.example.lists</string>' \
	  '  <key>CFBundleExecutable</key><string>Lists</string>' \
	  '  <key>CFBundleIconFile</key><string>document</string>' \
	  '  <key>CFBundlePackageType</key><string>APPL</string>' \
	  '  <key>CFBundleShortVersionString</key><string>$(VERSION)</string>' \
	  '  <key>CFBundleVersion</key><string>$(VERSION)</string>' \
	  '  <key>NSHighResolutionCapable</key><true/>' \
	  '</dict>' \
	  '</plist>' \
	  > "$(APP_DIR)/Contents/Info.plist"
	@echo "built $(APP_DIR)"

.PHONY: all run clean app
