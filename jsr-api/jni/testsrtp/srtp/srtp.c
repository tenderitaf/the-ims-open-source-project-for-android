/*
 * srtp.c
 *
 * the secure real-time transport protocol
 *
 * David A. McGrew
 * Cisco Systems, Inc.
 */
/*
 *
 * Copyright (c) 2001-2006, Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following
 *   disclaimer in the documentation and/or other materials provided
 *   with the distribution.
 *
 *   Neither the name of the Cisco Systems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "srtp_priv.h"
#include "aes_icm.h"         /* aes_icm is used in the KDF  */
#include "alloc.h"           /* for crypto_alloc()          */

#ifndef ASRTPA_KERNEL
# include <limits.h>
# ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
# elif defined(HAVE_WINSOCK2_H)
#  include <winsock2.h>
# endif
#endif /* ! ASRTPA_KERNEL */

extern cipher_type_t aes_icm;
extern auth_type_t tmmhv2;

/* the debug module for srtp */

debug_module_t mod_srtp = { 0, /* debugging is off by default */
"srtp" /* printable name for module   */
};

debug_module_t mod_srtpu = { 0, /* debugging is off by default */
"asrtpa_unprotect" /* printable name for module   */
};
#define octets_in_rtp_header   12
#define uint32s_in_rtp_header  3
#define octets_in_rtcp_header  8
#define uint32s_in_rtcp_header 2

#define LOG_TAG "Asrtpa-1.4.4"

err_status_t asrtpa_stream_alloc(asrtpa_stream_ctx_t **str_ptr,
        const asrtpa_policy_t *p) {
    asrtpa_stream_ctx_t *str;
    err_status_t stat;

    /*
     * This function allocates the stream context, rtp and rtcp ciphers
     * and auth functions, and key limit structure.  If there is a
     * failure during allocation, we free all previously allocated
     * memory and return a failure code.  The code could probably
     * be improved, but it works and should be clear.
     */

    /* allocate srtp stream and set str_ptr */
    str = (asrtpa_stream_ctx_t *) crypto_alloc(sizeof(asrtpa_stream_ctx_t));
    if (str == NULL)
        return err_status_alloc_fail;
    *str_ptr = str;

    /* allocate cipher */
    stat = crypto_kernel_alloc_cipher(p->rtp.cipher_type, &str->rtp_cipher,
            p->rtp.cipher_key_len);
    if (stat) {
        crypto_free(str);
        return stat;
    }

    /* allocate auth function */
    stat = crypto_kernel_alloc_auth(p->rtp.auth_type, &str->rtp_auth,
            p->rtp.auth_key_len, p->rtp.auth_tag_len);
    if (stat) {
        cipher_dealloc(str->rtp_cipher);
        crypto_free(str);
        return stat;
    }

    /* allocate key limit structure */
    str->limit = (key_limit_ctx_t*) crypto_alloc(sizeof(key_limit_ctx_t));
    if (str->limit == NULL) {
        auth_dealloc(str->rtp_auth);
        cipher_dealloc(str->rtp_cipher);
        crypto_free(str);
        return err_status_alloc_fail;
    }

    /*
     * ...and now the RTCP-specific initialization - first, allocate
     * the cipher
     */
    stat = crypto_kernel_alloc_cipher(p->rtcp.cipher_type, &str->rtcp_cipher,
            p->rtcp.cipher_key_len);
    if (stat) {
        auth_dealloc(str->rtp_auth);
        cipher_dealloc(str->rtp_cipher);
        crypto_free(str->limit);
        crypto_free(str);
        return stat;
    }

    /* allocate auth function */
    stat = crypto_kernel_alloc_auth(p->rtcp.auth_type, &str->rtcp_auth,
            p->rtcp.auth_key_len, p->rtcp.auth_tag_len);
    if (stat) {
        cipher_dealloc(str->rtcp_cipher);
        auth_dealloc(str->rtp_auth);
        cipher_dealloc(str->rtp_cipher);
        crypto_free(str->limit);
        crypto_free(str);
        return stat;
    }

    return err_status_ok;
}

err_status_t asrtpa_stream_dealloc(asrtpa_t session, asrtpa_stream_ctx_t *stream) {
    err_status_t status;

    /*
     * we use a conservative deallocation strategy - if any deallocation
     * fails, then we report that fact without trying to deallocate
     * anything else
     */

    /* deallocate cipher, if it is not the same as that in template */
    if (session->stream_template && stream->rtp_cipher
            == session->stream_template->rtp_cipher) {
        /* do nothing */
    } else {
        status = cipher_dealloc(stream->rtp_cipher);
        if (status)
            return status;
    }

    /* deallocate auth function, if it is not the same as that in template */
    if (session->stream_template && stream->rtp_auth
            == session->stream_template->rtp_auth) {
        /* do nothing */
    } else {
        status = auth_dealloc(stream->rtp_auth);
        if (status)
            return status;
    }

    /* deallocate key usage limit, if it is not the same as that in template */
    if (session->stream_template && stream->limit
            == session->stream_template->limit) {
        /* do nothing */
    } else {
        crypto_free(stream->limit);
    }

    /*
     * deallocate rtcp cipher, if it is not the same as that in
     * template
     */
    if (session->stream_template && stream->rtcp_cipher
            == session->stream_template->rtcp_cipher) {
        /* do nothing */
    } else {
        status = cipher_dealloc(stream->rtcp_cipher);
        if (status)
            return status;
    }

    /*
     * deallocate rtcp auth function, if it is not the same as that in
     * template
     */
    if (session->stream_template && stream->rtcp_auth
            == session->stream_template->rtcp_auth) {
        /* do nothing */
    } else {
        status = auth_dealloc(stream->rtcp_auth);
        if (status)
            return status;
    }

    /* deallocate srtp stream context */
    crypto_free(stream);

    return err_status_ok;
}

/*
 * asrtpa_stream_clone(stream_template, new) allocates a new stream and
 * initializes it using the cipher and auth of the stream_template
 *
 * the only unique data in a cloned stream is the replay database and
 * the SSRC
 */

err_status_t asrtpa_stream_clone(const asrtpa_stream_ctx_t *stream_template,
        uint32_t ssrc, asrtpa_stream_ctx_t **str_ptr) {
    err_status_t status;
    asrtpa_stream_ctx_t *str;

	LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: entering function", __FUNCTION__, __LINE__);
    // Add logging here to see if the function is called
    debug_print(mod_srtp, "cloning stream (SSRC: 0x%08x)", ssrc);

    /* allocate srtp stream and set str_ptr */
    str = (asrtpa_stream_ctx_t *) crypto_alloc(sizeof(asrtpa_stream_ctx_t));
    if (str == NULL)
        return err_status_alloc_fail;
    *str_ptr = str;

    /* set cipher and auth pointers to those of the template */
    str->rtp_cipher = stream_template->rtp_cipher;
    str->rtp_auth = stream_template->rtp_auth;
    str->rtcp_cipher = stream_template->rtcp_cipher;
    str->rtcp_auth = stream_template->rtcp_auth;

    /* set key limit to point to that of the template */
    status = key_limit_clone(stream_template->limit, &str->limit);
    if (status)
        return status;

    /* initialize replay databases */
    rdbx_init(&str->rtp_rdbx);
    rdb_init(&str->rtcp_rdb);

    /* set ssrc to that provided */
    str->ssrc = ssrc;

    /* set direction and security services */
    str->direction = stream_template->direction;
    str->rtp_services = stream_template->rtp_services;
    str->rtcp_services = stream_template->rtcp_services;

    /* defensive coding */
    str->next = NULL;

    return err_status_ok;
}

/*
 * key derivation functions, internal to libSRTP
 *
 * asrtpa_kdf_t is a key derivation context
 *
 * asrtpa_kdf_init(&kdf, k) initializes kdf with the key k
 *
 * asrtpa_kdf_generate(&kdf, l, kl, keylen) derives the key
 * corresponding to label l and puts it into kl; the length
 * of the key in octets is provided as keylen.  this function
 * should be called once for each subkey that is derived.
 *
 * asrtpa_kdf_clear(&kdf) zeroizes the kdf state
 */

