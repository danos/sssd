/*
 * System Security Services Daemon. NSS client interface
 *
 * Copyright (C) Simo Sorce 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* PASSWD database NSS interface */

#include <nss.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sss_cli.h"

static struct sss_nss_getpwent_data {
    size_t len;
    size_t ptr;
    uint8_t *data;
} sss_nss_getpwent_data;

static void sss_nss_getpwent_data_clean(void) {

    if (sss_nss_getpwent_data.data != NULL) {
        free(sss_nss_getpwent_data.data);
        sss_nss_getpwent_data.data = NULL;
    }
    sss_nss_getpwent_data.len = 0;
    sss_nss_getpwent_data.ptr = 0;
}

/* GETPWNAM Request:
 *
 * 0-X: string with name
 *
 * GERTPWUID Request:
 *
 * 0-3: 32bit number with uid
 *
 * Replies:
 *
 * 0-3: 32bit unsigned number of results
 * 4-7: 32bit unsigned (reserved/padding)
 * For each result:
 *  0-3: 32bit number uid
 *  4-7: 32bit number gid
 *  8-X: sequence of 5, 0 terminated, strings (name, passwd, gecos, dir, shell)
 */

struct sss_nss_pw_rep {
    struct passwd *result;
    char *buffer;
    size_t buflen;
};

static int sss_nss_getpw_readrep(struct sss_nss_pw_rep *pr,
                                 uint8_t *buf, size_t *len)
{
    size_t i, slen, dlen;
    char *sbuf;

    if (*len < 13) { /* not enough space for data, bad packet */
        return EBADMSG;
    }

    pr->result->pw_uid = ((uint32_t *)buf)[0];
    pr->result->pw_gid = ((uint32_t *)buf)[1];

    sbuf = (char *)&buf[8];
    slen = *len - 8;
    dlen = pr->buflen;

    i = 0;
    pr->result->pw_name = &(pr->buffer[i]);
    while (slen > i && dlen > 0) {
        pr->buffer[i] = sbuf[i];
        if (pr->buffer[i] == '\0') break;
        i++;
        dlen--;
    }
    if (slen <= i) { /* premature end of buf */
        return EBADMSG;
    }
    if (dlen <= 0) { /* not enough memory */
        return ERANGE; /* not ENOMEM, ERANGE is what glibc looks for */
    }
    i++;
    dlen--;

    pr->result->pw_passwd = &(pr->buffer[i]);
    while (slen > i && dlen > 0) {
        pr->buffer[i] = sbuf[i];
        if (pr->buffer[i] == '\0') break;
        i++;
        dlen--;
    }
    if (slen <= i) { /* premature end of buf */
        return EBADMSG;
    }
    if (dlen <= 0) { /* not enough memory */
        return ERANGE; /* not ENOMEM, ERANGE is what glibc looks for */
    }
    i++;
    dlen--;

    pr->result->pw_gecos = &(pr->buffer[i]);
    while (slen > i && dlen > 0) {
        pr->buffer[i] = sbuf[i];
        if (pr->buffer[i] == '\0') break;
        i++;
        dlen--;
    }
    if (slen <= i) { /* premature end of buf */
        return EBADMSG;
    }
    if (dlen <= 0) { /* not enough memory */
        return ERANGE; /* not ENOMEM, ERANGE is what glibc looks for */
    }
    i++;
    dlen--;

    pr->result->pw_dir = &(pr->buffer[i]);
    while (slen > i && dlen > 0) {
        pr->buffer[i] = sbuf[i];
        if (pr->buffer[i] == '\0') break;
        i++;
        dlen--;
    }
    if (slen <= i) { /* premature end of buf */
        return EBADMSG;
    }
    if (dlen <= 0) { /* not enough memory */
        return ERANGE; /* not ENOMEM, ERANGE is what glibc looks for */
    }
    i++;
    dlen--;

    pr->result->pw_shell = &(pr->buffer[i]);
    while (slen > i && dlen > 0) {
        pr->buffer[i] = sbuf[i];
        if (pr->buffer[i] == '\0') break;
        i++;
        dlen--;
    }
    if (slen <= i) { /* premature end of buf */
        return EBADMSG;
    }
    if (dlen <= 0) { /* not enough memory */
        return ERANGE; /* not ENOMEM, ERANGE is what glibc looks for */
    }

    *len = slen -i -1;

    return 0;
}

