/*
 * AVSystem Commons Library
 *
 * Copyright (C) 2014 AVSystem <http://www.avsystem.com/>
 *
 * This code is free and open source software licensed under the MIT License.
 * See the LICENSE file for details.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include <config.h>

#ifdef WITH_LWIP
#   undef LWIP_COMPAT_SOCKETS
#   define LWIP_COMPAT_SOCKETS 1
#   include "lwipopts.h"
#   include "lwip/socket.h"
#else
#   include <sys/socket.h>
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/md5.h>
#include <openssl/hmac.h>

#include "net.h"

#ifdef HAVE_VISIBILITY
#pragma GCC visibility push(hidden)
#endif

#define CERT_SUBJECT_NAME_SIZE 257

typedef struct {
    const avs_net_socket_v_table_t * const operations;
    SSL_CTX *ctx;
    SSL *ssl;
    int verification;
#ifdef AVS_LOG_WITH_TRACE
    char *error_buffer;
#endif
    int64_t next_deadline_ms;
    avs_net_socket_type_t backend_type;
    avs_net_abstract_socket_t *backend_socket;
    avs_net_socket_configuration_t backend_configuration;
    avs_net_socket_raw_resolved_endpoint_t endpoint_buffer;
} ssl_socket_t;

static int connect_ssl(avs_net_abstract_socket_t *ssl_socket,
                       const char* host,
                       const char *port);
static int decorate_ssl(avs_net_abstract_socket_t *socket,
                        avs_net_abstract_socket_t *backend_socket);
static int configure_ssl(ssl_socket_t *socket,
                         const avs_net_ssl_configuration_t *configuration);
static int send_ssl(avs_net_abstract_socket_t *ssl_socket,
                    const void *buffer,
                    size_t buffer_length);
static int receive_ssl(avs_net_abstract_socket_t *ssl_socket,
                       size_t *out,
                       void *buffer,
                       size_t buffer_length);
static int shutdown_ssl(avs_net_abstract_socket_t *socket);
static int close_ssl(avs_net_abstract_socket_t *ssl_socket);
static int cleanup_ssl(avs_net_abstract_socket_t **ssl_socket);
static int system_socket_ssl(avs_net_abstract_socket_t *ssl_socket,
                             const void **out);
static int interface_name_ssl(avs_net_abstract_socket_t *ssl_socket,
                              avs_net_socket_interface_name_t *if_name);
static int remote_host_ssl(avs_net_abstract_socket_t *socket,
                           char *out_buffer, size_t ouf_buffer_size);
static int remote_port_ssl(avs_net_abstract_socket_t *socket,
                           char *out_buffer, size_t ouf_buffer_size);
static int local_port_ssl(avs_net_abstract_socket_t *socket,
                          char *out_buffer, size_t ouf_buffer_size);
static int get_opt_ssl(avs_net_abstract_socket_t *ssl_socket_,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t *out_option_value);
static int set_opt_ssl(avs_net_abstract_socket_t *net_socket,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t option_value);

static int unimplemented() {
    return -1;
}

static const avs_net_socket_v_table_t ssl_vtable = {
    connect_ssl,
    decorate_ssl,
    send_ssl,
    (avs_net_socket_send_to_t) unimplemented,
    receive_ssl,
    (avs_net_socket_receive_from_t) unimplemented,
    (avs_net_socket_bind_t) unimplemented,
    (avs_net_socket_accept_t) unimplemented,
    close_ssl,
    shutdown_ssl,
    cleanup_ssl,
    system_socket_ssl,
    interface_name_ssl,
    remote_host_ssl,
    remote_port_ssl,
    local_port_ssl,
    get_opt_ssl,
    set_opt_ssl
};

#ifdef AVS_LOG_WITH_TRACE

#define log_openssl_error(ssl_socket) \
    LOG(ERROR, "%s", ERR_error_string(ERR_get_error(), ssl_socket->error_buffer))

#else /* !AVS_LOG_WITH_TRACE */

#define log_openssl_error(socket) \
    LOG(ERROR, "OpenSSL error %lu", ERR_get_error())

#endif /* AVS_LOG_WITH_TRACE */

static avs_net_af_t get_socket_af(avs_net_abstract_socket_t *sock) {
    avs_net_socket_opt_value_t opt_value;
    if (avs_net_socket_get_opt(sock, AVS_NET_SOCKET_OPT_ADDR_FAMILY,
                               &opt_value)) {
        return AVS_NET_AF_UNSPEC;
    } else {
        return opt_value.addr_family;
    }
}

