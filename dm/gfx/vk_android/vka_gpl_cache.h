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
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define VKA_GPL_CACHE_MAGIC 0x434C5047u   // "GPLC"
#define VKA_GPL_CACHE_VERSION 1u
#define VKA_GPL_CACHE_VERSION_SHARDED 2u
#define VKA_GPL_CACHE_F_COMPLETE 1u
#define VKA_GPL_CACHE_F_GL_DEPTH 2u
#define VKA_GPL_CACHE_F_DELTA 4u
#define VKA_GPL_CACHE_F_MONO 8u
#define VKA_GPL_CACHE_F_SHARDED 16u
#define VKA_GPL_CACHE_UUID_SIZE 16u
#define VKA_GPL_SHARD_DONE 0x80000000u
#define VKA_GPL_SHARD_MAX 256u

typedef struct {
    uint64_t offset;
    uint32_t size;
    uint32_t entries;   // low 31 bits = shader entries in shard, bit 31 = VKA_GPL_SHARD_DONE
    uint64_t fnv64;
} vka_gpl_shard_ent;

_Static_assert(sizeof(vka_gpl_shard_ent) == 24, "vka_gpl_shard_ent size");

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

static inline void vka_gpl_cache_uuid_hex(const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], char out[VKA_GPL_CACHE_UUID_SIZE * 2 + 1]) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < VKA_GPL_CACHE_UUID_SIZE; i++) {
        out[2 * i] = hx[uuid[i] >> 4];
        out[2 * i + 1] = hx[uuid[i] & 0xf];
    }
    out[VKA_GPL_CACHE_UUID_SIZE * 2] = 0;
}

static inline int vka_gpl_cache_filename(char *out, size_t n, const char *dir, const char *title_id, uint64_t dmsh_hash, const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE]) {
    if (!out || !n || !dir || !title_id || !uuid) return -1;
    char uhex[VKA_GPL_CACHE_UUID_SIZE * 2 + 1];
    vka_gpl_cache_uuid_hex(uuid, uhex);
    return snprintf(out, n, "%s/vk_gpl_cache_%s_%016llx_%s.bin", dir, title_id, (unsigned long long)dmsh_hash, uhex);
}

static inline int vka_gpl_mono_cache_filename(char *out, size_t n, const char *dir, const char *title_id, uint64_t dmsh_hash, const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE]) {
    if (!out || !n || !dir || !title_id || !uuid) return -1;
    char uhex[VKA_GPL_CACHE_UUID_SIZE * 2 + 1];
    vka_gpl_cache_uuid_hex(uuid, uhex);
    return snprintf(out, n, "%s/vk_monolithic_cache_%s_%016llx_%s.bin", dir, title_id, (unsigned long long)dmsh_hash, uhex);
}

static inline uint32_t vka_gpl_shard_count(uint32_t total_entries) {
    uint32_t s = 1u;
    while (s < VKA_GPL_SHARD_MAX && s * 160u < total_entries) s <<= 1;
    return s;
}

static inline uint32_t vka_gpl_shard_of(uint64_t key, uint32_t shard_count) {
    if (shard_count <= 1u) return 0u;
    uint64_t h = (key ^ (key >> 33)) * 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (uint32_t)(h & (shard_count - 1u));
}

static inline uint64_t vka_gpl_shard_table_offset(void) {
    return (uint64_t)sizeof(vka_gpl_cache_hdr);
}

static inline uint64_t vka_gpl_shard_data_offset(uint32_t shard_count) {
    uint64_t end = (uint64_t)sizeof(vka_gpl_cache_hdr) + (uint64_t)shard_count * sizeof(vka_gpl_shard_ent);
    return (end + 15ull) & ~15ull;
}

