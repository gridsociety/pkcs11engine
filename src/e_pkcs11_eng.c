/*
 * Copyright 2020 Antonio Iacono and the OpenSSL Project Authors.
 * All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

/* Required for secure_getenv */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include "e_pkcs11.h"
#include "e_pkcs11_err.c"
#include <openssl/x509v3.h>
#include <openssl/ui.h>
#include <ctype.h>

static int pkcs11_parse_items(PKCS11_CTX *ctx, const char *uri, int store);
static int pkcs11_parse(PKCS11_CTX *ctx, const char *path, int store);
static PKCS11_CTX *pkcs11_ctx_new(void);
static void pkcs11_ctx_free(PKCS11_CTX *ctx);
static int bind_pkcs11(ENGINE *e);
static int pkcs11_init(ENGINE *e);
static int pkcs11_destroy(ENGINE *e);
static int pkcs11_finish(ENGINE *e);
static EVP_PKEY *pkcs11_engine_load_private_key(ENGINE * e, const char *path,
                                                UI_METHOD * ui_method,
                                                void *callback_data);
static EVP_PKEY *pkcs11_engine_load_public_key(ENGINE * e, const char *path,
                                               UI_METHOD * ui_method,
                                               void *callback_data);
static int pkcs11_engine_load_cert(ENGINE *e, int cmd, long i,
                                   void *p, void (*f)(void));
void engine_load_pkcs11_int(void);
static int pkcs11_rsa_free(RSA *rsa);
static unsigned char *pkcs11_pad(char *field, int len);
static int cert_issuer_match(STACK_OF(X509_NAME) *ca_dn, X509 *x);
static char *pkcs11_get_console_pin(PKCS11_CTX *ctx);
CK_BYTE *pin_from_file(const char *filename);

static RSA_METHOD *pkcs11_rsa = NULL;
static const char *engine_id = "pkcs11";
static const char *engine_name = "PKCS#11 engine";
static int pkcs11_idx = -1;

/* store stuff */
static const char pkcs11_scheme[] = "pkcs11";
static OSSL_STORE_LOADER_CTX* pkcs11_store_open(
    const OSSL_STORE_LOADER *loader, const char *uri,
    const UI_METHOD *ui_method, void *ui_data);
static OSSL_STORE_INFO* pkcs11_store_load(OSSL_STORE_LOADER_CTX *ctx,
                                          const UI_METHOD *ui_method,
                                          void *ui_data);
static int pkcs11_store_eof(OSSL_STORE_LOADER_CTX *ctx);
static int pkcs11_store_close(OSSL_STORE_LOADER_CTX *ctx);
static int pkcs11_store_error(OSSL_STORE_LOADER_CTX *ctx);
static OSSL_STORE_LOADER_CTX* OSSL_STORE_LOADER_CTX_new(void);
static void OSSL_STORE_LOADER_CTX_free(OSSL_STORE_LOADER_CTX* ctx);
static OSSL_STORE_INFO* pkcs11_store_load_cert(OSSL_STORE_LOADER_CTX *ctx,
                                               const UI_METHOD *ui_method,
                                               void *ui_data);
static OSSL_STORE_INFO* pkcs11_store_load_key(OSSL_STORE_LOADER_CTX *ctx,
                                              const UI_METHOD *ui_method,
                                              void *ui_data);
static int pkcs11_load_ssl_client_cert(ENGINE *e, SSL *ssl,
                                       STACK_OF(X509_NAME) *ca_dn, X509 **pcert,
                                       EVP_PKEY **pkey, STACK_OF(X509) **pother,
                                       UI_METHOD *ui_method,
                                       void *callback_data);

int rsa_pkcs11_idx = -1;

unsigned char* urldecode(char *p)
{
    unsigned char *out = NULL;

    out = (unsigned char *)OPENSSL_strdup(p);
    unsigned char *save = out;

    if (out == NULL)
        goto memerr;

    for (; *p; p++) {
        if (*p != '%') {
            *out++ = *p;
        } else if (isxdigit((unsigned char)(p[1])) &&
                   isxdigit((unsigned char)(p[2]))) {
            *out++ = (OPENSSL_hexchar2int(p[1]) << 4)
                | OPENSSL_hexchar2int(p[2]);
            p += 2;
        } else {
            return NULL;
        }
    }
    *out = '\0';

    return save;

memerr:
    PKCS11err(PKCS11_F_PKCS11_INIT, ERR_R_MALLOC_FAILURE);
    return NULL;
}