static int get_socket_mtu(avs_net_abstract_socket_t *sock) {
    avs_net_socket_opt_value_t opt_value;
    if (avs_net_socket_get_opt(sock, AVS_NET_SOCKET_OPT_MTU, &opt_value)) {
        return -1;
    } else {
        return opt_value.mtu;
    }
}

static int get_dtls_overhead(avs_net_abstract_socket_t *sock) {
    switch (get_socket_af(sock)) {
    case AVS_NET_AF_INET4:
        return 28;
    case AVS_NET_AF_INET6:
        return 48;
    default:
        return -1;
    }
}

static int calculate_mtu_or_zero(int base, int overhead) {
    if (base < 0 || overhead < 0) {
        return 0;
    } else {
        return base - overhead;
    }
}

static int get_dtls_mtu_or_zero(ssl_socket_t *sock) {
    return calculate_mtu_or_zero(get_socket_mtu(sock->backend_socket),
                                 get_dtls_overhead(sock->backend_socket));
}

#ifdef BIO_TYPE_SOURCE_SINK
static int avs_bio_write(BIO *bio, const char *data, int size) {
    if (!data || size < 0) {
        return 0;
    }
    BIO_clear_retry_flags(bio);
    if (avs_net_socket_send(((ssl_socket_t *) bio->ptr)->backend_socket,
                            data, (size_t) size)) {
        return -1;
    } else {
        return size;
    }
}

static int64_t current_time_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t) t.tv_sec + t.tv_nsec / 1000000;
}

static int get_socket_timeout(avs_net_abstract_socket_t *sock) {
    avs_net_socket_opt_value_t opt_value;
    avs_net_socket_get_opt(sock, AVS_NET_SOCKET_OPT_RECV_TIMEOUT, &opt_value);
    return opt_value.recv_timeout;
}

static void set_socket_timeout(avs_net_abstract_socket_t *sock, int timeout) {
    avs_net_socket_opt_value_t opt_value;
    opt_value.recv_timeout = timeout;
    avs_net_socket_set_opt(sock, AVS_NET_SOCKET_OPT_RECV_TIMEOUT, opt_value);
}

static int adjust_receive_timeout(ssl_socket_t *sock) {
    int socket_timeout = get_socket_timeout(sock->backend_socket);
    if (sock->next_deadline_ms >= 0) {
        int64_t now_ms = current_time_ms();
        int timeout = (int) (sock->next_deadline_ms - now_ms);
        if (socket_timeout <= 0 || socket_timeout > timeout) {
            set_socket_timeout(sock->backend_socket, timeout);
        }
    }
    return socket_timeout;
}

static int avs_bio_read(BIO *bio, char *buffer, int size) {
    ssl_socket_t *sock = (ssl_socket_t *) bio->ptr;
    int prev_timeout;
    size_t read_bytes;
    int result;
    if (!buffer || size < 0) {
        return 0;
    }
    BIO_clear_retry_flags(bio);
    if (sock->backend_type == AVS_NET_UDP_SOCKET) {
        prev_timeout = adjust_receive_timeout(sock);
    }
    if (avs_net_socket_receive(sock->backend_socket,
                               &read_bytes, buffer, (size_t) size)) {
        result = -1;
    } else {
        result = (int) read_bytes;
    }
    if (sock->backend_type == AVS_NET_UDP_SOCKET) {
        set_socket_timeout(sock->backend_socket, prev_timeout);
    }
    return result;
}

static int avs_bio_puts(BIO *bio, const char *data) {
    return avs_bio_write(bio, data, (int) strlen(data));
}

static int avs_bio_gets(BIO *bio, char *buffer, int size) {
    (void) bio;
    (void) buffer;
    (void) size;
    return -1;
}

static int get_dtls_fallback_mtu_or_zero(ssl_socket_t *sock) {
    char host[NET_MAX_HOSTNAME_SIZE];
    if (avs_net_socket_get_remote_host(sock->backend_socket,
                                       host, sizeof(host))) {
        return 0;
    } else {
        if (strchr(host, ':')) { /* IPv6 */
            return 1232; /* 1280 - 48 */
        } else {
            return 548; /* 576 - 28 */
        }
    }
}