typedef enum {
    label_rtp_encryption = 0x00,
    label_rtp_msg_auth = 0x01,
    label_rtp_salt = 0x02,
    label_rtcp_encryption = 0x03,
    label_rtcp_msg_auth = 0x04,
    label_rtcp_salt = 0x05
} asrtpa_prf_label;

/*
 * asrtpa_kdf_t represents a key derivation function.  The SRTP
 * default KDF is the only one implemented at present.
 */

typedef struct {
    aes_icm_ctx_t c; /* cipher used for key derivation  */
} asrtpa_kdf_t;

err_status_t asrtpa_kdf_init(asrtpa_kdf_t *kdf, const uint8_t key[30]) {

    aes_icm_context_init(&kdf->c, key);

    return err_status_ok;
}

err_status_t asrtpa_kdf_generate(asrtpa_kdf_t *kdf, asrtpa_prf_label label,
        uint8_t *key, int length) {

    v128_t nonce;

    /* set eigth octet of nonce to <label>, set the rest of it to zero */
    v128_set_to_zero(&nonce);
    nonce.v8[7] = label;

    aes_icm_set_iv(&kdf->c, &nonce);

    /* generate keystream output */
    aes_icm_output(&kdf->c, key, length);

    return err_status_ok;
}

err_status_t asrtpa_kdf_clear(asrtpa_kdf_t *kdf) {

    /* zeroize aes context */
    octet_string_set_to_zero((uint8_t *) kdf, sizeof(asrtpa_kdf_t));

    return err_status_ok;
}

/*
 *  end of key derivation functions
 */

#define MAX_ASRTPA_KEY_LEN 256

err_status_t asrtpa_stream_init_keys(asrtpa_stream_ctx_t *srtp, const void *key) {
    err_status_t stat;
    asrtpa_kdf_t kdf;
    uint8_t tmp_key[MAX_ASRTPA_KEY_LEN];

    /* initialize KDF state     */
    asrtpa_kdf_init(&kdf, (const uint8_t *) key);

    /* generate encryption key  */
    asrtpa_kdf_generate(&kdf, label_rtp_encryption, tmp_key,
            cipher_get_key_length(srtp->rtp_cipher));
    /*
     * if the cipher in the srtp context is aes_icm, then we need
     * to generate the salt value
     */
    if (srtp->rtp_cipher->type == &aes_icm) {
        /* FIX!!! this is really the cipher key length; rest is salt */
        int base_key_len = 16;
        int salt_len = cipher_get_key_length(srtp->rtp_cipher) - base_key_len;

        debug_print(mod_srtp, "found aes_icm, generating salt", NULL);

        /* generate encryption salt, put after encryption key */
        asrtpa_kdf_generate(&kdf, label_rtp_salt, tmp_key + base_key_len,
                salt_len);
    }
    debug_print(mod_srtp, "cipher key: %s", octet_string_hex_string(tmp_key,
            cipher_get_key_length(srtp->rtp_cipher)));

    /* initialize cipher */
    stat = cipher_init(srtp->rtp_cipher, tmp_key, direction_any);
    if (stat) {
        /* zeroize temp buffer */
        octet_string_set_to_zero(tmp_key, MAX_ASRTPA_KEY_LEN);
        return err_status_init_fail;
    }

    /* generate authentication key */
    asrtpa_kdf_generate(&kdf, label_rtp_msg_auth, tmp_key, auth_get_key_length(
            srtp->rtp_auth));
    debug_print(mod_srtp, "auth key:   %s", octet_string_hex_string(tmp_key,
            auth_get_key_length(srtp->rtp_auth)));

    /* initialize auth function */
    stat = auth_init(srtp->rtp_auth, tmp_key);
    if (stat) {
        /* zeroize temp buffer */
        octet_string_set_to_zero(tmp_key, MAX_ASRTPA_KEY_LEN);
        return err_status_init_fail;
    }

    /*
     * ...now initialize SRTCP keys
     */

    /* generate encryption key  */
    asrtpa_kdf_generate(&kdf, label_rtcp_encryption, tmp_key,
            cipher_get_key_length(srtp->rtcp_cipher));
    /*
     * if the cipher in the srtp context is aes_icm, then we need
     * to generate the salt value
     */
    if (srtp->rtcp_cipher->type == &aes_icm) {
        /* FIX!!! this is really the cipher key length; rest is salt */
        int base_key_len = 16;
        int salt_len = cipher_get_key_length(srtp->rtcp_cipher) - base_key_len;

        debug_print(mod_srtp, "found aes_icm, generating rtcp salt", NULL);

        /* generate encryption salt, put after encryption key */
        asrtpa_kdf_generate(&kdf, label_rtcp_salt, tmp_key + base_key_len,
                salt_len);
    }
    debug_print(mod_srtp, "rtcp cipher key: %s", octet_string_hex_string(
            tmp_key, cipher_get_key_length(srtp->rtcp_cipher)));

    /* initialize cipher */
    stat = cipher_init(srtp->rtcp_cipher, tmp_key, direction_any);
    if (stat) {
        /* zeroize temp buffer */
        octet_string_set_to_zero(tmp_key, MAX_ASRTPA_KEY_LEN);
        return err_status_init_fail;
    }

    /* generate authentication key */
    asrtpa_kdf_generate(&kdf, label_rtcp_msg_auth, tmp_key, auth_get_key_length(
            srtp->rtcp_auth));
    debug_print(mod_srtp, "rtcp auth key:   %s", octet_string_hex_string(
            tmp_key, auth_get_key_length(srtp->rtcp_auth)));

    /* initialize auth function */
    stat = auth_init(srtp->rtcp_auth, tmp_key);
    if (stat) {
        /* zeroize temp buffer */
        octet_string_set_to_zero(tmp_key, MAX_ASRTPA_KEY_LEN);
        return err_status_init_fail;
    }

    /* clear memory then return */
    asrtpa_kdf_clear(&kdf);
    octet_string_set_to_zero(tmp_key, MAX_ASRTPA_KEY_LEN);

    return err_status_ok;
}

err_status_t asrtpa_stream_init(asrtpa_stream_ctx_t *srtp, const asrtpa_policy_t *p) {
    err_status_t err;

    debug_print(mod_srtp, "initializing stream (SSRC: 0x%08x)", p->ssrc.value);

    /* initialize replay database */
    rdbx_init(&srtp->rtp_rdbx);

    /* initialize key limit to maximum value */
#ifdef NO_64BIT_MATH
    {
        uint64_t temp;
        temp = make64(UINT_MAX,UINT_MAX);
        key_limit_set(srtp->limit, temp);
    }
#else
    key_limit_set(srtp->limit, 0xffffffffffffLL);
#endif

    /* set the SSRC value */
    srtp->ssrc = htonl(p->ssrc.value);

    /* set the security service flags */
    srtp->rtp_services = p->rtp.sec_serv;
    srtp->rtcp_services = p->rtcp.sec_serv;

    /*
     * set direction to unknown - this flag gets checked in asrtpa_protect(),
     * asrtpa_unprotect(), asrtpa_protect_rtcp(), and asrtpa_unprotect_rtcp(), and
     * gets set appropriately if it is set to unknown.
     */
    srtp->direction = dir_unknown;

    /* initialize SRTCP replay database */
    rdb_init(&srtp->rtcp_rdb);

    /* DAM - no RTCP key limit at present */

    /* initialize keys */
    err = asrtpa_stream_init_keys(srtp, p->key);
    if (err)
        return err;

    return err_status_ok;
}

/*
 * asrtpa_event_reporter is an event handler function that merely
 * reports the events that are reported by the callbacks
 */