static int pkcs11_init(ENGINE *e)
{
    PKCS11_CTX *ctx;

    if (pkcs11_idx < 0) {
        pkcs11_idx = ENGINE_get_ex_new_index(0, NULL, NULL, NULL, 0);
        if (pkcs11_idx < 0)
            goto memerr;
    }
    ctx = ENGINE_get_ex_data(e, pkcs11_idx);
    if (ctx == NULL) {
        ctx = pkcs11_ctx_new();

        if (ctx == NULL)
            goto memerr;

        ENGINE_set_ex_data(e, pkcs11_idx, ctx);
    }

    return 1;

 memerr:
    PKCS11err(PKCS11_F_PKCS11_INIT, ERR_R_MALLOC_FAILURE);
    return 0;
}

static int pkcs11_ctrl(ENGINE *e, int cmd, long i, void *p, void (*f) (void))
{
    int ret = 1;
    char *tmpstr;
    PKCS11_CTX *ctx;

    if (pkcs11_idx == -1 && !pkcs11_init(e)) {
        PKCS11err(PKCS11_F_PKCS11_CTRL, PKCS11_R_ENGINE_NOT_INITIALIZED);
        return 0;
    }
    ctx = ENGINE_get_ex_data(e, pkcs11_idx);

    switch (cmd) {
    case PKCS11_CMD_MODULE_PATH:
        tmpstr = OPENSSL_strdup(p);
        if (tmpstr != NULL) {
            ctx->module_path = tmpstr;
            PKCS11_trace("Setting module path to %s\n", ctx->module_path);
        } else {
            PKCS11err(PKCS11_F_PKCS11_CTRL, ERR_R_MALLOC_FAILURE);
            ret = 0;
        }
        break;

    /* TODO binary pin support */
    case PKCS11_CMD_PIN:
        tmpstr = OPENSSL_strdup(p);
        if (tmpstr != NULL) {
            ctx->pin = (CK_BYTE *) tmpstr;
            ctx->pinlen = (CK_ULONG) strlen((char *) ctx->pin);
            PKCS11_trace("Setting pin\n");
        } else {
            PKCS11err(PKCS11_F_PKCS11_CTRL, ERR_R_MALLOC_FAILURE);
            ret = 0;
        }
        break;
    case PKCS11_CMD_LOAD_CERT_CTRL:
        return pkcs11_engine_load_cert(e, cmd, i, p, f);
    }

    return ret;
}

static unsigned char *pkcs11_pad(char *field, int len)
{
    int i;
    unsigned char *ret = NULL;

    ret = OPENSSL_malloc(len);
    for (i=0; i<len; i++)
        ret[i]=' ';
    for (i=0; i<len; i++) {
        if (*(field+i) == '\0')
            break;
        ret[i]=*(field+i);
    }
    return ret;
}

CK_BYTE *pin_from_file(const char *filename)
{
    BIO *in = NULL;
    int i;
    static char buf[255];
    char *c;

    in = BIO_new_file(filename, "r");
    if (in == NULL) {
        BIO_free(in);
        goto err;
    }
    i = BIO_gets(in, buf, sizeof(buf));
    BIO_free(in);
    in = NULL;
    if (i <= 0)
        goto err;
    if ((c = strchr(buf, '\n')) != NULL) {
        *c = 0;     /* truncate at newline */
    }
    return (CK_BYTE *) buf;

 err:
    PKCS11_trace("Can't read PIN from %s\n", filename);
    return NULL;
}

