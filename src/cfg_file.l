/*
 * Copyright (c) 2007-2012, Vsevolod Stakhov
 * All rights reserved.

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer. Redistributions in binary form
 * must reproduce the above copyright notice, this list of conditions and the
 * following disclaimer in the documentation and/or other materials provided with
 * the distribution. Neither the name of the author nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.

 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

%x incl
%x try_incl

%{
#include "config.h"
#include <glob.h>
#include "cfg_file.h"
#include "cfg_yacc.h"

#define MAX_INCLUDE_DEPTH 100
YY_BUFFER_STATE include_stack[MAX_INCLUDE_DEPTH];
int line_stack[MAX_INCLUDE_DEPTH];
FILE* files_stack[MAX_INCLUDE_DEPTH];
char* fnames_stack[MAX_INCLUDE_DEPTH];
int include_stack_ptr = 0;

static size_t
parse_limit (const char *limit)
{
	size_t result = 0;
	char *err_str;

	if (!limit || *limit == '\0') return 0;

	result = strtoul (limit, &err_str, 10);

	if (*err_str != '\0') {
		/* Megabytes */
		if (*err_str == 'm' || *err_str == 'M') {
			result *= 1048576L;
		}
		/* Kilobytes */
		else if (*err_str == 'k' || *err_str == 'K') {
			result *= 1024;
		}
		/* Gigabytes */
		else if (*err_str == 'g' || *err_str == 'G') {
			result *= 1073741824L;
		}
	}

	return result;
}

static unsigned int
parse_seconds (const char *t)
{
	unsigned long int result = 0;
	char *err_str;

	if (!t || *t == '\0') return 0;

	result = strtoul (t, &err_str, 10);

	if (*err_str != '\0') {
		/* Seconds */
		if (*err_str == 's' || *err_str == 'S') {
			result *= 1000;
		}
		/* Minutes */
		if (*err_str == 'm' || *err_str == 'M') {
			result *= 60 * 1000;
		}
		/* Hours */
		if (*err_str == 'h' || *err_str == 'H') {
			result *= 60 * 60 * 1000;
		}
		/* Days */
		if (*err_str == 'd' || *err_str == 'D') {
			result *= 24 * 60 * 60 * 1000;
		}
	}

	return result;
}

static char
parse_flag (const char *str)
{
	if (!str || !*str) return -1;

	if ((*str == 'y' || *str == 'Y') && *(str + 1) == '\0') {
		return 1;
	}

	if ((*str == 'Y' || *str == 'y') &&
		(*(str + 1) == 'E' || *(str + 1) == 'e') &&
		(*(str + 2) == 'S' || *(str + 2) == 's') &&
		*(str + 3) == '\0') {
		return 1;
	}

	if ((*str == 'n' || *str == 'N') && *(str + 1) == '\0') {
		return 0;
	}

	if ((*str == 'N' || *str == 'n') &&
		(*(str + 1) == 'O' || *(str + 1) == 'o') &&
		*(str + 2) == '\0') {
		return 0;
	}

	if (strcmp (str, "true") == 0 || strcmp (str, "TRUE") == 0) {
		return 1;
	}
	if (strcmp (str, "false") == 0 || strcmp (str, "FALSE") == 0) {
		return 0;
	}

	if (strcmp (str, "on") == 0 || strcmp (str, "ON") == 0) {
		return 1;
	}
	if (strcmp (str, "off") == 0 || strcmp (str, "OFF") == 0) {
		return 0;
	}

	return -1;
}

static void
parse_bucket (char *str, bucket_t *bucket)
{
	char *cur_tok, *err_str;

	bucket->burst = 0;
	bucket->rate = 0;

	cur_tok = strsep (&str, ":");
	if (cur_tok == NULL || *cur_tok == '\0' || str == NULL || *str == '\0') {
		yywarn ("parse_bucket: invalid bucket value %s", str);
		return;
	}

	bucket->burst = strtol (cur_tok, &err_str, 10);
	if (*err_str != '\0') {
		yywarn ("parse_bucket: invalid bucket value %s", err_str);
		bucket->burst = 0;
		return;
	}
	bucket->rate = strtod (str, &err_str);
	if (*err_str != '\0') {
		yywarn ("parse_bucket: invalid bucket value %s", err_str);
		bucket->rate = 0;
		return;
	}
}

