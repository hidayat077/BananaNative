// Persistent Boot #1 GPL & Compute pipeline cache container (vka_pso.c): header validation, atomic checkpointing, resume bitset, stale-cache pruning,
// worker throttling, and gameplay debounce helpers. Plain C with no Vulkan headers so dm/host/tests/t_android_gpl_cache.c tests it directly.
#ifndef VKA_GPL_CACHE_H
#define VKA_GPL_CACHE_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VKA_GPL_CACHE_MAGIC 0x434C5047u   // "GPLC"
#define VKA_GPL_CACHE_VERSION 1u
#define VKA_GPL_CACHE_F_COMPLETE 1u
#define VKA_GPL_CACHE_F_GL_DEPTH 2u
#define VKA_GPL_CACHE_UUID_SIZE 16u

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint32_t flags;
    uint8_t pipeline_cache_uuid[VKA_GPL_CACHE_UUID_SIZE];
    uint64_t dmsh_vk_lib_hash;
    uint32_t total_entries;
    uint32_t compiled_entries;
    uint32_t bitset_bytes;
    uint32_t vk_cache_bytes;
    uint64_t vk_cache_fnv;
    uint32_t header_crc;
    uint32_t reserved;
} vka_gpl_cache_hdr;

_Static_assert(sizeof(vka_gpl_cache_hdr) == 80, "vka_gpl_cache_hdr size");

static inline uint64_t vka_gpl_cache_fnv64(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static inline uint32_t vka_gpl_cache_hdr_crc(const vka_gpl_cache_hdr *h) {
    return (uint32_t)vka_gpl_cache_fnv64(h, offsetof(vka_gpl_cache_hdr, header_crc));
}

static inline uint32_t vka_gpl_cache_bitset_bytes(uint32_t total) {
    return (total + 7u) / 8u;
}

static inline int vka_gpl_cache_bit_get(const uint8_t *bs, uint32_t i) {
    return (bs[i >> 3] >> (i & 7u)) & 1u;
}

static inline void vka_gpl_cache_bit_set(uint8_t *bs, uint32_t i) {
    bs[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

static inline uint32_t vka_gpl_cache_bit_count(const uint8_t *bs, uint32_t total) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < total; i++) if (vka_gpl_cache_bit_get(bs, i)) c++;
    return c;
}

static inline int vka_gpl_cache_filename(char *out, size_t n, const char *dir, const char *title_id, uint64_t dmsh_hash, const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE]) {
    if (!out || !n || !dir || !title_id || !uuid) return -1;
    char uhex[VKA_GPL_CACHE_UUID_SIZE * 2 + 1];
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < VKA_GPL_CACHE_UUID_SIZE; i++) {
        uhex[2 * i] = hx[uuid[i] >> 4];
        uhex[2 * i + 1] = hx[uuid[i] & 0xf];
    }
    uhex[VKA_GPL_CACHE_UUID_SIZE * 2] = 0;
    return snprintf(out, n, "%s/vk_gpl_cache_%s_%016llx_%s.bin", dir, title_id, (unsigned long long)dmsh_hash, uhex);
}