static int pkcs11_parse_items(PKCS11_CTX *ctx, const char *uri, int store)
{
    char *p, *q, *tmpstr;
    int len = 0;

    p = q = (char *) uri;

    len = strlen(uri);
    while (q - uri <= len) {
        if (*q != ';' && *q != '\0') {
            q++;
            continue;
        }
        if (p != q) {
            /* found */
            *q = '\0';
            if (strncmp(p, "pin-value=", 10) == 0 && ctx->pin == NULL) {
                p += 10;
                tmpstr = OPENSSL_strdup(p);
                if (tmpstr == NULL)
                    goto memerr;
                ctx->pin = (CK_BYTE *) tmpstr;
                ctx->pinlen = (CK_ULONG) strlen((char *) ctx->pin);
            } else if (strncmp(p, "pin-source=", 11) == 0 && ctx->pin == NULL) {
                p += 11;
                if (strncmp(p, "file:", 5) == 0) {
                    p += 5;
                    tmpstr = OPENSSL_strdup(p);
                        if (tmpstr == NULL)
                            goto memerr;
                        if ((ctx->pin = pin_from_file(tmpstr)) == NULL)
                            goto err;
                        ctx->pinlen = (CK_ULONG) strlen((char *) ctx->pin);
                } else {
                    PKCS11_trace("file source only supported\n");
                    goto err;
                }
            } else if (strncmp(p, "object=", 7) == 0 && ctx->label == NULL) {
                p += 7;
                ctx->label = (CK_BYTE *) urldecode(p);
            } else if (strncmp(p, "model=", 6) == 0) {
                p += 6;
                memcpy(ctx->model, pkcs11_pad(urldecode(p), 16), 16);
            } else if (strncmp(p, "serial=", 7) == 0) {
                p += 7;
                memcpy(ctx->serial, pkcs11_pad(urldecode(p), 16), 16);
            } else if (strncmp(p, "token=", 6) == 0) {
                p += 6;
                memcpy(ctx->token, pkcs11_pad(urldecode(p), 32), 32);
            } else if (strncmp(p, "manufacturer=", 13) == 0) {
                p += 13;
                memcpy(ctx->manufacturer, pkcs11_pad(urldecode(p), 32), 32);
            } else if (strncmp(p, "id=", 3) == 0 && ctx->id == NULL) {
                p += 3;
                ctx->id = (CK_BYTE *) urldecode(p);
                ctx->idlen = (CK_ULONG) strlen(ctx->id);
            } else if (strncmp(p, "type=", 5) == 0 && ctx->type == NULL) {
                p += 5;
                tmpstr = OPENSSL_strdup(p);
                if (tmpstr == NULL)
                    goto memerr;
                ctx->type = tmpstr;
            } else if (strncmp(p, "module-path=", 12) == 0
                && ctx->module_path == NULL) {
                p += 12;
                ctx->module_path = urldecode(p);
            } else if (strncmp(p, "slot-id=", 8) == 0 && ctx->slotid == 0) {
                p += 8;
                tmpstr = OPENSSL_strdup(p);
                if (tmpstr == NULL)
                    goto memerr;
                ctx->slotid = (CK_SLOT_ID) atoi(tmpstr);
            }
        }
        p = ++q;
    }
    return 1;

 memerr:
    PKCS11err(PKCS11_F_PKCS11_PARSE_ITEMS, ERR_R_MALLOC_FAILURE);
 err:
    return 0;

}

/*-
 *  Password prompting
 *  ------------------
 */

