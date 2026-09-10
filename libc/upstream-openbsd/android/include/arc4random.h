/*    $OpenBSD: arc4random_linux.h,v 1.7 2014/07/20 20:51:13 bcook Exp $    */

/*
 * Copyright (c) 1996, David Mazieres <dm@uun.org>
 * Copyright (c) 2008, Damien Miller <djm@openbsd.org>
 * Copyright (c) 2013, Markus Friedl <markus@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <async_safe/log.h>

static pthread_mutex_t g_arc4random_mutex = PTHREAD_MUTEX_INITIALIZER;

void arc4random_mutex_lock() {
  pthread_mutex_lock(&g_arc4random_mutex);
}

void arc4random_mutex_unlock() {
  pthread_mutex_unlock(&g_arc4random_mutex);
}

#define _ARC4_LOCK() arc4random_mutex_lock()
#define _ARC4_UNLOCK() arc4random_mutex_unlock()

static inline void _getentropy_fail(void) {
  async_safe_fatal("getentropy failed: %m");
}

// Disetel false bila kernel tidak mengenal MADV_WIPEONFORK (fitur itu masuk
// Linux 4.14). Lihat _rs_allocate di bawah.
static int g_arc4random_wipeonfork_works = 1;

static inline void _rs_forkdetect(void) {
  // Normalnya tidak diperlukan: MADV_WIPEONFORK di _rs_allocate sudah menjamin
  // pool di-nol-kan pada anak setelah fork.
  //
  // Pada kernel yang tidak mengenal MADV_WIPEONFORK, jaminan itu tidak ada, dan
  // anak akan mewarisi keystream induknya -- keduanya menghasilkan urutan acak
  // yang sama sampai reseed berikutnya. Karena itu di sana deteksi fork
  // dikembalikan secara manual lewat perbandingan PID.
  if (g_arc4random_wipeonfork_works) return;

  static pid_t last_pid = 0;
  pid_t pid = getpid();
  if (pid == last_pid) return;
  last_pid = pid;

  // Tiru persis akibat MADV_WIPEONFORK: seluruh mapping (rs DAN rsx) di-nol-kan,
  // sehingga _rs_stir_if_needed() melihat rs_count == 0 lalu memaksa _rs_stir()
  // yang mengambil entropi baru dan me-rekey chacha.
  if (rs != NULL) memset(rs, 0, sizeof(*rs));
  if (rsx != NULL) memset(rsx, 0, sizeof(*rsx));
}

static inline int _rs_allocate(struct _rs** rsp, struct _rsx** rsxp) {
  // OpenBSD's arc4random_linux.h allocates two separate mappings, but for
  // themselves they just allocate both structs into one mapping like this.
  struct data {
    struct _rs rs;
    struct _rsx rsx;
  };
  const size_t size = sizeof(struct data);

  struct data* p = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) {
    async_safe_fatal("arc4random data allocation failed: %m");
  }

  // Equivalent to OpenBSD's minherit(MAP_INHERIT_ZERO).
  //
  // MADV_WIPEONFORK baru ada sejak Linux 4.14. Pada kernel yang lebih tua
  // madvise() menjawab EINVAL, dan meng-abort di sini membuat SETIAP proses
  // gagal start -- arc4random dipakai saat inisialisasi hampir semua proses.
  // Gejalanya terlihat pertama kali saat memasang ROM lewat recovery lama:
  //   arc4random data MADV_WIPEONFORK failed: Invalid argument
  //   Updater process ended with signal: 6
  //
  // Kegagalan itu ditoleransi, TAPI jaminannya tidak dibuang: _rs_forkdetect()
  // di atas mengambil alih dengan deteksi berbasis PID. Kesalahan lain tetap
  // fatal, karena itu menandakan hal yang benar-benar tak terduga.
  if (madvise(p, size, MADV_WIPEONFORK) == -1) {
    if (errno == EINVAL || errno == ENOSYS) {
      g_arc4random_wipeonfork_works = 0;
    } else {
      async_safe_fatal("arc4random data MADV_WIPEONFORK failed: %m");
    }
  }

  // Give the allocation a name to make tombstones more intelligible.
  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, size, "arc4random data");

  *rsp = &p->rs;
  *rsxp = &p->rsx;
  return 0;
}
