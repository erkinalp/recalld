/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "server/http-server.h"
#include "server/jwt.h"
#include "server/query-service.h"
#include "server/pam-auth.h"
#include "common/recalld-config.h"
#include "common/recalld-log.h"

#define HTTP_MAX_REQUEST_SIZE 65536
#define HTTP_MAX_RESPONSE_SIZE 1048576

struct HttpServer {
        QueryServiceConfig config;
        QueryService *query_svc;
        JwtContext *jwt;
        PamAuthContext *pam;

        int listen_fd;
        SSL_CTX *ssl_ctx;
        bool running;
        pthread_t accept_thread;
};

typedef struct HttpRequest {
        char method[16];
        char path[512];
        char *body;
        size_t body_len;
        char *auth_header;
} HttpRequest;

typedef struct HttpResponse {
        int status;
        char *body;
        size_t body_len;
        char content_type[64];
} HttpResponse;

static int parse_http_request(const char *raw, size_t raw_len, HttpRequest *req) {
        const char *p, *line_end;

        memset(req, 0, sizeof(*req));

        /* Parse request line */
        line_end = strstr(raw, "\r\n");
        if (!line_end)
                return -EINVAL;

        sscanf(raw, "%15s %511s", req->method, req->path);

        /* Find Authorization header */
        p = strstr(raw, "Authorization: ");
        if (p && p < raw + raw_len) {
                const char *val = p + 15;
                const char *val_end = strstr(val, "\r\n");
                if (val_end) {
                        req->auth_header = strndup(val, (size_t)(val_end - val));
                }
        }

        /* Find body (after \r\n\r\n) */
        p = strstr(raw, "\r\n\r\n");
        if (p) {
                p += 4;
                req->body_len = raw_len - (size_t)(p - raw);
                if (req->body_len > 0) {
                        req->body = strndup(p, req->body_len);
                }
        }

        return 0;
}

static void http_request_free(HttpRequest *req) {
        free(req->auth_header);
        free(req->body);
}

static int http_send_response(int fd, SSL *ssl, const HttpResponse *resp) {
        char header[1024];
        int header_len;

        header_len = snprintf(header, sizeof(header),
                "HTTP/1.1 %d %s\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n",
                resp->status,
                resp->status == 200 ? "OK" :
                resp->status == 401 ? "Unauthorized" :
                resp->status == 404 ? "Not Found" :
                resp->status == 400 ? "Bad Request" :
                resp->status == 429 ? "Too Many Requests" :
                "Internal Server Error",
                resp->content_type,
                resp->body_len);

        if (ssl) {
                SSL_write(ssl, header, header_len);
                if (resp->body && resp->body_len > 0)
                        SSL_write(ssl, resp->body, (int) resp->body_len);
        } else {
                (void) write(fd, header, (size_t) header_len);
                if (resp->body && resp->body_len > 0)
                        (void) write(fd, resp->body, resp->body_len);
        }

        return 0;
}

static int handle_auth(HttpServer *srv, const HttpRequest *req, HttpResponse *resp) {
        char username[256] = "", password[256] = "";
        char *token = NULL;
        int r;

        if (!req->body)
                goto bad_request;

        /* Minimal JSON parsing for username and password */
        {
                char *u = strstr(req->body, "\"username\":\"");
                if (u) {
                        u += 12;
                        char *end = strchr(u, '"');
                        if (end)
                                snprintf(username, sizeof(username), "%.*s", (int)(end - u), u);
                }

                char *p = strstr(req->body, "\"password\":\"");
                if (p) {
                        p += 12;
                        char *end = strchr(p, '"');
                        if (end)
                                snprintf(password, sizeof(password), "%.*s", (int)(end - p), p);
                }
        }

        if (strlen(username) == 0 || strlen(password) == 0)
                goto bad_request;

        if (!srv->pam || !pam_auth_authenticate(srv->pam, username, password)) {
                resp->status = 401;
                resp->body = strdup("{\"error\":\"authentication_failed\"}");
                resp->body_len = strlen(resp->body);
                snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
                return 0;
        }

        r = jwt_generate(srv->jwt, username, srv->config.session_timeout, &token);
        if (r < 0) {
                resp->status = 500;
                resp->body = strdup("{\"error\":\"token_generation_failed\"}");
                resp->body_len = strlen(resp->body);
                snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
                return 0;
        }

        r = asprintf(&resp->body, "{\"token\":\"%s\"}", token);
        free(token);

        if (r < 0)
                return -ENOMEM;

        resp->body_len = (size_t) r;
        resp->status = 200;
        snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
        return 0;

bad_request:
        resp->status = 400;
        resp->body = strdup("{\"error\":\"invalid_request\"}");
        resp->body_len = strlen(resp->body);
        snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
        return 0;
}