void asrtpa_event_reporter(asrtpa_event_data_t *data) {

    LOGE("srtp: in stream 0x%x: ", data->stream->ssrc);

    switch (data->event) {
    case event_ssrc_collision:
        LOGE("\tSSRC collision\n");
        break;
    case event_key_soft_limit:
        LOGE("\tkey usage soft limit reached\n");
        break;
    case event_key_hard_limit:
        LOGE("\tkey usage hard limit reached\n");
        break;
    case event_packet_index_limit:
        LOGE("\tpacket index limit reached\n");
        break;
    default:
        LOGE("\tunknown event reported to handler\n");
    }
}

/*
 * asrtpa_event_handler is a global variable holding a pointer to the
 * event handler function; this function is called for any unexpected
 * event that needs to be handled out of the SRTP data path.  see
 * asrtpa_event_t in srtp.h for more info
 *
 * it is okay to set asrtpa_event_handler to NULL, but we set
 * it to the asrtpa_event_reporter.
 */

static asrtpa_event_handler_func_t *asrtpa_event_handler = asrtpa_event_reporter;

err_status_t asrtpa_install_event_handler(asrtpa_event_handler_func_t func) {

    /*
     * note that we accept NULL arguments intentionally - calling this
     * function with a NULL arguments removes an event handler that's
     * been previously installed
     */

    /* set global event handling function */
    asrtpa_event_handler = func;
    return err_status_ok;
}

err_status_t asrtpa_protect(asrtpa_ctx_t *ctx, void *rtp_hdr, int *pkt_octet_len) {
    asrtpa_hdr_t *hdr = (asrtpa_hdr_t *) rtp_hdr;
    uint32_t *enc_start; /* pointer to start of encrypted portion  */
    uint32_t *auth_start; /* pointer to start of auth. portion      */
    unsigned enc_octet_len = 0; /* number of octets in encrypted portion  */
    xtd_seq_num_t est; /* estimated xtd_seq_num_t of *hdr        */
    int delta; /* delta of local pkt idx and that in hdr */
    uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
    err_status_t status;
    int tag_len;
    asrtpa_stream_ctx_t *stream;
    int prefix_len;

    debug_print(mod_srtp, "function asrtpa_protect", NULL);

    /* we assume the hdr is 32-bit aligned to start */

    /* check the packet length - it must at least contain a full header */
    if (*pkt_octet_len < octets_in_rtp_header)
        return err_status_bad_param;

    /*
     * look up ssrc in asrtpa_stream list, and process the packet with
     * the appropriate stream.  if we haven't seen this stream before,
     * there's a template key for this asrtpa_session, and the cipher
     * supports key-sharing, then we assume that a new stream using
     * that key has just started up
     */
    stream = asrtpa_get_stream(ctx, hdr->ssrc);
    if (stream == NULL) {
        if (ctx->stream_template != NULL) {
            asrtpa_stream_ctx_t *new_stream;

            /* allocate and initialize a new stream */
            status = asrtpa_stream_clone(ctx->stream_template, hdr->ssrc,
                    &new_stream);
            if (status)
                return status;

            /* add new stream to the head of the stream_list */
            new_stream->next = ctx->stream_list;
            ctx->stream_list = new_stream;

            /* set direction to outbound */
            new_stream->direction = dir_asrtpa_sender;

            /* set stream (the pointer used in this function) */
            stream = new_stream;
        } else {
            /* no template stream, so we return an error */
            return err_status_no_ctx;
        }
    }

    /*
     * verify that stream is for sending traffic - this check will
     * detect SSRC collisions, since a stream that appears in both
     * asrtpa_protect() and asrtpa_unprotect() will fail this test in one of
     * those functions.
     */
    if (stream->direction != dir_asrtpa_sender) {
        if (stream->direction == dir_unknown) {
            stream->direction = dir_asrtpa_sender;
        } else {
            asrtpa_handle_event(ctx, stream, event_ssrc_collision);
        }
    }

    /*
     * update the key usage limit, and check it to make sure that we
     * didn't just hit either the soft limit or the hard limit, and call
     * the event handler if we hit either.
     */
    switch (key_limit_update(stream->limit)) {
    case key_event_normal:
        break;
    case key_event_soft_limit:
        asrtpa_handle_event(ctx, stream, event_key_soft_limit);
        break;
    case key_event_hard_limit:
        asrtpa_handle_event(ctx, stream, event_key_hard_limit);
        return err_status_key_expired;
    default:
        break;
    }

    /* get tag length from stream */
    tag_len = auth_get_tag_length(stream->rtp_auth);

    /*
     * find starting point for encryption and length of data to be
     * encrypted - the encrypted portion starts after the rtp header
     * extension, if present; otherwise, it starts after the last csrc,
     * if any are present
     *
     * if we're not providing confidentiality, set enc_start to NULL
     */
    if (stream->rtp_services & sec_serv_conf) {
        enc_start = (uint32_t *) hdr + uint32s_in_rtp_header + hdr->cc;
        if (hdr->x == 1) {
            asrtpa_hdr_xtnd_t *xtn_hdr = (asrtpa_hdr_xtnd_t *) enc_start;
            enc_start += (ntohs(xtn_hdr->length) + 1);
        }
        enc_octet_len = (unsigned int) (*pkt_octet_len - ((enc_start
                - (uint32_t *) hdr) << 2));
    } else {
        enc_start = NULL;
    }

    /*
     * if we're providing authentication, set the auth_start and auth_tag
     * pointers to the proper locations; otherwise, set auth_start to NULL
     * to indicate that no authentication is needed
     */
    if (stream->rtp_services & sec_serv_auth) {
        auth_start = (uint32_t *) hdr;
        auth_tag = (uint8_t *) hdr + *pkt_octet_len;
    } else {
        auth_start = NULL;
        auth_tag = NULL;
    }

    /*
     * estimate the packet index using the start of the replay window
     * and the sequence number from the header
     */
    delta = rdbx_estimate_index(&stream->rtp_rdbx, &est, ntohs(hdr->seq));
    status = rdbx_check(&stream->rtp_rdbx, delta);
    if (status)
        return status; /* we've been asked to reuse an index */
    rdbx_add_index(&stream->rtp_rdbx, delta);

#ifdef NO_64BIT_MATH
    debug_print2(mod_srtp, "estimated packet index: %08x%08x",
            high32(est),low32(est));
#else
    debug_print(mod_srtp, "estimated packet index: %016llx", est);
#endif

    /*
     * if we're using rindael counter mode, set nonce and seq
     */
    if (stream->rtp_cipher->type == &aes_icm) {
        v128_t iv;

        iv.v32[0] = 0;
        iv.v32[1] = hdr->ssrc;
#ifdef NO_64BIT_MATH
        iv.v64[1] = be64_to_cpu(make64((high32(est) << 16) | (low32(est) >> 16),
                        low32(est) << 16));
#else
        iv.v64[1] = be64_to_cpu(est << 16);
#endif
        status = cipher_set_iv(stream->rtp_cipher, &iv);

    } else {
        v128_t iv;

        /* otherwise, set the index to est */
#ifdef NO_64BIT_MATH
        iv.v32[0] = 0;
        iv.v32[1] = 0;
#else
        iv.v64[0] = 0;
#endif
        iv.v64[1] = be64_to_cpu(est);
        status = cipher_set_iv(stream->rtp_cipher, &iv);
    }
    if (status)
        return err_status_cipher_fail;

    /* shift est, put into network byte order */
#ifdef NO_64BIT_MATH
    est = be64_to_cpu(make64((high32(est) << 16) |
                    (low32(est) >> 16),
                    low32(est) << 16));
#else
    est = be64_to_cpu(est << 16);
#endif

    /*
     * if we're authenticating using a universal hash, put the keystream
     * prefix into the authentication tag
     */
    if (auth_start) {

        prefix_len = auth_get_prefix_length(stream->rtp_auth);
        if (prefix_len) {
            status = cipher_output(stream->rtp_cipher, auth_tag, prefix_len);
            if (status)
                return err_status_cipher_fail;
            debug_print(mod_srtp, "keystream prefix: %s",
                    octet_string_hex_string(auth_tag, prefix_len));
        }
    }

    /* if we're encrypting, exor keystream into the message */
    if (enc_start) {
        status = cipher_encrypt(stream->rtp_cipher, (uint8_t *) enc_start,
                &enc_octet_len);
        if (status)
            return err_status_cipher_fail;
    }

    /*
     *  if we're authenticating, run authentication function and put result
     *  into the auth_tag
     */
    if (auth_start) {

        /* initialize auth func context */
        status = auth_start(stream->rtp_auth);
        if (status)
            return status;

        /* run auth func over packet */
        status = auth_update(stream->rtp_auth, (uint8_t *) auth_start,
                *pkt_octet_len);
        if (status)
            return status;

        /* run auth func over ROC, put result into auth_tag */
        debug_print(mod_srtp, "estimated packet index: %016llx", est);
        status = auth_compute(stream->rtp_auth, (uint8_t *) &est, 4, auth_tag);
        debug_print(mod_srtp, "srtp auth tag:    %s", octet_string_hex_string(
                auth_tag, tag_len));
        if (status)
            return err_status_auth_fail;

    }

    if (auth_tag) {

        /* increase the packet length by the length of the auth tag */
        *pkt_octet_len += tag_len;
    }

    return err_status_ok;
}