static long avs_bio_ctrl(BIO *bio, int command, long intarg, void *ptrarg) {
    ssl_socket_t *sock = (ssl_socket_t *) bio->ptr;
    (void) intarg;
    switch (command) {
    case BIO_CTRL_FLUSH:
        return 1;
#if OPENSSL_VERSION_NUMBER >= 0x10001000L /* OpenSSL >= 1.0.1 */
    case BIO_CTRL_DGRAM_QUERY_MTU:
        return get_dtls_mtu_or_zero(sock);
    case BIO_CTRL_DGRAM_SET_NEXT_TIMEOUT:
        sock->next_deadline_ms =
                (int64_t) ((const struct timeval *) ptrarg)->tv_sec * 1000 +
                ((const struct timeval *) ptrarg)->tv_usec / 1000;
        return 0;
    case BIO_CTRL_DGRAM_GET_PEER:
        memcpy(ptrarg, sock->backend_configuration.preferred_endpoint->data,
               sock->backend_configuration.preferred_endpoint->size);
        return sock->backend_configuration.preferred_endpoint->size;
    case BIO_CTRL_DGRAM_GET_FALLBACK_MTU:
        return get_dtls_fallback_mtu_or_zero(sock);
#endif
    default:
        return 0;
    }
}

static int avs_bio_create(BIO *bio) {
    bio->init = 1;
    bio->num = 0;
    bio->ptr = NULL;
    bio->flags = 0;
    return 1;
}

static int avs_bio_destroy(BIO *bio) {
    if (!bio) {
        return 0;
    }
    bio->ptr = NULL; /* will be cleaned up elsewhere */
    bio->init = 0;
    bio->flags = 0;
    return 1;
}

static BIO_METHOD AVS_BIO = {
    (100 | BIO_TYPE_SOURCE_SINK),
    "avs_net",
    avs_bio_write,
    avs_bio_read,
    avs_bio_puts,
    avs_bio_gets,
    avs_bio_ctrl,
    avs_bio_create,
    avs_bio_destroy,
    NULL
};

static BIO *avs_bio_spawn(ssl_socket_t *socket) {
    BIO *bio = BIO_new(&AVS_BIO);
    if (bio) {
        bio->ptr = socket;
    }
    return bio;
}
#else /* BIO_TYPE_SOURCE_SINK */
static BIO *avs_bio_spawn(ssl_socket_t *socket) {
    const void *fd_ptr =
            avs_net_socket_get_system((avs_net_abstract_socket_t *) socket);
    if (fd_ptr) {
        int fd = *(const int *) fd_ptr;
        if (socket->backend_type == AVS_NET_TCP_SOCKET) {
            return BIO_new_socket(fd, 0);
        }
#if OPENSSL_VERSION_NUMBER >= 0x10001000L /* OpenSSL >= 1.0.1 */
        if (socket->backend_type == AVS_NET_UDP_SOCKET) {
            BIO *bio = BIO_new_dgram(fd, 0);
            BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0,
                     socket->backend_configuration.preferred_endpoint->data);
            return bio;
        }
#endif
    }
    return NULL;
}
#endif /* BIO_TYPE_SOURCE_SINK */

static int interface_name_ssl(avs_net_abstract_socket_t *ssl_socket_,
                              avs_net_socket_interface_name_t *if_name) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    if (ssl_socket->backend_socket) {
        return avs_net_socket_interface_name(
                (avs_net_abstract_socket_t *) ssl_socket->backend_socket,
                if_name);
    } else {
        return -1;
    }
}

static int remote_host_ssl(avs_net_abstract_socket_t *socket_,
                           char *out_buffer, size_t out_buffer_size) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (!socket->backend_socket) {
        return -1;
    }
    return avs_net_socket_get_remote_host(socket->backend_socket,
                                          out_buffer, out_buffer_size);
}

static int remote_port_ssl(avs_net_abstract_socket_t *socket_,
                           char *out_buffer, size_t out_buffer_size) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (!socket->backend_socket) {
        return -1;
    }
    return avs_net_socket_get_remote_port(socket->backend_socket,
                                          out_buffer, out_buffer_size);
}

static int local_port_ssl(avs_net_abstract_socket_t *socket_,
                          char *out_buffer, size_t out_buffer_size) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (!socket->backend_socket) {
        return -1;
    }
    return avs_net_socket_get_local_port(socket->backend_socket,
                                         out_buffer, out_buffer_size);
}

