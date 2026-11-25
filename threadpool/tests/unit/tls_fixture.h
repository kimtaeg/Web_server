/*
 * tls_fixture.h — TLS 단위 테스트 공용 도구 (test_tls.c, test_connio.c 에서 include).
 *
 * - 자체 서명 인증서와 개인키를 테스트 안에서 OpenSSL API 로 만들어 임시 디렉터리에 PEM 으로 쓴다.
 *   (openssl 명령이나 미리 만든 파일에 의존하지 않는다. 짝이 맞지 않는 두 번째 키도 만든다.)
 * - 클라이언트 쪽 TLS 를 별도 스레드에서 돌린다: 핸드셰이크는 양쪽이 동시에 진행돼야 하므로.
 * 헤더 안의 static 함수들이라 테스트 파일마다 한 벌씩 들어간다.
 */
#ifndef TLS_FIXTURE_H
#define TLS_FIXTURE_H

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "tls.h"

typedef struct {
    /* dir 을 결과 경로보다 짧게: "%s/server.crt" 가 잘릴 수 없음을 컴파일러(gcc -Wformat-truncation)가 안다 */
    char dir[PATH_MAX - 64];
    char cert[PATH_MAX];  /* server.crt: key 와 짝 */
    char key[PATH_MAX];   /* server.key */
    char key2[PATH_MAX];  /* other.key: cert 와 짝이 아님 */
    char junk[PATH_MAX];  /* PEM 이 아닌 파일 */
} tls_fixture_t;

static EVP_PKEY *fx_gen_key(void)
{
    EVP_PKEY *k = NULL;
    EVP_PKEY_CTX *kc = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);

    if (kc != NULL && EVP_PKEY_keygen_init(kc) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(kc, 2048) == 1)
        (void)EVP_PKEY_keygen(kc, &k);
    EVP_PKEY_CTX_free(kc);
    return k;
}

static X509 *fx_self_sign(EVP_PKEY *k)
{
    X509 *x = X509_new();
    X509_NAME *n = X509_NAME_new(); /* X509_get_subject_name 은 OpenSSL 4 에서 const 를 돌려준다 */

    if (x == NULL || n == NULL) {
        X509_free(x);
        X509_NAME_free(n);
        return NULL;
    }
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 86400);
    X509_set_pubkey(x, k);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_subject_name(x, n); /* 둘 다 복사해 간다 */
    X509_set_issuer_name(x, n);  /* 자체 서명: 발급자 = 주체 */
    X509_NAME_free(n);
    if (X509_sign(x, k, EVP_sha256()) <= 0) {
        X509_free(x);
        return NULL;
    }
    return x;
}

static int fx_write_key(const char *path, EVP_PKEY *k)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (f == NULL)
        return -1;
    ok = PEM_write_PrivateKey(f, k, NULL, NULL, 0, NULL, NULL) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

/* 성공 0. 실패하면 -1 (만든 것은 정리). */
static int fx_setup(tls_fixture_t *fx)
{
    const char *tmp = getenv("TMPDIR");
    EVP_PKEY *k = NULL, *k2 = NULL;
    X509 *x = NULL;
    FILE *f = NULL;
    int rc = -1;

    memset(fx, 0, sizeof *fx);
    snprintf(fx->dir, sizeof fx->dir, "%s/tlsfx.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (mkdtemp(fx->dir) == NULL)
        return -1;
    snprintf(fx->cert, sizeof fx->cert, "%s/server.crt", fx->dir);
    snprintf(fx->key, sizeof fx->key, "%s/server.key", fx->dir);
    snprintf(fx->key2, sizeof fx->key2, "%s/other.key", fx->dir);
    snprintf(fx->junk, sizeof fx->junk, "%s/junk.pem", fx->dir);

    k = fx_gen_key();
    k2 = fx_gen_key();
    if (k == NULL || k2 == NULL || (x = fx_self_sign(k)) == NULL)
        goto cleanup;
    if (fx_write_key(fx->key, k) != 0 || fx_write_key(fx->key2, k2) != 0)
        goto cleanup;
    f = fopen(fx->cert, "w");
    if (f == NULL || PEM_write_X509(f, x) != 1)
        goto cleanup;
    fclose(f);
    f = fopen(fx->junk, "w");
    if (f == NULL || fputs("-----BEGIN CERTIFICATE-----\nnot base64 !!\n-----END CERTIFICATE-----\n", f) < 0)
        goto cleanup;
    rc = 0;

cleanup:
    if (f != NULL)
        fclose(f);
    X509_free(x);
    EVP_PKEY_free(k);
    EVP_PKEY_free(k2);
    return rc;
}

static void fx_teardown(tls_fixture_t *fx)
{
    unlink(fx->cert);
    unlink(fx->key);
    unlink(fx->key2);
    unlink(fx->junk);
    rmdir(fx->dir);
}

/* ---- 클라이언트 (테스트 스레드) ---- */

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    int max_version; /* 0 = 기본 (TLS 1.2~1.3) */
    int ok;          /* SSL_connect 성공 */
} fx_client_t;

/* 검증 없는 클라이언트 CTX (curl -k 와 같다). 낮은 버전 테스트를 위해 보안 레벨 0 허용. */
static SSL_CTX *fx_client_ctx(void)
{
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());

    if (c != NULL) {
        SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);
        SSL_CTX_set_security_level(c, 0);
        SSL_CTX_set_min_proto_version(c, 0);
        SSL_CTX_set_mode(c, SSL_MODE_AUTO_RETRY);
    }
    return c;
}

static void *fx_client_connect_main(void *p)
{
    fx_client_t *c = p;

    c->ssl = SSL_new(c->ctx);
    if (c->ssl == NULL || SSL_set_fd(c->ssl, c->fd) != 1)
        return NULL;
    if (c->max_version)
        SSL_set_max_proto_version(c->ssl, c->max_version);
    c->ok = SSL_connect(c->ssl) == 1;
    ERR_clear_error();
    /* 클라이언트 쪽에서 실패하면(예: 시스템 설정이 낮은 버전 송신을 막음) 서버가 마감까지 기다리지
     * 않도록 쓰기 방향을 닫아 EOF 를 알린다 → 서버 쪽 결과가 환경과 무관하게 TLS_FAIL 로 정해진다 */
    if (!c->ok)
        shutdown(c->fd, SHUT_WR);
    return NULL;
}

static void fx_client_free(fx_client_t *c)
{
    if (c->ssl != NULL)
        SSL_free(c->ssl);
    c->ssl = NULL;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

/*
 * socketpair 를 만들어 서버 쪽은 tls_accept, 클라이언트 쪽은 스레드에서 SSL_connect.
 * 반환: tls_accept 결과 (준비 실패면 -99). 성공하면 srv->ssl 과 cl->ssl 이 연결된 상태.
 */
static int fx_handshake(SSL_CTX *sctx, SSL_CTX *cctx, int max_version, conn_t *srv, fx_client_t *cl)
{
    int sv[2];
    pthread_t th;
    int rc;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -99;
    conn_init_plain(srv, sv[0]);
    memset(cl, 0, sizeof *cl);
    cl->fd = sv[1];
    cl->ctx = cctx;
    cl->max_version = max_version;
    if (pthread_create(&th, NULL, fx_client_connect_main, cl) != 0)
        return -99;
    rc = tls_accept(sctx, srv, 2000, NULL);
    pthread_join(th, NULL);
    return rc;
}

#endif /* TLS_FIXTURE_H */