static int validate_token(HttpServer *srv, const char *auth_header, JwtClaims *ret_claims) {
        const char *bearer;

        if (!auth_header)
                return -EACCES;

        bearer = strstr(auth_header, "Bearer ");
        if (!bearer)
                return -EACCES;

        return jwt_validate(srv->jwt, bearer + 7, ret_claims);
}

static int handle_query(HttpServer *srv, const HttpRequest *req, HttpResponse *resp) {
        JwtClaims claims = {};
        QueryRequest qreq = {};
        QueryResponse qresp = {};
        int r;

        /* Validate JWT token */
        if (srv->config.auth_required) {
                r = validate_token(srv, req->auth_header, &claims);
                if (r < 0) {
                        resp->status = 401;
                        resp->body = strdup("{\"error\":\"unauthorized\"}");
                        resp->body_len = strlen(resp->body);
                        snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
                        jwt_claims_free(&claims);
                        return 0;
                }
        }

        /* Parse query from body (minimal JSON parsing) */
        if (req->body) {
                char *q = strstr(req->body, "\"query\":\"");
                if (q) {
                        q += 9;
                        char *end = strchr(q, '"');
                        if (end)
                                qreq.query_text = strndup(q, (size_t)(end - q));
                }
        }

        qreq.max_results = srv->config.max_results_per_query;
        qreq.confidence_threshold = srv->config.confidence_threshold;

        r = query_service_process(srv->query_svc, &qreq, &qresp);
        if (r < 0) {
                resp->status = 500;
                resp->body = strdup("{\"error\":\"query_processing_failed\"}");
                resp->body_len = strlen(resp->body);
        } else {
                /* Build JSON response */
                size_t cap = 1024 + (size_t) qresp.result_count * 256;
                char *buf = malloc(cap);
                if (!buf) {
                        query_response_free(&qresp);
                        query_request_free(&qreq);
                        jwt_claims_free(&claims);
                        return -ENOMEM;
                }

                int pos = snprintf(buf, cap,
                        "{\"total_matches\":%d,\"processing_time_ms\":%.1f,\"results\":[",
                        qresp.total_matches, qresp.processing_time_ms);

                for (int i = 0; i < qresp.result_count; i++) {
                        QueryResult *qr = &qresp.results[i];
                        pos += snprintf(buf + pos, cap - (size_t) pos,
                                "%s{\"id\":%ld,\"type\":\"%s\",\"confidence\":%.2f,"
                                "\"timestamp\":%ld,\"duration_ms\":%d}",
                                i > 0 ? "," : "",
                                (long) qr->capture_id,
                                qr->content_type ? qr->content_type : "unknown",
                                qr->confidence,
                                (long) qr->timestamp,
                                qr->duration_ms);
                }

                pos += snprintf(buf + pos, cap - (size_t) pos, "]}");

                resp->status = 200;
                resp->body = buf;
                resp->body_len = (size_t) pos;
        }

        snprintf(resp->content_type, sizeof(resp->content_type), "application/json");

        query_response_free(&qresp);
        query_request_free(&qreq);
        jwt_claims_free(&claims);
        return 0;
}

