/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * soup-auth.c: HTTP Authentication schemes (basic and digest)
 *
 * Authors:
 *      Joe Shaw (joe@ximian.com)
 *      Jeffrey Steadfast (fejj@ximian.com)
 *      Alex Graveley (alex@ximian.com)
 *
 * Copyright (C) 2001, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef SOUP_WIN32
#include <process.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <glib.h>

#include <ctype.h>
#include <stdio.h>
#include <time.h>

#include "md5-utils.h"
#include "soup-auth.h"
#include "soup-context.h"
#include "soup-message.h"
#include "soup-private.h"
#include "soup-ntlm.h"

static char       *copy_token_if_exists (GHashTable  *tokens, char *t);
static char       *decode_token         (char       **in);
static GHashTable *parse_param_list     (const char  *header);
static void        destroy_param_hash   (GHashTable  *table);


/* 
 * Basic Authentication Support 
 */

typedef struct {
	SoupAuth auth;
	gchar *token;
} SoupAuthBasic;

static gboolean
basic_compare_func (SoupAuth *a, SoupAuth *b)
{
	return FALSE;
}

static char *
basic_auth_func (SoupAuth *auth, SoupMessage *message)
{
	SoupAuthBasic *basic = (SoupAuthBasic *) auth;

	return g_strconcat ("Basic ", basic->token, NULL);
}

static void
basic_parse_func (SoupAuth *auth, const char *header)
{
	GHashTable *tokens;

	header += sizeof ("Basic");

	tokens = parse_param_list (header);
	if (!tokens) return;

	auth->realm = copy_token_if_exists (tokens, "realm");

	destroy_param_hash (tokens);
}

static void
basic_init_func (SoupAuth *auth, const SoupUri *uri)
{
	SoupAuthBasic *basic = (SoupAuthBasic *) auth;
	char *user_pass;

	user_pass = g_strdup_printf ("%s:%s", uri->user, uri->passwd);
	basic->token = soup_base64_encode (user_pass, strlen (user_pass));
	g_free (user_pass);
}

static void
basic_free (SoupAuth *auth)
{
	SoupAuthBasic *basic = (SoupAuthBasic *) auth;

	g_free (basic->token);
	g_free (basic);
}

static SoupAuth *
soup_auth_new_basic (void)
{
	SoupAuthBasic *basic;
	SoupAuth *auth;

	basic = g_new0 (SoupAuthBasic, 1);
	auth = (SoupAuth *) basic;
	auth->type = SOUP_AUTH_TYPE_BASIC;

	auth->compare_func = basic_compare_func;
	auth->parse_func = basic_parse_func;
	auth->init_func = basic_init_func;
	auth->auth_func = basic_auth_func;
	auth->free_func = basic_free;

	return auth;
}


/* 
 * Digest Authentication Support 
 */

typedef enum {
	QOP_NONE     = 0,
	QOP_AUTH     = 1 << 0,
	QOP_AUTH_INT = 1 << 1
} QOPType;

typedef enum {
	ALGORITHM_MD5      = 1 << 0,
	ALGORITHM_MD5_SESS = 1 << 1
} AlgorithmType;

typedef struct {
	SoupAuth auth;

	gchar  *user;
	guchar  hex_a1 [33];

	/* These are provided by the server */
	char *nonce;
	QOPType qop_options;
	AlgorithmType algorithm;
	gboolean stale;

	/* These are generated by the client */
	char *cnonce;
	int nc;
	QOPType qop;
} SoupAuthDigest;

static gboolean
digest_compare_func (SoupAuth *a, SoupAuth *b)
{
	return ((SoupAuthDigest *) a)->stale;
}

static void
digest_hex (guchar *digest, guchar hex[33])
{
	guchar *s, *p;

	/* lowercase hexify that bad-boy... */
	for (s = digest, p = hex; p < hex + 32; s++, p += 2)
		sprintf (p, "%.2x", *s);
}