enum nss_status _nss_sss_getpwnam_r(const char *name, struct passwd *result,
                                    char *buffer, size_t buflen, int *errnop)
{
    struct sss_cli_req_data rd;
    struct sss_nss_pw_rep pwrep;
    uint8_t *repbuf;
    size_t replen, len;
    enum nss_status nret;
    int ret;

    /* Caught once glibc passing in buffer == 0x0 */
    if (!buffer || !buflen) return ERANGE;

    rd.len = strlen(name) + 1;
    rd.data = name;

    nret = sss_nss_make_request(SSS_NSS_GETPWNAM, &rd,
                                &repbuf, &replen, errnop);
    if (nret != NSS_STATUS_SUCCESS) {
        return nret;
    }

    pwrep.result = result;
    pwrep.buffer = buffer;
    pwrep.buflen = buflen;

    /* no results if not found */
    if (((uint32_t *)repbuf)[0] == 0) {
        free(repbuf);
        return NSS_STATUS_NOTFOUND;
    }

    /* only 1 result is accepted for this function */
    if (((uint32_t *)repbuf)[0] != 1) {
        *errnop = EBADMSG;
        return NSS_STATUS_TRYAGAIN;
    }

    len = replen - 8;
    ret = sss_nss_getpw_readrep(&pwrep, repbuf+8, &len);
    free(repbuf);
    if (ret) {
        *errnop = ret;
        return NSS_STATUS_TRYAGAIN;
    }

    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_sss_getpwuid_r(uid_t uid, struct passwd *result,
                                    char *buffer, size_t buflen, int *errnop)
{
    struct sss_cli_req_data rd;
    struct sss_nss_pw_rep pwrep;
    uint8_t *repbuf;
    size_t replen, len;
    enum nss_status nret;
    uint32_t user_uid;
    int ret;

    /* Caught once glibc passing in buffer == 0x0 */
    if (!buffer || !buflen) return ERANGE;

    user_uid = uid;
    rd.len = sizeof(uint32_t);
    rd.data = &user_uid;

    nret = sss_nss_make_request(SSS_NSS_GETPWUID, &rd,
                                &repbuf, &replen, errnop);
    if (nret != NSS_STATUS_SUCCESS) {
        return nret;
    }

    pwrep.result = result;
    pwrep.buffer = buffer;
    pwrep.buflen = buflen;

    /* no results if not found */
    if (((uint32_t *)repbuf)[0] == 0) {
        free(repbuf);
        return NSS_STATUS_NOTFOUND;
    }

    /* only 1 result is accepted for this function */
    if (((uint32_t *)repbuf)[0] != 1) {
        *errnop = EBADMSG;
        return NSS_STATUS_TRYAGAIN;
    }

    len = replen - 8;
    ret = sss_nss_getpw_readrep(&pwrep, repbuf+8, &len);
    free(repbuf);
    if (ret) {
        *errnop = ret;
        return NSS_STATUS_TRYAGAIN;
    }

    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_sss_setpwent(void)
{
    enum nss_status nret;
    int errnop;

    /* make sure we do not have leftovers, and release memory */
    sss_nss_getpwent_data_clean();

    nret = sss_nss_make_request(SSS_NSS_SETPWENT,
                                NULL, NULL, NULL, &errnop);
    if (nret != NSS_STATUS_SUCCESS) {
        errno = errnop;
        return nret;
    }

    return NSS_STATUS_SUCCESS;
}

enum nss_status _nss_sss_getpwent_r(struct passwd *result,
                                    char *buffer, size_t buflen,
                                    int *errnop)
{
    struct sss_cli_req_data rd;
    struct sss_nss_pw_rep pwrep;
    uint8_t *repbuf;
    size_t replen;
    enum nss_status nret;
    uint32_t num_entries;
    int ret;

    /* Caught once glibc passing in buffer == 0x0 */
    if (!buffer || !buflen) return ERANGE;

    /* if there are leftovers return the next one */
    if (sss_nss_getpwent_data.data != NULL &&
        sss_nss_getpwent_data.ptr < sss_nss_getpwent_data.len) {

        repbuf = sss_nss_getpwent_data.data + sss_nss_getpwent_data.ptr;
        replen = sss_nss_getpwent_data.len - sss_nss_getpwent_data.ptr;

        pwrep.result = result;
        pwrep.buffer = buffer;
        pwrep.buflen = buflen;

        ret = sss_nss_getpw_readrep(&pwrep, repbuf, &replen);
        if (ret) {
            *errnop = ret;
            return NSS_STATUS_TRYAGAIN;
        }

        /* advance buffer pointer */
        sss_nss_getpwent_data.ptr = sss_nss_getpwent_data.len - replen;

        return NSS_STATUS_SUCCESS;
    }

    /* release memory if any */
    sss_nss_getpwent_data_clean();

    /* retrieve no more than SSS_NSS_MAX_ENTRIES at a time */
    num_entries = SSS_NSS_MAX_ENTRIES;
    rd.len = sizeof(uint32_t);
    rd.data = &num_entries;

    nret = sss_nss_make_request(SSS_NSS_GETPWENT, &rd,
                                &repbuf, &replen, errnop);
    if (nret != NSS_STATUS_SUCCESS) {
        return nret;
    }

    /* no results if not found */
    if ((((uint32_t *)repbuf)[0] == 0) || (replen - 8 == 0)) {
        free(repbuf);
        return NSS_STATUS_NOTFOUND;
    }

    sss_nss_getpwent_data.data = repbuf;
    sss_nss_getpwent_data.len = replen;
    sss_nss_getpwent_data.ptr = 8; /* skip metadata fields */

    /* call again ourselves, this will return the first result */
    return _nss_sss_getpwent_r(result, buffer, buflen, errnop);
}

enum nss_status _nss_sss_endpwent(void)
{
    enum nss_status nret;
    int errnop;

    /* make sure we do not have leftovers, and release memory */
    sss_nss_getpwent_data_clean();

    nret = sss_nss_make_request(SSS_NSS_ENDPWENT,
                                NULL, NULL, NULL, &errnop);
    if (nret != NSS_STATUS_SUCCESS) {
        errno = errnop;
        return nret;
    }

    return NSS_STATUS_SUCCESS;
}
