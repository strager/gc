#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

enum { GC_PROCMAPS_BUFFER_SIZE = 4096 };

typedef enum {
  GC_PROCMAPS_MAPTYPE_OTHER = 0,
  // Should not be read.
  GC_PROCMAPS_MAPTYPE_IGNORE,
  // [anon:gcC]
  GC_PROCMAPS_MAPTYPE_CHUNKS,
  // [anon:gcU]
  GC_PROCMAPS_MAPTYPE_USERDATA,
  // [anon:gcW]
  GC_PROCMAPS_MAPTYPE_WORKQUEUE,
} GC_ProcMapsMapType;

typedef enum {
  // Parsing the start address (before '-').
  GC_PROCMAPS_STATE_START,
  // Parsing the end address (after '-', before ' ').
  GC_PROCMAPS_STATE_END,
  // Skipping the perms, offset, dev, and inode fields.
  GC_PROCMAPS_STATE_FIELDS,
  // Skipping the spaces between the inode and the pathname.
  GC_PROCMAPS_STATE_PATH_SPACES,
  // Matching the pathname with the GC_ProcMapsPathState_ DFA.
  GC_PROCMAPS_STATE_PATH,
  // Pathname did not match. Skipping the rest of the line (until '\n').
  GC_PROCMAPS_STATE_SKIP_TO_ENTRY_END,
  // Malformed line. Skipping the rest of the line (until '\n') without
  // reporting an entry.
  GC_PROCMAPS_STATE_SKIP_LINE,
  // read() returned EOF or failed.
  GC_PROCMAPS_STATE_DONE,
} GC_ProcMapsState;

// DFA states for matching the pathname.
//
// A literal state matches a fixed string one character at a time (tracked by
// GC_ProcMaps::counter), then moves to the state after the literal. A branch
// state picks the next state based on one character. An accept state has no
// transitions and gives the map type if the pathname ends there. The
// transitions are in the GC_PROCMAPS_STATE_PATH case of GC_procmaps_next.
typedef enum {
  // No pattern can match. Must be 0.
  GC_PROCMAPS_PATH_DEAD_ = 0,

  // Literal "[", then KIND.
  GC_PROCMAPS_PATH_INITIAL_,
  // Branch after "[": 'a' -> ANON, 'v' -> VVAR.
  GC_PROCMAPS_PATH_KIND_,

  // [anon:gc?]
  // Literal "non:gc", then ANON_GC.
  GC_PROCMAPS_PATH_ANON_,
  // Branch after "[anon:gc": 'C', 'U', 'W'.
  GC_PROCMAPS_PATH_ANON_GC_,
  // Literal "]", then the matching accept state.
  GC_PROCMAPS_PATH_ANON_GC_C_,
  GC_PROCMAPS_PATH_ANON_GC_U_,
  GC_PROCMAPS_PATH_ANON_GC_W_,
  // Accept states (no transitions).
  GC_PROCMAPS_PATH_ACCEPT_CHUNKS_,
  GC_PROCMAPS_PATH_ACCEPT_USERDATA_,
  GC_PROCMAPS_PATH_ACCEPT_WORKQUEUE_,

  // [vvar] and [vvar_vclock]
  // Literal "var", then VVAR_END.
  GC_PROCMAPS_PATH_VVAR_,
  // Branch after "[vvar": ']' -> ACCEPT_IGNORE, '_' -> VVAR_VCLOCK.
  GC_PROCMAPS_PATH_VVAR_END_,
  // Literal "vclock]", then ACCEPT_IGNORE.
  GC_PROCMAPS_PATH_VVAR_VCLOCK_,
  // Accept state (no transitions).
  GC_PROCMAPS_PATH_ACCEPT_IGNORE_,

  GC_PROCMAPS_PATH_STATE_COUNT_,
} GC_ProcMapsPathState_;

_Static_assert(GC_PROCMAPS_PATH_STATE_COUNT_ <= 256,
               "path_state is uint8_t");

// Number of space-separated fields between the address range and the
// pathname: perms, offset, dev, inode.
enum { GC_PROCMAPS_FIELD_COUNT_ = 4 };