static inline int vka_gpl_cache_hdr_validate(const vka_gpl_cache_hdr *h, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                             const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, uint32_t expected_entries,
                                             int expected_gl_depth, const char **why) {
    if (!h) { if (why) *why = "null header"; return 0; }
    if (h->magic != VKA_GPL_CACHE_MAGIC) { if (why) *why = "bad magic"; return 0; }
    if (h->schema_version != VKA_GPL_CACHE_VERSION) { if (why) *why = "schema version mismatch"; return 0; }
    if (h->header_crc != vka_gpl_cache_hdr_crc(h)) { if (why) *why = "header checksum mismatch"; return 0; }
    if (h->vendor_id != vendor_id) { if (why) *why = "vendorID mismatch"; return 0; }
    if (h->device_id != device_id) { if (why) *why = "deviceID mismatch"; return 0; }
    if (h->driver_version != driver_version) { if (why) *why = "driverVersion mismatch"; return 0; }
    if (memcmp(h->pipeline_cache_uuid, uuid, VKA_GPL_CACHE_UUID_SIZE) != 0) { if (why) *why = "pipelineCacheUUID mismatch"; return 0; }
    if (h->dmsh_vk_lib_hash != dmsh_hash) { if (why) *why = "dmsh_vk.lib hash mismatch"; return 0; }
    if (h->total_entries != expected_entries) { if (why) *why = "entry count mismatch"; return 0; }
    if (((h->flags & VKA_GPL_CACHE_F_GL_DEPTH) != 0) != (expected_gl_depth != 0)) { if (why) *why = "GL depth mode mismatch"; return 0; }
    if (h->bitset_bytes != vka_gpl_cache_bitset_bytes(expected_entries)) { if (why) *why = "bitset size mismatch"; return 0; }
    if (h->compiled_entries > expected_entries) { if (why) *why = "compiled count out of range"; return 0; }
    if (h->vk_cache_bytes == 0) { if (why) *why = "empty pipeline cache payload"; return 0; }
    if (why) *why = NULL;
    return 1;
}

static inline int vka_gpl_cache_write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (w == 0) return 0;
        off += (size_t)w;
    }
    return 1;
}