static int get_opt_ssl(avs_net_abstract_socket_t *ssl_socket_,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t *out_option_value) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    switch (option_key) {
    case AVS_NET_SOCKET_OPT_MTU:
        out_option_value->mtu = get_dtls_mtu_or_zero(ssl_socket);
        return out_option_value->mtu > 0 ? 0 : -1;
    default:
        return avs_net_socket_get_opt(ssl_socket->backend_socket, option_key,
                                    out_option_value);
    }
}

static int set_opt_ssl(avs_net_abstract_socket_t *ssl_socket_,
                       avs_net_socket_opt_key_t option_key,
                       avs_net_socket_opt_value_t option_value) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    return avs_net_socket_set_opt(ssl_socket->backend_socket, option_key,
                                option_value);
}

static int system_socket_ssl(avs_net_abstract_socket_t *ssl_socket_,
                             const void **out) {
    ssl_socket_t *ssl_socket = (ssl_socket_t *) ssl_socket_;
    if (ssl_socket->backend_socket) {
        *out = avs_net_socket_get_system(ssl_socket->backend_socket);
    } else {
        *out = NULL;
    }
    return *out ? 0 : -1;
}

static int verify_peer_subject_cn(ssl_socket_t *ssl_socket,
                                  const char *host) {
    char buffer[CERT_SUBJECT_NAME_SIZE];
    char *cn = NULL;
    X509* peer_certificate = NULL;

    /* check whether CN matches host portion of ACS URL */
    peer_certificate = SSL_get_peer_certificate(ssl_socket->ssl);
    if (!peer_certificate) {
        LOG(ERROR, "Cannot load peer certificate");
        return -1;
    }
    X509_NAME_oneline(X509_get_subject_name(peer_certificate),
                      buffer, sizeof (buffer));
    X509_free(peer_certificate);

    cn = strstr(buffer, "CN=");
    if (cn != NULL) {
        char* cne = strchr(cn, '/');
        if (cne) *cne = '\0';
        cn += 3;
    }
    if (cn == NULL || strcmp(cn, host)) {
        LOG(ERROR, "Subject CN(%s) does not match ACS URL (%s)", cn, host);
        return -1;
    }

    return 0;
}

static int ssl_handshake(ssl_socket_t *socket) {
    avs_net_socket_opt_value_t state_opt;
    if (avs_net_socket_get_opt(socket->backend_socket,
                               AVS_NET_SOCKET_OPT_STATE, &state_opt)) {
        LOG(ERROR, "ssl_handshake: could not get socket state");
        return -1;
    }
    if (state_opt.state == AVS_NET_SOCKET_STATE_CONSUMING) {
        return SSL_connect(socket->ssl);
    }
    if (state_opt.state == AVS_NET_SOCKET_STATE_SERVING) {
        return SSL_accept(socket->ssl);
    }
    LOG(ERROR, "ssl_handshake: invalid socket state");
    return -1;
}

static int start_ssl(ssl_socket_t *socket, const char *host) {
    BIO *bio = NULL;
    LOG(TRACE, "start_ssl(socket=%p)", (void *) socket);

    if (socket->ssl) {
        LOG(ERROR, "SSL socket already connected");
        return -1;
    }

    socket->ssl = SSL_new(socket->ctx);
    if (!socket->ssl) {
        return -1;
    }

#ifdef SSL_MODE_AUTO_RETRY
    SSL_set_mode(socket->ssl, SSL_MODE_AUTO_RETRY);
#endif

    bio = avs_bio_spawn(socket);
    if (!bio) {
        LOG(ERROR, "cannot create BIO object");
        return -1;
    }
    SSL_set_bio(socket->ssl, bio, bio);

    {
        int handshake_result = ssl_handshake(socket);
        if (handshake_result <= 0) {
            LOG(ERROR, "SSL handshake failed.");
            log_openssl_error(socket);
            LOG(TRACE, "handshake_result = %d", handshake_result);
            return -1;
        }
    }

    if (socket->verification && verify_peer_subject_cn(socket, host) != 0) {
        LOG(ERROR, "server certificate verification failure");
        return -1;
    }

    return 0;
}