#define LEX_INCLUDE_FILE(pattern, require) do { \
	FILE *f; \
	glob_t g; \
	unsigned int i; \
	const char *fname; \
	if (include_stack_ptr >= MAX_INCLUDE_DEPTH ) { \
		yyerror ("yylex: includes nested too deeply" ); \
	} \
	if (glob (pattern, 0, NULL, &g) != 0) { \
		if (require) { \
			yyerror ("yylex: cannot glob include file %s: %s", pattern, \
					strerror (errno)); \
		} \
		else { \
			if (errno != ENOENT) { \
				yywarn ("yylex: cannot glob include file %s: %s, ignore error", \
					pattern, strerror (errno)); \
			} \
		} \
	} \
	else { \
	if (require && g.gl_pathc == 0) { \
		yyerror ("yylex: cannot glob include file %s: %s", pattern, \
					strerror (errno)); \
	} \
	for (i = 0; i < g.gl_pathc; i ++) { \
		fname = g.gl_pathv[i]; \
		f = fopen (fname, "r"); \
		if (!f) { \
			if (require) { \
				globfree (&g); \
				yyerror ("yylex: cannot open include file %s: %s", fname, \
						strerror (errno)); \
			} \
			else { \
				if (errno != ENOENT) { \
					yywarn ("yylex: cannot open include file %s: %s, ignore error", \
						fname, strerror (errno)); \
				} \
			} \
		} \
		else { \
			line_stack[include_stack_ptr] = yylineno; \
			files_stack[include_stack_ptr] = yyin; \
			yyin = f; \
			fnames_stack[include_stack_ptr] = strdup (fname); \
			include_stack[include_stack_ptr++] = YY_CURRENT_BUFFER; \
			yylineno = 1; \
			yy_switch_to_buffer (yy_create_buffer (f, YY_BUF_SIZE)); \
		} \
	} \
	} \
	globfree (&g); \
} while (0)

%}

%option noyywrap
%option yylineno

dec-octet     [0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5]
IPv4address   {dec-octet}\.{dec-octet}\.{dec-octet}\.{dec-octet}
h16           [0-9A-Fa-f]{1,4}
ls32          {h16}:{h16}|{IPv4address}
IPv6address   ({h16}:){6}{ls32}|::({h16}:){5}{ls32}|({h16})?::({h16}:){4}{ls32}|(({h16}:){0,1}{h16})?::({h16}:){3}{ls32}|(({h16}:){0,2}{h16})?::({h16}:){2}{ls32}|(({h16}:){0,3}{h16})?::{h16}:{ls32}|(({h16}:){0,4}{h16})?::{ls32}|(({h16}:){0,5}{h16})?::{h16}|(({h16}:){0,6}{h16})?::
IPaddress      {IPv4address}|\[{IPv6address}\]
IPnetwork      {IPaddress}\/[0-9]{1,3}
flag           yes|YES|no|NO|true|TRUE|false|FALSE|on|ON|off|OFF
%%
%{
/* Rules section */
%}
^[ \t]*#.*						/* ignore comments */;
.include						BEGIN(incl);
.try_include					BEGIN(try_incl);
tempdir							return TEMPDIR;
tempfiles_mode					return TEMPFILES_MODE;
pidfile							return PIDFILE;
strict_auth						return STRICT_AUTH;
check_auth						return STRICT_AUTH;
rule							return RULE;
clamav							return CLAMAV;
spamd							return SPAMD;
also_check						return ALSO_CHECK;
diff_dir						return DIFF_DIR;
check_symbols					return CHECK_SYMBOLS;
symbols_dir						return SYMBOLS_DIR;
rspamd_metric					return RSPAMD_METRIC;
spamd_soft_fail					return SPAMD_SOFT_FAIL;
spamd_never_reject				return SPAMD_NEVER_REJECT;
spamd_greylist					return SPAMD_GREYLIST;
spamd_settings_id				return SPAMD_SETTINGS_ID;
spamd_spam_add_header				return SPAMD_SPAM_ADD_HEADER;
spam_header					return SPAM_HEADER;
spam_header_value				return SPAM_HEADER_VALUE;
spam_bar_char					return SPAM_BAR_CHAR;
spam_no_auth_header				return SPAM_NO_AUTH_HEADER;
extended_spam_headers			return EXTENDED_SPAM_HEADERS;
retry_count						return SPAMD_RETRY_COUNT;
retry_timeout					return SPAMD_RETRY_TIMEOUT;
tempfail_on_error				return SPAMD_TEMPFAIL;
reject_message					return REJECT_MESSAGE;
trace_symbol					return TRACE_SYMBOL;
trace_addr						return TRACE_ADDR;
servers							return SERVERS;
servers_limits					return SERVERS_LIMITS;
servers_grey					return SERVERS_GREY;
servers_white					return SERVERS_WHITE;
servers_id						return SERVERS_ID;
servers_copy					return SERVERS_COPY;
servers_spam					return SERVERS_SPAM;
error_time						return ERROR_TIME;
dead_time						return DEAD_TIME;
maxerrors						return MAXERRORS;
connect_timeout					return CONNECT_TIMEOUT;
port_timeout					return PORT_TIMEOUT;
results_timeout					return RESULTS_TIMEOUT;
id_prefix						return ID_PREFIX;
id_regexp						return ID_REGEXP;
lifetime						return LIFETIME;
grey_prefix						return GREY_PREFIX;
white_prefix					return WHITE_PREFIX;
memcached						return MEMCACHED;
redis							return REDIS;
copy_probability				return COPY_PROBABILITY;
use_redis						return USE_REDIS;
password						return PASSWORD;
dbname							return DBNAME;
spam_channel					return SPAM_CHANNEL;
copy_channel					return COPY_CHANNEL;
enable							return ENABLE;

