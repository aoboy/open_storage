/*
 * Fountain decoder front-end (plan prompt W3.2): the ONLY definition of
 * the public fd_* symbols, dispatching on the arm tag the encoder
 * front-end stamped (the decoder is configured per arm by the
 * coordinator, which knows the job class's codec).
 */

#include "fountain_internal.h"

#include <errno.h>

#include <string.h>

struct fountain_dec {
    uint32_t codec;
    uint32_t _pad;
    unsigned char arm[]; /* exactly max(lt, nrq) size */
};

size_t fd_sizeof(const fd_cfg_t *cfg) {
    if (cfg == NULL || cfg->k == 0 || cfg->sym_len == 0)
        return 0;
    size_t arm = nrq_dec_sizeof() > lt_dec_sizeof() ? nrq_dec_sizeof()
                                                    : lt_dec_sizeof();
    return sizeof(struct fountain_dec) + arm;
}

int fd_init_in(fountain_dec_t *d, const fd_cfg_t *cfg) {
    if (d == NULL || cfg == NULL || fd_sizeof(cfg) == 0)
        return -EINVAL;
    struct fountain_dec *fd = (struct fountain_dec *)d;
    memset(fd, 0, sizeof(*fd));
    /*
     * The decode-side arm is the coordinator's choice and must match
     * what the sender used; fd_cfg carries only k/sym_len, so the
     * default here is the LT arm and the nanorq coordinator path uses
     * fd_init_codec() below.
     */
    fd->codec = SDF_FOUNTAIN_LT;
    int rc = lt_dec_init(fd->arm, cfg);
    if (rc != 0) {
        fd->codec = 0xffff;
        return rc;
    }
    return 0;
}

int fd_init(fountain_dec_t *d, uint32_t k, size_t sym_len) {
    fd_cfg_t cfg = {.k = k, .sym_len = sym_len};
    return fd_init_in(d, &cfg);
}

int fd_push(fountain_dec_t *d, uint32_t esi, const uint8_t *sym, size_t len) {
    struct fountain_dec *fd = (struct fountain_dec *)d;
    if (fd == NULL || fd->codec == 0xffff)
        return -EINVAL;
    return fd->codec == SDF_FOUNTAIN_NANORQ ? nrq_push(fd->arm, esi, sym, len)
                                           : lt_push(fd->arm, esi, sym, len);
}

bool fd_ready(const fountain_dec_t *d) {
    const struct fountain_dec *fd = (const struct fountain_dec *)d;
    if (fd == NULL || fd->codec == 0xffff)
        return false;
    return fd->codec == SDF_FOUNTAIN_NANORQ ? nrq_ready(fd->arm)
                                            : lt_ready(fd->arm);
}

uint32_t fd_rank(const fountain_dec_t *d) {
    const struct fountain_dec *fd = (const struct fountain_dec *)d;
    if (fd == NULL || fd->codec == 0xffff)
        return 0;
    return fd->codec == SDF_FOUNTAIN_NANORQ ? nrq_rank(fd->arm)
                                            : lt_rank(fd->arm);
}

int fd_decode(fountain_dec_t *d, uint8_t *out, size_t block_len) {
    struct fountain_dec *fd = (struct fountain_dec *)d;
    if (fd == NULL || fd->codec == 0xffff)
        return -EINVAL;
    return fd->codec == SDF_FOUNTAIN_NANORQ ? nrq_decode(fd->arm, out, block_len)
                                            : lt_decode(fd->arm, out, block_len);
}

void fd_destroy(fountain_dec_t *d) {
    struct fountain_dec *fd = (struct fountain_dec *)d;
    if (fd == NULL)
        return;
    if (fd->codec == SDF_FOUNTAIN_NANORQ)
        nrq_dec_destroy(fd->arm);
    else if (fd->codec == SDF_FOUNTAIN_LT)
        lt_dec_destroy(fd->arm);
}

/* ---- decode-side arm selection (the coordinator sets it) ------------- */

int fd_init_codec(fountain_dec_t *d, const fd_cfg_t *cfg, uint32_t codec) {
    if (d == NULL || cfg == NULL || fd_sizeof(cfg) == 0)
        return -EINVAL;
    struct fountain_dec *fd = (struct fountain_dec *)d;
    memset(fd, 0, sizeof(*fd));
    fd->codec = codec;
    int rc = codec == SDF_FOUNTAIN_NANORQ ? nrq_dec_init(fd->arm, cfg)
                                          : lt_dec_init(fd->arm, cfg);
    if (rc != 0) {
        fd->codec = 0xffff;
        return rc;
    }
    return 0;
}