err_status_t asrtpa_unprotect(asrtpa_ctx_t *ctx, void *asrtpa_hdr, int *pkt_octet_len) {
    //LOGE("[SRTP] Calling asrtpa_unprotect() here!");
    //printf("[SRTP]printf Calling asrtpa_unprotect() here!");
    asrtpa_hdr_t *hdr = (asrtpa_hdr_t *) asrtpa_hdr;
    uint32_t *enc_start; /* pointer to start of encrypted portion  */
    uint32_t *auth_start; /* pointer to start of auth. portion      */
    unsigned enc_octet_len = 0;/* number of octets in encrypted portion */
    uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
    xtd_seq_num_t est; /* estimated xtd_seq_num_t of *hdr        */
    int delta; /* delta of local pkt idx and that in hdr */
    v128_t iv;
    err_status_t status;
    asrtpa_stream_ctx_t *stream;
    uint8_t tmp_tag[ASRTPA_MAX_TAG_LEN];
    int tag_len, prefix_len;

    debug_print(mod_srtpu, "function asrtpa_unprotect", NULL);

    /* we assume the hdr is 32-bit aligned to start */

    /* check the packet length - it must at least contain a full header */
    if (*pkt_octet_len < octets_in_rtp_header)
        return err_status_bad_param;

    /*
     * look up ssrc in asrtpa_stream list, and process the packet with
     * the appropriate stream.  if we haven't seen this stream before,
     * there's only one key for this asrtpa_session, and the cipher
     * supports key-sharing, then we assume that a new stream using
     * that key has just started up
     */
    stream = asrtpa_get_stream(ctx, hdr->ssrc);
    if (stream == NULL) {
        if (ctx->stream_template != NULL) {
            stream = ctx->stream_template;
            debug_print(mod_srtpu, "using provisional stream (SSRC: 0x%08x)",
                    hdr->ssrc);

            /*
             * set estimated packet index to sequence number from header,
             * and set delta equal to the same value
             */
#ifdef NO_64BIT_MATH
            est = (xtd_seq_num_t) make64(0,ntohs(hdr->seq));
            delta = low32(est);
#else
            est = (xtd_seq_num_t) ntohs(hdr->seq);
            delta = (int) est;
#endif
        } else {

            /*
             * no stream corresponding to SSRC found, and we don't do
             * key-sharing, so return an error
             */
            return err_status_no_ctx;
        }
    } else {

        /* estimate packet index from seq. num. in header */
        delta = rdbx_estimate_index(&stream->rtp_rdbx, &est, ntohs(hdr->seq));

        /* check replay database */
        status = rdbx_check(&stream->rtp_rdbx, delta);
        if (status)
            return status;
    }

#ifdef NO_64BIT_MATH
    debug_print2(mod_srtpu, "estimated u_packet index: %08x%08x", high32(est),low32(est));
#else
    debug_print(mod_srtpu, "estimated u_packet index: %016llx", est);
#endif

    /* get tag length from stream */
    tag_len = auth_get_tag_length(stream->rtp_auth);

    /*
     * set the cipher's IV properly, depending on whatever cipher we
     * happen to be using
     */
    if (stream->rtp_cipher->type == &aes_icm) {

        /* aes counter mode */
        iv.v32[0] = 0;
        iv.v32[1] = hdr->ssrc; /* still in network order */
#ifdef NO_64BIT_MATH
        iv.v64[1] = be64_to_cpu(make64((high32(est) << 16) | (low32(est) >> 16),
                        low32(est) << 16));
#else
        iv.v64[1] = be64_to_cpu(est << 16);
#endif
        status
                = aes_icm_set_iv((aes_icm_ctx_t*) stream->rtp_cipher->state,
                        &iv);
    } else {

        /* no particular format - set the iv to the pakcet index */
#ifdef NO_64BIT_MATH
        iv.v32[0] = 0;
        iv.v32[1] = 0;
#else
        iv.v64[0] = 0;
#endif
        iv.v64[1] = be64_to_cpu(est);
        status = cipher_set_iv(stream->rtp_cipher, &iv);
    }
    if (status)
        return err_status_cipher_fail;

    /* shift est, put into network byte order */
#ifdef NO_64BIT_MATH
    est = be64_to_cpu(make64((high32(est) << 16) |
                    (low32(est) >> 16),
                    low32(est) << 16));
#else
    est = be64_to_cpu(est << 16);
#endif

    /*
     * find starting point for decryption and length of data to be
     * decrypted - the encrypted portion starts after the rtp header
     * extension, if present; otherwise, it starts after the last csrc,
     * if any are present
     *
     * if we're not providing confidentiality, set enc_start to NULL
     */
    if (stream->rtp_services & sec_serv_conf) {
        enc_start = (uint32_t *) hdr + uint32s_in_rtp_header + hdr->cc;
        if (hdr->x == 1) {
            asrtpa_hdr_xtnd_t *xtn_hdr = (asrtpa_hdr_xtnd_t *) enc_start;
            enc_start += (ntohs(xtn_hdr->length) + 1);
        }
        enc_octet_len = (uint32_t)(*pkt_octet_len - tag_len - ((enc_start
                - (uint32_t *) hdr) << 2));
    } else {
        enc_start = NULL;
    }

    /*
     * if we're providing authentication, set the auth_start and auth_tag
     * pointers to the proper locations; otherwise, set auth_start to NULL
     * to indicate that no authentication is needed
     */
    if (stream->rtp_services & sec_serv_auth) {
        auth_start = (uint32_t *) hdr;
        auth_tag = (uint8_t *) hdr + *pkt_octet_len - tag_len;
    } else {
        auth_start = NULL;
        auth_tag = NULL;
    }

    /*
     * if we expect message authentication, run the authentication
     * function and compare the result with the value of the auth_tag
     */
    if (auth_start) {

        /*
         * if we're using a universal hash, then we need to compute the
         * keystream prefix for encrypting the universal hash output
         *
         * if the keystream prefix length is zero, then we know that
         * the authenticator isn't using a universal hash function
         */
        if (stream->rtp_auth->prefix_len != 0) {

            prefix_len = auth_get_prefix_length(stream->rtp_auth);
            status = cipher_output(stream->rtp_cipher, tmp_tag, prefix_len);
            debug_print(mod_srtpu, "keystream prefix: %s",
                    octet_string_hex_string(tmp_tag, prefix_len));
            if (status)
                return err_status_cipher_fail;
        }

        /* initialize auth func context */
        status = auth_start(stream->rtp_auth);
        if (status)
            return status;

        /* now compute auth function over packet */
        status = auth_update(stream->rtp_auth, (uint8_t *) auth_start,
                *pkt_octet_len - tag_len);

        /* run auth func over ROC, then write tmp tag */
        status = auth_compute(stream->rtp_auth, (uint8_t *) &est, 4, tmp_tag);

        debug_print(mod_srtpu, "computed auth tag:    %s",
                octet_string_hex_string(tmp_tag, tag_len));
        debug_print(mod_srtpu, "packet auth tag:      %s",
                octet_string_hex_string(auth_tag, tag_len));
        if (status)
            return err_status_auth_fail;

        if (octet_string_is_eq(tmp_tag, auth_tag, tag_len))
            return err_status_auth_fail;
    }

    /*
     * update the key usage limit, and check it to make sure that we
     * didn't just hit either the soft limit or the hard limit, and call
     * the event handler if we hit either.
     */
    switch (key_limit_update(stream->limit)) {
    case key_event_normal:
        break;
    case key_event_soft_limit:
        asrtpa_handle_event(ctx, stream, event_key_soft_limit);
        break;
    case key_event_hard_limit:
        asrtpa_handle_event(ctx, stream, event_key_hard_limit);
        return err_status_key_expired;
    default:
        break;
    }

    /* if we're encrypting, add keystream into ciphertext */
    if (enc_start) {
        status = cipher_encrypt(stream->rtp_cipher, (uint8_t *) enc_start,
                &enc_octet_len);
        if (status)
            return err_status_cipher_fail;
    }

    /*
     * verify that stream is for received traffic - this check will
     * detect SSRC collisions, since a stream that appears in both
     * asrtpa_protect() and asrtpa_unprotect() will fail this test in one of
     * those functions.
     *
     * we do this check *after* the authentication check, so that the
     * latter check will catch any attempts to fool us into thinking
     * that we've got a collision
     */
    if (stream->direction != dir_asrtpa_receiver) {
        if (stream->direction == dir_unknown) {
            stream->direction = dir_asrtpa_receiver;
        } else {
            asrtpa_handle_event(ctx, stream, event_ssrc_collision);
        }
    }

    /*
     * if the stream is a 'provisional' one, in which the template context
     * is used, then we need to allocate a new stream at this point, since
     * the authentication passed
     */
    if (stream == ctx->stream_template) {
        asrtpa_stream_ctx_t *new_stream;

        /*
         * allocate and initialize a new stream
         *
         * note that we indicate failure if we can't allocate the new
         * stream, and some implementations will want to not return
         * failure here
         */
        status
                = asrtpa_stream_clone(ctx->stream_template, hdr->ssrc,
                        &new_stream);
        if (status)
            return status;

        /* add new stream to the head of the stream_list */
        new_stream->next = ctx->stream_list;
        ctx->stream_list = new_stream;

        /* set stream (the pointer used in this function) */
        stream = new_stream;
    }

    /*
     * the message authentication function passed, so add the packet
     * index into the replay database
     */
    rdbx_add_index(&stream->rtp_rdbx, delta);

    /* decrease the packet length by the length of the auth tag */
    *pkt_octet_len -= tag_len;

    return err_status_ok;
}