dkim							return DKIM_SECTION;
key								return DKIM_KEY;
domain							return DKIM_DOMAIN;
selector						return DKIM_SELECTOR;
header_canon					return DKIM_HEADER_CANON;
body_canon						return DKIM_BODY_CANON;
sign_alg						return DKIM_SIGN_ALG;
auth_only						return DKIM_AUTH_ONLY;
fold_header						return DKIM_FOLD_HEADER;
sign_networks					return DKIM_SIGN_NETWORKS;

relaxed							return DKIM_RELAXED;
simple							return DKIM_SIMPLE;
sha1							return DKIM_SHA1;
sha256							return DKIM_SHA256;

protocol						return PROTOCOL;
spf_domains						return SPF;
bind_socket						return BINDSOCK;
max_size						return MAXSIZE;
use_dcc							return USEDCC;
greylisting						return GREYLISTING;
whitelist						return WHITELIST;
timeout							return TIMEOUT;
expire_white					return EXPIRE_WHITE;
expire							return EXPIRE;
greylisted_message				return GREYLISTED_MESSAGE;

limits							return LIMITS;
limit_to						return LIMIT_TO;
limit_to_ip						return LIMIT_TO_IP;
limit_to_ip_from				return LIMIT_TO_IP_FROM;
limit_whitelist					return LIMIT_WHITELIST;
limit_whitelist_rcpt			return LIMIT_WHITELIST_RCPT;
limit_bounce_addrs				return LIMIT_BOUNCE_ADDRS;
limit_bounce_to					return LIMIT_BOUNCE_TO;
limit_bounce_to_ip				return LIMIT_BOUNCE_TO_IP;

accept							return ACCEPT;
body							return BODY;
connect							return CONNECT;
discard							return DISCARD;
envfrom							return ENVFROM;
envrcpt							return ENVRCPT;
header							return HEADER;
helo							return HELO;
not								return NOT;
quarantine						return QUARANTINE;
reject							return REJECTL;
tempfail						return TEMPFAIL;
our_networks					return OUR_NETWORKS;


\"								return QUOTE;
\{								return OBRACE;
\}								return EBRACE;
;								return SEMICOLON;
,								return COMMA;
=								return EQSIGN;
{flag}							yylval.flag=parse_flag(yytext); return FLAG;
\n								/* ignore EOL */;
[ \t]+							/* ignore whitespace */;
\"[^"]*\"						yylval.string=trim_quotes(strdup(yytext)); return QUOTEDSTRING;
[0-9]+							yylval.number=strtol(yytext, NULL, 10); return NUMBER;
[0-9]+\.[0-9]*					yylval.frac=strtod(yytext, NULL); return FLOAT;
[0-9]+[kKmMgG]?					yylval.limit=parse_limit(yytext); return SIZELIMIT;
[0-9]+[sShHdD]|[0-9]+[mM][sS]	yylval.seconds=parse_seconds(yytext); return SECONDS;
[0-9]+:[0-9]+[.]?[0-9]*			parse_bucket(yytext, &yylval.bucket); return BUCKET;
unix:[a-zA-Z0-9\/.-]+ 			yylval.string=strdup(yytext); return SOCKCRED;
local:[a-zA-Z0-9\/.-]+ 			yylval.string=strdup(yytext); return SOCKCRED;
inet6?:[0-9]+(@\[?[a-zA-Z0-9.-:]+\]?)?	yylval.string=strdup(yytext); return SOCKCRED;
fd:[0-9]+						yylval.string=strdup(yytext); return SOCKCRED;
{IPaddress}						yylval.string=strdup(yytext); return IPADDR;
{IPnetwork}						yylval.string=strdup(yytext); return IPNETWORK;
\/[^/\n]+\/						yylval.string=strdup(yytext); return REGEXP;
[a-zA-Z<@][.a-zA-Z@+>_-]*			yylval.string=strdup(yytext); return STRING;
[a-zA-Z0-9].[a-zA-Z0-9\/.-]+	yylval.string=strdup(yytext); return DOMAIN_STR;
r?:?[a-zA-Z0-9.-]+:[0-9]{1,5}	yylval.string=strdup(yytext); return HOSTPORT;
r?:?[a-zA-Z0-9\/.-]+			yylval.string=strdup(yytext); return FILENAME;

<incl>[ \t]*      				/* eat the whitespace */
<incl>[^ \t\n]+   {
		LEX_INCLUDE_FILE(yytext, 1);
		BEGIN(INITIAL);
}

<try_incl>[ \t]*      			/* eat the whitespace */
<try_incl>[^ \t\n]+   {
		LEX_INCLUDE_FILE(yytext, 0);
		BEGIN(INITIAL);
}

<<EOF>> {
		if ( --include_stack_ptr < 0 ) {
			include_stack_ptr = 0;
			yylineno = 1;
			yyterminate ();
		}
		else {
			free (fnames_stack[include_stack_ptr + 1]);
			fclose (yyin);
			yyin = files_stack[include_stack_ptr];
			yy_delete_buffer (YY_CURRENT_BUFFER);
			yy_switch_to_buffer (include_stack[include_stack_ptr] );
			yylineno = line_stack[include_stack_ptr];
		}
}

%%
/*
 * vi:ts=4
 */