typedef struct {
  int fd;
  GC_ProcMapsState state;
  // START, END: number of hex digits parsed in the current address.
  // FIELDS: number of fields completed.
  // PATH: number of characters of the current literal matched.
  unsigned counter;
  // FIELDS: whether the previous character was part of a field.
  bool in_field;
  // PATH: current GC_ProcMapsPathState_.
  uint8_t path_state;
  // Whether the current line's perms field has 'r' (PROT_READ).
  bool pending_readable;
  // buffer[buffer_pos..buffer_len] is unconsumed data.
  size_t buffer_pos;
  size_t buffer_len;
  char buffer[GC_PROCMAPS_BUFFER_SIZE];

  // Parse results.
  uint64_t start;
  uint64_t end;
  GC_ProcMapsMapType maptype;
} GC_ProcMaps;

// Takes ownership of fd.
static void GC_procmaps_open_fd(GC_ProcMaps *procmaps, int fd);

// Opens /proc/self/maps.
static void GC_procmaps_open(GC_ProcMaps *procmaps);

static bool GC_procmaps_next(GC_ProcMaps *procmaps);

// Closes fd.
static void GC_procmaps_close(GC_ProcMaps *procmaps);

static void GC_procmaps_open_fd(GC_ProcMaps *procmaps, int fd) {
  procmaps->fd = fd;
  procmaps->state = fd < 0 ? GC_PROCMAPS_STATE_DONE : GC_PROCMAPS_STATE_START;
  procmaps->counter = 0;
  procmaps->in_field = false;
  procmaps->path_state = GC_PROCMAPS_PATH_DEAD_;
  procmaps->buffer_pos = 0;
  procmaps->buffer_len = 0;
  procmaps->start = 0;
  procmaps->end = 0;
  procmaps->maptype = GC_PROCMAPS_MAPTYPE_OTHER;
  procmaps->pending_readable = false;
}

static void GC_procmaps_open(GC_ProcMaps *procmaps) {
  int fd;
  do {
    fd = open("/proc/self/maps", O_RDONLY);
  } while (fd < 0 && errno == EINTR);
  GC_procmaps_open_fd(procmaps, fd);
}

static void GC_procmaps_close(GC_ProcMaps *procmaps) {
  if (procmaps->fd >= 0) {
    close(procmaps->fd);
    procmaps->fd = -1;
  }
  procmaps->state = GC_PROCMAPS_STATE_DONE;
}