static int connect_ssl(avs_net_abstract_socket_t *socket_,
                       const char *host,
                       const char *port) {
    int result;
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    LOG(TRACE, "connect_ssl(socket=%p, host=%s, port=%s)",
        (void *) socket, host, port);

    if (avs_net_socket_create(&socket->backend_socket, socket->backend_type,
                              &socket->backend_configuration)) {
        return -1;
    }
    if (avs_net_socket_connect(socket->backend_socket, host, port)) {
        LOG(ERROR, "cannot establish TCP connection");
        return -1;
    }

    result = start_ssl(socket, host);
    if (result) {
        close_ssl(socket_);
    }
    return result;
}

static int decorate_ssl(avs_net_abstract_socket_t *socket_,
                        avs_net_abstract_socket_t *backend_socket) {
    char host[NET_MAX_HOSTNAME_SIZE];
    int result;
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    LOG(TRACE, "decorate_ssl(socket=%p, backend_socket=%p)",
        (void *) socket, (void *) backend_socket);

    if (socket->backend_socket) {
        avs_net_socket_cleanup(&socket->backend_socket);
    }

    if (remote_host_ssl(backend_socket, host, sizeof(host))) {
        return -1;
    }

    socket->backend_socket = backend_socket;
    result = start_ssl(socket, host);
    if (result) {
        socket->backend_socket = NULL;
        close_ssl(socket_);
    }
    return result;
}

static int load_ca_certs(const char *ca_cert_path,
                         const char *ca_cert_file,
                         ssl_socket_t *socket) {

    if (!ca_cert_path && !ca_cert_file) {
        LOG(ERROR, "no certificate for CA provided");
        return -1;
    }

    if (!SSL_CTX_load_verify_locations(socket->ctx,
                                       ca_cert_file,
                                       ca_cert_path)) {
        return -1;
    }
    if (!SSL_CTX_set_default_verify_paths(socket->ctx)) {
        return -1;
    }

    return 0;
}

static int password_cb(char *buf, int num, int rwflag, void *userdata) {
    int retval = snprintf(buf, (size_t) num, "%s", (const char *) userdata);
    (void) rwflag;
    return (retval < 0 || retval >= num) ? -1 : 0;
}

static int load_client_cert(const char *client_cert_file,
                            const char *client_key_file,
                            const char *client_key_password,
                            ssl_socket_t *socket) {

    if (!client_cert_file) {
        LOG(TRACE, "client certificate not specified");
        return 0;
    }
    if (!client_key_file || !client_key_password) {
        LOG(ERROR, "private key with password not specified");
        return -1;
    }

    if (!SSL_CTX_use_certificate_chain_file(socket->ctx, client_cert_file)) {
        log_openssl_error(socket);
        return -1;
    }

    SSL_CTX_set_default_passwd_cb_userdata(socket->ctx,
                      /* const_cast */ (void *) (intptr_t) client_key_password);
    SSL_CTX_set_default_passwd_cb(socket->ctx, password_cb);

    if (!SSL_CTX_use_PrivateKey_file(socket->ctx,
                                     client_key_file,
                                     SSL_FILETYPE_PEM)) {
        log_openssl_error(socket);
        return -1;
    }

    return 0;
}

static int server_auth_enabled(const avs_net_ssl_configuration_t *configuration) {
    return configuration->ca_cert_file || configuration->ca_cert_path;
}

static int configure_ssl(ssl_socket_t *socket,
                         const avs_net_ssl_configuration_t *configuration) {
    LOG(TRACE, "configure_ssl(socket=%p, configuration=%p)",
        (void *) socket, (const void *) configuration);

    socket->backend_configuration = configuration->backend_configuration;
    if (!socket->backend_configuration.preferred_endpoint) {
        socket->backend_configuration.preferred_endpoint =
                &socket->endpoint_buffer;
    }

    ERR_clear_error();
    SSL_CTX_set_options(socket->ctx, (long) (SSL_OP_ALL | SSL_OP_NO_SSLv2));
    if (socket->backend_type == AVS_NET_UDP_SOCKET) {
        SSL_CTX_set_read_ahead(socket->ctx, 1);
    }
    SSL_CTX_set_verify(socket->ctx, SSL_VERIFY_NONE, NULL);
#ifdef WITH_OPENSSL_CUSTOM_CIPHERS
    SSL_CTX_set_cipher_list(socket->ctx, WITH_OPENSSL_CUSTOM_CIPHERS);
#endif

    if (!configuration) {
        LOG(WARNING, "configuration not provided");
        return 0;
    }

    if (server_auth_enabled(configuration)) {
        socket->verification = 1;
        SSL_CTX_set_verify(socket->ctx, SSL_VERIFY_PEER, NULL);
#if defined(OPENSSL_VERSION_NUMBER) && (OPENSSL_VERSION_NUMBER < 0x00905100L)
        SSL_CTX_set_verify_depth(socket->ctx, 1);
#endif
        if (load_ca_certs(configuration->ca_cert_path,
                          configuration->ca_cert_file,
                          socket)) {
            LOG(ERROR, "Error loading CA certs");
            return -1;
        }
    } else {
        LOG(DEBUG, "Server authentication disabled");
    }

    if (load_client_cert(configuration->client_cert_file,
                         configuration->client_key_file,
                         configuration->client_key_password,
                         socket)) {
        LOG(ERROR, "Error loading client certificate");
        return -1;
    }

    if (configuration->additional_configuration_clb
            && configuration->additional_configuration_clb(socket->ctx)) {
        LOG(ERROR, "Error while setting additional SSL configuration");
        return -1;
    }
    return 0;
}

