/*
 * Copyright (c) 2016, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *	 * Redistributions of source code must retain the above copyright
 *	   notice, this list of conditions and the following disclaimer.
 *	 * Redistributions in binary form must reproduce the above copyright
 *	   notice, this list of conditions and the following disclaimer in the
 *	   documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#ifdef WITH_MEMCACHED
#include "libmemcached/memcached.h"
#endif
#include "cfg_file.h"
#include "cache.h"
#include "hiredis.h"
#include "rmilter.h"
#include "upstream.h"
#include "util.h"
#include <assert.h>

#define DEFAULT_REDIS_PORT 6379

#ifdef WITH_MEMCACHED
static inline bool compat_memcached_success(int rc)
{
	return (rc == MEMCACHED_BUFFERED ||
			rc == MEMCACHED_DELETED ||
			rc == MEMCACHED_END ||
			rc == MEMCACHED_ITEM ||
			rc == MEMCACHED_STAT ||
			rc == MEMCACHED_STORED ||
			rc == MEMCACHED_SUCCESS ||
			rc == MEMCACHED_VALUE);
}

static inline bool compat_memcached_fatal(int rc)
{
	return (
			rc != MEMCACHED_BUFFERED &&
			rc != MEMCACHED_DATA_EXISTS &&
			rc != MEMCACHED_DELETED &&
			rc != MEMCACHED_END &&
			rc != MEMCACHED_ITEM &&
			rc != MEMCACHED_NOTFOUND &&
			rc != MEMCACHED_NOTSTORED &&
			rc != MEMCACHED_STAT &&
			rc != MEMCACHED_STORED &&
			rc != MEMCACHED_SUCCESS &&
			rc != MEMCACHED_VALUE);
}
#endif

static struct cache_server *
rmilter_get_server (struct config_file *cfg, enum rmilter_query_type type,
		const unsigned char *key, size_t keylen, struct mlfi_priv *priv)
{
	struct cache_server *serv = NULL;
	void *ptr = NULL;
	unsigned mlen = 0;

	switch (type) {
	case RMILTER_QUERY_GREYLIST:
		if (cfg->cache_servers_grey_num > 0) {
			ptr = cfg->cache_servers_grey;
			mlen = cfg->cache_servers_grey_num;
		}
		break;
	case RMILTER_QUERY_WHITELIST:
		if (cfg->cache_servers_white_num > 0) {
			ptr = cfg->cache_servers_white;
			mlen = cfg->cache_servers_white_num;
		}
		else if (cfg->cache_servers_grey_num > 0) {
			ptr = cfg->cache_servers_grey;
			mlen = cfg->cache_servers_grey_num;
		}
		break;
	case RMILTER_QUERY_RATELIMIT:
		if (cfg->cache_servers_limits_num > 0) {
			ptr = cfg->cache_servers_limits;
			mlen = cfg->cache_servers_limits_num;
		}
		break;
	case RMILTER_QUERY_ID:
		if (cfg->cache_servers_id_num > 0) {
			ptr = cfg->cache_servers_id;
			mlen = cfg->cache_servers_id_num;
		}
		break;
	}

	if (ptr) {
		serv = (struct cache_server *)get_upstream_by_hash (ptr, mlen,
				sizeof (*serv), time (NULL),
				cfg->cache_error_time, cfg->cache_dead_time,
				cfg->cache_maxerrors, key, keylen, priv);
	}

	return serv;
}

static struct cache_server *
rmilter_get_publish_server (struct config_file *cfg, enum rmilter_publish_type type,
		struct mlfi_priv *priv)
{
	struct cache_server *serv = NULL;
	void *ptr = NULL;
	unsigned mlen = 0;

	switch (type) {
	case RMILTER_PUBLISH_COPY:
		if (cfg->cache_servers_copy_num > 0) {
			ptr = cfg->cache_servers_copy;
			mlen = cfg->cache_servers_copy_num;
		}
		break;
	case RMILTER_PUBLISH_SPAM:
		if (cfg->cache_servers_spam_num > 0) {
			ptr = cfg->cache_servers_spam;
			mlen = cfg->cache_servers_spam_num;
		}
		break;
	}

	if (ptr) {
		serv = (struct cache_server *)get_upstream_master_slave (ptr, mlen,
				sizeof (*serv), time (NULL),
				cfg->cache_error_time, cfg->cache_dead_time,
				cfg->cache_maxerrors, priv);
	}

	return serv;
}

#ifdef WITH_MEMCACHED
static void
rmilter_format_libmemcached_config (struct config_file *cfg,
		struct cache_server *serv,
		memcached_st *ctx)
{
	if (serv->addr[0] == '/' || serv->addr[0] == '.') {
		/* Assume unix socket */
		memcached_server_add_unix_socket (ctx, serv->addr);
	}
	else {
		memcached_server_add (ctx, serv->addr, serv->port);
	}

	memcached_behavior_set (ctx, MEMCACHED_BEHAVIOR_CONNECT_TIMEOUT,
			cfg->cache_connect_timeout);
	memcached_behavior_set (ctx, MEMCACHED_BEHAVIOR_POLL_TIMEOUT,
				cfg->cache_connect_timeout);
}
#endif