static char *
compute_response (SoupAuthDigest *digest, SoupMessage *msg)
{
	const SoupUri *uri;
	guchar hex_a2[33], o[33];
	guchar d[16];
	MD5Context ctx;
	char *url;

	uri = soup_context_get_uri (msg->context);
	if (uri->querystring)
		url = g_strdup_printf ("%s?%s", uri->path, uri->querystring);
	else
		url = g_strdup (uri->path);

	/* compute A2 */
	md5_init (&ctx);
	md5_update (&ctx, msg->method, strlen (msg->method));
	md5_update (&ctx, ":", 1);
	md5_update (&ctx, url, strlen (url));
	g_free (url);

	if (digest->qop == QOP_AUTH_INT) {
		/* FIXME: Actually implement. Ugh. */
		md5_update (&ctx, ":", 1);
		md5_update (&ctx, "00000000000000000000000000000000", 32);
	}

	/* now hexify A2 */
	md5_final (&ctx, d);
	digest_hex (d, hex_a2);

	/* compute KD */
	md5_init (&ctx);
	md5_update (&ctx, digest->hex_a1, 32);
	md5_update (&ctx, ":", 1);
	md5_update (&ctx, digest->nonce, strlen (digest->nonce));
	md5_update (&ctx, ":", 1);

	if (digest->qop) {
		char *tmp;

		tmp = g_strdup_printf ("%.8x", digest->nc);

		md5_update (&ctx, tmp, strlen (tmp));
		g_free (tmp);
		md5_update (&ctx, ":", 1);
		md5_update (&ctx, digest->cnonce, strlen (digest->cnonce));
		md5_update (&ctx, ":", 1);

		if (digest->qop == QOP_AUTH)
			tmp = "auth";
		else if (digest->qop == QOP_AUTH_INT)
			tmp = "auth-int";
		else
			g_assert_not_reached ();

		md5_update (&ctx, tmp, strlen (tmp));
		md5_update (&ctx, ":", 1);
	}

	md5_update (&ctx, hex_a2, 32);
	md5_final (&ctx, d);

	digest_hex (d, o);

	return g_strdup (o);
}

static char *
digest_auth_func (SoupAuth *auth, SoupMessage *message)
{
	SoupAuthDigest *digest = (SoupAuthDigest *) auth;
	const SoupUri *uri;
	char *response;
	char *qop = NULL;
	char *nc;
	char *url;
	char *out;

	g_return_val_if_fail (message, NULL);

	response = compute_response (digest, message);

	if (digest->qop == QOP_AUTH)
		qop = "auth";
	else if (digest->qop == QOP_AUTH_INT)
		qop = "auth-int";
	else
		g_assert_not_reached ();

	uri = soup_context_get_uri (message->context);
	if (uri->querystring)
		url = g_strdup_printf ("%s?%s", uri->path, uri->querystring);
	else
		url = g_strdup (uri->path);

	nc = g_strdup_printf ("%.8x", digest->nc);

	out = g_strdup_printf (
		"Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", %s%s%s "
		"%s%s%s %s%s%s uri=\"%s\", response=\"%s\"",
		digest->user,
		auth->realm,
		digest->nonce,

		digest->qop ? "cnonce=\"" : "",
		digest->qop ? digest->cnonce : "",
		digest->qop ? "\"," : "",

		digest->qop ? "nc=" : "",
		digest->qop ? nc : "",
		digest->qop ? "," : "",

		digest->qop ? "qop=" : "",
		digest->qop ? qop : "",
		digest->qop ? "," : "",

		url,
		response);

	g_free (url);
	g_free (nc);

	digest->nc++;

	return out;
}

typedef struct {
	char *name;
	guint type;
} DataType;

static DataType qop_types[] = {
	{ "auth",     QOP_AUTH     },
	{ "auth-int", QOP_AUTH_INT }
};

static DataType algorithm_types[] = {
	{ "MD5",      ALGORITHM_MD5      },
	{ "MD5-sess", ALGORITHM_MD5_SESS }
};

static guint
decode_data_type (DataType *dtype, const char *name)
{
        int i;

        for (i = 0; dtype[i].name; i++) {
                if (!g_strcasecmp (dtype[i].name, name))
			return dtype[i].type;
        }

	return 0;
}

static inline guint
decode_qop (const char *name)
{
	return decode_data_type (qop_types, name);
}

static inline guint
decode_algorithm (const char *name)
{
	return decode_data_type (algorithm_types, name);
}