static int shutdown_ssl(avs_net_abstract_socket_t *socket_) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    if (socket->backend_socket) {
        return avs_net_socket_shutdown(socket->backend_socket);
    } else {
        return 0;
    }
}

static int close_ssl(avs_net_abstract_socket_t *socket_) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    LOG(TRACE, "close_ssl(socket=%p)", (void *) socket);
    if (socket->ssl) {
        SSL_shutdown(socket->ssl);
        SSL_free(socket->ssl);
        socket->ssl = NULL;
    }
    if (socket->backend_socket) {
        avs_net_socket_close(socket->backend_socket);
        avs_net_socket_cleanup(&socket->backend_socket);
    }

    return 0;
}

static int send_ssl(avs_net_abstract_socket_t *socket_,
                    const void *buffer,
                    size_t buffer_length) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    int result;

    LOG(TRACE, "send_ssl(socket=%p, buffer=%p, buffer_length=%lu)",
        (void *) socket, buffer, (unsigned long) buffer_length);

    result = SSL_write(socket->ssl, buffer, (int) buffer_length);
    if (result < 0 || (size_t) result < buffer_length) {
        LOG(ERROR, "write failed");
        return -1;
    }
    return 0;
}

static int receive_ssl(avs_net_abstract_socket_t *socket_,
                       size_t *out,
                       void *buffer,
                       size_t buffer_length) {
    ssl_socket_t *socket = (ssl_socket_t *) socket_;
    int result;
    LOG(TRACE, "receive_ssl(socket=%p, buffer=%p, buffer_length=%lu)",
        (void *) socket, buffer, (unsigned long) buffer_length);

    result = SSL_read(socket->ssl, buffer, (int) buffer_length);
    if (result < 0) {
        *out = 0;
        return result;
    } else {
        VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE(buffer, result);
        *out = (size_t) result;
        return 0;
    }
}

static int cleanup_ssl(avs_net_abstract_socket_t **socket_) {
    ssl_socket_t **socket = (ssl_socket_t **) socket_;
    LOG(TRACE, "cleanup_ssl(*socket=%p)", (void *) *socket);

    close_ssl(*socket_);
    if ((*socket)->ctx) {
        SSL_CTX_free((*socket)->ctx);
        (*socket)->ctx = NULL;
    }
#ifdef AVS_LOG_WITH_TRACE
    free((*socket)->error_buffer);
#endif
    free(*socket);
    *socket = NULL;
    return 0;
}

static int avs_ssl_init() {
    static volatile int initialized = 0;
    if (!initialized) {
        initialized = 1;
        LOG(TRACE, "OpenSSL initialization");

        SSL_library_init();
#ifdef AVS_LOG_WITH_TRACE
        SSL_load_error_strings();
#endif
        OpenSSL_add_all_algorithms();
        if (!RAND_load_file("/dev/urandom", -1)) {
            LOG(WARNING, "RAND_load_file error");
        }
        /* On some OpenSSL version, RAND_load file causes hell to break loose.
         * Get rid of any "uninitialized" memory that it created :( */
        VALGRIND_MAKE_MEM_DEFINED_IF_ADDRESSABLE(0, sbrk(0));
    }
    return 0;
}