bool
rmilter_query_cache (struct config_file *cfg, enum rmilter_query_type type,
		const unsigned char *key, size_t keylen,
		unsigned char **data, size_t *datalen, struct mlfi_priv *priv)
{
	struct cache_server *serv;
	redisContext *redis;
	redisReply *r = NULL;
	struct timeval tv;
	bool ret = false;
	size_t nelems = 1;
	int rep;

	serv = rmilter_get_server (cfg, type, key, keylen, priv);

	if (serv) {
		if (cfg->cache_use_redis) {
			/* Special workaround */
			if (serv->port == DEFAULT_MEMCACHED_PORT) {
				serv->port = DEFAULT_REDIS_PORT;
			}

			msec_to_tv (cfg->cache_connect_timeout, &tv);
			redis = redisConnectWithTimeout (serv->addr, serv->port, tv);

			if (redis == NULL || redis->err != 0) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
					(int)serv->port, redis ? redis->errstr : "unknown error");
				upstream_fail (&serv->up, time (NULL));

				if (redis) {
					redisFree (redis);
				}

				return false;
			}
			else {
				rep = 1;
				if (cfg->cache_password) {
					redisAppendCommand (redis, "AUTH %s",
							cfg->cache_password);
					rep ++;
				}
				if (cfg->cache_dbname) {
					redisAppendCommand (redis, "SELECT %s",
							cfg->cache_dbname);
					rep ++;
				}

				redisAppendCommand (redis, "GET %b", key, keylen);

				while (rep > 0) {
					redisGetReply (redis, (void **)&r);

					/* Ignore all replies but the last one */
					if (r != NULL && rep != 1) {
						freeReplyObject (r);
						r = NULL;
					}
					rep --;
				}

				if (r != NULL) {
					if (r->type == REDIS_REPLY_STRING && r->len > 0) {
						*data = malloc (r->len);
						if (*data) {
							memcpy (*data, r->str, r->len);
							ret = true;
							if (*datalen) {
								*datalen = r->len;
							}
						}
					}

					freeReplyObject (r);
				}

				redisFree (redis);
				upstream_ok (&serv->up, time (NULL));
			}
		}
		else {
#ifdef WITH_MEMCACHED
			char *kval;
			size_t value_len = 0;
			uint32_t mflags;
			int mret;
			memcached_st *mctx;

			mctx = memcached_create (NULL);
			rmilter_format_libmemcached_config (cfg, serv, mctx);

			if (mctx == NULL) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
					(int)serv->port, strerror (errno));
				upstream_fail (&serv->up, time (NULL));

				return false;
			}

			kval = memcached_get (mctx, key, keylen,
					&value_len, &mflags, &mret);

			if (!compat_memcached_success (mret)) {
				if (kval) {
					free (kval);
				}
				*datalen = 0;

				if (compat_memcached_fatal (mret)) {
					msg_err ("<%s>; cannot get key on %s:%d: %s", priv->mlfi_id, serv->addr,
						(int)serv->port, memcached_strerror (mctx, mret));
					upstream_fail (&serv->up, time (NULL));
				}
				else {
					upstream_ok (&serv->up, time (NULL));
				}
			}
			else {
				*data = kval;
				*datalen = value_len;
				upstream_ok (&serv->up, time (NULL));
				ret = true;
			}

			memcached_free (mctx);