static char *pkcs11_get_console_pin(PKCS11_CTX *ctx)
{
    UI *ui = UI_new();
    char *prompt = NULL;
    size_t maxsize = 512;
    const int buflen = 512;
    char *pass = NULL;
    char *prompt_info = "Token";

    pass = OPENSSL_malloc(buflen);
    if (pass == NULL) {
        PKCS11err(PKCS11_F_PKCS11_GET_CONSOLE_PIN, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    if (ui == NULL) {
        PKCS11err(PKCS11_F_PKCS11_GET_CONSOLE_PIN, ERR_R_MALLOC_FAILURE);
        return NULL;
    }

    if (ctx->ui_method != NULL)
        UI_set_method(ui, ctx->ui_method);
    UI_add_user_data(ui, ctx->callback_data);

    if ((prompt = UI_construct_prompt(ui, "PIN",
                                      prompt_info)) == NULL) {
        PKCS11err(PKCS11_F_PKCS11_GET_CONSOLE_PIN, ERR_R_MALLOC_FAILURE);
        pass = NULL;
    } else if (!UI_add_input_string(ui, prompt, UI_INPUT_FLAG_DEFAULT_PWD,
                                    pass, 0, maxsize - 1)) {
        PKCS11_trace("ERR UI_LIB\n");
        pass = NULL;
    } else {
        switch (UI_process(ui)) {
        case -2:
            PKCS11_trace("PROCESS INTERRUPTED \n");
            pass = NULL;
            break;
        case -1:
            PKCS11_trace("ERR UI_LIB\n");
            pass = NULL;
            break;
        default:
            break;
        }
    }

    OPENSSL_free(prompt);
    UI_free(ui);
    return pass;
}

static int pkcs11_parse(PKCS11_CTX *ctx, const char *path, int store)
{
    char *pin = NULL;
    char *id = NULL;

    if (path == NULL) {
        PKCS11_trace("URI is empty\n");
        return 0;
    }

    if (strncmp(path, "pkcs11:", 7) == 0) {
        path += 7;
        if (!pkcs11_parse_items(ctx, path, store))
            goto err;

        if (ctx->id == NULL && ctx->label == NULL && !store) {
            PKCS11_trace("ID and OBJECT are null\n");
            goto err;
         }
    } else {
        id = OPENSSL_strdup(path);
        if (id == NULL) {
            PKCS11err(PKCS11_F_PKCS11_PARSE, ERR_R_MALLOC_FAILURE);
            goto err;
        }
        ctx->id = (CK_BYTE *) urldecode(id);
        ctx->idlen = (CK_ULONG) strlen(ctx->id);
    }

    if (ctx->module_path == NULL) {
        if ((ctx->module_path =
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
# if __GLIBC_PREREQ(2, 17)
            secure_getenv("PKCS11_MODULE_PATH")) == NULL) {
# else
            getenv("PKCS11_MODULE_PATH")) == NULL) {
# endif
#else
            getenv("PKCS11_MODULE_PATH")) == NULL) {
#endif
            PKCS11_trace("Module path is null\n");
            goto err;
        }
    }

    if (ctx->pin == NULL && (!store || (store
        && ctx->type != NULL && strncmp(ctx->type, "private", 7) == 0))) {
        pin = pkcs11_get_console_pin(ctx);
        if (pin == NULL) {
            PKCS11_trace("PIN is invalid\n");
            OPENSSL_free(id);
            return 0;
        }
        ctx->pin = (CK_BYTE *) pin;
        ctx->pinlen = (CK_ULONG) strlen((char *) ctx->pin);
    }
    return 1;

 err:
    OPENSSL_free(pin);
    OPENSSL_free(id);
    return 0;
}

static int cert_issuer_match(STACK_OF(X509_NAME) *ca_dn, X509 *x)
{
    int i;
    X509_NAME *nm;
    /* Special case: empty list: match anything */
    if (sk_X509_NAME_num(ca_dn) <= 0)
        return 1;
    for (i = 0; i < sk_X509_NAME_num(ca_dn); i++) {
        nm = sk_X509_NAME_value(ca_dn, i);
        if (!X509_NAME_cmp(nm, X509_get_issuer_name(x)))
            return 1;
    }
    return 0;
}

static int pkcs11_engine_load_cert(ENGINE *e, int cmd, long i,
                                   void *p, void (*f)(void))
{
    PKCS11_CTX *pkcs11_ctx;
    OSSL_STORE_LOADER_CTX *store_ctx = NULL;
    CK_SESSION_HANDLE session = 0;
    CK_OBJECT_CLASS class;
    int ret = 0;
    struct {
        const char *uri_string;
        X509 *cert;
    } *params = p;

    store_ctx = OSSL_STORE_LOADER_CTX_new();
    pkcs11_ctx = ENGINE_get_ex_data(e, pkcs11_idx);
    if (pkcs11_ctx == NULL)
        goto err;

    if (!pkcs11_parse(pkcs11_ctx, params->uri_string, 1))
        goto err;

    if (pkcs11_initialize(pkcs11_ctx->module_path) != CKR_OK)
        goto err;

    if (!pkcs11_get_slot(pkcs11_ctx)) {
        pkcs11_finalize();
        goto err;
     }

    if (!pkcs11_start_session(pkcs11_ctx, &session))
        goto end;

    store_ctx->session = session;
    pkcs11_ctx->type = "cert";

    if (!pkcs11_search_start(store_ctx, pkcs11_ctx))
        goto end;

    pkcs11_search_next_object(store_ctx, &class);

    if (class == CKO_CERTIFICATE) {
        params->cert = store_ctx->cert;
        ret = 1;
    }

 end:
    pkcs11_end_session(session);
    pkcs11_finalize();
 err:
    OSSL_STORE_LOADER_CTX_free(store_ctx);
    return ret;
}