err_status_t asrtpa_init(int forceInit) {
    err_status_t status;

    /* initialize crypto kernel */
    status = crypto_kernel_init(forceInit);
    if (status)
        return status;

    /* load srtp debug module into the kernel */
    status = crypto_kernel_load_debug_module(&mod_srtp);
    if (status)
        return status;

    return err_status_ok;
}

err_status_t asrtpa_deinit() {
  err_status_t status;

  status = crypto_kernel_shutdown();

  return status;
}

/*
 * The following code is under consideration for removal.  See
 * ASRTPA_MAX_TRAILER_LEN
 */
#if 0

/*
 * asrtpa_get_trailer_length(&a) returns the number of octets that will
 * be added to an RTP packet by the SRTP processing.  This value
 * is constant for a given asrtpa_stream_t (i.e. between initializations).
 */

int
asrtpa_get_trailer_length(const asrtpa_stream_t s) {
    return auth_get_tag_length(s->rtp_auth);
}

#endif

/*
 * asrtpa_get_stream(ssrc) returns a pointer to the stream corresponding
 * to ssrc, or NULL if no stream exists for that ssrc
 *
 * this is an internal function
 */

asrtpa_stream_ctx_t *
asrtpa_get_stream(asrtpa_t srtp, uint32_t ssrc) {
    asrtpa_stream_ctx_t *stream;

    /* walk down list until ssrc is found */
    stream = srtp->stream_list;
    while (stream != NULL) {
        if (stream->ssrc == ssrc)
            return stream;
        stream = stream->next;
    }

    /* we haven't found our ssrc, so return a null */
    return NULL;
}

err_status_t asrtpa_dealloc(asrtpa_t session) {
    asrtpa_stream_ctx_t *stream;
    err_status_t status;

    /*
     * we take a conservative deallocation strategy - if we encounter an
     * error deallocating a stream, then we stop trying to deallocate
     * memory and just return an error
     */

    /* walk list of streams, deallocating as we go */
    stream = session->stream_list;
    while (stream != NULL) {
        asrtpa_stream_t next = stream->next;
        status = asrtpa_stream_dealloc(session, stream);
        if (status)
            return status;
        stream = next;
    }

    /* deallocate stream template, if there is one */
    if (session->stream_template != NULL) {
        status = auth_dealloc(session->stream_template->rtcp_auth);
        if (status)
            return status;
        status = cipher_dealloc(session->stream_template->rtcp_cipher);
        if (status)
            return status;
        crypto_free(session->stream_template->limit);
        status = cipher_dealloc(session->stream_template->rtp_cipher);
        if (status)
            return status;
        status = auth_dealloc(session->stream_template->rtp_auth);
        if (status)
            return status;
        crypto_free(session->stream_template);
    }

    /* deallocate session context */
    crypto_free(session);

    return err_status_ok;
}

err_status_t asrtpa_add_stream(asrtpa_t session, const asrtpa_policy_t *policy) {
    err_status_t status;
    asrtpa_stream_t tmp;

    /* sanity check arguments */
    if ((session == NULL) || (policy == NULL) || (policy->key == NULL))
        return err_status_bad_param;

    /* allocate stream  */
    status = asrtpa_stream_alloc(&tmp, policy);
    if (status) {
        return status;
    }

    /* initialize stream  */
    status = asrtpa_stream_init(tmp, policy);
    if (status) {
        crypto_free(tmp);
        return status;
    }

    /*
     * set the head of the stream list or the template to point to the
     * stream that we've just alloced and init'ed, depending on whether
     * or not it has a wildcard SSRC value or not
     *
     * if the template stream has already been set, then the policy is
     * inconsistent, so we return a bad_param error code
     */
    switch (policy->ssrc.type) {
    case (ssrc_any_outbound):
        if (session->stream_template) {
            return err_status_bad_param;
        }
        session->stream_template = tmp;
        session->stream_template->direction = dir_asrtpa_sender;
        break;
    case (ssrc_any_inbound):
        if (session->stream_template) {
            return err_status_bad_param;
        }
        session->stream_template = tmp;
        session->stream_template->direction = dir_asrtpa_receiver;
        break;
    case (ssrc_specific):
        tmp->next = session->stream_list;
        session->stream_list = tmp;
        break;
    case (ssrc_undefined):
    default:
        crypto_free(tmp);
        return err_status_bad_param;
    }

    return err_status_ok;
}

