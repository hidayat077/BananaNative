// Driver-agnostic runtime shader variant manifest (vk_shader_variants_<TitleID>_<dmsh_hash>.bin).
// Records every SPIR-V retype / specialization encountered in-game so startup rehydrates or pre-compiles them
// into the lightweight delta VkPipelineCache without touching the read-only Boot #1 base cache. Plain C, Vulkan-free.
#ifndef VKA_SHADER_VARIANTS_H
#define VKA_SHADER_VARIANTS_H
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
#include "vka_gpl_cache.h"

#define VKA_SVAR_MAGIC   0x52415653u   // "SVAR"
#define VKA_SVAR_VERSION 1u
#define VKA_SVAR_MAX     16384u

#define VKA_SVAR_F_GL_DEPTH (1u << 0)
#define VKA_SVAR_F_PFB_LATE (1u << 1)
#define VKA_SVAR_F_A2C      (1u << 2)
#define VKA_SVAR_F_GS_CLAIM (1u << 3)

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    uint64_t dmsh_vk_lib_hash;
    uint32_t entry_size;
    uint32_t entry_count;
    uint64_t payload_fnv;
    uint32_t header_crc;
    uint32_t reserved[3];
} vka_svar_hdr;

_Static_assert(sizeof(vka_svar_hdr) == 48, "vka_svar_hdr size");

typedef struct {
    uint64_t shader_hash;
    uint64_t tex_u;
    uint64_t tex_s;
    uint32_t vin_u;
    uint32_t vin_s;
    uint32_t fout_u;
    uint32_t fout_s;
    uint32_t flags;
    uint8_t  stage;
    uint8_t  lib_variant;
    uint8_t  alpha_func;
    uint8_t  samples;
    uint32_t reserved[2];
} vka_svar_rec;

_Static_assert(sizeof(vka_svar_rec) == 56, "vka_svar_rec size");

typedef struct {
    vka_svar_rec *recs;
    uint32_t n;
    uint32_t cap;
    uint32_t *slots;   // open-addressing indices (+1 so 0 is empty)
    size_t slot_cap;
} vka_svar_set;

static inline uint32_t vka_svar_hdr_crc(const vka_svar_hdr *h) {
    return (uint32_t)vka_gpl_cache_fnv64(h, offsetof(vka_svar_hdr, header_crc));
}

static inline uint64_t vka_svar_rec_hash(const vka_svar_rec *r) {
    vka_svar_rec norm = *r;
    norm.reserved[0] = 0;
    norm.reserved[1] = 0;
    uint64_t h = vka_gpl_cache_fnv64(&norm, sizeof norm);
    return h ? h : 1ull;
}

static inline int vka_svar_rec_eq(const vka_svar_rec *a, const vka_svar_rec *b) {
    return a->shader_hash == b->shader_hash && a->tex_u == b->tex_u && a->tex_s == b->tex_s &&
           a->vin_u == b->vin_u && a->vin_s == b->vin_s && a->fout_u == b->fout_u && a->fout_s == b->fout_s &&
           a->flags == b->flags && a->stage == b->stage && a->lib_variant == b->lib_variant &&
           a->alpha_func == b->alpha_func && a->samples == b->samples;
}

static inline void vka_svar_set_free(vka_svar_set *s) {
    if (!s) return;
    free(s->recs);
    free(s->slots);
    memset(s, 0, sizeof *s);
}

static inline int vka_svar_set_rehash_(vka_svar_set *s, size_t new_cap) {
    uint32_t *ns = (uint32_t *)calloc(new_cap, sizeof *ns);
    if (!ns) return 0;
    size_t mask = new_cap - 1u;
    for (uint32_t i = 0; i < s->n; i++) {
        uint64_t h = vka_svar_rec_hash(&s->recs[i]);
        size_t pos = (size_t)h & mask;
        while (ns[pos]) pos = (pos + 1u) & mask;
        ns[pos] = i + 1u;
    }
    free(s->slots);
    s->slots = ns;
    s->slot_cap = new_cap;
    return 1;
}

// Inserts a variant record if not already present. Returns 1 when newly inserted, 0 if duplicate or at capacity/OOM.
static inline int vka_svar_set_add(vka_svar_set *s, const vka_svar_rec *r_in) {
    if (!s || !r_in || !r_in->shader_hash) return 0;
    vka_svar_rec r = *r_in;
    r.reserved[0] = 0;
    r.reserved[1] = 0;
    if (s->slot_cap) {
        size_t mask = s->slot_cap - 1u;
        size_t pos = (size_t)vka_svar_rec_hash(&r) & mask;
        while (s->slots[pos]) {
            uint32_t idx = s->slots[pos] - 1u;
            if (vka_svar_rec_eq(&s->recs[idx], &r)) return 0;
            pos = (pos + 1u) & mask;
        }
    }
    if (s->n >= VKA_SVAR_MAX) return 0;
    if ((size_t)(s->n + 1u) * 2u > s->slot_cap) {
        size_t nc = s->slot_cap ? s->slot_cap * 2u : 256u;
        if (!vka_svar_set_rehash_(s, nc)) return 0;
    }
    if (s->n == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2u : 64u;
        if (nc > VKA_SVAR_MAX) nc = VKA_SVAR_MAX;
        vka_svar_rec *nr = (vka_svar_rec *)realloc(s->recs, (size_t)nc * sizeof *nr);
        if (!nr) return 0;
        s->recs = nr;
        s->cap = nc;
    }
    uint32_t new_idx = s->n++;
    s->recs[new_idx] = r;
    size_t mask = s->slot_cap - 1u;
    size_t pos = (size_t)vka_svar_rec_hash(&r) & mask;
    while (s->slots[pos]) pos = (pos + 1u) & mask;
    s->slots[pos] = new_idx + 1u;
    return 1;
}