static void
digest_parse_func (SoupAuth *auth, const char *header)
{
	SoupAuthDigest *digest = (SoupAuthDigest *) auth;
	GHashTable *tokens;
	char *tmp, *ptr;

	header += sizeof ("Digest");

	tokens = parse_param_list (header);
	if (!tokens) return;

	auth->realm = copy_token_if_exists (tokens, "realm");

	digest->nonce = copy_token_if_exists (tokens, "nonce");

	tmp = copy_token_if_exists (tokens, "qop");
	ptr = tmp;

	while (ptr && *ptr) {
		char *token;

		token = decode_token (&ptr);
		if (token)
			digest->qop_options |= decode_qop (token);
		g_free (token);

		if (*ptr == ',')
			ptr++;
	}

	g_free (tmp);

	tmp = copy_token_if_exists (tokens, "stale");

	if (tmp && g_strcasecmp (tmp, "true") == 0)
		digest->stale = TRUE;
	else
		digest->stale = FALSE;

	g_free (tmp);

	tmp = copy_token_if_exists (tokens, "algorithm");
	digest->algorithm = decode_algorithm (tmp);
	g_free (tmp);

	destroy_param_hash (tokens);
}

static void
digest_init_func (SoupAuth *auth, const SoupUri *uri)
{
	SoupAuthDigest *digest = (SoupAuthDigest *) auth;
	MD5Context ctx;
	guchar d[16];
	char *tmp;

	digest->user = g_strdup (uri->user);

	/* compute A1 */
	md5_init (&ctx);
	md5_update (&ctx, uri->user, strlen (uri->user));
	md5_update (&ctx, ":", 1);
	md5_update (&ctx, auth->realm, strlen (auth->realm));
	md5_update (&ctx, ":", 1);
	tmp = uri->passwd ? uri->passwd : "";
	md5_update (&ctx, tmp, strlen (tmp));

	if (digest->algorithm == ALGORITHM_MD5_SESS) {
		md5_final (&ctx, d);

		md5_init (&ctx);
		md5_update (&ctx, d, 16);
		md5_update (&ctx, ":", 1);
		md5_update (&ctx, digest->nonce, strlen (digest->nonce));
		md5_update (&ctx, ":", 1);
		md5_update (&ctx, digest->cnonce, strlen (digest->cnonce));
	}

	/* hexify A1 */
	md5_final (&ctx, d);
	digest_hex (d, digest->hex_a1);
}

static void
digest_free (SoupAuth *auth)
{
	SoupAuthDigest *digest = (SoupAuthDigest *) auth;

	g_free (digest->user);

	g_free (digest->nonce);
	g_free (digest->cnonce);
	g_free (digest);
}

static SoupAuth *
soup_auth_new_digest (void)
{
	SoupAuthDigest *digest;
	SoupAuth *auth;
	char *bgen;

	digest = g_new0 (SoupAuthDigest, 1);

	auth = (SoupAuth *) digest;
	auth->type = SOUP_AUTH_TYPE_DIGEST;

	auth->compare_func = digest_compare_func;
	auth->parse_func = digest_parse_func;
	auth->init_func = digest_init_func;
	auth->auth_func = digest_auth_func;
	auth->free_func = digest_free;

	bgen = g_strdup_printf ("%p:%lu:%lu",
			       auth,
			       (unsigned long) getpid (),
			       (unsigned long) time (0));
	digest->cnonce = soup_base64_encode (bgen, strlen (bgen));
	digest->nc = 1;
	/* We're just going to do qop=auth for now */
	digest->qop = QOP_AUTH;

	return auth;
}


/*
 * NTLM Authentication Support
 */

typedef struct {
	SoupAuth  auth;
	gchar    *response;
	gchar    *header;
} SoupAuthNTLM;

static gboolean
ntlm_compare_func (SoupAuth *a, SoupAuth *b)
{
	return TRUE;
} 

/*
 * SoupAuthNTLMs are one time use. Just return the response, and set our
 * reference to NULL so future requests do not include this header.
 */
static gchar *
ntlm_auth (SoupAuth *sa, SoupMessage *msg)
{
	SoupAuthNTLM *auth = (SoupAuthNTLM *) sa;
	gchar *ret;

	ret = auth->response;
	auth->response = NULL;

	return ret;
}

static inline gchar *
ntlm_get_authmech_token (const SoupUri *uri, gchar *key)
{
	gchar *idx;
	gint len;

	if (!uri->authmech) return NULL;

      	idx = strstr (uri->authmech, key);
	if (idx) {
		idx += strlen (key);

		len = strcspn (idx, ",; ");
		if (len)
			return g_strndup (idx, len);
		else
			return g_strdup (idx);
	}

	return NULL;
}

static void
ntlm_parse (SoupAuth *sa, const char *header)
{
	SoupAuthNTLM *auth = (SoupAuthNTLM *) sa;

	auth->header = g_strdup (header);
	g_strstrip (auth->header);
}