#else
			msg_err ("<%s>; memcached query requested when memcached support is"
					" not compiled", priv->mlfi_id);
#endif
		}
	}

	return ret;
}

bool
rmilter_set_cache (struct config_file *cfg, enum rmilter_query_type type ,
		const unsigned char *key, size_t keylen,
		const unsigned char *data, size_t datalen,
		unsigned expire, struct mlfi_priv *priv)
{
	struct cache_server *serv;
	redisContext *redis;
	redisReply *r = NULL;
	struct timeval tv;
	size_t nelems = 1;
	int rep;

	serv = rmilter_get_server (cfg, type, key, keylen, priv);

	if (serv) {
		if (cfg->cache_use_redis) {
			if (serv->port == DEFAULT_MEMCACHED_PORT) {
				serv->port = DEFAULT_REDIS_PORT;
			}

			msec_to_tv (cfg->cache_connect_timeout, &tv);
			redis = redisConnectWithTimeout (serv->addr, serv->port, tv);

			if (redis == NULL || redis->err != 0) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
					(int)serv->port, redis ? redis->errstr : "unknown error");
				upstream_fail (&serv->up, time (NULL));

				if (redis) {
					redisFree (redis);
				}

				return false;
			}
			else {
				rep = 1;
				if (cfg->cache_password) {
					redisAppendCommand (redis, "AUTH %s",
							cfg->cache_password);
					rep ++;
				}
				if (cfg->cache_dbname) {
					redisAppendCommand (redis, "SELECT %s",
							cfg->cache_dbname);
					rep ++;
				}

				if (expire > 0) {
					redisAppendCommand (redis, "SETEX %b %d %b", key, keylen,
							expire, data, datalen);
				}
				else {
					redisAppendCommand (redis, "SET %b %b", key, keylen,
							data, datalen);
				}

				while (rep > 0) {
					redisGetReply (redis, (void **)&r);

					/* Ignore all replies but the last one */
					if (r != NULL && rep != 1) {
						freeReplyObject (r);
						r = NULL;
					}
					rep --;
				}

				if (r != NULL) {
					freeReplyObject (r);
				}

				redisFree (redis);
				upstream_ok (&serv->up, time (NULL));
			}
		}
		else {
#ifdef WITH_MEMCACHED
			char *kval;
			size_t value_len = 0;
			uint32_t mflags;
			int mret;
			memcached_st *mctx;

			mctx = memcached_create (NULL);
			rmilter_format_libmemcached_config (cfg, serv, mctx);

			if (mctx == NULL) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
					(int)serv->port, strerror (errno));
				upstream_fail (&serv->up, time (NULL));

				return false;
			}

			mret = memcached_set (mctx, key, keylen, data, datalen,
					expire, 0);

			if (!compat_memcached_success (mret)) {
				msg_err ("<%s>; cannot set key on %s:%d: %s", priv->mlfi_id, serv->addr,
					    (int)serv->port, memcached_strerror (mctx, mret));
				upstream_fail (&serv->up, time (NULL));
				memcached_free (mctx);

				return false;
			}
			else {
				upstream_ok (&serv->up, time (NULL));
			}

			memcached_free (mctx);
#else
		msg_err ("<%s>; memcached query requested when memcached support is"
				" not compiled", priv->mlfi_id);
#endif
		}
	}

	return true;
}