static int handle_ingest(HttpServer *srv, const HttpRequest *req, HttpResponse *resp) {
        JwtClaims claims = {};
        int r;

        /* Validate JWT token */
        if (srv->config.auth_required) {
                r = validate_token(srv, req->auth_header, &claims);
                if (r < 0) {
                        resp->status = 401;
                        resp->body = strdup("{\"error\":\"unauthorized\"}");
                        resp->body_len = strlen(resp->body);
                        snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
                        jwt_claims_free(&claims);
                        return 0;
                }
        }

        if (!req->body || req->body_len == 0)
                goto bad_request;

        /* Parse ingest request from JSON body:
         * {"type":"audio|video|screenshot","duration_ms":N,"source":"...","data":"<base64>"} */
        {
                CaptureType type = _CAPTURE_TYPE_INVALID;
                int duration_ms = 0;
                char source[256] = "remote-ingest";

                /* Parse type */
                char *t = strstr(req->body, "\"type\":\"");
                if (t) {
                        t += 8;
                        char *end = strchr(t, '"');
                        if (end) {
                                char type_str[32];
                                snprintf(type_str, sizeof(type_str), "%.*s", (int)(end - t), t);
                                type = capture_type_from_string(type_str);
                        }
                }

                if (type == _CAPTURE_TYPE_INVALID)
                        goto bad_request;

                /* Parse duration_ms */
                char *d = strstr(req->body, "\"duration_ms\":");
                if (d) {
                        d += 14;
                        duration_ms = atoi(d);
                }

                /* Parse source */
                char *s = strstr(req->body, "\"source\":\"");
                if (s) {
                        s += 10;
                        char *end = strchr(s, '"');
                        if (end)
                                snprintf(source, sizeof(source), "%.*s", (int)(end - s), s);
                }

                /* Parse data (base64-encoded payload).
                 * For now, accept raw bytes after the JSON header if Content-Type
                 * indicates multipart, or the "data" field for JSON payloads. */
                char *data_field = strstr(req->body, "\"data\":\"");
                if (data_field) {
                        data_field += 8;
                        char *data_end = strchr(data_field, '"');
                        if (data_end) {
                                size_t encoded_len = (size_t)(data_end - data_field);

                                /* Simple base64 decode — the data is the capture payload.
                                 * For large payloads, bulk binary protocol is preferred. */
                                r = query_service_ingest(srv->query_svc, type,
                                                (const uint8_t *) data_field, encoded_len,
                                                duration_ms, source);
                                if (r < 0) {
                                        resp->status = 500;
                                        resp->body = strdup("{\"error\":\"ingest_failed\"}");
                                        resp->body_len = strlen(resp->body);
                                        snprintf(resp->content_type, sizeof(resp->content_type),
                                                 "application/json");
                                        jwt_claims_free(&claims);
                                        return 0;
                                }
                        }
                } else {
                        /* No data field — try to ingest the raw body as binary data.
                         * This supports the bulk binary protocol from the client transmitter. */
                        r = query_service_ingest(srv->query_svc, type,
                                        (const uint8_t *) req->body, req->body_len,
                                        duration_ms, source);
                        if (r < 0) {
                                resp->status = 500;
                                resp->body = strdup("{\"error\":\"ingest_failed\"}");
                                resp->body_len = strlen(resp->body);
                                snprintf(resp->content_type, sizeof(resp->content_type),
                                         "application/json");
                                jwt_claims_free(&claims);
                                return 0;
                        }
                }

                r = asprintf(&resp->body,
                        "{\"status\":\"ok\",\"type\":\"%s\",\"duration_ms\":%d,\"source\":\"%s\"}",
                        capture_type_to_string(type), duration_ms, source);
                if (r < 0) {
                        jwt_claims_free(&claims);
                        return -ENOMEM;
                }

                resp->body_len = (size_t) r;
                resp->status = 200;
                snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
        }

        jwt_claims_free(&claims);
        return 0;

bad_request:
        resp->status = 400;
        resp->body = strdup("{\"error\":\"invalid_ingest_request\"}");
        resp->body_len = strlen(resp->body);
        snprintf(resp->content_type, sizeof(resp->content_type), "application/json");
        jwt_claims_free(&claims);
        return 0;
}

static void handle_client(HttpServer *srv, int client_fd) {
        SSL *ssl = NULL;
        char buf[HTTP_MAX_REQUEST_SIZE];
        ssize_t n;
        HttpRequest req = {};
        HttpResponse resp = {};

        if (srv->ssl_ctx) {
                ssl = SSL_new(srv->ssl_ctx);
                SSL_set_fd(ssl, client_fd);
                if (SSL_accept(ssl) <= 0) {
                        log_warning("TLS handshake failed.");
                        SSL_free(ssl);
                        close(client_fd);
                        return;
                }
                n = SSL_read(ssl, buf, sizeof(buf) - 1);
        } else {
                n = read(client_fd, buf, sizeof(buf) - 1);
        }

        if (n <= 0)
                goto finish;

        buf[n] = '\0';

        if (parse_http_request(buf, (size_t) n, &req) < 0)
                goto finish;

        if (strcmp(req.path, "/api/v1/auth") == 0 && strcmp(req.method, "POST") == 0)
                handle_auth(srv, &req, &resp);
        else if (strcmp(req.path, "/api/v1/query") == 0 && strcmp(req.method, "POST") == 0)
                handle_query(srv, &req, &resp);
        else if (strcmp(req.path, "/api/v1/ingest") == 0 && strcmp(req.method, "POST") == 0)
                handle_ingest(srv, &req, &resp);
        else {
                resp.status = 404;
                resp.body = strdup("{\"error\":\"not_found\"}");
                resp.body_len = strlen(resp.body);
                snprintf(resp.content_type, sizeof(resp.content_type), "application/json");
        }

        http_send_response(client_fd, ssl, &resp);

finish:
        http_request_free(&req);
        free(resp.body);
        if (ssl)
                SSL_free(ssl);
        close(client_fd);
}