static EVP_PKEY *pkcs11_engine_load_private_key(ENGINE * e, const char *path,
                                                UI_METHOD * ui_method,
                                                void *callback_data)
{
    CK_RV rv;
    PKCS11_CTX *ctx;
    CK_SESSION_HANDLE session = 0;
    CK_OBJECT_HANDLE key = 0;

    ctx = ENGINE_get_ex_data(e, pkcs11_idx);

    if (ctx == NULL)
        goto err;

    ctx->ui_method = ui_method;
    ctx->callback_data = callback_data;

    if (!pkcs11_parse(ctx, path, 0))
        goto err;

    rv = pkcs11_initialize(ctx->module_path);
    if (rv != CKR_OK)
        goto err;
    if (!pkcs11_get_slot(ctx))
        goto err;
    if (!pkcs11_start_session(ctx, &session))
        goto err;
    if (!pkcs11_login(session, ctx, CKU_USER))
        goto err;
    key = pkcs11_find_private_key(session, ctx);
    if (!key)
        goto err;

    return pkcs11_load_pkey(session, ctx, key);

 err:
    PKCS11_trace("pkcs11_engine_load_private_key failed\n");
    return NULL;
}

static EVP_PKEY *pkcs11_engine_load_public_key(ENGINE * e, const char *path,
                                               UI_METHOD * ui_method,
                                               void *callback_data)
{
    CK_RV rv;
    PKCS11_CTX *ctx;
    CK_SESSION_HANDLE session = 0;
    CK_OBJECT_HANDLE key = 0;

    ctx = ENGINE_get_ex_data(e, pkcs11_idx);

    if (ctx == NULL)
        goto err;

    ctx->ui_method = ui_method;
    ctx->callback_data = callback_data;

    if (!pkcs11_parse(ctx, path, 0))
        goto err;

    rv = pkcs11_initialize(ctx->module_path);
    if (rv != CKR_OK)
        goto err;
    if (!pkcs11_get_slot(ctx))
        goto err;
    if (!pkcs11_start_session(ctx, &session))
        goto err;
    if (!pkcs11_login(session, ctx, CKU_USER))
        goto err;
    key = pkcs11_find_public_key(session, ctx);
    if (!key)
        goto err;
    return pkcs11_load_pkey(session, ctx, key);

 err:
    PKCS11_trace("pkcs11_engine_load_public_key failed\n");
    return 0;
}

static OSSL_STORE_LOADER_CTX* pkcs11_store_open(
    const OSSL_STORE_LOADER *loader, const char *uri,
    const UI_METHOD *ui_method, void *ui_data)
{
    ENGINE *e;
    PKCS11_CTX *pkcs11_ctx;
    OSSL_STORE_LOADER_CTX *store_ctx = NULL;
    CK_SESSION_HANDLE session = 0;

    store_ctx = OSSL_STORE_LOADER_CTX_new();

    e = (ENGINE *) OSSL_STORE_LOADER_get0_engine(loader);
    if (e == NULL)
        goto err;

    pkcs11_ctx = ENGINE_get_ex_data(e, pkcs11_idx);

    if (pkcs11_ctx == NULL)
        goto err;

    pkcs11_ctx->ui_method = ui_method;
    pkcs11_ctx->callback_data = ui_data;

    if (!pkcs11_parse(pkcs11_ctx, uri, 1))
        goto err;

    if (pkcs11_initialize(pkcs11_ctx->module_path) != CKR_OK)
        goto err;

    if (!pkcs11_get_slot(pkcs11_ctx))
        goto err;

    if (!pkcs11_start_session(pkcs11_ctx, &session))
        goto err;

    /* NEW store-ctx->session, not a copy of pkcs11_ctx->session */
    store_ctx->session = session;

    if (!pkcs11_search_start(store_ctx, pkcs11_ctx))
        goto err;

    if (pkcs11_ctx->label == NULL && pkcs11_ctx->id == NULL)
        store_ctx->listflag = 1;    /* we want names */

    return store_ctx;

 err:
    OSSL_STORE_LOADER_CTX_free(store_ctx);
    return NULL;
}