bool
rmilter_delete_cache (struct config_file *cfg, enum rmilter_query_type type ,
		const unsigned char *key, size_t keylen, struct mlfi_priv *priv)
{
	struct cache_server *serv;
	redisContext *redis;
	redisReply *r = NULL;
	struct timeval tv;
	size_t nelems = 1;
	int rep;

	serv = rmilter_get_server (cfg, type, key, keylen, priv);

	if (serv) {
		if (cfg->cache_use_redis) {
			if (serv->port == DEFAULT_MEMCACHED_PORT) {
				serv->port = DEFAULT_REDIS_PORT;
			}

			msec_to_tv (cfg->cache_connect_timeout, &tv);
			redis = redisConnectWithTimeout (serv->addr, serv->port, tv);

			if (redis == NULL || redis->err != 0) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
						(int)serv->port, redis ? redis->errstr : "unknown error");
				upstream_fail (&serv->up, time (NULL));

				if (redis) {
					redisFree (redis);
				}

				return false;
			}
			else {

				rep = 1;
				if (cfg->cache_password) {
					redisAppendCommand (redis, "AUTH %s",
							cfg->cache_password);
					rep ++;
				}
				if (cfg->cache_dbname) {
					redisAppendCommand (redis, "SELECT %s",
							cfg->cache_dbname);
					rep ++;
				}

				redisAppendCommand (redis, "DELETE %b", key, keylen);

				while (rep > 0) {
					redisGetReply (redis, (void **)&r);

					/* Ignore all replies but the last one */
					if (r != NULL && rep != 1) {
						freeReplyObject (r);
						r = NULL;
					}
					rep --;
				}

				if (r != NULL) {
					freeReplyObject (r);
				}

				redisFree (redis);
				upstream_ok (&serv->up, time (NULL));
			}
		}
		else {
#ifdef WITH_MEMCACHED
			char *kval;
			size_t value_len = 0;
			uint32_t mflags;
			int mret;
			memcached_st *mctx;

			mctx = memcached_create (NULL);
			rmilter_format_libmemcached_config (cfg, serv, mctx);

			if (mctx == NULL) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
						(int)serv->port, strerror (errno));
				upstream_fail (&serv->up, time (NULL));

				return false;
			}

			mret = memcached_delete (mctx, key, keylen, 0);

			if (!compat_memcached_success (mret)) {
				if (compat_memcached_fatal (mret)) {
					msg_err ("<%s>; cannot delete key on %s:%d: %s", priv->mlfi_id, serv->addr,
							(int)serv->port, memcached_strerror (mctx, mret));
					upstream_fail (&serv->up, time (NULL));
					memcached_free (mctx);

					return false;
				}
			}

			upstream_ok (&serv->up, time (NULL));
			memcached_free (mctx);
#else
			msg_err ("<%s>; memcached query requested when memcached support is"
					" not compiled", priv->mlfi_id);
#endif
		}

	}

	return true;
}

int
rmilter_publish_cache (struct config_file *cfg, enum rmilter_publish_type type,
		const unsigned char *channel, size_t channel_len,
		const unsigned char *data, size_t datalen,
		struct mlfi_priv *priv)
{
	struct cache_server *serv;
	redisContext *redis;
	redisReply *r = NULL;
	struct timeval tv;
	size_t nelems = 1;
	int rep, n = 0;

	serv = rmilter_get_publish_server (cfg, type, priv);

	if (serv) {
		if (cfg->cache_use_redis) {
			if (serv->port == DEFAULT_MEMCACHED_PORT) {
				serv->port = DEFAULT_REDIS_PORT;
			}

			msec_to_tv (cfg->cache_connect_timeout, &tv);
			redis = redisConnectWithTimeout (serv->addr, serv->port, tv);

			if (redis == NULL || redis->err != 0) {
				msg_err ("<%s>; cannot connect to %s:%d: %s", priv->mlfi_id, serv->addr,
					(int)serv->port, redis ? redis->errstr : "unknown error");
				upstream_fail (&serv->up, time (NULL));

				if (redis) {
					redisFree (redis);
				}

				return -1;
			}
			else {
				rep = 1;
				if (cfg->cache_password) {
					redisAppendCommand (redis, "AUTH %s",
							cfg->cache_password);
					rep ++;
				}
				if (cfg->cache_dbname) {
					redisAppendCommand (redis, "SELECT %s",
							cfg->cache_dbname);
					rep ++;
				}


				redisAppendCommand (redis, "PUBLISH %b %b", channel, channel_len,
						data, datalen);

				while (rep > 0) {
					redisGetReply (redis, (void **)&r);

					/* Ignore all replies but the last one */
					if (r != NULL && rep != 1) {
						freeReplyObject (r);
						r = NULL;
					}
					rep --;
				}

				if (r != NULL) {
					n = r->integer;
					freeReplyObject (r);
				}

				redisFree (redis);
				upstream_ok (&serv->up, time (NULL));
			}
		}
		else {
			msg_err ("<%s>; memcached query requested when pubsub is available "
					"only for redis", priv->mlfi_id);

			return -1;
		}
	}

	return n;
}
