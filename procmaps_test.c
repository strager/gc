#include "procmaps.inc.c"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <stdio.h>

static GC_ProcMaps procmaps;

static void setup(const char *proc_maps) {
  int fd = fileno(tmpfile());
  ssize_t rc = write(fd, proc_maps, strlen(proc_maps));
  if (rc != strlen(proc_maps)) {
    fprintf(stderr, "error: failed to write: %s\n", strerror(errno));
  }
  lseek(fd, 0, SEEK_SET);
  GC_procmaps_open_fd(&procmaps, fd);
}

#define assert_next(expected_start, expected_end, expected_maptype)     \
  do {                                                                  \
    bool ok = GC_procmaps_next(&procmaps);                              \
    if (!ok) {                                                          \
      fprintf(stderr, "%s:%d: early EOF\n", __FILE__, __LINE__);        \
      exit(1);                                                          \
    }                                                                   \
    if (procmaps.start != expected_start) {                             \
      fprintf(stderr, "%s:%d: actual start %#lx != expected %#lx\n", __FILE__, __LINE__, procmaps.start, expected_start); \
      ok = false;                                                       \
    }                                                                   \
    if (procmaps.end != expected_end) {                                 \
      fprintf(stderr, "%s:%d: actual end %#lx != expected %#lx\n", __FILE__, __LINE__, procmaps.end, expected_end); \
      ok = false;                                                       \
    }                                                                   \
    if (procmaps.maptype != expected_maptype) {                         \
      fprintf(stderr, "%s:%d: actual type %#lx != expected %#lx\n", __FILE__, __LINE__, procmaps.maptype, expected_maptype); \
      ok = false;                                                       \
    }                                                                   \
    if (!ok) {                                                          \
      exit(1);                                                          \
    }                                                                   \
  } while (false)

#define assert_done(...)                                                \
  do {                                                                  \
    bool ok = GC_procmaps_next(&procmaps);                              \
    if (ok) {                                                           \
      fprintf(stderr, "%s:%d: expected EOF\n", __FILE__, __LINE__);     \
      exit(1);                                                          \
    }                                                                   \
  } while (false)