static inline int vka_gpl_retype_cache_filename(char *out, size_t n, const char *dir, const char *title_id, uint64_t dmsh_hash, const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE]) {
    if (!out || !n || !dir || !title_id || !uuid) return -1;
    char uhex[VKA_GPL_CACHE_UUID_SIZE * 2 + 1];
    vka_gpl_cache_uuid_hex(uuid, uhex);
    return snprintf(out, n, "%s/vk_gpl_retype_cache_%s_%016llx_%s.bin", dir, title_id, (unsigned long long)dmsh_hash, uhex);
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

static inline int vka_gpl_cache_prune_prefix(const char *dir, const char *fmt_prefix, const char *title_id, const char *keep_basename) {
    if (!dir || !*dir || !title_id || !*title_id) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    char prefix[128];
    int plen = snprintf(prefix, sizeof prefix, fmt_prefix, title_id);
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

static inline int vka_gpl_cache_prune_stale(const char *dir, const char *title_id, const char *keep_basename) {
    return vka_gpl_cache_prune_prefix(dir, "vk_gpl_cache_%s_", title_id, keep_basename);
}

static inline int vka_gpl_retype_cache_prune_stale(const char *dir, const char *title_id, const char *keep_basename) {
    return vka_gpl_cache_prune_prefix(dir, "vk_gpl_retype_cache_%s_", title_id, keep_basename);
}

static inline int vka_gpl_mono_cache_prune_stale(const char *dir, const char *title_id, const char *keep_basename) {
    return vka_gpl_cache_prune_prefix(dir, "vk_monolithic_cache_%s_", title_id, keep_basename);
}

static inline int vka_gpl_aux_cache_save_atomic(const char *path, const vka_gpl_cache_hdr *h_in, uint32_t req_flag, const void *vk_blob, uint32_t vk_len) {
    if (!h_in) return 0;
    vka_gpl_cache_hdr h = *h_in;
    h.flags |= req_flag;
    return vka_gpl_cache_save_atomic(path, &h, NULL, 0, vk_blob, vk_len);
}

static inline int vka_gpl_retype_cache_save_atomic(const char *path, const vka_gpl_cache_hdr *h_in, const void *vk_blob, uint32_t vk_len) {
    return vka_gpl_aux_cache_save_atomic(path, h_in, VKA_GPL_CACHE_F_DELTA, vk_blob, vk_len);
}

static inline int vka_gpl_mono_cache_save_atomic(const char *path, const vka_gpl_cache_hdr *h_in, const void *vk_blob, uint32_t vk_len) {
    return vka_gpl_aux_cache_save_atomic(path, h_in, VKA_GPL_CACHE_F_MONO, vk_blob, vk_len);
}

static inline int vka_gpl_aux_cache_hdr_validate(const vka_gpl_cache_hdr *h, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                                 const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, int expected_gl_depth,
                                                 uint32_t required_flag, off_t file_size, const char **why) {
    if (h->magic != VKA_GPL_CACHE_MAGIC) { if (why) *why = "bad magic"; return 0; }
    if (h->schema_version != VKA_GPL_CACHE_VERSION) { if (why) *why = "schema version mismatch"; return 0; }
    if (h->header_crc != vka_gpl_cache_hdr_crc(h)) { if (why) *why = "header checksum mismatch"; return 0; }
    if (h->vendor_id != vendor_id) { if (why) *why = "vendorID mismatch"; return 0; }
    if (h->device_id != device_id) { if (why) *why = "deviceID mismatch"; return 0; }
    if (h->driver_version != driver_version) { if (why) *why = "driverVersion mismatch"; return 0; }
    if (memcmp(h->pipeline_cache_uuid, uuid, VKA_GPL_CACHE_UUID_SIZE) != 0) { if (why) *why = "pipelineCacheUUID mismatch"; return 0; }
    if (h->dmsh_vk_lib_hash != dmsh_hash) { if (why) *why = "dmsh_vk.lib hash mismatch"; return 0; }
    if (!(h->flags & required_flag)) { if (why) *why = (required_flag == VKA_GPL_CACHE_F_MONO) ? "missing mono flag" : "missing delta flag"; return 0; }
    if (((h->flags & VKA_GPL_CACHE_F_GL_DEPTH) != 0) != (expected_gl_depth != 0)) { if (why) *why = "GL depth mode mismatch"; return 0; }
    if (h->bitset_bytes != 0) { if (why) *why = "unexpected bitset"; return 0; }
    if (h->vk_cache_bytes == 0) { if (why) *why = "empty pipeline cache payload"; return 0; }
    if ((uint64_t)file_size != (uint64_t)sizeof(*h) + h->vk_cache_bytes) { if (why) *why = "file size mismatch"; return 0; }
    if (why) *why = NULL;
    return 1;
}

static inline int vka_gpl_aux_cache_load_mmap(const char *path, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                              const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, int expected_gl_depth,
                                              uint32_t required_flag, vka_gpl_cache_hdr *out_hdr, void **out_map, size_t *out_map_size,
                                              const void **out_blob, const char **why) {
    if (out_map) *out_map = NULL;
    if (out_map_size) *out_map_size = 0;
    if (out_blob) *out_blob = NULL;
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
    if (!vka_gpl_aux_cache_hdr_validate(&h, vendor_id, device_id, driver_version, uuid, dmsh_hash, expected_gl_depth, required_flag, st.st_size, why)) {
        close(fd);
        return 0;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        if (why) *why = "mmap failed";
        return 0;
    }
    const void *blob = (const uint8_t *)m + sizeof h;
    if (vka_gpl_cache_fnv64(blob, h.vk_cache_bytes) != h.vk_cache_fnv) {
        munmap(m, (size_t)st.st_size);
        if (why) *why = "payload checksum mismatch";
        return 0;
    }
    if (out_hdr) *out_hdr = h;
    if (out_map) *out_map = m;
    if (out_map_size) *out_map_size = (size_t)st.st_size;
    if (out_blob) *out_blob = blob;
    if (!out_map) munmap(m, (size_t)st.st_size);
    if (why) *why = NULL;
    return 1;
}

static inline int vka_gpl_mono_cache_load_mmap(const char *path, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                               const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, int expected_gl_depth,
                                               vka_gpl_cache_hdr *out_hdr, void **out_map, size_t *out_map_size, const void **out_blob, const char **why) {
    return vka_gpl_aux_cache_load_mmap(path, vendor_id, device_id, driver_version, uuid, dmsh_hash, expected_gl_depth,
                                       VKA_GPL_CACHE_F_MONO, out_hdr, out_map, out_map_size, out_blob, why);
}

static inline int vka_gpl_retype_cache_load(const char *path, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                            const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, int expected_gl_depth,
                                            vka_gpl_cache_hdr *out_hdr, void **out_vk_blob, const char **why) {
    if (out_vk_blob) *out_vk_blob = NULL;
    vka_gpl_cache_hdr h; void *m = NULL; size_t ms = 0; const void *b = NULL;
    if (!vka_gpl_aux_cache_load_mmap(path, vendor_id, device_id, driver_version, uuid, dmsh_hash, expected_gl_depth,
                                     VKA_GPL_CACHE_F_DELTA, &h, &m, &ms, &b, why)) return 0;
    if (out_vk_blob) {
        void *copy = malloc(h.vk_cache_bytes);
        if (!copy) { munmap(m, ms); if (why) *why = "out of memory"; return 0; }
        memcpy(copy, b, h.vk_cache_bytes);
        *out_vk_blob = copy;
    }
    munmap(m, ms);
    if (out_hdr) *out_hdr = h;
    return 1;
}

// ---------- Sharded v2 GPL Base Cache Helpers ----------
static inline int vka_gpl_cache_pwrite_all(int fd, const void *buf, size_t len, uint64_t off) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t w = pwrite(fd, p + written, len - written, (off_t)(off + written));
        if (w < 0) { if (errno == EINTR) continue; return 0; }
        if (w == 0) return 0;
        written += (size_t)w;
    }
    return 1;
}

