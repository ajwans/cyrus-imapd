/*
 * Copyright (c) 1994-2011 Carnegie Mellon University.  All rights reserved.
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
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
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
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <string.h>
#include <sys/stat.h>

/* cyrus includes */
#include "assert.h"
#include "exitcodes.h"
#include "global.h"
#include "imap_err.h"
#include "index.h"
#include "conversations.h"
#include "mboxlist.h"
#include "message.h"
#include "sysexits.h"
#include "util.h"
#include "xmalloc.h"

#if !HAVE___ATTRIBUTE__
#define __attribute__(x)
#endif

/* config.c stuff */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;

int verbose = 0;

static int do_dump(const char *fname)
{
    struct conversations_state state;
    struct stat sb;
    int r;

    /* What we really want here is read-only database access without
     * the create-if-nonexistant semantics.  However, the cyrusdb
     * interface makes it difficult to do that properly.  In the
     * meantime, we can just check if the file exists here. */
    r = stat(fname, &sb);
    if (r < 0) {
	perror(fname);
	return -1;
    }

    r = conversations_open(&state, fname);
    if (r) {
	/* TODO: wouldn't it be nice if we could translate this
	 * error code into somethine useful for humans? */
	fprintf(stderr, "Failed to open conversations database %s: %d\n",
		fname, r);
	return -1;
    }

    conversations_dump(&state, stdout);

    conversations_close(&state);
    return 0;
}

static int do_undump(const char *fname)
{
    struct conversations_state state;
    int r;

    r = conversations_open(&state, fname);
    if (r) {
	/* TODO: wouldn't it be nice if we could translate this
	 * error code into somethine useful for humans? */
	fprintf(stderr, "Failed to open conversations database %s: %s\n",
		fname, error_message(r));
	return -1;
    }

    r = conversations_truncate(&state);
    if (r) {
	fprintf(stderr, "Failed to truncate conversations database %s: %s\n",
		fname, error_message(r));
	goto out;
    }

    r = conversations_undump(&state, stdin);
    if (r) {
	fprintf(stderr, "Failed to undump to conversations database %s: %s\n",
		fname, error_message(r));
	goto out;
    }

    r = conversations_commit(&state);
    if (r)
	fprintf(stderr, "Failed to commit conversations database %s: %s\n",
		fname, error_message(r));

out:
    conversations_close(&state);
    return r;
}

static int zero_cid_cb(const char *mboxname,
		       int matchlen __attribute__((unused)),
		       int maycreate __attribute__((unused)),
		       void *rock __attribute__((unused)))
{
    struct mailbox *mailbox = NULL;
    struct index_record record;
    int r;
    uint32_t recno;

    r = mailbox_open_iwl(mboxname, &mailbox);
    if (r) return r;

    for (recno = 1; recno <= mailbox->i.num_records; recno++) {
	r = mailbox_read_index_record(mailbox, recno, &record);
	if (r) goto done;

	/* already zero, fine */
	if (record.cid == NULLCONVERSATION)
	    continue;
    
	record.cid = NULLCONVERSATION;
	r = mailbox_rewrite_index_record(mailbox, &record);
	if (r) goto done;
    }

 done:
    mailbox_close(&mailbox);
    return r;
}

static int do_zero(const char *inboxname, const char *fname)
{
    char buf[MAX_MAILBOX_NAME];
    int r;

    /* XXX: truncate?  What about corruption */
    unlink(fname);

    r = zero_cid_cb(inboxname, 0, 0, NULL);
    if (r) return r;

    snprintf(buf, sizeof(buf), "%s.*", inboxname);
    r = mboxlist_findall(NULL, buf, 1, NULL,
			 NULL, zero_cid_cb, NULL);

    return r;
}

static int build_cid_cb(const char *mboxname,
		        int matchlen __attribute__((unused)),
		        int maycreate __attribute__((unused)),
		        void *rock __attribute__((unused)))
{
    struct mailbox *mailbox = NULL;
    const char *fname = NULL;
    struct index_record record;
    uint32_t recno;
    int r;

    r = mailbox_open_iwl(mboxname, &mailbox);
    if (r) return r;

    r = mailbox_open_conversations(mailbox);
    if (r) goto done;

    for (recno = 1; recno <= mailbox->i.num_records; recno++) {
	r = mailbox_read_index_record(mailbox, recno, &record);
	if (r) goto done;

	/* already assigned, fine */
	if (record.cid != NULLCONVERSATION)
	    continue;

	/* we don't care about expunged messages */
	if (record.system_flags & FLAG_EXPUNGED)
	    continue;
    
	/* parse the file - XXX: should abstract this bit */
	fname = mailbox_message_fname(mailbox, record.uid);

	r = message_update_conversations_file(&mailbox->cstate, &record, fname);
	if (r) goto done;

	r = mailbox_rewrite_index_record(mailbox, &record);
	if (r) goto done;
    }

 done:
    mailbox_close(&mailbox);
    return r;
}