int main() {
  setup("00400000-00452000 r-xp 00000000 08:02 173521      /usr/bin/dbus-daemon\n");
  assert_next(0x00400000, 0x00452000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_done();

  setup("");
  assert_done();

  setup(
    "00400000-00452000 r--p 00000000 08:02 173521      /usr/bin/dbus-daemon\n"
    "00452000-00453000 ---p 00000000 00:00 0 \n"
    "00453000-00454000 -w-p 00000000 00:00 0                  [anon:gcU]\n"
    "00454000-00455000 rw-p 00000000 00:00 0                  [anon:gcU]\n"
    "00455000-00456000\n"
  );
  assert_next(0x00400000, 0x00452000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x00452000, 0x00453000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_next(0x00453000, 0x00454000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_next(0x00454000, 0x00455000, GC_PROCMAPS_MAPTYPE_USERDATA);
  assert_next(0x00455000, 0x00456000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_done();

  setup(
    "00400000-00401000 r--p 00000000 00:00 0                  [vvar]\n"
    "00401000-00402000 r--p 00000000 00:00 0                  [vvar_vclock]\n"
    "00402000-00403000 r--p 00000000 00:00 0                  [vvar_vclock] (deleted)\n"
    "00403000-00404000 r--p 00000000 00:00 0                  [vvar_v]\n"
    "00404000-00405000 r--p 00000000 00:00 0                  [vva]\n"
    "00405000-00406000 r--p 00000000 00:00 0                  [anon:gcX]\n"
    "00406000-00407000 r--p 00000000 00:00 0                  [anon:gcC]x\n"
    "00407000-00408000 r-xp 00000000 00:00 0                  [vdso]\n"
  );
  assert_next(0x00400000, 0x00401000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_next(0x00401000, 0x00402000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_next(0x00402000, 0x00403000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x00403000, 0x00404000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x00404000, 0x00405000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x00405000, 0x00406000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x00406000, 0x00407000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x00407000, 0x00408000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_done();

  setup("00400000-00452000 r-xp 00000000 08:02 173521      [anon:gcW]\n");
  assert_next(0x00400000, 0x00452000, GC_PROCMAPS_MAPTYPE_WORKQUEUE);
  assert_done();

  setup("00400000-00452000 r-xp 00000000 08:02 173521      [anon:gcC]\n");
  assert_next(0x00400000, 0x00452000, GC_PROCMAPS_MAPTYPE_CHUNKS);
  assert_done();

  setup("00400000-00452000 r-xp 00000000 08:02 173521      [anon:gcU]\n");
  assert_next(0x00400000, 0x00452000, GC_PROCMAPS_MAPTYPE_USERDATA);
  assert_done();

  setup(
    "64b3e260a000-64b3e2620000 r--p 00000000 00:20 167788                     /nix/store/jk4zvn7jb9fcpy4qidz31z521z6p1ij0-coreutils-full-9.11/bin/coreutils\n"
    "64b3e2620000-64b3e272f000 r-xp 00016000 00:20 167788                     /nix/store/jk4zvn7jb9fcpy4qidz31z521z6p1ij0-coreutils-full-9.11/bin/coreutils\n"
    "64b3e272f000-64b3e2783000 r--p 00125000 00:20 167788                     /nix/store/jk4zvn7jb9fcpy4qidz31z521z6p1ij0-coreutils-full-9.11/bin/coreutils\n"
    "64b3e2783000-64b3e2790000 r--p 00178000 00:20 167788                     /nix/store/jk4zvn7jb9fcpy4qidz31z521z6p1ij0-coreutils-full-9.11/bin/coreutils\n"
    "64b3e2790000-64b3e2791000 rw-p 00185000 00:20 167788                     /nix/store/jk4zvn7jb9fcpy4qidz31z521z6p1ij0-coreutils-full-9.11/bin/coreutils\n"
    "64b3e2791000-64b3e29a8000 rw-p 00000000 00:00 0 \n"
    "64b3ec555000-64b3ec576000 rw-p 00000000 00:00 0                          [heap]\n"
    "7779356d0000-777935712000 rw-p 00000000 00:00 0 \n"
    "777935712000-777935b0a000 r--p 00000000 00:20 775501                     /nix/store/q55hz122qj3707k0rr4xx530xrc0112a-glibc-locales-2.42-103/lib/locale/locale-archive\n"
    "777935b0a000-777935b0c000 rw-p 00000000 00:00 0 \n"
    "777935b0c000-777935b0d000 r--p 00000000 00:20 130850                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libpthread.so.0\n"
    "777935b0d000-777935b0e000 r-xp 00001000 00:20 130850                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libpthread.so.0\n"
    "777935b0e000-777935b0f000 r--p 00002000 00:20 130850                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libpthread.so.0\n"
    "777935b0f000-777935b10000 r--p 00002000 00:20 130850                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libpthread.so.0\n"
    "777935b10000-777935b11000 rw-p 00003000 00:20 130850                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libpthread.so.0\n"
    "777935b11000-777935b12000 r--p 00000000 00:20 131516                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libdl.so.2\n"
    "777935b12000-777935b13000 r-xp 00001000 00:20 131516                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libdl.so.2\n"
    "777935b13000-777935b14000 r--p 00002000 00:20 131516                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libdl.so.2\n"
    "777935b14000-777935b15000 r--p 00002000 00:20 131516                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libdl.so.2\n"
    "777935b15000-777935b16000 rw-p 00003000 00:20 131516                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libdl.so.2\n"
    "777935b16000-777935b3e000 r--p 00000000 00:20 134912                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libc.so.6\n"
    "777935b3e000-777935cbd000 r-xp 00028000 00:20 134912                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libc.so.6\n"
    "777935cbd000-777935d0c000 r--p 001a7000 00:20 134912                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libc.so.6\n"
    "777935d0c000-777935d10000 r--p 001f5000 00:20 134912                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libc.so.6\n"
    "777935d10000-777935d12000 rw-p 001f9000 00:20 134912                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/libc.so.6\n"
    "777935d12000-777935d21000 rw-p 00000000 00:00 0 \n"
    "777935d21000-777935d23000 r--p 00000000 00:20 94967                      /nix/store/g23wn4x1ny1bwgn3g5d2s0zhvhsk0r8a-attr-2.6.0/lib/libattr.so.1.1.2600\n"
    "777935d23000-777935d26000 r-xp 00002000 00:20 94967                      /nix/store/g23wn4x1ny1bwgn3g5d2s0zhvhsk0r8a-attr-2.6.0/lib/libattr.so.1.1.2600\n"
    "777935d26000-777935d27000 r--p 00005000 00:20 94967                      /nix/store/g23wn4x1ny1bwgn3g5d2s0zhvhsk0r8a-attr-2.6.0/lib/libattr.so.1.1.2600\n"
    "777935d27000-777935d28000 r--p 00005000 00:20 94967                      /nix/store/g23wn4x1ny1bwgn3g5d2s0zhvhsk0r8a-attr-2.6.0/lib/libattr.so.1.1.2600\n"
    "777935d28000-777935d29000 rw-p 00006000 00:20 94967                      /nix/store/g23wn4x1ny1bwgn3g5d2s0zhvhsk0r8a-attr-2.6.0/lib/libattr.so.1.1.2600\n"
    "777935d29000-777935d2c000 r--p 00000000 00:20 151466                     /nix/store/y3wsyn8qwa6l34pwn3p3602jnwc8p0q1-acl-2.4.0/lib/libacl.so.1.2.2400\n"
    "777935d2c000-777935d33000 r-xp 00003000 00:20 151466                     /nix/store/y3wsyn8qwa6l34pwn3p3602jnwc8p0q1-acl-2.4.0/lib/libacl.so.1.2.2400\n"
    "777935d33000-777935d35000 r--p 0000a000 00:20 151466                     /nix/store/y3wsyn8qwa6l34pwn3p3602jnwc8p0q1-acl-2.4.0/lib/libacl.so.1.2.2400\n"
    "777935d35000-777935d36000 r--p 0000b000 00:20 151466                     /nix/store/y3wsyn8qwa6l34pwn3p3602jnwc8p0q1-acl-2.4.0/lib/libacl.so.1.2.2400\n"
    "777935d36000-777935d37000 rw-p 0000c000 00:20 151466                     /nix/store/y3wsyn8qwa6l34pwn3p3602jnwc8p0q1-acl-2.4.0/lib/libacl.so.1.2.2400\n"
    "777935d37000-777935e26000 r--p 00000000 00:20 169495                     /nix/store/fxd9n2781bcgdpx0rfcn3f8jmll2miam-openssl-3.6.5/lib/libcrypto.so.3\n"
    "777935e26000-777936216000 r-xp 000ef000 00:20 169495                     /nix/store/fxd9n2781bcgdpx0rfcn3f8jmll2miam-openssl-3.6.5/lib/libcrypto.so.3\n"
    "777936216000-777936337000 r--p 004df000 00:20 169495                     /nix/store/fxd9n2781bcgdpx0rfcn3f8jmll2miam-openssl-3.6.5/lib/libcrypto.so.3\n"
    "777936337000-7779363b6000 r--p 005ff000 00:20 169495                     /nix/store/fxd9n2781bcgdpx0rfcn3f8jmll2miam-openssl-3.6.5/lib/libcrypto.so.3\n"
    "7779363b6000-7779363b9000 rw-p 0067e000 00:20 169495                     /nix/store/fxd9n2781bcgdpx0rfcn3f8jmll2miam-openssl-3.6.5/lib/libcrypto.so.3\n"
    "7779363b9000-7779363bc000 rw-p 00000000 00:00 0 \n"
    "7779363bc000-7779363ce000 r--p 00000000 00:20 154723                     /nix/store/fg827wzs88z40g883yp3zjb1l5rv2b52-gmp-with-cxx-6.3.0/lib/libgmp.so.10.5.0\n"
    "7779363ce000-77793644e000 r-xp 00012000 00:20 154723                     /nix/store/fg827wzs88z40g883yp3zjb1l5rv2b52-gmp-with-cxx-6.3.0/lib/libgmp.so.10.5.0\n"
    "77793644e000-777936463000 r--p 00092000 00:20 154723                     /nix/store/fg827wzs88z40g883yp3zjb1l5rv2b52-gmp-with-cxx-6.3.0/lib/libgmp.so.10.5.0\n"
    "777936463000-777936465000 r--p 000a6000 00:20 154723                     /nix/store/fg827wzs88z40g883yp3zjb1l5rv2b52-gmp-with-cxx-6.3.0/lib/libgmp.so.10.5.0\n"
    "777936465000-777936466000 rw-p 000a8000 00:20 154723                     /nix/store/fg827wzs88z40g883yp3zjb1l5rv2b52-gmp-with-cxx-6.3.0/lib/libgmp.so.10.5.0\n"
    "777936466000-777936468000 rw-p 00000000 00:00 0 \n"
    "777936468000-77793646c000 r--p 00000000 00:00 0                          [vvar]\n"
    "77793646c000-77793646e000 r--p 00000000 00:00 0                          [vvar_vclock]\n"
    "77793646e000-777936470000 r-xp 00000000 00:00 0                          [vdso]\n"
    "777936470000-777936471000 r--p 00000000 00:20 130832                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/ld-linux-x86-64.so.2\n"
    "777936471000-77793649c000 r-xp 00001000 00:20 130832                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/ld-linux-x86-64.so.2\n"
    "77793649c000-7779364a7000 r--p 0002c000 00:20 130832                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/ld-linux-x86-64.so.2\n"
    "7779364a7000-7779364a9000 r--p 00037000 00:20 130832                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/ld-linux-x86-64.so.2\n"
    "7779364a9000-7779364aa000 rw-p 00039000 00:20 130832                     /nix/store/pagq7gjgxqbi09g0091x2cxwq2kwxyx8-glibc-2.42-103/lib/ld-linux-x86-64.so.2\n"
    "7779364aa000-7779364ab000 rw-p 00000000 00:00 0 \n"
    "7ffeeb2c5000-7ffeeb2ea000 rw-p 00000000 00:00 0                          [stack]\n"
    "ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0                  [vsyscall]\n"
  );
  assert_next(0x64b3e260a000, 0x64b3e2620000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x64b3e2620000, 0x64b3e272f000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x64b3e272f000, 0x64b3e2783000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x64b3e2783000, 0x64b3e2790000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x64b3e2790000, 0x64b3e2791000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x64b3e2791000, 0x64b3e29a8000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x64b3ec555000, 0x64b3ec576000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779356d0000, 0x777935712000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935712000, 0x777935b0a000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b0a000, 0x777935b0c000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b0c000, 0x777935b0d000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b0d000, 0x777935b0e000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b0e000, 0x777935b0f000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b0f000, 0x777935b10000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b10000, 0x777935b11000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b11000, 0x777935b12000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b12000, 0x777935b13000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b13000, 0x777935b14000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b14000, 0x777935b15000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b15000, 0x777935b16000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b16000, 0x777935b3e000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935b3e000, 0x777935cbd000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935cbd000, 0x777935d0c000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d0c000, 0x777935d10000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d10000, 0x777935d12000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d12000, 0x777935d21000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d21000, 0x777935d23000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d23000, 0x777935d26000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d26000, 0x777935d27000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d27000, 0x777935d28000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d28000, 0x777935d29000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d29000, 0x777935d2c000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d2c000, 0x777935d33000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d33000, 0x777935d35000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d35000, 0x777935d36000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d36000, 0x777935d37000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935d37000, 0x777935e26000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777935e26000, 0x777936216000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936216000, 0x777936337000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936337000, 0x7779363b6000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779363b6000, 0x7779363b9000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779363b9000, 0x7779363bc000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779363bc000, 0x7779363ce000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779363ce000, 0x77793644e000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x77793644e000, 0x777936463000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936463000, 0x777936465000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936465000, 0x777936466000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936466000, 0x777936468000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936468000, 0x77793646c000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_next(0x77793646c000, 0x77793646e000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_next(0x77793646e000, 0x777936470000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936470000, 0x777936471000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x777936471000, 0x77793649c000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x77793649c000, 0x7779364a7000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779364a7000, 0x7779364a9000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779364a9000, 0x7779364aa000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7779364aa000, 0x7779364ab000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0x7ffeeb2c5000, 0x7ffeeb2ea000, GC_PROCMAPS_MAPTYPE_OTHER);
  assert_next(0xffffffffff600000, 0xffffffffff601000, GC_PROCMAPS_MAPTYPE_IGNORE);
  assert_done();
}