static const SSL_METHOD *stream_method(avs_net_ssl_version_t version) {
    switch (version) {
#ifndef OPENSSL_NO_SSL2
    case AVS_NET_SSL_VERSION_SSLv2:
        return SSLv2_method();
#endif

#ifndef OPENSSL_NO_SSL3
    case AVS_NET_SSL_VERSION_SSLv2_OR_3:
        return SSLv23_method();

    case AVS_NET_SSL_VERSION_SSLv3:
        return SSLv3_method();
#endif

#ifndef OPENSSL_NO_TLS1
    case AVS_NET_SSL_VERSION_TLSv1:
        return TLSv1_method();

#if OPENSSL_VERSION_NUMBER >= 0x10001000L /* OpenSSL >= 1.0.1 */
    case AVS_NET_SSL_VERSION_TLSv1_1:
        return TLSv1_1_method();

    case AVS_NET_SSL_VERSION_TLSv1_2:
        return TLSv1_2_method();
#endif
#endif /* OPENSSL_NO_TLS1 */

    default:
        return NULL;
    }
}

static const SSL_METHOD *dgram_method(avs_net_ssl_version_t version) {
    switch (version) {
#if OPENSSL_VERSION_NUMBER >= 0x10001000L /* OpenSSL >= 1.0.1 */
    case AVS_NET_SSL_VERSION_TLSv1:
    case AVS_NET_SSL_VERSION_TLSv1_1:
        return DTLSv1_method();
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10002000L /* OpenSSL >= 1.0.2 */
    case AVS_NET_SSL_VERSION_TLSv1_2:
        return DTLSv1_2_method();
#endif

    default:
        return NULL;
    }
}

static const SSL_METHOD *ssl_method(avs_net_socket_type_t backend_type,
                                    avs_net_ssl_version_t version) {
    switch (backend_type) {
    case AVS_NET_TCP_SOCKET:
        return stream_method(version);
    case AVS_NET_UDP_SOCKET:
        return dgram_method(version);
    default:
        return NULL;
    }
}

static int initialize_ssl_socket(ssl_socket_t *socket,
                                 avs_net_socket_type_t backend_type,
                                 const avs_net_ssl_configuration_t *configuration) {
    const SSL_METHOD *method = NULL;

    memset(socket, 0, sizeof(*socket));
    *(const avs_net_socket_v_table_t **) (intptr_t) &socket->operations =
            &ssl_vtable;
#ifdef AVS_LOG_WITH_TRACE
    socket->error_buffer = (char *) malloc(120); /* see 'man ERR_error_string' */
    if (!socket->error_buffer) {
        LOG(WARNING, "Cannot create buffer for OpenSSL error strings");
    }
#endif /* AVS_LOG_WITH_TRACE */
    socket->backend_type = backend_type;

    if (!(method = ssl_method(backend_type, configuration->version))) {
        LOG(ERROR, "Unsupported SSL version");
        return -1;
    }

    /* older versions of OpenSSL expect non-const pointer here... */
    socket->ctx = SSL_CTX_new((SSL_METHOD *) (intptr_t) method);
    if (socket->ctx == NULL) {
        log_openssl_error(socket);
        return -1;
    }

    return configure_ssl(socket, configuration);
}

static int create_ssl_socket(avs_net_abstract_socket_t **socket,
                             avs_net_socket_type_t backend_type,
                             const void *socket_configuration) {
    LOG(TRACE, "_cwmp_create_ssl_socket(socket=%p)", (void *) socket);

    if (avs_ssl_init()) {
        LOG(ERROR, "OpenSSL initialization error");
        return -1;
    }

    *socket = (avs_net_abstract_socket_t *) malloc(sizeof (ssl_socket_t));
    if (*socket) {
        if (initialize_ssl_socket((ssl_socket_t *) * socket, backend_type,
                                  (const avs_net_ssl_configuration_t *)
                                  socket_configuration)) {
            LOG(ERROR, "socket initialization error");
            avs_net_socket_cleanup(socket);
            return -1;
        } else {
            return 0;
        }
    } else {
        LOG(ERROR, "memory allocation error");
        return -1;
    }
}

int _avs_net_create_ssl_socket(avs_net_abstract_socket_t **socket,
                               const void *socket_configuration) {
    return create_ssl_socket(socket, AVS_NET_TCP_SOCKET, socket_configuration);
}

int _avs_net_create_dtls_socket(avs_net_abstract_socket_t **socket,
                               const void *socket_configuration) {
    return create_ssl_socket(socket, AVS_NET_UDP_SOCKET, socket_configuration);
}