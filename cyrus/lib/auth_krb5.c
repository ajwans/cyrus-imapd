/* auth_krb5.c -- Kerberos V authorization for Cyrus IMAP
 * $Id: auth_krb5.c,v 1.2 2003/10/22 18:03:03 rjs3 Exp $
 * Copyright (c) 1998-2003 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any other legal
 *    details, please contact  
 *      Office of Technology Transfer
 *      Carnegie Mellon University
 *      5000 Forbes Avenue
 *      Pittsburgh, PA  15213-3890
 *      (412) 268-4387, fax: (412) 268-7395
 *      tech-transfer@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>

#include <krb5.h>

#include "auth.h"
#include "xmalloc.h"

const char *auth_method_desc = "krb5";

struct auth_state {
    char *userid; /* Canonified Userid */
};

/*
 * Determine if the user is a member of 'identifier'
 * Returns one of:
 * 	0	User does not match identifier
 * 	1	identifier matches everybody
 *	2	User is in the group that is identifier
 *	3	User is identifer
 */
int auth_memberof(struct auth_state *auth_state, const char *identifier)
{
    char *ident;
    int ret=0;

    if (strcmp(identifier,"anyone") == 0) return 1;
    if (!auth_state && !strcmp(identifier, "anonymous")) return 3;
    if (strcmp(identifier,auth_state->userid) == 0) return 3;
    if (strcmp(auth_state->userid,"anonymous") == 0) return 0;

    ident = auth_canonifyid(identifier,0);

    if(!strcmp(ident, auth_state->userid)) {
	ret = 3;
    }
    
    return ret;
}

/*
 * Convert 'identifier' into canonical form.
 * Returns a pointer to a static buffer containing the canonical form
 * or NULL if 'identifier' is invalid.
 */
char *auth_canonifyid(const char *identifier, size_t len)
{
    static char *retbuf = NULL;
    krb5_context context;
    krb5_principal princ, princ_dummy;
    char *realm;
    int striprealm = 0;

    if(retbuf) free(retbuf);
    retbuf = NULL;

    if(!identifier) return NULL;
    if(!len) len = strlen(identifier);

    if (strcasecmp(identifier, "anonymous") == 0)
	return "anonymous";
    
    if (strcasecmp(identifier, "anyone") == 0) 
	return "anyone";

    if (krb5_init_context(&context))
	return NULL;

    if (krb5_parse_name(context,identifier,&princ))
    {
	krb5_free_context(context);
	return NULL;
    }

    /* get local realm */
    if (krb5_get_default_realm(context,&realm))
    {
	krb5_free_principal(context,princ);
	krb5_free_context(context);
	return NULL;
    }

    /* build dummy princ to compare realms */
    if (krb5_build_principal(context,&princ_dummy,
			     strlen(realm),realm,"dummy",0))
    {
	krb5_free_principal(context,princ);
	krb5_free_context(context);
	free(realm);
	return NULL;
    }

    /* is this principal local ? */
    if (krb5_realm_compare(context,princ,princ_dummy))
    {
	striprealm = 1;
    }

    /* done w/ dummy princ free it & realm */
    krb5_free_principal(context,princ_dummy);
    free(realm);

    /* get the text version of princ */
    if (krb5_unparse_name(context,princ,&retbuf))
    {
	krb5_free_principal(context,princ);
	krb5_free_context(context);
	return NULL;
    }

    /* we have the canonical name pointed to by p -- strip realm if local */
    if (striprealm)
    {
	char *realmbegin = strrchr(retbuf, '@');
	if(realmbegin) *realmbegin = '\0';
    }
    
    krb5_free_principal(context,princ);
    krb5_free_context(context);	
    return retbuf;
}

/*
 * Set the current user to 'identifier'.
 */
struct auth_state *auth_newstate(const char *identifier)
{
    struct auth_state *newstate;
    char *ident;
    ident = auth_canonifyid(identifier, 0);
    if (!ident) return NULL;

    newstate = (struct auth_state *)xmalloc(sizeof(struct auth_state));
    newstate->userid = xstrdup(ident);   

    return newstate;
}

void auth_freestate(struct auth_state *auth_state)
{
    if(!auth_state) return;
    
    free(auth_state->userid);
    free(auth_state);
}