static inline int vka_svar_manifest_filename(char *out, size_t n, const char *dir, const char *title_id, uint64_t dmsh_hash) {
    if (!out || !n || !dir || !title_id) return -1;
    return snprintf(out, n, "%s/vk_shader_variants_%s_%016llx.bin", dir, title_id, (unsigned long long)dmsh_hash);
}

static inline int vka_svar_manifest_prune_stale(const char *dir, const char *title_id, const char *keep_basename) {
    if (!dir || !*dir || !title_id || !*title_id) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    char prefix[96];
    int plen = snprintf(prefix, sizeof prefix, "vk_shader_variants_%s_", title_id);
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

static inline int vka_svar_manifest_save_atomic(const char *path, uint64_t dmsh_hash, const vka_svar_rec *recs, uint32_t count) {
    if (!path || !*path || count > VKA_SVAR_MAX || (count && !recs)) return 0;
    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    size_t pay_bytes = (size_t)count * sizeof *recs;
    vka_svar_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = VKA_SVAR_MAGIC;
    h.schema_version = VKA_SVAR_VERSION;
    h.dmsh_vk_lib_hash = dmsh_hash;
    h.entry_size = (uint32_t)sizeof(vka_svar_rec);
    h.entry_count = count;
    h.payload_fnv = pay_bytes ? vka_gpl_cache_fnv64(recs, pay_bytes) : 0;
    h.header_crc = vka_svar_hdr_crc(&h);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = vka_gpl_cache_write_all(fd, &h, sizeof h);
    if (ok && pay_bytes) ok = vka_gpl_cache_write_all(fd, recs, pay_bytes);
    if (ok && fsync(fd) != 0) ok = 0;
    if (close(fd) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
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

static inline int vka_svar_manifest_load(const char *path, uint64_t expected_dmsh_hash, vka_svar_set *out, const char **why) {
    if (!out) { if (why) *why = "null set"; return 0; }
    memset(out, 0, sizeof *out);
    if (!path || !*path) { if (why) *why = "empty path"; return 0; }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (why) *why = "missing"; return 0; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(vka_svar_hdr)) {
        close(fd);
        if (why) *why = "truncated header";
        return 0;
    }
    vka_svar_hdr h;
    if (!vka_gpl_cache_read_all(fd, &h, sizeof h)) {
        close(fd);
        if (why) *why = "header read error";
        return 0;
    }
    if (h.magic != VKA_SVAR_MAGIC) { close(fd); if (why) *why = "bad magic"; return 0; }
    if (h.schema_version != VKA_SVAR_VERSION) { close(fd); if (why) *why = "schema version mismatch"; return 0; }
    if (h.header_crc != vka_svar_hdr_crc(&h)) { close(fd); if (why) *why = "header checksum mismatch"; return 0; }
    if (h.dmsh_vk_lib_hash != expected_dmsh_hash) { close(fd); if (why) *why = "dmsh_vk.lib hash mismatch"; return 0; }
    if (h.entry_size != sizeof(vka_svar_rec)) { close(fd); if (why) *why = "entry size mismatch"; return 0; }
    if (h.entry_count > VKA_SVAR_MAX) { close(fd); if (why) *why = "entry count out of range"; return 0; }
    uint64_t pay_bytes = (uint64_t)h.entry_count * sizeof(vka_svar_rec);
    if ((uint64_t)st.st_size != (uint64_t)sizeof h + pay_bytes) {
        close(fd);
        if (why) *why = "file size mismatch";
        return 0;
    }
    if (h.entry_count > 0) {
        vka_svar_rec *tmp = (vka_svar_rec *)malloc((size_t)pay_bytes);
        if (!tmp) { close(fd); if (why) *why = "out of memory"; return 0; }
        int ok = vka_gpl_cache_read_all(fd, tmp, (size_t)pay_bytes);
        close(fd);
        if (!ok) { free(tmp); if (why) *why = "payload read error"; return 0; }
        if (vka_gpl_cache_fnv64(tmp, (size_t)pay_bytes) != h.payload_fnv) {
            free(tmp);
            if (why) *why = "payload checksum mismatch";
            return 0;
        }
        for (uint32_t i = 0; i < h.entry_count; i++) vka_svar_set_add(out, &tmp[i]);
        free(tmp);
    } else {
        close(fd);
        if (h.payload_fnv != 0) { if (why) *why = "empty payload checksum mismatch"; return 0; }
    }
    if (why) *why = NULL;
    return 1;
}
#endif