/*
 * FIXME: Because NTLM is a two step process, we parse the host and domain out
 *        of the context's uri twice. This is because there is no way to reparse
 *        a new header with an existing SoupAuth, so a new one is created for
 *        each negotiation step.
 */
static void
ntlm_init (SoupAuth *sa, const SoupUri *uri)
{
	SoupAuthNTLM *auth = (SoupAuthNTLM *) sa;
	gchar *host, *domain;

	host   = ntlm_get_authmech_token (uri, "host=");
	domain = ntlm_get_authmech_token (uri, "domain=");

	if (strlen (auth->header) < sizeof ("NTLM"))
		auth->response = 
			soup_ntlm_request (host ? host : "UNKNOWN", 
					   domain ? domain : "UNKNOWN");
	else {
		gchar lm_hash [21], nt_hash [21];

		soup_ntlm_lanmanager_hash (uri->passwd, lm_hash);
		soup_ntlm_nt_hash (uri->passwd, nt_hash);

		auth->response = 
			soup_ntlm_response (auth->header,
					    uri->user,
					    (gchar *) &lm_hash,
					    (gchar *) &nt_hash,
					    host ? host : "UNKNOWN",
					    domain ? domain : "UNKNOWN");
	}

	g_free (host);
	g_free (domain);

	g_free (auth->header);
	auth->header = NULL;
}

static void
ntlm_free (SoupAuth *sa)
{
	SoupAuthNTLM *auth = (SoupAuthNTLM *) sa;

	g_free (auth->response);
	g_free (auth);
}

static SoupAuth *
ntlm_new (void)
{
	SoupAuthNTLM *auth;

	auth = g_new0 (SoupAuthNTLM, 1);
	auth->auth.type = SOUP_AUTH_TYPE_NTLM;

	auth->auth.compare_func = ntlm_compare_func;
	auth->auth.parse_func = ntlm_parse;
	auth->auth.init_func = ntlm_init;
	auth->auth.auth_func = ntlm_auth;
	auth->auth.free_func = ntlm_free;

	return (SoupAuth *) auth;
}


/*
 * Generic Authentication Interface
 */

SoupAuth *
soup_auth_lookup (SoupContext  *ctx)
{
	GHashTable *auth_hash = ctx->server->valid_auths;
	SoupAuth *ret = NULL;
	gchar *mypath, *dir;

	if (!auth_hash) return NULL;

	mypath = g_strdup (ctx->uri->path);
	dir = mypath;

        do {
                ret = g_hash_table_lookup (auth_hash, mypath);
                if (ret) break;

                dir = strrchr (mypath, '/');
                if (dir) *dir = '\0';
        } while (dir);

	g_free (mypath);
	return ret;
}

void
soup_auth_set_context (SoupAuth *auth, SoupContext *ctx)
{
	SoupHost *server;
	SoupAuth *old_auth = NULL;
	gchar *old_path;
	const SoupUri *uri;

	g_return_if_fail (ctx != NULL);
	g_return_if_fail (auth != NULL);

	server = ctx->server;
	uri = soup_context_get_uri (ctx);

	if (!server->valid_auths)
		server->valid_auths = 
			g_hash_table_new (g_str_hash, g_str_equal);
	else if (g_hash_table_lookup_extended (server->valid_auths, 
					       uri->path,
					       (gpointer *) &old_path,
					       (gpointer *) &old_auth)) {
		g_free (old_path);
		soup_auth_free (old_auth);
	}

	g_hash_table_insert (server->valid_auths,
			     g_strdup (uri->path),
			     auth);
}

typedef SoupAuth *(*SoupAuthNewFn) (void);

typedef struct {
	const gchar   *scheme;
	SoupAuthNewFn  ctor;
	gint           strength;
} AuthScheme; 

static AuthScheme known_auth_schemes [] = {
	{ "Basic",  soup_auth_new_basic,  0 },
	{ "NTLM",   ntlm_new,             2 },
	{ "Digest", soup_auth_new_digest, 3 },
	{ NULL }
};

SoupAuth *
soup_auth_new_from_header_list (const GSList  *vals)
{
	gchar *header = NULL;
	AuthScheme *scheme = NULL, *iter;
	SoupAuth *auth = NULL;

	g_return_val_if_fail (vals != NULL, NULL);

	while (vals) {
		for (iter = scheme = known_auth_schemes; iter->scheme; iter++) {
			if (!g_strncasecmp ((gchar *) vals->data, 
					    iter->scheme, 
					    strlen (iter->scheme)) &&
			    scheme->strength < iter->strength) {
				header = vals->data;			
				scheme = iter;
				break;
			}
		}

		vals = vals->next;
	}

	if (!scheme) return NULL;

	auth = scheme->ctor ();
	if (!auth) return NULL;

	if (!auth->compare_func || 
	    !auth->parse_func || 
	    !auth->init_func || 
	    !auth->auth_func || 
	    !auth->free_func)
		g_error ("Faulty Auth Created!!");

	auth->parse_func (auth, header);

	return auth;
}