err_status_t asrtpa_create(asrtpa_t *session, /* handle for session     */
const asrtpa_policy_t *policy) { /* SRTP policy (list)     */
    err_status_t stat;
    asrtpa_ctx_t *ctx;

    /* sanity check arguments */
    if (session == NULL)
        return err_status_bad_param;

    /* allocate srtp context and set ctx_ptr */
    ctx = (asrtpa_ctx_t *) crypto_alloc(sizeof(asrtpa_ctx_t));
    if (ctx == NULL)
        return err_status_alloc_fail;
    *session = ctx;

    /*
     * loop over elements in the policy list, allocating and
     * initializing a stream for each element
     */
    ctx->stream_template = NULL;
    ctx->stream_list = NULL;
    while (policy != NULL) {

        stat = asrtpa_add_stream(ctx, policy);
        if (stat) {
            /* clean up everything */
            asrtpa_dealloc(*session);
            return stat;
        }

        /* set policy to next item in list  */
        policy = policy->next;
    }

    return err_status_ok;
}

err_status_t asrtpa_remove_stream(asrtpa_t session, uint32_t ssrc) {
    asrtpa_stream_ctx_t *stream, *last_stream;
    err_status_t status;

    /* sanity check arguments */
    if (session == NULL)
        return err_status_bad_param;

    /* find stream in list; complain if not found */
    last_stream = stream = session->stream_list;
    while ((stream != NULL) && (ssrc != stream->ssrc)) {
        last_stream = stream;
        stream = stream->next;
    }
    if (stream == NULL)
        return err_status_no_ctx;

    /* remove stream from the list */
    last_stream->next = stream->next;

    /* deallocate the stream */
    status = asrtpa_stream_dealloc(session, stream);
    if (status)
        return status;

    return err_status_ok;
}

/*
 * the default policy - provides a convenient way for callers to use
 * the default security policy
 *
 * this policy is that defined in the current SRTP internet draft.
 *
 */

/*
 * NOTE: cipher_key_len is really key len (128 bits) plus salt len
 *  (112 bits)
 */
/* There are hard-coded 16's for base_key_len in the key generation code */

void crypto_policy_set_rtp_default(crypto_policy_t *p) {

    p->cipher_type = AES_128_ICM;
    p->cipher_key_len = 30; /* default 128 bits per RFC 3711 */
    p->auth_type = HMAC_SHA1;
    p->auth_key_len = 20; /* default 160 bits per RFC 3711 */
    p->auth_tag_len = 10; /* default 80 bits per RFC 3711 */
    p->sec_serv = sec_serv_conf_and_auth;

}

void crypto_policy_set_rtcp_default(crypto_policy_t *p) {

    p->cipher_type = AES_128_ICM;
    p->cipher_key_len = 30; /* default 128 bits per RFC 3711 */
    p->auth_type = HMAC_SHA1;
    p->auth_key_len = 20; /* default 160 bits per RFC 3711 */
    p->auth_tag_len = 10; /* default 80 bits per RFC 3711 */
    p->sec_serv = sec_serv_conf_and_auth;

}

void crypto_policy_set_aes_cm_128_hmac_sha1_32(crypto_policy_t *p) {

    /*
     * corresponds to draft-ietf-mmusic-sdescriptions-12.txt
     *
     * note that this crypto policy is intended for SRTP, but not SRTCP
     */

    p->cipher_type = AES_128_ICM;
    p->cipher_key_len = 30; /* 128 bit key, 112 bit salt */
    p->auth_type = HMAC_SHA1;
    p->auth_key_len = 20; /* 160 bit key               */
    p->auth_tag_len = 4; /* 32 bit tag                */
    p->sec_serv = sec_serv_conf_and_auth;

}

void crypto_policy_set_aes_cm_128_null_auth(crypto_policy_t *p) {

    /*
     * corresponds to draft-ietf-mmusic-sdescriptions-12.txt
     *
     * note that this crypto policy is intended for SRTP, but not SRTCP
     */

    p->cipher_type = AES_128_ICM;
    p->cipher_key_len = 30; /* 128 bit key, 112 bit salt */
    p->auth_type = NULL_AUTH;
    p->auth_key_len = 0;
    p->auth_tag_len = 0;
    p->sec_serv = sec_serv_conf;

}

void crypto_policy_set_null_cipher_hmac_sha1_80(crypto_policy_t *p) {

    /*
     * corresponds to draft-ietf-mmusic-sdescriptions-12.txt
     */

    p->cipher_type = NULL_CIPHER;
    p->cipher_key_len = 0;
    p->auth_type = HMAC_SHA1;
    p->auth_key_len = 20;
    p->auth_tag_len = 10;
    p->sec_serv = sec_serv_auth;

}

/*
 * secure rtcp functions
 */