static void* accept_thread_fn(void *arg) {
        HttpServer *srv = arg;

        while (srv->running) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                int client_fd;

                client_fd = accept(srv->listen_fd, (struct sockaddr*) &client_addr, &addr_len);
                if (client_fd < 0) {
                        if (srv->running)
                                log_warning_errno(errno, "Accept failed: %m");
                        continue;
                }

                handle_client(srv, client_fd);
        }

        return NULL;
}

int http_server_new(
                HttpServer **ret,
                const QueryServiceConfig *config,
                QueryService *query_svc,
                JwtContext *jwt) {

        HttpServer *srv;

        if (!ret || !config || !query_svc || !jwt)
                return -EINVAL;

        srv = calloc(1, sizeof(HttpServer));
        if (!srv)
                return log_oom(), -ENOMEM;

        srv->config = *config;
        srv->query_svc = query_svc;
        srv->jwt = jwt;
        srv->listen_fd = -1;

        /* Initialize PAM */
        if (config->pam_service_name) {
                int r = pam_auth_new(&srv->pam, config->pam_service_name);
                if (r < 0)
                        log_warning_errno(-r, "PAM initialization failed, auth will be unavailable: %m");
        }

        /* Initialize TLS if enabled */
        if (config->tls_enabled && config->tls_cert_path && config->tls_key_path) {
                srv->ssl_ctx = SSL_CTX_new(TLS_server_method());
                if (srv->ssl_ctx) {
                        if (SSL_CTX_use_certificate_file(srv->ssl_ctx, config->tls_cert_path,
                                                         SSL_FILETYPE_PEM) <= 0 ||
                            SSL_CTX_use_PrivateKey_file(srv->ssl_ctx, config->tls_key_path,
                                                        SSL_FILETYPE_PEM) <= 0) {
                                log_warning("Failed to load TLS certificate/key, falling back to plain HTTP.");
                                SSL_CTX_free(srv->ssl_ctx);
                                srv->ssl_ctx = NULL;
                        } else {
                                log_info("TLS enabled with cert: %s", config->tls_cert_path);
                        }
                }
        }

        *ret = srv;
        return 0;
}

HttpServer* http_server_free(HttpServer *srv) {
        if (!srv)
                return NULL;

        if (srv->running)
                http_server_stop(srv);

        if (srv->listen_fd >= 0)
                close(srv->listen_fd);

        if (srv->ssl_ctx)
                SSL_CTX_free(srv->ssl_ctx);

        pam_auth_free(srv->pam);
        free(srv);
        return NULL;
}

int http_server_start(HttpServer *srv) {
        struct sockaddr_in addr = {};
        int opt = 1;
        int r;

        if (!srv)
                return -EINVAL;
        if (srv->running)
                return -EALREADY;

        srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (srv->listen_fd < 0)
                return log_error_errno(errno, "Failed to create socket: %m");

        setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons((uint16_t) srv->config.port);

        if (bind(srv->listen_fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return log_error_errno(errno, "Failed to bind to port %d: %m", srv->config.port);
        }

        if (listen(srv->listen_fd, 128) < 0) {
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return log_error_errno(errno, "Failed to listen: %m");
        }

        srv->running = true;

        r = pthread_create(&srv->accept_thread, /* attr= */ NULL, accept_thread_fn, srv);
        if (r != 0) {
                srv->running = false;
                close(srv->listen_fd);
                srv->listen_fd = -1;
                return -r;
        }

        log_info("HTTP server started on port %d%s.",
                 srv->config.port, srv->ssl_ctx ? " (TLS)" : "");
        return 0;
}

int http_server_stop(HttpServer *srv) {
        if (!srv)
                return -EINVAL;
        if (!srv->running)
                return 0;

        srv->running = false;

        /* Close the listening socket to unblock accept() */
        if (srv->listen_fd >= 0) {
                close(srv->listen_fd);
                srv->listen_fd = -1;
        }

        pthread_join(srv->accept_thread, /* retval= */ NULL);

        log_info("HTTP server stopped.");
        return 0;
}

bool http_server_is_running(const HttpServer *srv) {
        return srv ? srv->running : false;
}