static inline int vka_gpl_cache_save_atomic(const char *path, const vka_gpl_cache_hdr *h_in, const uint8_t *bitset, uint32_t bitset_len,
                                            const void *vk_blob, uint32_t vk_len) {
    if (!path || !*path || !h_in) return 0;
    if (bitset_len && !bitset) return 0;
    if (vk_len && !vk_blob) return 0;
    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    vka_gpl_cache_hdr h = *h_in;
    h.magic = VKA_GPL_CACHE_MAGIC;
    h.schema_version = VKA_GPL_CACHE_VERSION;
    h.bitset_bytes = bitset_len;
    h.vk_cache_bytes = vk_len;
    h.vk_cache_fnv = vk_len ? vka_gpl_cache_fnv64(vk_blob, vk_len) : 0;
    h.reserved = 0;
    h.header_crc = vka_gpl_cache_hdr_crc(&h);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = vka_gpl_cache_write_all(fd, &h, sizeof h);
    if (ok && bitset_len) ok = vka_gpl_cache_write_all(fd, bitset, bitset_len);
    if (ok && vk_len) ok = vka_gpl_cache_write_all(fd, vk_blob, vk_len);
    if (ok && fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    // Best-effort directory fsync so the renamed entry survives sudden power loss.
    const char *sl = strrchr(path, '/');
    if (sl && sl > path) {
        char dpath[1100];
        size_t dl = (size_t)(sl - path);
        if (dl < sizeof dpath) {
            memcpy(dpath, path, dl);
            dpath[dl] = 0;
            int dfd = open(dpath, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (dfd >= 0) { (void)fsync(dfd); close(dfd); }
        }
    }
    return 1;
}

static inline int vka_gpl_cache_read_all(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, p + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (r == 0) return 0;
        off += (size_t)r;
    }
    return 1;
}

static inline int vka_gpl_cache_load(const char *path, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                     const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, uint32_t expected_entries,
                                     int expected_gl_depth, vka_gpl_cache_hdr *out_hdr, uint8_t **out_bitset, void **out_vk_blob, const char **why) {
    if (out_bitset) *out_bitset = NULL;
    if (out_vk_blob) *out_vk_blob = NULL;
    if (!path || !*path) { if (why) *why = "empty path"; return 0; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (why) *why = "missing"; return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(vka_gpl_cache_hdr)) {
        close(fd);
        if (why) *why = "truncated header";
        return 0;
    }
    vka_gpl_cache_hdr h;
    if (!vka_gpl_cache_read_all(fd, &h, sizeof h)) {
        close(fd);
        if (why) *why = "header read error";
        return 0;
    }
    if (!vka_gpl_cache_hdr_validate(&h, vendor_id, device_id, driver_version, uuid, dmsh_hash, expected_entries, expected_gl_depth, why)) {
        close(fd);
        return 0;
    }
    uint64_t need = (uint64_t)sizeof h + h.bitset_bytes + h.vk_cache_bytes;
    if ((uint64_t)st.st_size != need) {
        close(fd);
        if (why) *why = "file size mismatch";
        return 0;
    }
    uint8_t *bs = (uint8_t *)malloc(h.bitset_bytes ? h.bitset_bytes : 1u);
    void *blob = malloc(h.vk_cache_bytes);
    if (!bs || !blob) {
        free(bs); free(blob); close(fd);
        if (why) *why = "out of memory";
        return 0;
    }
    int ok = (!h.bitset_bytes || vka_gpl_cache_read_all(fd, bs, h.bitset_bytes)) && vka_gpl_cache_read_all(fd, blob, h.vk_cache_bytes);
    close(fd);
    if (!ok) {
        free(bs); free(blob);
        if (why) *why = "payload read error";
        return 0;
    }
    if (vka_gpl_cache_fnv64(blob, h.vk_cache_bytes) != h.vk_cache_fnv) {
        free(bs); free(blob);
        if (why) *why = "payload checksum mismatch";
        return 0;
    }
    if (out_hdr) *out_hdr = h;
    if (out_bitset) *out_bitset = bs; else free(bs);
    if (out_vk_blob) *out_vk_blob = blob; else free(blob);
    if (why) *why = NULL;
    return 1;
}

static inline int vka_gpl_cache_prune_stale(const char *dir, const char *title_id, const char *keep_basename) {
    if (!dir || !*dir || !title_id || !*title_id) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    char prefix[96];
    int plen = snprintf(prefix, sizeof prefix, "vk_gpl_cache_%s_", title_id);
    if (plen <= 0 || plen >= (int)sizeof prefix) { closedir(d); return 0; }
    int removed = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        if (strncmp(nm, prefix, (size_t)plen) != 0) continue;
        size_t nl = strlen(nm);
        int is_bin = nl > 4 && !strcmp(nm + nl - 4, ".bin");
        int is_tmp = nl > 8 && !strcmp(nm + nl - 8, ".bin.tmp");
        if (!is_bin && !is_tmp) continue;
        if (keep_basename && !strcmp(nm, keep_basename)) continue;
        char full[1200];
        if (snprintf(full, sizeof full, "%s/%s", dir, nm) < (int)sizeof full && unlink(full) == 0) removed++;
    }
    closedir(d);
    return removed;
}

static inline int vka_precompile_target_workers(int max_workers, double elapsed_s, uint64_t avail_mb, long maps, long maps_max, int *abort_early) {
    if (abort_early) *abort_early = 0;
    if (max_workers < 1) max_workers = 1;
    if (max_workers > 4) max_workers = 4;
    int target = max_workers;
    if (elapsed_s >= 45.0 && target > 2) target = 2;
    if (avail_mb > 0) {
        if (avail_mb < 600) {
            if (abort_early) *abort_early = 1;
            return 0;
        }
        if (avail_mb < 1200) target = 1;
        else if (avail_mb < 1800 && target > 2) target = 2;
    }
    if (maps_max > 0 && maps >= 0) {
        long pct = (maps * 100) / maps_max;
        if (pct >= 75) {
            if (abort_early) *abort_early = 1;
            return 0;
        }
        if (pct >= 60) target = 1;
        else if (pct >= 45 && target > 2) target = 2;
    }
    return target;
}

static inline int vka_precompile_debounce_due(uint64_t new_entries, uint64_t elapsed_ns, uint64_t min_entries, uint64_t min_sec) {
    return new_entries >= min_entries && elapsed_ns >= min_sec * 1000000000ull;
}
#endif