err_status_t asrtpa_protect_rtcp(asrtpa_t ctx, void *rtcp_hdr, int *pkt_octet_len) {
    srtcp_hdr_t *hdr = (srtcp_hdr_t *) rtcp_hdr;
    uint32_t *enc_start; /* pointer to start of encrypted portion  */
    uint32_t *auth_start; /* pointer to start of auth. portion      */
    uint32_t *trailer; /* pointer to start of trailer            */
    unsigned enc_octet_len = 0;/* number of octets in encrypted portion */
    uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
    err_status_t status;
    int tag_len;
    asrtpa_stream_ctx_t *stream;
    int prefix_len;
    uint32_t seq_num;

	LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: entering function", __FUNCTION__, __LINE__);
    // Add logging here to see if the function is called
	
    /* we assume the hdr is 32-bit aligned to start */
    /*
     * look up ssrc in asrtpa_stream list, and process the packet with
     * the appropriate stream.  if we haven't seen this stream before,
     * there's only one key for this asrtpa_session, and the cipher
     * supports key-sharing, then we assume that a new stream using
     * that key has just started up
     */
    stream = asrtpa_get_stream(ctx, hdr->ssrc);
    if (stream == NULL) {
		
       if (ctx->stream_template != NULL) {
		   LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: new stream started", __FUNCTION__, __LINE__);
    // Add logging here to see if a new stream is started
            asrtpa_stream_ctx_t *new_stream;

            /* allocate and initialize a new stream */
            status = asrtpa_stream_clone(ctx->stream_template, hdr->ssrc,
                    &new_stream);
            if (status) {
				LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error cloning stream, code=%d", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the stream was cloned
                return status;
            }

            /* add new stream to the head of the stream_list */
            new_stream->next = ctx->stream_list;
            ctx->stream_list = new_stream;

            /* set stream (the pointer used in this function) */
            stream = new_stream;
        } else {
			LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: stream template is null, returning err_status_no_ctx", __FUNCTION__, __LINE__);
    // Add logging here to see if an error is returned when starting a new stream
            /* no template stream, so we return an error */
            return err_status_no_ctx;
        }
    }

    /*
     * verify that stream is for sending traffic - this check will
     * detect SSRC collisions, since a stream that appears in both
     * asrtpa_protect() and asrtpa_unprotect() will fail this test in one of
     * those functions.
     */
    if (stream->direction != dir_asrtpa_sender) {
        if (stream->direction == dir_unknown) {
            stream->direction = dir_asrtpa_sender;
        } else {
            asrtpa_handle_event(ctx, stream, event_ssrc_collision);
        }
    }

    /* get tag length from stream context */
    tag_len = auth_get_tag_length(stream->rtcp_auth);

    /*
     * set encryption start and encryption length - if we're not
     * providing confidentiality, set enc_start to NULL
     */
    enc_start = (uint32_t *) hdr + uint32s_in_rtcp_header;
    enc_octet_len = *pkt_octet_len - octets_in_rtcp_header;

    /* all of the packet, except the header, gets encrypted */
    /* NOTE: hdr->length is not usable - it refers to only the first
     RTCP report in the compound packet! */
    /* NOTE: trailer is 32-bit aligned because RTCP 'packets' are always
     multiples of 32-bits (RFC 3550 6.1) */
    trailer = (uint32_t *) ((char *) enc_start + enc_octet_len);

    if (stream->rtcp_services & sec_serv_conf) {
        *trailer = htonl(SRTCP_E_BIT); /* set encrypt bit */
    } else {
        enc_start = NULL;
        enc_octet_len = 0;
        /* 0 is network-order independant */
        *trailer = 0x00000000; /* set encrypt bit */
    }

    /*
     * set the auth_start and auth_tag pointers to the proper locations
     * (note that srtpc *always* provides authentication, unlike srtp)
     */
    /* Note: This would need to change for optional mikey data */
    auth_start = (uint32_t *) hdr;
    auth_tag = (uint8_t *) hdr + *pkt_octet_len + sizeof(srtcp_trailer_t);

    /*
     * check sequence number for overruns, and copy it into the packet
     * if its value isn't too big
     */
  status = rdb_overflow(&stream->rtcp_rdb);
    if (status) {
		LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error from rdb_overflow, code=%d", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns here before the auth tagis computed
        return status;
    }
    seq_num = rdb_get_value(&stream->rtcp_rdb);
  rdb_increment(&stream->rtcp_rdb);
    *trailer |= htonl(seq_num);
    debug_print(mod_srtp, "srtcp index: %x", seq_num);

    /*
     * if we're using rindael counter mode, set nonce and seq
     */
    if (stream->rtcp_cipher->type == &aes_icm) {
        v128_t iv;

        iv.v32[0] = 0;
        iv.v32[1] = hdr->ssrc; /* still in network order! */
        iv.v32[2] = htonl(seq_num >> 16);
        iv.v32[3] = htonl(seq_num << 16);
        status = aes_icm_set_iv((aes_icm_ctx_t*) stream->rtcp_cipher->state,
                &iv);

    } else {
        v128_t iv;

        /* otherwise, just set the index to seq_num */
        iv.v32[0] = 0;
        iv.v32[1] = 0;
        iv.v32[2] = 0;
        iv.v32[3] = htonl(seq_num);
        status = cipher_set_iv(stream->rtcp_cipher, &iv);
    }
    if (status) {
		LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: setting cipher failed, returning err_status_cipher_fail", __FUNCTION__, __LINE__);
    // Add logging here to see if the cipher fials and the function returns
        return err_status_cipher_fail;
    }

    /*
     * if we're authenticating using a universal hash, put the keystream
     * prefix into the authentication tag
     */

    /* if auth_start is non-null, then put keystream into tag  */
    if (auth_start) {

        /* put keystream prefix into auth_tag */
        prefix_len = auth_get_prefix_length(stream->rtcp_auth);
        status = cipher_output(stream->rtcp_cipher, auth_tag, prefix_len);

        debug_print(mod_srtp, "keystream prefix: %s", octet_string_hex_string(
                auth_tag, prefix_len));

        if (status) {
			LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error from cipher_output, code=[%d], returning err_status_cipher_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
            return err_status_cipher_fail;
        }
    }

    /* if we're encrypting, exor keystream into the message */
    if (enc_start) {
        status = cipher_encrypt(stream->rtcp_cipher, (uint8_t *) enc_start,
                &enc_octet_len);
        if (status) {
			LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error from cipher_encrypt, code=[%d], returning err_status_cipher_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if function returns
            return err_status_cipher_fail;
        }
    }

    /* initialize auth func context */
    auth_start(stream->rtcp_auth);

    /*
     * run auth func over packet (including trailer), and write the
     * result at auth_tag
     */
    status = auth_compute(stream->rtcp_auth, (uint8_t *) auth_start,
            (*pkt_octet_len) + sizeof(srtcp_trailer_t), auth_tag);
    debug_print(mod_srtp, "srtcp auth tag:    %s", octet_string_hex_string(
            auth_tag, tag_len));
    if (status) {
		LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error from auth_compute, code=[%d], returning err_status_auth_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the authentication fails
        return err_status_auth_fail;
    }

    /* increase the packet length by the length of the auth tag and seq_num*/
    *pkt_octet_len += (tag_len + sizeof(srtcp_trailer_t));
	//LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: everything good, returning OK", __FUNCTION__, __LINE__);
    // Add logging here to see if the method returns OK
    return err_status_ok;
}