// Returns 0-15 for a hex digit, or -1 otherwise.
static int GC_procmaps_hex_digit_(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Refills the buffer. Returns false on EOF or error.
static bool GC_procmaps_fill_(GC_ProcMaps *procmaps) {
  for (;;) {
    ssize_t n = read(procmaps->fd, procmaps->buffer, sizeof(procmaps->buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    procmaps->buffer_pos = 0;
    procmaps->buffer_len = (size_t)n;
    return true;
  }
}

// Called at the '\n' ending an entry. Publishes the map type (IGNORE if the
// mapping is not readable) and prepares for the next line.
static void GC_procmaps_finish_entry_(GC_ProcMaps *procmaps,
                                      GC_ProcMapsMapType maptype) {
  procmaps->maptype =
      procmaps->pending_readable ? maptype : GC_PROCMAPS_MAPTYPE_IGNORE;
  procmaps->state = GC_PROCMAPS_STATE_START;
  procmaps->counter = 0;
}

static bool GC_procmaps_next(GC_ProcMaps *procmaps) {
  for (;;) {
    if (procmaps->state == GC_PROCMAPS_STATE_DONE) return false;

    if (procmaps->buffer_pos == procmaps->buffer_len) {
      if (!GC_procmaps_fill_(procmaps)) {
        procmaps->state = GC_PROCMAPS_STATE_DONE;
        return false;
      }
    }

    char c = procmaps->buffer[procmaps->buffer_pos];
    procmaps->buffer_pos += 1;
    switch (procmaps->state) {
    case GC_PROCMAPS_STATE_START: {
      int digit = GC_procmaps_hex_digit_(c);
      if (digit >= 0) {
        // Reset on the first digit so the previous result stays readable
        // until the next line starts.
        uint64_t prev = procmaps->counter == 0 ? 0 : procmaps->start;
        procmaps->start = (prev << 4) | (uint64_t)digit;
        procmaps->counter += 1;
      } else if (c == '-' && procmaps->counter > 0) {
        procmaps->state = GC_PROCMAPS_STATE_END;
        procmaps->end = 0;
        procmaps->counter = 0;
      } else if (c == '\n') {
        // Empty or malformed line.
        procmaps->counter = 0;
      } else {
        procmaps->state = GC_PROCMAPS_STATE_SKIP_LINE;
      }
      break;
    }

    case GC_PROCMAPS_STATE_END: {
      int digit = GC_procmaps_hex_digit_(c);
      if (digit >= 0) {
        procmaps->end = (procmaps->end << 4) | (uint64_t)digit;
        procmaps->counter += 1;
      } else if (c == ' ' && procmaps->counter > 0) {
        procmaps->state = GC_PROCMAPS_STATE_FIELDS;
        procmaps->counter = 0;
        procmaps->in_field = false;
        procmaps->pending_readable = false;
      } else if (c == '\n' && procmaps->counter > 0) {
        // "start-end\n": no other fields.
        procmaps->pending_readable = false;
        GC_procmaps_finish_entry_(procmaps, GC_PROCMAPS_MAPTYPE_OTHER);
        return true;
      } else if (c == '\n') {
        procmaps->state = GC_PROCMAPS_STATE_START;
        procmaps->counter = 0;
      } else {
        procmaps->state = GC_PROCMAPS_STATE_SKIP_LINE;
      }
      break;
    }

    case GC_PROCMAPS_STATE_FIELDS:
      if (c == '\n') {
        GC_procmaps_finish_entry_(procmaps, GC_PROCMAPS_MAPTYPE_OTHER);
        return true;
      } else if (c == ' ') {
        if (procmaps->in_field) {
          procmaps->in_field = false;
          procmaps->counter += 1;
          if (procmaps->counter == GC_PROCMAPS_FIELD_COUNT_) {
            procmaps->state = GC_PROCMAPS_STATE_PATH_SPACES;
          }
        }
      } else {
        if (procmaps->counter == 0 && !procmaps->in_field) {
          // First character of the perms field (e.g. "r-xp").
          procmaps->pending_readable = c == 'r';
        }
        procmaps->in_field = true;
      }
      break;

    case GC_PROCMAPS_STATE_PATH_SPACES:
      if (c == '\n') {
        // No pathname.
        GC_procmaps_finish_entry_(procmaps, GC_PROCMAPS_MAPTYPE_OTHER);
        return true;
      } else if (c != ' ') {
        procmaps->state = GC_PROCMAPS_STATE_PATH;
        procmaps->path_state = GC_PROCMAPS_PATH_INITIAL_;
        procmaps->counter = 0;
        // Reprocess this character in the PATH state.
        procmaps->buffer_pos -= 1;
      }
      break;

    case GC_PROCMAPS_STATE_PATH: {
      GC_ProcMapsPathState_ path_state =
          (GC_ProcMapsPathState_)procmaps->path_state;
      if (c == '\n') {
        // End of pathname. Literal and branch states don't accept, so a partial
        // match is not a match.
        GC_ProcMapsMapType maptype;
        switch (path_state) {
        case GC_PROCMAPS_PATH_ACCEPT_CHUNKS_:    maptype = GC_PROCMAPS_MAPTYPE_CHUNKS; break;
        case GC_PROCMAPS_PATH_ACCEPT_USERDATA_:  maptype = GC_PROCMAPS_MAPTYPE_USERDATA; break;
        case GC_PROCMAPS_PATH_ACCEPT_WORKQUEUE_: maptype = GC_PROCMAPS_MAPTYPE_WORKQUEUE; break;
        case GC_PROCMAPS_PATH_ACCEPT_IGNORE_:    maptype = GC_PROCMAPS_MAPTYPE_IGNORE; break;
        default:                                 maptype = GC_PROCMAPS_MAPTYPE_OTHER; break;
        }
        GC_procmaps_finish_entry_(procmaps, maptype);
        return true;
      }

      // Literal states set literal and after_literal. Branch states set
      // path_state directly.
      const char *literal = nullptr;
      GC_ProcMapsPathState_ after_literal = GC_PROCMAPS_PATH_DEAD_;
      switch (path_state) {
      // Literal states.
      case GC_PROCMAPS_PATH_INITIAL_:
        literal = "[";
        after_literal = GC_PROCMAPS_PATH_KIND_;
        break;
      case GC_PROCMAPS_PATH_ANON_:
        literal = "non:gc";
        after_literal = GC_PROCMAPS_PATH_ANON_GC_;
        break;
      case GC_PROCMAPS_PATH_ANON_GC_C_:
        literal = "]";
        after_literal = GC_PROCMAPS_PATH_ACCEPT_CHUNKS_;
        break;
      case GC_PROCMAPS_PATH_ANON_GC_U_:
        literal = "]";
        after_literal = GC_PROCMAPS_PATH_ACCEPT_USERDATA_;
        break;
      case GC_PROCMAPS_PATH_ANON_GC_W_:
        literal = "]";
        after_literal = GC_PROCMAPS_PATH_ACCEPT_WORKQUEUE_;
        break;
      case GC_PROCMAPS_PATH_VVAR_:
        literal = "var";
        after_literal = GC_PROCMAPS_PATH_VVAR_END_;
        break;
      case GC_PROCMAPS_PATH_VVAR_VCLOCK_:
        literal = "vclock]";
        after_literal = GC_PROCMAPS_PATH_ACCEPT_IGNORE_;
        break;

      // Branch states.
      case GC_PROCMAPS_PATH_KIND_:
        switch (c) {
        case 'a': path_state = GC_PROCMAPS_PATH_ANON_; break;
        case 'v': path_state = GC_PROCMAPS_PATH_VVAR_; break;
        default:  path_state = GC_PROCMAPS_PATH_DEAD_; break;
        }
        break;
      case GC_PROCMAPS_PATH_ANON_GC_:
        switch (c) {
        case 'C': path_state = GC_PROCMAPS_PATH_ANON_GC_C_; break;
        case 'U': path_state = GC_PROCMAPS_PATH_ANON_GC_U_; break;
        case 'W': path_state = GC_PROCMAPS_PATH_ANON_GC_W_; break;
        default:  path_state = GC_PROCMAPS_PATH_DEAD_; break;
        }
        break;
      case GC_PROCMAPS_PATH_VVAR_END_:
        switch (c) {
        case ']': path_state = GC_PROCMAPS_PATH_ACCEPT_IGNORE_; break;
        case '_': path_state = GC_PROCMAPS_PATH_VVAR_VCLOCK_; break;
        default:  path_state = GC_PROCMAPS_PATH_DEAD_; break;
        }
        break;

      // Accept states (and DEAD) have no transitions.
      default:
        path_state = GC_PROCMAPS_PATH_DEAD_;
        break;
      }

      if (literal != nullptr) {
        if (c != literal[procmaps->counter]) {
          path_state = GC_PROCMAPS_PATH_DEAD_;
        } else {
          procmaps->counter += 1;
          if (literal[procmaps->counter] == '\0') {
            path_state = after_literal;
            procmaps->counter = 0;
          }
        }
      }
      procmaps->path_state = (uint8_t)path_state;
      if (path_state == GC_PROCMAPS_PATH_DEAD_) {
        procmaps->state = GC_PROCMAPS_STATE_SKIP_TO_ENTRY_END;
      }
      break;
    }

    case GC_PROCMAPS_STATE_SKIP_TO_ENTRY_END:
      if (c == '\n') {
        GC_procmaps_finish_entry_(procmaps, GC_PROCMAPS_MAPTYPE_OTHER);
        return true;
      }
      break;

    case GC_PROCMAPS_STATE_SKIP_LINE:
      if (c == '\n') {
        procmaps->state = GC_PROCMAPS_STATE_START;
        procmaps->counter = 0;
      }
      break;

    case GC_PROCMAPS_STATE_DONE:
      __builtin_trap();
      break;
    }
  }
}