static inline int vka_gpl_cache_pread_all(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        if (r == 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static inline int vka_gpl_sharded_hdr_validate(const vka_gpl_cache_hdr *h, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                               const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, uint32_t expected_entries,
                                               uint32_t expected_shards, int expected_gl_depth, const char **why) {
    if (!h) { if (why) *why = "null header"; return 0; }
    if (h->magic != VKA_GPL_CACHE_MAGIC) { if (why) *why = "bad magic"; return 0; }
    if (h->schema_version != VKA_GPL_CACHE_VERSION_SHARDED) { if (why) *why = "schema version mismatch"; return 0; }
    if (h->header_crc != vka_gpl_cache_hdr_crc(h)) { if (why) *why = "header checksum mismatch"; return 0; }
    if (h->vendor_id != vendor_id) { if (why) *why = "vendorID mismatch"; return 0; }
    if (h->device_id != device_id) { if (why) *why = "deviceID mismatch"; return 0; }
    if (h->driver_version != driver_version) { if (why) *why = "driverVersion mismatch"; return 0; }
    if (memcmp(h->pipeline_cache_uuid, uuid, VKA_GPL_CACHE_UUID_SIZE) != 0) { if (why) *why = "pipelineCacheUUID mismatch"; return 0; }
    if (h->dmsh_vk_lib_hash != dmsh_hash) { if (why) *why = "dmsh_vk.lib hash mismatch"; return 0; }
    if (!(h->flags & VKA_GPL_CACHE_F_SHARDED)) { if (why) *why = "missing sharded flag"; return 0; }
    if (((h->flags & VKA_GPL_CACHE_F_GL_DEPTH) != 0) != (expected_gl_depth != 0)) { if (why) *why = "GL depth mode mismatch"; return 0; }
    if (h->total_entries != expected_entries) { if (why) *why = "entry count mismatch"; return 0; }
    if (h->compiled_entries > expected_entries) { if (why) *why = "compiled count out of range"; return 0; }
    if (h->bitset_bytes != expected_shards) { if (why) *why = "shard count mismatch"; return 0; }
    if (h->vk_cache_bytes != expected_shards * (uint32_t)sizeof(vka_gpl_shard_ent)) { if (why) *why = "shard table size mismatch"; return 0; }
    if (why) *why = NULL;
    return 1;
}

static inline int vka_gpl_sharded_create(const char *path, vka_gpl_cache_hdr *h_in, uint32_t shard_count,
                                         vka_gpl_shard_ent *out_table, uint64_t *out_append_off) {
    if (!path || !*path || !h_in || !shard_count || shard_count > VKA_GPL_SHARD_MAX || !out_table) return -1;
    uint32_t tbytes = shard_count * (uint32_t)sizeof(vka_gpl_shard_ent);
    uint64_t data_off = vka_gpl_shard_data_offset(shard_count);
    memset(out_table, 0, tbytes);
    vka_gpl_cache_hdr h = *h_in;
    h.magic = VKA_GPL_CACHE_MAGIC;
    h.schema_version = VKA_GPL_CACHE_VERSION_SHARDED;
    h.flags = (h_in->flags & VKA_GPL_CACHE_F_GL_DEPTH) | VKA_GPL_CACHE_F_SHARDED;
    h.compiled_entries = 0;
    h.bitset_bytes = shard_count;
    h.vk_cache_bytes = tbytes;
    h.vk_cache_fnv = vka_gpl_cache_fnv64(out_table, tbytes);
    h.reserved = 0;
    h.header_crc = vka_gpl_cache_hdr_crc(&h);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    uint8_t pad[16] = {0};
    size_t pad_len = (size_t)(data_off - (sizeof(h) + tbytes));
    int ok = vka_gpl_cache_pwrite_all(fd, &h, sizeof h, 0)
          && vka_gpl_cache_pwrite_all(fd, out_table, tbytes, sizeof h)
          && (!pad_len || vka_gpl_cache_pwrite_all(fd, pad, pad_len, sizeof(h) + tbytes))
          && fsync(fd) == 0;
    if (!ok) { close(fd); unlink(path); return -1; }
    *h_in = h;
    if (out_append_off) *out_append_off = data_off;
    return fd;
}

static inline int vka_gpl_sharded_open_rw(const char *path, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                          const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, uint32_t expected_entries,
                                          uint32_t expected_shards, int expected_gl_depth, vka_gpl_cache_hdr *out_hdr,
                                          vka_gpl_shard_ent *out_table, uint64_t *out_append_off, const char **why) {
    if (!path || !*path || !out_hdr || !out_table || !expected_shards || expected_shards > VKA_GPL_SHARD_MAX) {
        if (why) *why = "invalid args";
        return -1;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { if (why) *why = "missing"; return -1; }
    struct stat st;
    uint64_t data_off = vka_gpl_shard_data_offset(expected_shards);
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < data_off) {
        close(fd);
        if (why) *why = "truncated sharded file";
        return -1;
    }
    vka_gpl_cache_hdr h;
    if (!vka_gpl_cache_pread_all(fd, &h, sizeof h, 0)) {
        close(fd);
        if (why) *why = "header read error";
        return -1;
    }
    if (!vka_gpl_sharded_hdr_validate(&h, vendor_id, device_id, driver_version, uuid, dmsh_hash,
                                      expected_entries, expected_shards, expected_gl_depth, why)) {
        close(fd);
        return -1;
    }
    uint32_t tbytes = expected_shards * (uint32_t)sizeof(vka_gpl_shard_ent);
    if (!vka_gpl_cache_pread_all(fd, out_table, tbytes, sizeof h)) {
        close(fd);
        if (why) *why = "shard table read error";
        return -1;
    }
    if (vka_gpl_cache_fnv64(out_table, tbytes) != h.vk_cache_fnv) {
        close(fd);
        if (why) *why = "shard table checksum mismatch";
        return -1;
    }
    uint64_t max_end = data_off;
    uint32_t compiled = 0;
    uint8_t *chk = malloc(65536);
    for (uint32_t i = 0; i < expected_shards; i++) {
        if (!(out_table[i].entries & VKA_GPL_SHARD_DONE)) {
            memset(&out_table[i], 0, sizeof out_table[i]);
            continue;
        }
        uint64_t off = out_table[i].offset, sz = out_table[i].size;
        uint32_t cnt = out_table[i].entries & ~VKA_GPL_SHARD_DONE;
        if (off < data_off || off + sz > (uint64_t)st.st_size || cnt > expected_entries) {
            memset(&out_table[i], 0, sizeof out_table[i]);
            continue;
        }
        int intact = 1;
        if (sz > 0) {
            if (!chk) intact = 0;
            else {
                uint64_t fnv = 1469598103934665603ull, rem = sz, cur = off;
                while (rem > 0) {
                    size_t take = rem > 65536u ? 65536u : (size_t)rem;
                    if (!vka_gpl_cache_pread_all(fd, chk, take, cur)) { intact = 0; break; }
                    for (size_t k = 0; k < take; k++) fnv = (fnv ^ chk[k]) * 1099511628211ull;
                    rem -= take; cur += take;
                }
                if (intact && fnv != out_table[i].fnv64) intact = 0;
            }
        }
        if (!intact) {
            memset(&out_table[i], 0, sizeof out_table[i]);
            continue;
        }
        uint64_t end = (off + sz + 15ull) & ~15ull;
        if (end > max_end) max_end = end;
        compiled += cnt;
    }
    free(chk);
    if ((uint64_t)st.st_size > max_end) (void)ftruncate(fd, (off_t)max_end);
    h.compiled_entries = compiled > expected_entries ? expected_entries : compiled;
    h.vk_cache_fnv = vka_gpl_cache_fnv64(out_table, tbytes);
    h.header_crc = vka_gpl_cache_hdr_crc(&h);
    (void)vka_gpl_cache_pwrite_all(fd, out_table, tbytes, sizeof h);
    (void)vka_gpl_cache_pwrite_all(fd, &h, sizeof h, 0);
    *out_hdr = h;
    if (out_append_off) *out_append_off = max_end;
    if (why) *why = NULL;
    return fd;
}

static inline int vka_gpl_sharded_write_shard(int fd, vka_gpl_cache_hdr *hdr, vka_gpl_shard_ent *table, uint32_t shard_count,
                                              uint32_t shard_id, uint32_t entry_count, const void *blob, uint32_t blob_size,
                                              uint64_t *append_off, int do_sync) {
    if (fd < 0 || !hdr || !table || shard_id >= shard_count || !append_off) return 0;
    if (blob_size && !blob) return 0;
    uint64_t off = (*append_off + 15ull) & ~15ull;
    if (blob_size && !vka_gpl_cache_pwrite_all(fd, blob, blob_size, off)) return 0;
    uint32_t prev_cnt = (table[shard_id].entries & VKA_GPL_SHARD_DONE) ? (table[shard_id].entries & ~VKA_GPL_SHARD_DONE) : 0u;
    table[shard_id].offset = off;
    table[shard_id].size = blob_size;
    table[shard_id].entries = VKA_GPL_SHARD_DONE | (entry_count & ~VKA_GPL_SHARD_DONE);
    table[shard_id].fnv64 = blob_size ? vka_gpl_cache_fnv64(blob, blob_size) : 0ull;
    hdr->compiled_entries = hdr->compiled_entries - prev_cnt + (entry_count & ~VKA_GPL_SHARD_DONE);
    if (hdr->compiled_entries > hdr->total_entries) hdr->compiled_entries = hdr->total_entries;
    uint32_t tbytes = shard_count * (uint32_t)sizeof(vka_gpl_shard_ent);
    hdr->vk_cache_fnv = vka_gpl_cache_fnv64(table, tbytes);
    hdr->header_crc = vka_gpl_cache_hdr_crc(hdr);
    uint64_t toff = vka_gpl_shard_table_offset() + (uint64_t)shard_id * sizeof(vka_gpl_shard_ent);
    if (!vka_gpl_cache_pwrite_all(fd, &table[shard_id], sizeof(vka_gpl_shard_ent), toff)) return 0;
    if (!vka_gpl_cache_pwrite_all(fd, hdr, sizeof *hdr, 0)) return 0;
    *append_off = (off + blob_size + 15ull) & ~15ull;
    if (do_sync) (void)fdatasync(fd);
    return 1;
}

static inline int vka_gpl_sharded_finalize(int fd, vka_gpl_cache_hdr *hdr, const vka_gpl_shard_ent *table, uint32_t shard_count, int mark_complete) {
    if (fd < 0 || !hdr || !table) return 0;
    if (mark_complete) hdr->flags |= VKA_GPL_CACHE_F_COMPLETE;
    uint32_t tbytes = shard_count * (uint32_t)sizeof(vka_gpl_shard_ent);
    hdr->vk_cache_fnv = vka_gpl_cache_fnv64(table, tbytes);
    hdr->header_crc = vka_gpl_cache_hdr_crc(hdr);
    int ok = vka_gpl_cache_pwrite_all(fd, &hdr[0], sizeof *hdr, 0) && fsync(fd) == 0;
    return ok;
}

static inline int vka_gpl_sharded_open_ro(const char *path, uint32_t vendor_id, uint32_t device_id, uint32_t driver_version,
                                          const uint8_t uuid[VKA_GPL_CACHE_UUID_SIZE], uint64_t dmsh_hash, uint32_t expected_entries,
                                          uint32_t expected_shards, int expected_gl_depth, vka_gpl_cache_hdr *out_hdr,
                                          const vka_gpl_shard_ent **out_table, void **out_map, size_t *out_map_size, const char **why) {
    if (out_table) *out_table = NULL;
    if (out_map) *out_map = NULL;
    if (out_map_size) *out_map_size = 0;
    if (!path || !*path || !expected_shards || expected_shards > VKA_GPL_SHARD_MAX) { if (why) *why = "invalid args"; return 0; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (why) *why = "missing"; return 0; }
    struct stat st;
    uint64_t data_off = vka_gpl_shard_data_offset(expected_shards);
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < data_off) {
        close(fd);
        if (why) *why = "truncated sharded file";
        return 0;
    }
    vka_gpl_cache_hdr h;
    if (!vka_gpl_cache_read_all(fd, &h, sizeof h)) {
        close(fd);
        if (why) *why = "header read error";
        return 0;
    }
    if (!vka_gpl_sharded_hdr_validate(&h, vendor_id, device_id, driver_version, uuid, dmsh_hash,
                                      expected_entries, expected_shards, expected_gl_depth, why)) {
        close(fd);
        return 0;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { if (why) *why = "mmap failed"; return 0; }
    const vka_gpl_shard_ent *tab = (const vka_gpl_shard_ent *)((const uint8_t *)m + vka_gpl_shard_table_offset());
    if (vka_gpl_cache_fnv64(tab, h.vk_cache_bytes) != h.vk_cache_fnv) {
        munmap(m, (size_t)st.st_size);
        if (why) *why = "shard table checksum mismatch";
        return 0;
    }
    for (uint32_t i = 0; i < expected_shards; i++) {
        if (!(tab[i].entries & VKA_GPL_SHARD_DONE)) continue;
        if (tab[i].offset < data_off || (uint64_t)tab[i].offset + tab[i].size > (uint64_t)st.st_size) {
            munmap(m, (size_t)st.st_size);
            if (why) *why = "shard bounds out of range";
            return 0;
        }
    }
    if (out_hdr) *out_hdr = h;
    if (out_table) *out_table = tab;
    if (out_map) *out_map = m;
    if (out_map_size) *out_map_size = (size_t)st.st_size;
    if (!out_map) munmap(m, (size_t)st.st_size);
    if (why) *why = NULL;
    return 1;
}

static inline int vka_precompile_target_workers(int max_workers, double elapsed_s, uint64_t avail_mb, long maps, long maps_max, int *abort_early) {
    (void)elapsed_s;
    if (abort_early) *abort_early = 0;
    if (max_workers < 1) max_workers = 1;
    if (max_workers > 4) max_workers = 4;
    int target = max_workers;
    if (avail_mb > 0) {
        if (avail_mb < 600) {
            if (abort_early) *abort_early = 1;
            return 0;
        }
        if (avail_mb < 1200) target = 1;
        else if (avail_mb <= 1500 && target > 2) target = 2;
    }
    if (maps_max > 0 && maps >= 0) {
        long pct = (maps * 100) / maps_max;
        if (pct >= 75) {
            if (abort_early) *abort_early = 1;
            return 0;
        }
        if (pct >= 65) target = 1;
        else if (pct >= 50 && target > 2) target = 2;
    }
    return target;
}

static inline int vka_precompile_debounce_due(uint64_t new_entries, uint64_t elapsed_ns, uint64_t min_entries, uint64_t min_sec) {
    return new_entries >= min_entries && elapsed_ns >= min_sec * 1000000000ull;
}
#endif