static OSSL_STORE_INFO* pkcs11_store_load(OSSL_STORE_LOADER_CTX *ctx,
                                          const UI_METHOD *ui_method,
                                          void *ui_data)
{
    OSSL_STORE_INFO *ret = NULL;

    if (ctx->listflag) {
        char *name = NULL;
        char *description = NULL;

        ctx->eof = pkcs11_search_next_ids(ctx, &name, &description);
        if (!ctx->eof) {
            ret = OSSL_STORE_INFO_new_NAME(name);
            OSSL_STORE_INFO_set0_NAME_description(ret, description);
        }
    } else {
        CK_OBJECT_CLASS class;

        ctx->eof = pkcs11_search_next_object(ctx, &class);
        if (!ctx->eof) {
            if (class == CKO_CERTIFICATE)
                ret = pkcs11_store_load_cert(ctx, ui_method, ui_data);
            if (class == CKO_PUBLIC_KEY)
                ret = pkcs11_store_load_key(ctx, ui_method, ui_data);
        }
    }
    return ret;
}

static int pkcs11_store_eof(OSSL_STORE_LOADER_CTX *ctx)
{
    return ctx->eof;
}

static int pkcs11_store_close(OSSL_STORE_LOADER_CTX *ctx)
{
    pkcs11_end_session(ctx->session);
    OSSL_STORE_LOADER_CTX_free(ctx);
    return 1;
}

static int pkcs11_store_error(OSSL_STORE_LOADER_CTX *ctx)
{
/* TODO */
    return 0;
}

static OSSL_STORE_LOADER_CTX* OSSL_STORE_LOADER_CTX_new(void)
{
    OSSL_STORE_LOADER_CTX *ctx;

    ctx = OPENSSL_zalloc(sizeof(*ctx));
    if (ctx == NULL)
        return NULL;
    ctx->error = 0;
    ctx->listflag = 0;
    ctx->eof = 0;
    ctx->cert = NULL;
    ctx->session = 0;
    return ctx;
}

static void OSSL_STORE_LOADER_CTX_free(OSSL_STORE_LOADER_CTX* ctx)
{
    if (ctx == NULL)
        return;
    EVP_PKEY_free(ctx->key);
    OPENSSL_free(ctx);
    OSSL_STORE_unregister_loader(pkcs11_scheme);
}

static OSSL_STORE_INFO* pkcs11_store_load_cert(OSSL_STORE_LOADER_CTX *ctx,
                                               const UI_METHOD *ui_method,
                                               void *ui_data)
{
    return OSSL_STORE_INFO_new_CERT(ctx->cert);
}

static OSSL_STORE_INFO* pkcs11_store_load_key(OSSL_STORE_LOADER_CTX *ctx,
                                               const UI_METHOD *ui_method,
                                               void *ui_data)
{
    return OSSL_STORE_INFO_new_PKEY(ctx->key);
}