void
soup_auth_initialize (SoupAuth *auth, const SoupUri *uri)
{
	g_return_if_fail (auth != NULL);
	g_return_if_fail (uri != NULL);

	auth->init_func (auth, uri);
}

gchar *
soup_auth_authorize (SoupAuth *auth, SoupMessage *msg)
{
	g_return_val_if_fail (auth != NULL, NULL);
	g_return_val_if_fail (msg != NULL, NULL);

	return auth->auth_func (auth, msg);
}

void
soup_auth_free (SoupAuth *auth)
{
	g_return_if_fail (auth != NULL);

	g_free (auth->realm);
	auth->free_func (auth);
}

gboolean
soup_auth_invalidates_prior (SoupAuth *new_auth, SoupAuth *old_auth)
{
	g_return_val_if_fail (new_auth != NULL, FALSE);
	g_return_val_if_fail (old_auth != NULL, TRUE);

	if (new_auth == old_auth) 
		return FALSE;

	if (new_auth->type != old_auth->type) 
		return TRUE;

	return new_auth->compare_func (new_auth, old_auth);
}


/*
 * Internal parsing routines
 */

static char *
copy_token_if_exists (GHashTable *tokens, char *t)
{
	char *data;

	g_return_val_if_fail (tokens, NULL);
	g_return_val_if_fail (t, NULL);

	if ( (data = g_hash_table_lookup (tokens, t)))
		return g_strdup (data);
	else
		return NULL;
}

static void
decode_lwsp (char **in)
{
	char *inptr = *in;

	while (isspace (*inptr))
		inptr++;

	*in = inptr;
}

static char *
decode_quoted_string (char **in)
{
	char *inptr = *in;
	char *out = NULL, *outptr;
	int outlen;
	int c;

	decode_lwsp (&inptr);
	if (*inptr == '"') {
		char *intmp;
		int skip = 0;

                /* first, calc length */
                inptr++;
                intmp = inptr;
                while ( (c = *intmp++) && c != '"') {
                        if (c == '\\' && *intmp) {
                                intmp++;
                                skip++;
                        }
                }

                outlen = intmp - inptr - skip;
                out = outptr = g_malloc (outlen + 1);

                while ( (c = *inptr++) && c != '"') {
                        if (c == '\\' && *inptr) {
                                c = *inptr++;
                        }
                        *outptr++ = c;
                }
                *outptr = 0;
        }

        *in = inptr;

        return out;
}

static char *
decode_token (char **in)
{
	char *inptr = *in;
	char *start;

	decode_lwsp (&inptr);
	start = inptr;

	while (*inptr && *inptr != '=' && *inptr != ',')
		inptr++;

	if (inptr > start) {
		*in = inptr;
		return g_strndup (start, inptr - start);
	}
	else
		return NULL;
}

static char *
decode_value (char **in)
{
	char *inptr = *in;

	decode_lwsp (&inptr);
	if (*inptr == '"')
		return decode_quoted_string (in);
	else
		return decode_token (in);
}

static GHashTable *
parse_param_list (const char *header)
{
	char *ptr;
	gboolean added = FALSE;
	GHashTable *params = g_hash_table_new (soup_str_case_hash, 
					       soup_str_case_equal);

	ptr = (char *) header;
	while (ptr && *ptr) {
		char *name;
		char *value;

		name = decode_token (&ptr);
		if (*ptr == '=') {
			ptr++;
			value = decode_value (&ptr);
			g_hash_table_insert (params, name, value);
			added = TRUE;
		}

		if (*ptr == ',')
			ptr++;
	}

	if (!added) {
		g_hash_table_destroy (params);
		params = NULL;
	}

	return params;
}

static void
destroy_param_hash_elements (gpointer key, gpointer value, gpointer user_data)
{
	g_free (key);
	g_free (value);
}

static void
destroy_param_hash (GHashTable *table)
{
	g_hash_table_foreach (table, destroy_param_hash_elements, NULL);
	g_hash_table_destroy (table);
}