static int do_build(const char *inboxname)
{
    char buf[MAX_MAILBOX_NAME];
    int r;

    r = build_cid_cb(inboxname, 0, 0, NULL);
    if (r) return r;

    snprintf(buf, sizeof(buf), "%s.*", inboxname);
    r = mboxlist_findall(NULL, buf, 1, NULL,
			 NULL, build_cid_cb, NULL);
    return r;
}

static int usage(const char *name)
    __attribute__((noreturn));

int main(int argc, char **argv)
{
    int c;
    const char *alt_config = NULL;
    const char *userid = NULL;
    const char *inboxname = NULL;
    char *fname;
    int r = 0;
    enum { UNKNOWN, DUMP, UNDUMP, ZERO, BUILD } mode = UNKNOWN;

    if ((geteuid()) == 0 && (become_cyrus() != 0)) {
	fatal("must run as the Cyrus user", EC_USAGE);
    }

    while ((c = getopt(argc, argv, "duzbvC:")) != EOF) {
	switch (c) {
	case 'd':
	    if (mode != UNKNOWN)
		usage(argv[0]);
	    mode = DUMP;
	    break;

	case 'u':
	    if (mode != UNKNOWN)
		usage(argv[0]);
	    mode = UNDUMP;
	    break;

	case 'z':
	    if (mode != UNKNOWN)
		usage(argv[0]);
	    mode = ZERO;
	    break;

	case 'b':
	    if (mode != UNKNOWN)
		usage(argv[0]);
	    mode = BUILD;
	    break;

	case 'v':
	    verbose++;
	    break;

	case 'C': /* alt config file */
	    alt_config = optarg;
	    break;

	default:
	    usage(argv[0]);
	    break;
	}
    }

    if (mode == UNKNOWN)
	usage(argv[0]);

    if (optind == argc-1)
	userid = argv[optind];
    else
	usage(argv[0]);

    cyrus_init(alt_config, "ctl_conversationsdb", 0);

    mboxlist_init(0);
    mboxlist_open(NULL);


    inboxname = mboxname_user_inbox(userid);
    if (inboxname == NULL) {
	fprintf(stderr, "Invalid userid %s", userid);
	exit(EC_NOINPUT);
    }

    fname = conversations_getpath(inboxname);
    if (fname == NULL) {
	fprintf(stderr, "Unable to get conversations database "
			"filename for userid \"%s\"\n",
			userid);
	exit(EC_NOINPUT);
    }

    switch (mode)
    {
    case DUMP:
	if (do_dump(fname))
	    r = EC_NOINPUT;
	break;

    case UNDUMP:
	if (do_undump(fname))
	    r = EC_NOINPUT;
	break;

    case ZERO:
	if (do_zero(inboxname, fname))
	    r = EC_NOINPUT;
	break;

    case BUILD:
	if (do_build(inboxname))
	    r = EC_NOINPUT;
	break;

    case UNKNOWN:
	fatal("UNKNOWN MODE", EC_SOFTWARE);
    }
    
    mboxlist_close();
    mboxlist_done();

    cyrus_done();
    free(fname);
    fname = NULL;

    return r;
}

static int usage(const char *name)
{
    fprintf(stderr, "usage: %s [options] [-u|-d|-z|-f] username\n", name);
    fprintf(stderr, "\n");
    fprintf(stderr, "options are:\n");
    fprintf(stderr, "    -v             be more verbose\n");
    fprintf(stderr, "    -C altconfig   use altconfig instead of imapd.conf\n");
    fprintf(stderr, "    -u             undump the conversations database from stdin\n");
    fprintf(stderr, "    -d             dump the conversations database to stdout\n");
    fprintf(stderr, "    -z             zero the conversations DB (make all NULLs)\n");
    fprintf(stderr, "    -b             build conversations entries for any NULL records\n");

    exit(EC_USAGE);
}

void fatal(const char* s, int code)
{
    fprintf(stderr, "ctl_conversationsdb: %s\n", s);
    cyrus_done();
    exit(code);
}