static int bind_pkcs11(ENGINE *e)
{
    const RSA_METHOD *ossl_rsa_meth;
    OSSL_STORE_LOADER *loader = NULL;

    loader = OSSL_STORE_LOADER_new(e, pkcs11_scheme);
    if (loader == NULL)
        return 0;

    if (!OSSL_STORE_LOADER_set_open(loader, pkcs11_store_open)
        || !OSSL_STORE_LOADER_set_load(loader, pkcs11_store_load)
        || !OSSL_STORE_LOADER_set_eof(loader, pkcs11_store_eof)
        || !OSSL_STORE_LOADER_set_error(loader, pkcs11_store_error)
        || !OSSL_STORE_LOADER_set_close(loader, pkcs11_store_close)
        || !OSSL_STORE_register_loader(loader))
        return 0;

    rsa_pkcs11_idx = RSA_get_ex_new_index(0, NULL, NULL, NULL, 0);
    ossl_rsa_meth = RSA_PKCS1_OpenSSL();

    if ((pkcs11_rsa = RSA_meth_new("PKCS#11 RSA method", 0)) == NULL
        || !RSA_meth_set_sign(pkcs11_rsa, pkcs11_rsa_sign)
        || !RSA_meth_set_finish(pkcs11_rsa, pkcs11_rsa_free)
        || !RSA_meth_set_pub_enc(pkcs11_rsa,
                                 RSA_meth_get_pub_enc(ossl_rsa_meth))
        || !RSA_meth_set_pub_dec(pkcs11_rsa,
                                 RSA_meth_get_pub_dec(ossl_rsa_meth))
        || !RSA_meth_set_priv_enc(pkcs11_rsa, pkcs11_rsa_priv_enc)
        || !RSA_meth_set_priv_dec(pkcs11_rsa, pkcs11_rsa_priv_dec)
        || !RSA_meth_set_mod_exp(pkcs11_rsa,
                                 RSA_meth_get_mod_exp(ossl_rsa_meth))
        || !RSA_meth_set_bn_mod_exp(pkcs11_rsa,
                                    RSA_meth_get_bn_mod_exp(ossl_rsa_meth))) {
        PKCS11err(PKCS11_F_BIND_PKCS11, PKCS11_R_RSA_INIT_FAILED);
        return 0;
    }

    if (!ENGINE_set_id(e, engine_id)
        || !ENGINE_set_name(e, engine_name)
        || !ENGINE_set_RSA(e, pkcs11_rsa)
        || !ENGINE_set_load_privkey_function(e, pkcs11_engine_load_private_key)
        || !ENGINE_set_load_pubkey_function(e, pkcs11_engine_load_public_key)
        || !ENGINE_set_destroy_function(e, pkcs11_destroy)
        || !ENGINE_set_init_function(e, pkcs11_init)
        || !ENGINE_set_finish_function(e, pkcs11_finish)
        || !ENGINE_set_cmd_defns(e, pkcs11_cmd_defns)
        || !ENGINE_set_load_ssl_client_cert_function(e,
                                                     pkcs11_load_ssl_client_cert)
        || !ENGINE_set_ctrl_function(e, pkcs11_ctrl))
        goto end;

    ERR_load_PKCS11_strings();
    return 1;

 end:
    PKCS11_trace("ENGINE_set failed\n");
    return 0;
}

void printf_stderr(char *fmt, ...)
{
#ifndef OPENSSL_NO_STDIO
    BIO *out;
    va_list args;

    out = BIO_new_fp(stderr, BIO_NOCLOSE);
    if (out == NULL) {
        PKCS11err(PKCS11_F_PKCS11_TRACE, PKCS11_R_FILE_OPEN_ERROR);
        return;
    }

    va_start(args, fmt);
    BIO_vprintf(out, fmt, args);
    va_end(args);
    BIO_free(out);
#endif
}

void PKCS11_trace(char *fmt, ...)
{
#ifdef DEBUG
    printf_stderr(fmt);
#endif
}

static PKCS11_CTX *pkcs11_ctx_new(void)
{
    PKCS11_CTX *ctx = OPENSSL_zalloc(sizeof(*ctx));
    if (ctx == NULL) {
        PKCS11err(PKCS11_F_PKCS11_CTX_NEW, ERR_R_MALLOC_FAILURE);
        return NULL;
    }
    ctx->lock = CRYPTO_THREAD_lock_new();
    return ctx;
}

static int pkcs11_finish(ENGINE *e)
{
    PKCS11_CTX *ctx;
    ctx = ENGINE_get_ex_data(e, pkcs11_idx);
    pkcs11_ctx_free(ctx);
    return 1;
}

static int pkcs11_destroy(ENGINE *e)
{
    RSA_meth_free(pkcs11_rsa);
    pkcs11_rsa = NULL;
    PKCS11_trace("Calling pkcs11_destroy with engine: %p\n", e);
    OSSL_STORE_unregister_loader(pkcs11_scheme);
    ERR_unload_PKCS11_strings();
    return 1;
}

static int pkcs11_rsa_free(RSA *rsa)
{
    RSA_set_ex_data(rsa, rsa_pkcs11_idx, 0);
    return 1;
}

