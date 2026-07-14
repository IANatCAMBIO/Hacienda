# =============================================================================
# Blue Tasks — Makefile
#
# Builds the Blue Tasks application (a GTK3 + SQLite task-list app written
# in plain C — the companion app to Blue Notes).  Requires GTK3, SQLite3
# and libcurl, discovered via pkg-config.
#
# On macOS with MacPorts:
#     sudo port install pkgconf gtk3 +quartz curl
#
# Targets:
#     make          — build the `blue_tasks` binary
#     make clean    — remove build artifacts
#     make run      — build and launch the app
# =============================================================================

# Semantic version — the single source: baked into the binary (BT_VERSION,
# shown in the About dialog).
VERSION  := 1.0.0

# The compiler to use.  clang is the system compiler on macOS.
CC       := cc

# pkg-config binary.  MacPorts installs into /opt/local/bin, which may not
# be on PATH in every shell, so fall back to the absolute path if needed.
PKGCONF  := $(shell command -v pkg-config 2>/dev/null || echo /opt/local/bin/pkg-config)

# Compiler flags: C11, broad warnings, debug symbols, plus the include
# paths for GTK3, SQLite3 and libcurl from pkg-config.
CFLAGS   := -std=c11 -Wall -Wextra -g \
            -DBT_VERSION='"$(VERSION)"' \
            $(shell $(PKGCONF) --cflags gtk+-3.0 sqlite3 libcurl)

# Linker flags: the GTK3, SQLite3 and libcurl libraries, plus libm.
LDFLAGS  := $(shell $(PKGCONF) --libs gtk+-3.0 sqlite3 libcurl) -lm

# Optional macOS menu-bar integration (MacPorts: gtk-osx-application-gtk3;
# the pkg-config module is gtk-mac-integration-gtk3).  When present, the
# Settings window offers moving the menu into the native macOS menu bar;
# without it the option shows as unavailable.  After toggling the
# dependency, run `make clean && make` so every object sees the new flags.
HAVE_GTKOSX := $(shell $(PKGCONF) --exists gtk-mac-integration-gtk3 && echo 1)
ifeq ($(HAVE_GTKOSX),1)
CFLAGS  += -DHAVE_GTKOSX $(shell $(PKGCONF) --cflags gtk-mac-integration-gtk3)
LDFLAGS += $(shell $(PKGCONF) --libs gtk-mac-integration-gtk3)
endif

# The app's own Google OAuth client, baked into the binary so users just
# click Sync and sign in — no configuration.  One-time developer setup:
# copy client_credentials.mk.example to client_credentials.mk (gitignored)
# and fill in the id/secret of a "Desktop app" OAuth client from the
# Google Cloud console (Google Tasks API enabled).  Installed-app client
# secrets are not confidential (Google's own docs) — baking them in is
# the standard desktop-app pattern.
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
BIN      := blue_tasks

# Default target: build the application binary.
all: $(BIN)

# Link all object files into the final binary.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

# Compile each .c file into a .o in build/.  Every object depends on all
# headers for simplicity (the project is small enough that full rebuilds
# on header change are cheap), and on the Makefile so a VERSION bump
# recompiles the baked-in BT_VERSION.
build/%.o: src/%.c $(wildcard src/*.h) Makefile
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

# Build and launch the application.
run: $(BIN)
	./$(BIN)

# Remove all build artifacts.
clean:
	rm -rf build $(BIN)

.PHONY: all run clean