err_status_t asrtpa_unprotect_rtcp(asrtpa_t ctx, void *srtcp_hdr,
        int *pkt_octet_len) {
    srtcp_hdr_t *hdr = (srtcp_hdr_t *) srtcp_hdr;
    uint32_t *enc_start; /* pointer to start of encrypted portion  */
    uint32_t *auth_start; /* pointer to start of auth. portion      */
    uint32_t *trailer; /* pointer to start of trailer            */
    unsigned enc_octet_len = 0;/* number of octets in encrypted portion */
    uint8_t *auth_tag = NULL; /* location of auth_tag within packet     */
    uint8_t tmp_tag[ASRTPA_MAX_TAG_LEN];
    err_status_t status;
    int tag_len;
    asrtpa_stream_ctx_t *stream;
    int prefix_len;
    uint32_t seq_num;

    /* we assume the hdr is 32-bit aligned to start */
    /*
     * look up ssrc in asrtpa_stream list, and process the packet with
     * the appropriate stream.  if we haven't seen this stream before,
     * there's only one key for this asrtpa_session, and the cipher
     * supports key-sharing, then we assume that a new stream using
     * that key has just started up
     */
    stream = asrtpa_get_stream(ctx, hdr->ssrc);
    if (stream == NULL) {
        if (ctx->stream_template != NULL) {
            stream = ctx->stream_template;
            debug_print(mod_srtp,
                    "srtcp using provisional stream (SSRC: 0x%08x)", hdr->ssrc);
        } else {
            /* no template stream, so we return an error */
			LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: no template stream, so we return err_status_no_ctx", __FUNCTION__, __LINE__);
    // Add logging here to see if the function returns 
            return err_status_no_ctx;
        }
    }

    /* get tag length from stream context */
    tag_len = auth_get_tag_length(stream->rtcp_auth);

    /*
     * set encryption start, encryption length, and trailer
     */
    enc_octet_len = *pkt_octet_len - (octets_in_rtcp_header + tag_len
            + sizeof(srtcp_trailer_t));
    /* index & E (encryption) bit follow normal data.  hdr->len
     is the number of words (32-bit) in the normal packet minus 1 */
    /* This should point trailer to the word past the end of the
     normal data. */
    /* This would need to be modified for optional mikey data */
    /*
     * NOTE: trailer is 32-bit aligned because RTCP 'packets' are always
     *     multiples of 32-bits (RFC 3550 6.1)
     */
    trailer = (uint32_t *) ((char *) hdr + *pkt_octet_len - (tag_len
            + sizeof(srtcp_trailer_t)));
    if (*((unsigned char *) trailer) & SRTCP_E_BYTE_BIT) {
        enc_start = (uint32_t *) hdr + uint32s_in_rtcp_header;
    } else {
        enc_octet_len = 0;
        enc_start = NULL; /* this indicates that there's no encryption */
    }

    /*
     * set the auth_start and auth_tag pointers to the proper locations
     * (note that srtcp *always* uses authentication, unlike srtp)
     */
    auth_start = (uint32_t *) hdr;
    auth_tag = (uint8_t *) hdr + *pkt_octet_len - tag_len;

    /*
     * check the sequence number for replays
     */
    /* this is easier than dealing with bitfield access */
    seq_num = ntohl(*trailer) & SRTCP_INDEX_MASK;
    debug_print(mod_srtp, "srtcp index: %x", seq_num);
    status = rdb_check(&stream->rtcp_rdb, seq_num);
    if (status) {
        LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error from rdb_check, returning error code=%d", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
		return status;
    }

    /*
     * if we're using aes counter mode, set nonce and seq
     */
    if (stream->rtcp_cipher->type == &aes_icm) {
        v128_t iv;

        iv.v32[0] = 0;
        iv.v32[1] = hdr->ssrc; /* still in network order! */
        iv.v32[2] = htonl(seq_num >> 16);
        iv.v32[3] = htonl(seq_num << 16);
        status = aes_icm_set_iv((aes_icm_ctx_t*) stream->rtcp_cipher->state,
                &iv);

    } else {
        v128_t iv;

        /* otherwise, just set the index to seq_num */
        iv.v32[0] = 0;
        iv.v32[1] = 0;
        iv.v32[2] = 0;
        iv.v32[3] = htonl(seq_num);
        status = cipher_set_iv(stream->rtcp_cipher, &iv);

    }
    if (status) {
        LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error setting cipher, code=[%d], returning err_status_cipher_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
		return err_status_cipher_fail;
    }

    /* initialize auth func context */
    auth_start(stream->rtcp_auth);

    /* run auth func over packet, put result into tmp_tag */
    status = auth_compute(stream->rtcp_auth, (uint8_t *) auth_start,
            *pkt_octet_len - tag_len, tmp_tag);
    debug_print(mod_srtp, "srtcp computed tag:       %s",
            octet_string_hex_string(tmp_tag, tag_len));
    if (status) {
        LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error from auth_compute, code=[%d], returning err_status_auth_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
		return err_status_auth_fail;
    }

    /* compare the tag just computed with the one in the packet */
    debug_print(mod_srtp, "srtcp tag from packet:    %s",
            octet_string_hex_string(auth_tag, tag_len));
    if (octet_string_is_eq(tmp_tag, auth_tag, tag_len)) {
        LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: tmp_tag is equal to auth tag, returning err_status_auth_fail", __FUNCTION__, __LINE__);
    // Add logging here to see if the function returns
		return err_status_auth_fail;
    }

    /*
     * if we're authenticating using a universal hash, put the keystream
     * prefix into the authentication tag
     */
    prefix_len = auth_get_prefix_length(stream->rtcp_auth);
    if (prefix_len) {
        status = cipher_output(stream->rtcp_cipher, auth_tag, prefix_len);
        debug_print(mod_srtp, "keystream prefix: %s", octet_string_hex_string(
                auth_tag, prefix_len));
        if (status) {
            LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error #%d from cipher_output, returning err_status_cipher_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
			return err_status_cipher_fail;
        }
    }

    /* if we're decrypting, exor keystream into the message */
    if (enc_start) {
        status = cipher_encrypt(stream->rtcp_cipher, (uint8_t *) enc_start,
                &enc_octet_len);
        if (status) {
            LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error #%d from cipher_encrypt, returning err_status_cipher_fail", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
			return err_status_cipher_fail;
        }
    }

    /* decrease the packet length by the length of the auth tag and seq_num*/
    *pkt_octet_len -= (tag_len + sizeof(srtcp_trailer_t));

    /*
     * verify that stream is for received traffic - this check will
     * detect SSRC collisions, since a stream that appears in both
     * asrtpa_protect() and asrtpa_unprotect() will fail this test in one of
     * those functions.
     *
     * we do this check *after* the authentication check, so that the
     * latter check will catch any attempts to fool us into thinking
     * that we've got a collision
     */
    if (stream->direction != dir_asrtpa_receiver) {
        if (stream->direction == dir_unknown) {
            stream->direction = dir_asrtpa_receiver;
        } else {
            asrtpa_handle_event(ctx, stream, event_ssrc_collision);
        }
    }

    /*
     * if the stream is a 'provisional' one, in which the template context
     * is used, then we need to allocate a new stream at this point, since
     * the authentication passed
     */
    if (stream == ctx->stream_template) {
        asrtpa_stream_ctx_t *new_stream;

        /*
         * allocate and initialize a new stream
         *
         * note that we indicate failure if we can't allocate the new
         * stream, and some implementations will want to not return
         * failure here
         */
        status
                = asrtpa_stream_clone(ctx->stream_template, hdr->ssrc,
                        &new_stream);
        if (status) {
            LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: error #%d from asrtpa_stream_clone, returning it", __FUNCTION__, __LINE__, status);
    // Add logging here to see if the function returns
			return status;
        }

        /* add new stream to the head of the stream_list */
        new_stream->next = ctx->stream_list;
        ctx->stream_list = new_stream;

        /* set stream (the pointer used in this function) */
        stream = new_stream;
    }

    /* we've passed the authentication check, so add seq_num to the rdb */
    rdb_add_index(&stream->rtcp_rdb, seq_num);

    LOGE("[LIBSRTP] srtp.c, func %s(), line# %d: everything good, returning OK", __FUNCTION__, __LINE__);
    // Add logging here to see if the function returns
	return err_status_ok;
}

/*
 * dtls keying for srtp
 */

err_status_t crypto_policy_set_from_profile_for_rtp(crypto_policy_t *policy,
        asrtpa_profile_t profile) {

    /* set SRTP policy from the SRTP profile in the key set */
    switch (profile) {
    case asrtpa_profile_aes128_cm_sha1_80:
        crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
        crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
        break;
    case asrtpa_profile_aes128_cm_sha1_32:
        crypto_policy_set_aes_cm_128_hmac_sha1_32(policy);
        crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
        break;
    case asrtpa_profile_null_sha1_80:
        crypto_policy_set_null_cipher_hmac_sha1_80(policy);
        crypto_policy_set_null_cipher_hmac_sha1_80(policy);
        break;
        /* the following profiles are not (yet) supported */
    case asrtpa_profile_null_sha1_32:
    case asrtpa_profile_aes256_cm_sha1_80:
    case asrtpa_profile_aes256_cm_sha1_32:
    default:
        return err_status_bad_param;
    }

    return err_status_ok;
}

err_status_t crypto_policy_set_from_profile_for_rtcp(crypto_policy_t *policy,
        asrtpa_profile_t profile) {

    /* set SRTP policy from the SRTP profile in the key set */
    switch (profile) {
    case asrtpa_profile_aes128_cm_sha1_80:
        crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
        break;
    case asrtpa_profile_aes128_cm_sha1_32:
        crypto_policy_set_aes_cm_128_hmac_sha1_80(policy);
        break;
    case asrtpa_profile_null_sha1_80:
        crypto_policy_set_null_cipher_hmac_sha1_80(policy);
        break;
        /* the following profiles are not (yet) supported */
    case asrtpa_profile_null_sha1_32:
    case asrtpa_profile_aes256_cm_sha1_80:
    case asrtpa_profile_aes256_cm_sha1_32:
    default:
        return err_status_bad_param;
    }

    return err_status_ok;
}

void append_salt_to_key(uint8_t *key, unsigned int bytes_in_key, uint8_t *salt,
        unsigned int bytes_in_salt) {

    memcpy(key + bytes_in_key, salt, bytes_in_salt);

}

unsigned int asrtpa_profile_get_master_key_length(asrtpa_profile_t profile) {

    switch (profile) {
    case asrtpa_profile_aes128_cm_sha1_80:
        return 16;
        break;
    case asrtpa_profile_aes128_cm_sha1_32:
        return 16;
        break;
    case asrtpa_profile_null_sha1_80:
        return 16;
        break;
        /* the following profiles are not (yet) supported */
    case asrtpa_profile_null_sha1_32:
    case asrtpa_profile_aes256_cm_sha1_80:
    case asrtpa_profile_aes256_cm_sha1_32:
    default:
        return 0; /* indicate error by returning a zero */
    }
}

unsigned int asrtpa_profile_get_master_salt_length(asrtpa_profile_t profile) {

    switch (profile) {
    case asrtpa_profile_aes128_cm_sha1_80:
        return 14;
        break;
    case asrtpa_profile_aes128_cm_sha1_32:
        return 14;
        break;
    case asrtpa_profile_null_sha1_80:
        return 14;
        break;
        /* the following profiles are not (yet) supported */
    case asrtpa_profile_null_sha1_32:
    case asrtpa_profile_aes256_cm_sha1_80:
    case asrtpa_profile_aes256_cm_sha1_32:
    default:
        return 0; /* indicate error by returning a zero */
    }
}