static void pkcs11_ctx_free(PKCS11_CTX *ctx)
{
    PKCS11_trace("Calling pkcs11_ctx_free with %p\n", ctx);
    CRYPTO_THREAD_lock_free(ctx->lock);
    free(ctx->id);
    free(ctx->label);
}

PKCS11_CTX *pkcs11_get_ctx(const RSA *rsa)
{
    return ENGINE_get_ex_data(RSA_get0_engine(rsa), pkcs11_idx);
}

static int pkcs11_load_ssl_client_cert(ENGINE *e, SSL *ssl,
                                       STACK_OF(X509_NAME) *ca_dn, X509 **pcert,
                                       EVP_PKEY **pkey, STACK_OF(X509) **pother,
                                       UI_METHOD *ui_method,
                                       void *callback_data)
{
    PKCS11_CTX *pkcs11_ctx;
    OSSL_STORE_LOADER_CTX *store_ctx = NULL;
    CK_SESSION_HANDLE session = 0;
    CK_BYTE *id;
    CK_ULONG idlen;
    char *pin = NULL;
    CK_OBJECT_HANDLE key = 0;
    int ret = 0;
    int i;

    *pcert = NULL;
    *pkey = NULL;
    store_ctx = OSSL_STORE_LOADER_CTX_new();
    pkcs11_ctx = ENGINE_get_ex_data(e, pkcs11_idx);
    if (pkcs11_ctx == NULL)
        goto err;

    pkcs11_ctx->ui_method = ui_method;
    pkcs11_ctx->callback_data = callback_data;

    if (pkcs11_initialize(pkcs11_ctx->module_path) != CKR_OK)
        goto err;

    if (!pkcs11_get_slot(pkcs11_ctx)) {
        pkcs11_finalize();
        goto err;
     }

    if (!pkcs11_start_session(pkcs11_ctx, &session))
        goto err;

    if (pkcs11_ctx->pin == NULL) {
        pin = pkcs11_get_console_pin(pkcs11_ctx);
        if (pin == NULL) {
            PKCS11_trace("PIN is invalid\n");
            goto err;
        }
        pkcs11_ctx->pin = (CK_BYTE *) pin;
        pkcs11_ctx->pinlen = (CK_ULONG) strlen((char *) pkcs11_ctx->pin);
    }

    store_ctx->session = session;
    pkcs11_ctx->type = "cert";

    if (!pkcs11_search_start(store_ctx, pkcs11_ctx))
        goto err;

    for (i = 0;; i++) {
        if (pkcs11_search_next_cert(store_ctx, &id, &idlen))
            break;
        if (cert_issuer_match(ca_dn, store_ctx->cert)
            && X509_check_purpose(store_ctx->cert,
            X509_PURPOSE_SSL_CLIENT, 0)) {
            *pcert = store_ctx->cert;
            pkcs11_ctx->id = id;
            pkcs11_ctx->idlen = idlen;
            pkcs11_close_operation(session);
            key = pkcs11_find_private_key(session, pkcs11_ctx);
            if (!key)
                goto err;
            *pkey = pkcs11_load_pkey(session, pkcs11_ctx, key);
            ret = 1;
            break;
        }
    }

 err:
    OSSL_STORE_LOADER_CTX_free(store_ctx);
    return ret;
}

#ifndef OPENSSL_NO_DYNAMIC_ENGINE
static int bind_helper(ENGINE *e, const char *id)
{
    if (id && (strcmp(id, engine_id) != 0))
        return 0;
    if (!bind_pkcs11(e))
        return 0;
    return 1;
}

IMPLEMENT_DYNAMIC_BIND_FN(bind_helper)
IMPLEMENT_DYNAMIC_CHECK_FN()
#else
static ENGINE *engine_pkcs11(void)
{
    ENGINE *ret = ENGINE_new();
    if (ret == NULL)
        return NULL;
    if (!bind_pkcs11(ret)) {
        ENGINE_free(ret);
        return NULL;
    }
    return ret;
}
void engine_load_pkcs11_int(void)
{
    /* Copied from eng_[openssl|dyn].c */
    ENGINE *toadd = engine_pkcs11();
    if (!toadd)
        return;
    ENGINE_add(toadd);
    ENGINE_free(toadd);
    ERR_clear_error();
}
#endif

